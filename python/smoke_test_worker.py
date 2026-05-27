"""Phase 1 smoke test for panel_worker.py.

Spawns ONE panel_worker.py subprocess in the station_venv, drives it
through the full init → powerOn → getPanelId → getTemperature → sendImage
→ powerOff → shutdown cycle, and prints a PASS/FAIL summary.

If this runs end-to-end on a real LEA hardware setup, the GIL-isolation
hypothesis is validated: cornea_rax720 works inside a fresh python.exe,
no embedded interpreter needed. Phase 2 then wraps this protocol from C++
via QProcess.

Usage:
    cd "D:\\projects\\src\\google driver\\CorneaController\\python"
    "C:\\google_env\\station_venv\\Scripts\\python.exe" smoke_test_worker.py \\
        --cornea-index 0 \\
        --variant F33L \\
        --cal-path D:/cornea/hdf5_files

If you don't have a real panel attached, pass --skip-power to exercise
just init + ping + shutdown (validates the protocol plumbing without
needing hardware).
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path


WORKER_SCRIPT = Path(__file__).parent / "panel_worker.py"


class WorkerClient:
    """Minimal JSON-RPC client over a child process's stdin/stdout. Mirrors
    what the C++ QProcess wrapper in Phase 2 will do."""

    def __init__(self, python_exe: str):
        env_ascii = {"PYTHONIOENCODING": "utf-8"}
        self.proc = subprocess.Popen(
            [python_exe, str(WORKER_SCRIPT)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,   # captured separately so prints don't pollute
            text=True,
            encoding="utf-8",
            bufsize=1,                 # line-buffered
        )
        self._next_id = 1

    def call(self, cmd: str, **args) -> dict:
        req_id = self._next_id
        self._next_id += 1
        payload = {"id": req_id, "cmd": cmd, "args": args}
        line = json.dumps(payload, separators=(",", ":")) + "\n"
        self.proc.stdin.write(line)
        self.proc.stdin.flush()

        # Skip any stderr-bound print that bleeds into stdout (it shouldn't
        # if the worker is well-behaved). We wait for the line matching
        # this req_id.
        deadline = time.monotonic() + 120.0
        while time.monotonic() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("worker stdout closed unexpectedly")
            try:
                resp = json.loads(line)
            except json.JSONDecodeError:
                # Not JSON — shouldn't happen but ignore so a stray print
                # doesn't kill the test
                print(f"  (non-JSON stdout: {line.rstrip()})", file=sys.stderr)
                continue
            if resp.get("id") == req_id:
                return resp
        raise TimeoutError(f"no response for cmd={cmd} id={req_id}")

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def stderr_dump(self) -> str:
        # Non-blocking read of whatever the worker has logged so far.
        try:
            self.proc.stderr.flush()
        except Exception:
            pass
        # stderr is captured via PIPE — drain whatever's there without blocking.
        # On Windows, peek isn't straightforward; just read everything after close.
        return self.proc.stderr.read() if self.proc.stderr else ""


def fmt(resp: dict) -> str:
    if resp.get("success"):
        return f"OK   {resp.get('data', {})}"
    return f"FAIL {resp.get('error', '?')}"


def main() -> int:
    ap = argparse.ArgumentParser()
    # Default to whichever python is running this script — guarantees the
    # worker subprocess inherits the same venv (and therefore the same
    # ar_display_lab_lib / pyftdi etc.). Operators on stations where the
    # venv lives outside C:\google_env (e.g. D:\Seeyond Release ...) don't
    # have to remember the explicit path.
    ap.add_argument("--python-exe",
                    default=sys.executable,
                    help="path to station_venv python.exe (defaults to current interpreter)")
    ap.add_argument("--cornea-index", type=int, default=0)
    ap.add_argument("--variant", default="F33L")
    ap.add_argument("--cal-path", default="D:/cornea/hdf5_files")
    ap.add_argument("--image", default="",
                    help="image file to sendImage (PNG / NPY); skip if empty")
    ap.add_argument("--skip-power", action="store_true",
                    help="exercise protocol only — no powerOn/powerOff/etc.")
    args = ap.parse_args()

    print(f"== panel_worker smoke test ==")
    print(f"  python: {args.python_exe}")
    print(f"  cornea: index={args.cornea_index}  variant={args.variant}")
    print(f"  cal:    {args.cal_path}")
    print(f"  image:  {args.image or '(none)'}")
    print(f"  skip-power: {args.skip_power}")
    print()

    client = WorkerClient(args.python_exe)
    failures = []
    try:
        print(f"[1] ping            → {fmt(client.call('ping'))}")

        print(f"[2] init            ...", flush=True)
        resp = client.call(
            "init",
            cornea_index=args.cornea_index,
            hardware_variant=args.variant,
            cal_path=args.cal_path,
            init_cornea=not args.skip_power,
            init_rj1=not args.skip_power,
            allow_default_hdf5=False,
            spi_clk_freq=15e6,
        )
        print(f"    {fmt(resp)}")
        if not resp.get("success"):
            failures.append(f"init: {resp.get('error')}")

        if not args.skip_power:
            print(f"[3] powerOn         ...", flush=True)
            resp = client.call("powerOn")
            print(f"    {fmt(resp)}")
            if not resp.get("success") or not resp["data"].get("init_ok"):
                failures.append(f"powerOn: {resp}")

            print(f"[4] getPanelId      → {fmt(client.call('getPanelId'))}")
            print(f"[5] getBrightness   → {fmt(client.call('getBrightness'))}")
            print(f"[6] setBrightness 0.03 → {fmt(client.call('setBrightness', level=0.03))}")
            print(f"[7] getTemperature  → {fmt(client.call('getTemperature'))}")

            if args.image:
                print(f"[8] sendImage {args.image} → "
                      f"{fmt(client.call('sendImage', path=args.image))}")

            print(f"[9] powerOff        → {fmt(client.call('powerOff'))}")

        print(f"[10] shutdown       → {fmt(client.call('shutdown'))}")
    except Exception as e:
        failures.append(f"protocol error: {e}")
        print(f"    EXCEPTION: {e}", file=sys.stderr)
    finally:
        client.close()
        stderr = client.stderr_dump()
        if stderr.strip():
            print("\n--- worker stderr (log) ---")
            print(stderr)

    print()
    if failures:
        print(f"FAIL  {len(failures)} step(s) failed:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("PASS  all steps OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
