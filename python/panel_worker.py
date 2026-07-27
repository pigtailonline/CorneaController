"""CorneaController subprocess worker — owns ONE panel's CorneaRax720 instance.

Phase 1 prototype for the per-panel-subprocess refactor that replaces CC
server's embedded Python multi-panel model. Each worker process imports
cornea_rax720, holds ONE instance, and serves JSON-RPC requests over
stdin/stdout. Six panels => six worker processes => six independent
Python interpreters => six independent GILs. No more cross-panel GIL
contention.

Protocol
--------
Requests come in line-delimited JSON on stdin. Each request:
    {"id": <int>, "cmd": "<name>", "args": {...}}

Each response on stdout (also line-delimited):
    {"id": <int>, "success": <bool>, "data": {...}, "error": "<str>"}

Diagnostic / log output goes to STDERR — never stdout, so the parent
process's response parser doesn't get confused by lib chatter. The
cornea_rax720 logger is rerouted to stderr too.

Commands
--------
init        Construct the CorneaRax720 instance (called once, before any
            powerOn/sendImage). args: cornea_index, hardware_variant,
            cal_path, spi_clk_freq, allow_default_hdf5, init_cornea,
            init_rj1.
powerOn     system_power_on() — returns init_ok (bool).
powerOff    system_power_off() — no return value.
setBrightness  args: level (0.0-1.0).
getBrightness  → float.
getPanelId  → str (UCID); empty string if not available yet.
getTemperature  args: cached (bool, default false). When false, calls
            get_lea_temperature(); when true, just returns the last cached
            HW read. Cache is owned by this worker process.
sendImage   args: path (str) — load .png / .raw from disk and write_rj1_frame.
            Phase 2 will add raw bytes via stdin; for Phase 1 we keep it
            file-based to match the CC server's existing imageloader path.
shutdown    Graceful exit. Cleans up the instance + closes the stdout pipe.
ping        Liveness check — no side effects.

Phase 1 success criteria
------------------------
Manual:
    echo {"id":1,"cmd":"init","args":{"cornea_index":0,"hardware_variant":"F33L",
         "cal_path":"D:/cornea/hdf5_files"}}
    {"id":1,"success":true,...}
    echo {"id":2,"cmd":"powerOn"}
    {"id":2,"success":true,"data":{"init_ok":true}}
    echo {"id":3,"cmd":"getPanelId"}
    {"id":3,"success":true,"data":{"panel_id":"05CD0058"}}

If this runs end-to-end in a fresh python.exe, the GIL-isolation
hypothesis is validated and we can proceed to Phase 2 (C++ QProcess
wiring).
"""
from __future__ import annotations

import json
import logging
import os
import gc
import subprocess
import sys
import threading
import time
import traceback
from pathlib import Path


