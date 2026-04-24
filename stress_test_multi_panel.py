#!/usr/bin/env python3
"""
Stress test for CorneaController per-device worker threads (v1.1.6+).

Spawns one worker thread per device and hammers setBrightness + sendImageByName
concurrently. Validates that:
  - Responses are still correctly tagged by serial (no cross-panel confusion)
  - No dispatch timeouts (>15s)
  - No hangs / crashes

Usage:
    python stress_test_multi_panel.py --host 192.168.0.213 --port 5566 \
        --serials LITE20240330,LITE20240345,LITE20240359 \
        --iterations 200 --image-name cross_5p.bmp

The script does not need the driver source; it only speaks the documented
TCP JSON protocol.
"""
from __future__ import annotations
import argparse
import json
import socket
import statistics
import threading
import time
from dataclasses import dataclass, field
from typing import List


@dataclass
class PanelStats:
    serial: str
    ok: int = 0
    fail: int = 0
    mismatch: int = 0
    timeout: int = 0
    latencies_ms: List[float] = field(default_factory=list)

    def record(self, resp_ok: bool, resp_serial: str | None, elapsed_ms: float) -> None:
        self.latencies_ms.append(elapsed_ms)
        if resp_serial is not None and resp_serial != self.serial:
            self.mismatch += 1
            self.fail += 1
            return
        if resp_ok:
            self.ok += 1
        else:
            self.fail += 1


def send_tcp(host: str, port: int, payload: dict, timeout: float = 20.0) -> tuple[bool, dict | None, float]:
    """Send one JSON command, return (success_flag, response_dict, elapsed_ms)."""
    t0 = time.perf_counter()
    try:
        with socket.create_connection((host, port), timeout=timeout) as s:
            s.settimeout(timeout)
            s.sendall(json.dumps(payload).encode("utf-8"))
            # The server closes the connection after responding in most flows;
            # read until EOF or newline.
            buf = b""
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
                if b"\n" in chunk or len(buf) > 128 * 1024:
                    break
        elapsed = (time.perf_counter() - t0) * 1000.0
        if not buf:
            return False, None, elapsed
        try:
            resp = json.loads(buf.decode("utf-8", errors="replace").strip())
        except json.JSONDecodeError:
            return False, None, elapsed
        return bool(resp.get("success", False)), resp, elapsed
    except (socket.timeout, OSError):
        return False, None, (time.perf_counter() - t0) * 1000.0


def panel_worker(host: str, port: int, serial: str, iterations: int,
                 image_name: str, stats: PanelStats, stop_event: threading.Event) -> None:
    """Worker that alternates setBrightness + sendImageByName for one panel."""
    level = 0.05
    for i in range(iterations):
        if stop_event.is_set():
            return

        # setBrightness
        ok, resp, ms = send_tcp(host, port, {
            "cmd": "setBrightness",
            "serial": serial,
            "level": level,
        })
        resp_serial = resp.get("serial") if isinstance(resp, dict) else None
        if resp is None:
            stats.timeout += 1
            stats.fail += 1
        stats.record(ok, resp_serial, ms)

        # sendImageByName
        ok, resp, ms = send_tcp(host, port, {
            "cmd": "sendImageByName",
            "serial": serial,
            "name": image_name,
        })
        resp_serial = resp.get("serial") if isinstance(resp, dict) else None
        if resp is None:
            stats.timeout += 1
            stats.fail += 1
        stats.record(ok, resp_serial, ms)

        # Alternate brightness to exercise BrightnessProtection
        level = 0.50 if level < 0.1 else 0.05


def summarize(stats: PanelStats) -> str:
    lats = stats.latencies_ms
    if not lats:
        return f"  {stats.serial}: no responses"
    p50 = statistics.median(lats)
    p95 = statistics.quantiles(lats, n=20)[18] if len(lats) > 20 else max(lats)
    return (f"  {stats.serial}: ok={stats.ok} fail={stats.fail} "
            f"timeout={stats.timeout} cross-serial={stats.mismatch} "
            f"| p50={p50:.0f}ms p95={p95:.0f}ms max={max(lats):.0f}ms")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5566)
    ap.add_argument("--serials", required=True,
                    help="Comma-separated panel serial numbers (e.g. LITE20240330,LITE20240345)")
    ap.add_argument("--iterations", type=int, default=200,
                    help="setBrightness+sendImage iterations per panel")
    ap.add_argument("--image-name", default="cross_5p.bmp")
    args = ap.parse_args()

    serials = [s.strip() for s in args.serials.split(",") if s.strip()]
    if not serials:
        print("No serials given")
        return 1

    print(f"Stress test: {len(serials)} panels × {args.iterations} iterations "
          f"→ {len(serials) * args.iterations * 2} TCP commands")
    print(f"Target: {args.host}:{args.port}")

    stats = [PanelStats(serial=s) for s in serials]
    stop = threading.Event()
    threads: list[threading.Thread] = []
    t0 = time.perf_counter()
    for st in stats:
        t = threading.Thread(
            target=panel_worker,
            args=(args.host, args.port, st.serial, args.iterations, args.image_name, st, stop),
            name=f"panel-{st.serial}",
            daemon=True,
        )
        t.start()
        threads.append(t)

    try:
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        print("\nCtrl-C — stopping…")
        stop.set()
        for t in threads:
            t.join(timeout=5)

    elapsed = time.perf_counter() - t0

    total_ok = sum(s.ok for s in stats)
    total_fail = sum(s.fail for s in stats)
    total_to = sum(s.timeout for s in stats)
    total_cs = sum(s.mismatch for s in stats)
    total_cmds = total_ok + total_fail

    print("\n=== Results ===")
    for s in stats:
        print(summarize(s))
    print(f"\nTotals: ok={total_ok} fail={total_fail} timeout={total_to} cross-serial={total_cs}")
    print(f"Wall time: {elapsed:.1f} s  |  Throughput: {total_cmds / elapsed:.1f} cmd/s")

    # Non-zero exit if anything unexpected happened
    return 0 if (total_fail == 0 and total_cs == 0) else 2


if __name__ == "__main__":
    raise SystemExit(main())
