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
import sys
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


def emit(resp: dict) -> None:
    """Write one line of JSON to stdout, flushed immediately. stdout is
    pipe-buffered by default which would defeat the protocol if the parent
    is doing a blocking read."""
    sys.stdout.write(json.dumps(resp, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def ok(req_id, **data) -> dict:
    return {"id": req_id, "success": True, "data": data}


def err(req_id, message: str) -> dict:
    return {"id": req_id, "success": False, "error": message}


# ---------------------------------------------------------------------------
# Worker state — one cornea instance per process
# ---------------------------------------------------------------------------
class Worker:
    def __init__(self):
        self.cornea = None              # CorneaRax720 instance
        self.cached_temp: float = -999.0  # last successful get_lea_temperature

    def cmd_init(self, args: dict) -> dict:
        if self.cornea is not None:
            return {"already_initialized": True}

        # Import deferred — keeps `ping` / `init` self-tests fast and lets
        # the parent test stdin/stdout wiring without paying the ~1-3 s
        # cornea_rax720 import cost.
        from ar_display_lab_lib.control_boards.cornea_rax720 import CorneaRax720

        kwargs = {
            "cornea_index":      int(args.get("cornea_index", 0)),
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
        return {
            "init_ok": bool(getattr(self.cornea, "init_ok", True)),
            "cornea_serial": getattr(self.cornea, "cornea_serial", None),
            "duration_ms": dt_ms,
        }

    def _require(self):
        if self.cornea is None:
            raise RuntimeError("instance not initialized; call 'init' first")
        return self.cornea

    def cmd_powerOn(self, args: dict) -> dict:
        c = self._require()
        # rax_lib returns init_ok (bool). False means panel did not respond
        # (Pogo unseated / brown-out / etc.) but no exception is thrown —
        # mirror CC's PythonBridge::systemPowerOn handling so the parent
        # doesn't misinterpret a "False" return as "success".
        result = c.system_power_on()
        return {"init_ok": bool(result)}

    def cmd_powerOff(self, args: dict) -> dict:
        c = self._require()
        c.system_power_off()
        return {}

    def cmd_setBrightness(self, args: dict) -> dict:
        c = self._require()
        level = float(args["level"])
        c.set_brightness(level)
        return {"level": level}

    def cmd_getBrightness(self, args: dict) -> dict:
        c = self._require()
        return {"level": float(c.get_brightness())}

    def cmd_getPanelId(self, args: dict) -> dict:
        c = self._require()
        # rax_lib exposes panel UCID through the RJ1 chip info dict.
        # When the panel hasn't been programmed yet (cold pre-init state)
        # the field is absent — return empty string rather than raising.
        info = c.get_rj1_chip_info_decoded()
        return {"panel_id": str(info.get("unique_chip_id_str", ""))}

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
            img = Image.open(path).convert("RGB")
            frame = np.array(img)
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

        try:
            data = handler(args) or {}
            emit(ok(req_id, **data))
            if cmd == "shutdown":
                break
        except Exception as e:
            log.error("cmd %s raised: %s\n%s", cmd, e, traceback.format_exc())
            emit(err(req_id, f"{type(e).__name__}: {e}"))

    log.info("panel_worker exiting cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