# ---------------------------------------------------------------------------
# stderr-only logging — stdout is reserved for the JSON protocol
# ---------------------------------------------------------------------------
logging.basicConfig(
    stream=sys.stderr,
    level=logging.INFO,
    format="%(asctime)s.%(msecs)03d [%(levelname)s] %(name)s: %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("panel_worker")


class UsbGateTimeout(RuntimeError):
    """The process-wide USB gate could not be acquired within its deadline."""


class _UsbGate:
    """Cross-process guard for every pyftdi/libusb transaction.

    CorneaController normally serializes commands in the C++ parent.  The
    Windows named mutex is a second line of defence: it still protects the
    FT4232/libusb-win32 stack if two controller applications are accidentally
    started, or a future C++ call path bypasses the parent scheduler.
    """

    _NAME = r"Global\CorneaController_USB_GATE_v1"

    def __init__(self):
        self._local_lock = threading.Lock()
        self._handle = None
        if os.name == "nt":
            import ctypes
            self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            self._kernel32.CreateMutexW.argtypes = (
                ctypes.c_void_p, ctypes.c_bool, ctypes.c_wchar_p
            )
            self._kernel32.CreateMutexW.restype = ctypes.c_void_p
            self._kernel32.WaitForSingleObject.argtypes = (
                ctypes.c_void_p, ctypes.c_uint32
            )
            self._kernel32.WaitForSingleObject.restype = ctypes.c_uint32
            self._kernel32.ReleaseMutex.argtypes = (ctypes.c_void_p,)
            self._kernel32.ReleaseMutex.restype = ctypes.c_bool
            self._kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
            self._kernel32.CloseHandle.restype = ctypes.c_bool
            self._handle = self._kernel32.CreateMutexW(None, False, self._NAME)
            if not self._handle:
                raise OSError(ctypes.get_last_error(), "CreateMutexW failed")

    def acquire(self, timeout_s: float) -> int:
        started = time.monotonic()
        if self._handle is None:
            acquired = self._local_lock.acquire(timeout=timeout_s)
        else:
            # WAIT_OBJECT_0=0, WAIT_ABANDONED=0x80.  An abandoned mutex is
            # safely acquired by this process and proves crash recovery works.
            result = self._kernel32.WaitForSingleObject(
                self._handle, max(0, int(timeout_s * 1000))
            )
            acquired = result in (0x00000000, 0x00000080)
        wait_ms = int((time.monotonic() - started) * 1000)
        if not acquired:
            raise UsbGateTimeout(
                f"USB_BUSY_TIMEOUT: named USB gate wait exceeded "
                f"{int(timeout_s * 1000)}ms"
            )
        return wait_ms

    def release(self) -> None:
        if self._handle is None:
            self._local_lock.release()
        elif not self._kernel32.ReleaseMutex(self._handle):
            import ctypes
            raise OSError(ctypes.get_last_error(), "ReleaseMutex failed")

    def close(self) -> None:
        if self._handle is not None:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None


USB_GATE = _UsbGate()


class _PowerErrorCapture(logging.Handler):
    def __init__(self):
        super().__init__(level=logging.ERROR)
        self.messages: list[str] = []

    def emit(self, record: logging.LogRecord) -> None:
        msg = record.getMessage()
        if (
            "Failed to complete cornea_power_down" in msg
            or "Failure to sequence power supplies" in msg
            or "NACK from slave" in msg
        ):
            self.messages.append(msg)


def _handler_logger(logger):
    if logger is None:
        return None
    if hasattr(logger, "addHandler") and hasattr(logger, "removeHandler"):
        return logger
    wrapped = getattr(logger, "logger", None)
    if hasattr(wrapped, "addHandler") and hasattr(wrapped, "removeHandler"):
        return wrapped
    return None


def emit(resp: dict) -> None:
    """Write one line of JSON to stdout, flushed immediately. stdout is
    pipe-buffered by default which would defeat the protocol if the parent
    is doing a blocking read."""
    sys.stdout.write(json.dumps(resp, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def ok(req_id, **data) -> dict:
    return {"id": req_id, "success": True, "data": data}


def err(req_id, message: str, error_code: str = "") -> dict:
    response = {"id": req_id, "success": False, "error": message}
    if error_code:
        response["errorCode"] = error_code
    return response


# ---------------------------------------------------------------------------
# Worker state — one cornea instance per process
# ---------------------------------------------------------------------------
class Worker:
    def __init__(self):
        self.cornea = None              # CorneaRax720 instance
        self.cached_temp: float = -999.0  # last successful get_lea_temperature
        self.cal_path: str = ""
        self.defect_map_output_root: str = ""
        self.python_exe: str = sys.executable
        self._exported_defect_map_ids: set[str] = set()

    def cmd_init(self, args: dict) -> dict:
        if self.cornea is not None:
            return {"already_initialized": True}

        self.cal_path = str(args.get("cal_path", ""))
        self.defect_map_output_root = str(args.get("defect_map_output_root", ""))

        # Import deferred — keeps `ping` / `init` self-tests fast and lets
        # the parent test stdin/stdout wiring without paying the ~1-3 s
        # cornea_rax720 import cost.
        from ar_display_lab_lib.control_boards.cornea_rax720 import CorneaRax720

        expected_serial = str(args.get("cornea_serial") or "").strip()
        resolved_index = int(args.get("cornea_index", 0))
        if expected_serial:
            # Never trust a cached numeric index.  Windows can reorder FT4232
            # devices after a transient disconnect, so require two consecutive
            # identical enumerations and resolve the current index by serial.
            stable_result = None
            previous_result = None
            for enum_attempt in range(3):
                try:
                    from pyftdi.usbtools import UsbTools
                    UsbTools.flush_cache()
                except Exception as e:
                    log.warning("pyftdi cache flush before init failed: %s", e)

                indices, serials = CorneaRax720.get_available_corneas()
                current_result = (
                    tuple(int(v) for v in indices),
                    tuple(str(v) for v in serials),
                )
                log.info(
                    "USB enumeration %d/3: indices=%s serials=%s",
                    enum_attempt + 1, current_result[0], current_result[1],
                )
                if current_result == previous_result:
                    stable_result = current_result
                    break
                previous_result = current_result
                time.sleep(0.2)

            if stable_result is None:
                raise RuntimeError(
                    "SERIAL_NOT_FOUND: USB enumeration did not stabilize "
                    "for two consecutive reads"
                )
            indices, serials = stable_result
            try:
                serial_pos = serials.index(expected_serial)
            except ValueError as exc:
                raise RuntimeError(
                    f"SERIAL_NOT_FOUND: expected {expected_serial}, "
                    f"visible={list(serials)}"
                ) from exc
            resolved_index = indices[serial_pos]
            log.info(
                "Resolved expected serial %s: requested_index=%s current_index=%s",
                expected_serial, args.get("cornea_index"), resolved_index,
            )

        kwargs = {
            "cornea_index":      resolved_index,
            "init_cornea":       bool(args.get("init_cornea", True)),
            "init_rj1":          bool(args.get("init_rj1", True)),
            "cal_path":          str(args.get("cal_path", "")),
            "rj1_use_i2c":       True,
            "rj1_use_spi":       True,
            "allow_default_hdf5": bool(args.get("allow_default_hdf5", False)),
            "cal_revision":      None,
            "cornea_serial":     args.get("cornea_serial"),
            "rj1_version":       None,
            "spi_clk_freq":      float(args.get("spi_clk_freq", 15e6)),
            "console_log_level": int(args.get("console_log_level", 20)),
            "hardware_variant":  str(args.get("hardware_variant", "F33L")),
        }
        t0 = time.monotonic()
        self.cornea = CorneaRax720(**kwargs)
        dt_ms = int((time.monotonic() - t0) * 1000)
        log.info("CorneaRax720 ctor done in %d ms (variant=%s, index=%d)",
                 dt_ms, kwargs["hardware_variant"], kwargs["cornea_index"])
        init_ok = bool(getattr(self.cornea, "init_ok", True))
        actual_serial = str(getattr(self.cornea, "cornea_serial", "") or "")
        if expected_serial and actual_serial != expected_serial:
            try:
                self.cornea.system_power_off()
            except Exception as e:
                log.warning("power off after serial mismatch raised: %s", e)
            self.cornea = None
            gc.collect()
            raise RuntimeError(
                f"SERIAL_MISMATCH: expected {expected_serial}, "
                f"actual {actual_serial or '<empty>'}"
            )
        panel_id = self._require_panel_id("init") if init_ok else ""
        return {
            "init_ok": init_ok,
            "cornea_serial": actual_serial,
            "panel_id": panel_id,
            "duration_ms": dt_ms,
            "defect_maps": self._export_defect_maps_once(panel_id) if init_ok else {
                "ok": False,
                "skipped": True,
                "reason": "init_not_ok",
            },
        }

    def _require(self):
        if self.cornea is None:
            raise RuntimeError("instance not initialized; call 'init' first")
        return self.cornea

    def _current_panel_id(self) -> str:
        c = self._require()
        state_vals = getattr(c, "state_vals", None)
        if isinstance(state_vals, dict):
            for key in ("unique_chip_id_str", "panel_ucid", "ucid"):
                value = state_vals.get(key)
                if value:
                    return str(value)

        info = c.get_rj1_chip_info_decoded()
        return str(info.get("unique_chip_id_str", ""))

    def _require_panel_id(self, operation: str) -> str:
        panel_id = self._current_panel_id().strip()
        if not panel_id:
            raise RuntimeError(f"{operation} failed: empty Panel ID / UCID")
        return panel_id

    def _defect_map_output_dir(self, panel_id: str) -> Path:
        if self.defect_map_output_root:
            return Path(self.defect_map_output_root) / panel_id
        return Path(self.cal_path) / "defect_maps" / panel_id

    def _export_defect_maps_once(self, panel_id: str) -> dict:
        if not panel_id:
            return {"ok": False, "skipped": True, "reason": "empty_panel_id"}
        if not self.cal_path:
            return {"ok": False, "skipped": True, "reason": "empty_cal_path"}

        output_dir = self._defect_map_output_dir(panel_id)
        if panel_id in self._exported_defect_map_ids:
            return {
                "ok": True,
                "skipped": True,
                "reason": "already_exported",
                "output_path": str(output_dir),
            }

        output_dir.mkdir(parents=True, exist_ok=True)
        cmd = [
            self.python_exe,
            "-m",
            "ar_display_lab_lib.utilities.data_structures.hdf5_cal_file_updater",
            self.cal_path,
            panel_id,
            "--get-defect-maps",
            str(output_dir),
        ]
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=120,
            )
        except Exception as e:
            log.warning("defect map export failed for panel %s: %s", panel_id, e)
            return {"ok": False, "error": str(e), "output_path": str(output_dir)}

        if result.returncode != 0:
            error = (result.stderr or result.stdout or "").strip()
            log.warning("defect map export failed for panel %s: %s", panel_id, error)
            return {"ok": False, "error": error, "output_path": str(output_dir)}

        self._exported_defect_map_ids.add(panel_id)
        log.info("defect maps exported for panel %s -> %s", panel_id, output_dir)
        return {"ok": True, "output_path": str(output_dir)}

    def _maybe_export_defect_maps(self) -> dict:
        try:
            return self._export_defect_maps_once(self._current_panel_id())
        except Exception as e:
            log.warning("defect map auto-export skipped: %s", e)
            return {"ok": False, "error": str(e)}

    def cmd_powerOn(self, args: dict) -> dict:
        c = self._require()
        # rax_lib returns init_ok (bool). False means panel did not respond
        # (Pogo unseated / brown-out / etc.) but no exception is thrown —
        # mirror CC's PythonBridge::systemPowerOn handling so the parent
        # doesn't misinterpret a "False" return as "success".
        capture = _PowerErrorCapture()
        cornea_logger = _handler_logger(getattr(c, "logger", None))
        if cornea_logger is not None:
            cornea_logger.addHandler(capture)
        try:
            result = c.system_power_on()
        finally:
            if cornea_logger is not None:
                cornea_logger.removeHandler(capture)
        data = {"init_ok": bool(result)}
        if not result and capture.messages:
            raise RuntimeError(capture.messages[-1])
        if result:
            panel_id = self._require_panel_id("powerOn")
            data["panel_id"] = panel_id
            data["defect_maps"] = self._export_defect_maps_once(panel_id)
        return data

    def cmd_powerOff(self, args: dict) -> dict:
        c = self._require()
        capture = _PowerErrorCapture()
        cornea_logger = _handler_logger(getattr(c, "logger", None))
        if cornea_logger is not None:
            cornea_logger.addHandler(capture)
        try:
            c.system_power_off()
        finally:
            if cornea_logger is not None:
                cornea_logger.removeHandler(capture)
        if capture.messages:
            raise RuntimeError(capture.messages[-1])
        return {}

    def cmd_reset(self, args: dict) -> dict:
        """Release the CorneaRax720 object but keep this python.exe alive.

        The parent uses this between DUTs: the next init creates a fresh
        hardware instance while avoiding Python process startup/import cost.
        """
        had_instance = self.cornea is not None
        if self.cornea is not None:
            try:
                self.cornea.system_power_off()
            except Exception as e:
                log.warning("system_power_off during reset raised: %s", e)
            self.cornea = None
            gc.collect()
        self.cached_temp = -999.0
        self.cal_path = ""
        self.defect_map_output_root = ""
        return {"had_instance": had_instance}

    def cmd_setBrightness(self, args: dict) -> dict:
        c = self._require()
        level = float(args["level"])
        c.set_brightness(level)
        return {"level": level}

    def cmd_getBrightness(self, args: dict) -> dict:
        c = self._require()
        return {"level": float(c.get_brightness())}

    def cmd_getPanelId(self, args: dict) -> dict:
        # rax_lib exposes panel UCID through the RJ1 chip info dict.
        # When the panel hasn't been programmed yet (cold pre-init state)
        # the field is absent — return empty string rather than raising.
        panel_id = self._require_panel_id("getPanelId")
        return {
            "panel_id": panel_id,
            "defect_maps": self._export_defect_maps_once(panel_id),
        }

    def cmd_getTemperature(self, args: dict) -> dict:
        c = self._require()
        if bool(args.get("cached", False)):
            return {"temperature": self.cached_temp, "from_cache": True}
        temp = c.get_lea_temperature()
        if temp is None or temp <= -900.0:
            return {"temperature": -999.0, "from_cache": False, "ok": False}
        self.cached_temp = float(temp)
        return {"temperature": self.cached_temp, "from_cache": False, "ok": True}

    def cmd_sendImage(self, args: dict) -> dict:
        """Load an image from disk (PNG / NPY) and write_rj1_frame.

        Phase 1 supports two source layouts:
          1. NPY: pre-baked uint16 frame array — fastest, no PIL needed
          2. PNG: 720×720 RGB — decoded inline; matches what CC server's
             imageloader builds via Qt's QImage. We accept anything PIL
             can open and let cornea_rax720 fail loudly if shape is wrong.
        """
        c = self._require()
        path = str(args["path"])
        if not Path(path).exists():
            raise FileNotFoundError(f"image not found: {path}")

        ext = Path(path).suffix.lower()
        if ext == ".npy":
            import numpy as np
            frame = np.load(path)
        elif ext in (".png", ".jpg", ".jpeg", ".bmp"):
            from PIL import Image  # pillow is in station_venv already
            import numpy as np
            # PIL loads as RGB; write_rj1_frame expects BGR (opencv_frame default).
            # Reverse channel order to match the non-subprocess C++ path which
            # builds BGR via qimageToPyArray + passes opencv_frame=True.
            img = Image.open(path).convert("RGB")
            frame = np.array(img)[:, :, ::-1]  # RGB → BGR
        else:
            raise ValueError(f"unsupported image extension: {ext}")

        t0 = time.monotonic()
        ok_ = c.write_rj1_frame(frame)
        dt_ms = int((time.monotonic() - t0) * 1000)
        return {"ok": bool(ok_), "duration_ms": dt_ms, "shape": list(frame.shape)}

    def cmd_setXFlip(self, args: dict) -> dict:
        c = self._require()
        # rax_lib's API is rj1_set_x_flip_offset(flip, offset). CC server
        # only carries a boolean through its API surface; preserve the
        # current offset by reading it first and writing back unchanged.
        cur_flip, cur_offset = c.rj1_get_x_flip_offset()
        c.rj1_set_x_flip_offset(flip=bool(args["flip"]), offset=int(cur_offset))
        return {"flip": bool(args["flip"]), "offset": int(cur_offset)}

    def cmd_setYFlip(self, args: dict) -> dict:
        c = self._require()
        cur_flip, cur_offset = c.rj1_get_y_flip_offset()
        c.rj1_set_y_flip_offset(flip=bool(args["flip"]), offset=int(cur_offset))
        return {"flip": bool(args["flip"]), "offset": int(cur_offset)}

    def cmd_getXFlip(self, args: dict) -> dict:
        c = self._require()
        flip, offset = c.rj1_get_x_flip_offset()
        return {"flip": bool(flip), "offset": int(offset)}

    def cmd_getYFlip(self, args: dict) -> dict:
        c = self._require()
        flip, offset = c.rj1_get_y_flip_offset()
        return {"flip": bool(flip), "offset": int(offset)}

    def cmd_getChipInfoDecoded(self, args: dict) -> dict:
        c = self._require()
        info = c.get_rj1_chip_info_decoded()
        # Force everything to JSON-friendly types — the dict from rax_lib
        # may contain ints / strs / bytes mixed; we coerce here so the
        # C++ side sees consistent QJsonValue types.
        clean = {}
        for k, v in info.items():
            if isinstance(v, (str, int, float, bool)) or v is None:
                clean[k] = v
            else:
                clean[k] = str(v)
        return {"info": clean}

    def cmd_getDa9272Temperature(self, args: dict) -> dict:
        c = self._require()
        # rax_lib's get_da9272_temperature often returns None / NACKs on
        # boards where that probe isn't wired. Return -999.0 sentinel so
        # CC's existing "if temp > -900" logic stays correct.
        try:
            t = c.get_da9272_temperature()
            return {"temperature": float(t) if t is not None else -999.0,
                    "ok": t is not None}
        except Exception:
            return {"temperature": -999.0, "ok": False}

    def cmd_ping(self, args: dict) -> dict:
        # No state access — usable before init too.
        return {"alive": True, "pid": os.getpid(), "initialized": self.cornea is not None}

    def cmd_shutdown(self, args: dict) -> dict:
        # Best-effort explicit power-off of the panel before we exit.
        # Python's __del__ on cornea_rax720 is unreliable at process exit
        # (interpreter teardown order, daemon-thread cleanup, etc.), so
        # leaving the power-off to GC means the panel rails stay up and
        # the operator sees "驅動板軟件 关机了光机还是亮的". CC's
        # destroyDeviceInstance also sends a powerOff before this, but
        # having it here protects standalone uses (tests, smoke runs).
        if self.cornea is not None:
            try:
                self.cornea.system_power_off()
                log.info("system_power_off OK during shutdown")
            except Exception as e:
                log.warning("system_power_off during shutdown raised: %s", e)
        # Returning {"goodbye": True} signals the main loop to break.
        return {"goodbye": True}


# ---------------------------------------------------------------------------
# Main read-loop
# ---------------------------------------------------------------------------
def main() -> int:
    log.info("panel_worker starting (pid=%d, python=%s)",
             os.getpid(), sys.version.split()[0])

    worker = Worker()
    # Map cmd name → method. Adding a new command means adding a method
    # named cmd_<X> and (optionally) registering here — keep introspection
    # explicit so the parent gets a clear 'unknown cmd' error rather than
    # an arbitrary attribute call.
    handlers = {
        "init":                 worker.cmd_init,
        "powerOn":              worker.cmd_powerOn,
        "powerOff":             worker.cmd_powerOff,
        "reset":                worker.cmd_reset,
        "setBrightness":        worker.cmd_setBrightness,
        "getBrightness":        worker.cmd_getBrightness,
        "getPanelId":           worker.cmd_getPanelId,
        "getTemperature":       worker.cmd_getTemperature,
        "sendImage":            worker.cmd_sendImage,
        "setXFlip":             worker.cmd_setXFlip,
        "setYFlip":             worker.cmd_setYFlip,
        "getXFlip":             worker.cmd_getXFlip,
        "getYFlip":             worker.cmd_getYFlip,
        "getChipInfoDecoded":   worker.cmd_getChipInfoDecoded,
        "getDa9272Temperature": worker.cmd_getDa9272Temperature,
        "ping":                 worker.cmd_ping,
        "shutdown":             worker.cmd_shutdown,
    }

    # Line-buffered stdin read. sys.stdin.readline() returns "" on EOF —
    # treat that as a clean shutdown signal (parent process closed pipe).
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError as e:
            emit(err(None, f"malformed JSON: {e}"))
            continue

        req_id = req.get("id")
        cmd = req.get("cmd", "")
        args = req.get("args", {}) or {}

        handler = handlers.get(cmd)
        if handler is None:
            emit(err(req_id, f"unknown cmd: {cmd!r}"))
            continue

        gate_required = cmd not in {"ping"}
        gate_acquired = False
        try:
            gate_wait_ms = 0
            if gate_required:
                gate_wait_ms = USB_GATE.acquire(60.0 if cmd == "init" else 30.0)
                gate_acquired = True
                log.info(
                    "[USB-GATE] acquired cmd=%s wait_ms=%d pid=%d",
                    cmd, gate_wait_ms, os.getpid(),
                )
            started = time.monotonic()
            data = handler(args) or {}
            if gate_required:
                log.info(
                    "[USB-GATE] done cmd=%s wait_ms=%d execute_ms=%d pid=%d",
                    cmd, gate_wait_ms,
                    int((time.monotonic() - started) * 1000), os.getpid(),
                )
            emit(ok(req_id, **data))
            if cmd == "shutdown":
                break
        except Exception as e:
            log.error("cmd %s raised: %s\n%s", cmd, e, traceback.format_exc())
            message = f"{type(e).__name__}: {e}"
            error_code = ""
            for candidate in (
                "USB_BUSY_TIMEOUT",
                "SERIAL_NOT_FOUND",
                "SERIAL_MISMATCH",
            ):
                if candidate in message:
                    error_code = candidate
                    break
            emit(err(req_id, message, error_code))
        finally:
            if gate_acquired:
                USB_GATE.release()

    log.info("panel_worker exiting cleanly")
    USB_GATE.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
