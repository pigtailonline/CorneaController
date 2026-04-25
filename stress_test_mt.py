"""
Cornea Controller Multi-Thread Stress Test
Simulates N stations sending commands concurrently.

Usage:
    python stress_test_mt.py
    python stress_test_mt.py --host 192.168.0.100 --port 5566 --threads 6 --seconds 120
"""

import socket
import json
import time
import random
import threading
import argparse
from collections import defaultdict

class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.sent = 0
        self.success = 0
        self.fail = 0
        self.timeout = 0
        self.error = 0
        self.by_cmd = defaultdict(lambda: {"ok": 0, "fail": 0, "timeout": 0})

    def record(self, cmd, result):
        with self.lock:
            self.sent += 1
            if result == "ok":
                self.success += 1
                self.by_cmd[cmd]["ok"] += 1
            elif result == "timeout":
                self.timeout += 1
                self.by_cmd[cmd]["timeout"] += 1
            else:
                self.fail += 1
                self.by_cmd[cmd]["fail"] += 1


def send_command(host, port, cmd_json, timeout=10):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect((host, port))
        s.sendall((json.dumps(cmd_json) + "\n").encode())
        data = b""
        while b"\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        s.close()
        resp = json.loads(data.decode().strip())
        return resp
    except socket.timeout:
        return {"_error": "timeout"}
    except Exception as e:
        return {"_error": str(e)}


def worker(thread_id, host, port, devices, seconds, stats, stop_event):
    """Simulate one station's operations"""
    start = time.time()
    round_num = 0

    while not stop_event.is_set() and (time.time() - start) < seconds:
        round_num += 1

        # Pick a random device for this round
        serial = random.choice(devices) if devices else f"FAKE_{thread_id:03d}"

        # 1. setBrightness
        level = round(random.uniform(0.01, 0.5), 2)
        resp = send_command(host, port, {
            "cmd": "setBrightness", "serial": serial, "level": level
        }, timeout=15)
        if "_error" in resp:
            stats.record("setBrightness", "timeout" if resp["_error"] == "timeout" else "error")
        elif resp.get("success"):
            stats.record("setBrightness", "ok")
        else:
            stats.record("setBrightness", "fail")

        # 2. sendImageByName
        resp = send_command(host, port, {
            "cmd": "sendImageByName", "serial": serial, "name": "720x720_Black.png"
        }, timeout=15)
        if "_error" in resp:
            stats.record("sendImageByName", "timeout" if resp["_error"] == "timeout" else "error")
        elif resp.get("success"):
            stats.record("sendImageByName", "ok")
        else:
            stats.record("sendImageByName", "fail")

        # 3. getTemperature
        resp = send_command(host, port, {
            "cmd": "getTemperature", "serial": serial
        }, timeout=5)
        if "_error" in resp:
            stats.record("getTemperature", "timeout" if resp["_error"] == "timeout" else "error")
        elif resp.get("success"):
            stats.record("getTemperature", "ok")
        else:
            stats.record("getTemperature", "fail")

        # 4. Random: getStatus or refreshDevices (less frequent)
        if random.random() < 0.1:
            cmd = random.choice(["getStatus", "refreshDevices"])
            resp = send_command(host, port, {"cmd": cmd}, timeout=10)
            if "_error" in resp:
                stats.record(cmd, "timeout" if resp["_error"] == "timeout" else "error")
            elif resp.get("success"):
                stats.record(cmd, "ok")
            else:
                stats.record(cmd, "fail")

        # Brief random pause (simulate real operation gaps)
        time.sleep(random.uniform(0.1, 0.5))


def main():
    parser = argparse.ArgumentParser(description="Cornea Controller Multi-Thread Stress Test")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5566)
    parser.add_argument("--threads", type=int, default=6, help="Number of concurrent stations")
    parser.add_argument("--seconds", type=int, default=60)
    args = parser.parse_args()

    print(f"=== Cornea Controller Multi-Thread Stress Test ===")
    print(f"Target: {args.host}:{args.port}")
    print(f"Threads: {args.threads}")
    print(f"Duration: {args.seconds}s")
    print()

    # Get device list
    print("[Phase 1] Getting device list...")
    resp = send_command(args.host, args.port, {"cmd": "getStatus"}, timeout=5)
    devices = []
    if not resp.get("_error") and resp.get("data", {}).get("devices"):
        devices = [d["serial"] for d in resp["data"]["devices"]]
    if not devices:
        print("No real devices, using fake serials")
        devices = [f"FAKE_{i:03d}" for i in range(args.threads)]
    print(f"Devices: {', '.join(devices)}")
    print()

    # Run stress test
    print(f"[Phase 2] Running {args.threads} threads for {args.seconds}s...")
    print()

    stats = Stats()
    stop_event = threading.Event()
    threads = []

    start_time = time.time()

    for i in range(args.threads):
        t = threading.Thread(target=worker, args=(i, args.host, args.port, devices, args.seconds, stats, stop_event))
        t.daemon = True
        threads.append(t)
        t.start()

    # Progress monitor
    try:
        while any(t.is_alive() for t in threads):
            time.sleep(2)
            elapsed = time.time() - start_time
            rate = (stats.success / stats.sent * 100) if stats.sent > 0 else 0
            print(f"  [{elapsed:.0f}s] Sent:{stats.sent} OK:{stats.success} "
                  f"Fail:{stats.fail} Timeout:{stats.timeout} ({rate:.1f}%)")
            if elapsed > args.seconds + 30:
                print("Force stopping...")
                stop_event.set()
                break
    except KeyboardInterrupt:
        print("\nStopping...")
        stop_event.set()

    for t in threads:
        t.join(timeout=5)

    elapsed = time.time() - start_time

    # Summary
    print()
    print("=== Results ===")
    print(f"  Duration:  {elapsed:.1f}s")
    print(f"  Threads:   {args.threads}")
    print(f"  Total:     {stats.sent} commands")
    print(f"  Success:   {stats.success}")
    print(f"  Failed:    {stats.fail}")
    print(f"  Timeout:   {stats.timeout}")
    rate = (stats.success / stats.sent * 100) if stats.sent > 0 else 0
    print(f"  Rate:      {rate:.1f}%")
    print()

    # Per-command breakdown
    print("=== Per-Command Breakdown ===")
    print(f"  {'Command':<25} {'OK':>6} {'Fail':>6} {'Timeout':>8}")
    print(f"  {'-'*25} {'-'*6} {'-'*6} {'-'*8}")
    for cmd, counts in sorted(stats.by_cmd.items()):
        print(f"  {cmd:<25} {counts['ok']:>6} {counts['fail']:>6} {counts['timeout']:>8}")
    print()

    if stats.timeout > 0:
        print(f"WARNING: {stats.timeout} timeouts - server may be unstable or overloaded")
    if stats.timeout == 0 and stats.sent > 0:
        print("Server handled all commands without timeout (stable)")


if __name__ == "__main__":
    main()
