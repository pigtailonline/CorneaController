"""N-panel concurrent capacity scan.

Reproduces the 2026-05-26 observation that LEA driver software can light up
at most 4 panels concurrently — connecting a 5th causes getPanelId to fail
AND already-lit panels lose sendImage.

For N in 1..max_panels:
  1. Cold powerOn N panels concurrently (matches production trigger)
  2. getPanelId on each — record per-panel success
  3. sendImage Blue on each — record per-panel success + latency
  4. Wait briefly (cache stabilization), then second getPanelId pass
     to detect "already-lit panels lose sendImage" cascade
  5. powerOff all N before next iteration (clean state)

The cliff (transition from all-pass to partial-fail) reveals the shared
resource that saturates. Output is a per-N table; save the CC server log
in parallel and you have a complete diagnostic packet.

Usage:
  python bench_n_panel_scan.py \\
      --serials LITE20240376,LITE20240380,LITE20240393,LITE20240398,LITE20240431,LITE20240437 \\
      --variants F33RV2,F33LV2,F33RV2,F33RV2,F33LV2,F33LV2 \\
      --max-n 6 \\
      --out scan_$(date +%Y%m%d_%H%M%S).log

The --variants list must align 1:1 with --serials.
"""
import argparse
import datetime
import json
import socket
import sys
import threading
import time


def send_cmd(host, port, cmd_json, timeout_s):
    """Send one cmd, return (elapsed_ms, ok, response_json_or_None)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout_s)
    t0 = time.monotonic()
    try:
        s.connect((host, port))
        s.sendall((json.dumps(cmd_json, separators=(",", ":")) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(8192)
            if not chunk:
                break
            buf += chunk
        elapsed = (time.monotonic() - t0) * 1000
        try:
            resp = json.loads(buf.decode().strip())
        except (json.JSONDecodeError, UnicodeDecodeError):
            resp = None
        ok = bool(resp and resp.get("success"))
        return elapsed, ok, resp
    except (socket.timeout, OSError) as e:
        return (time.monotonic() - t0) * 1000, False, {"error": str(e)}
    finally:
        try:
            s.close()
        except OSError:
            pass


def parallel(fn, args_list, timeout_s):
    """Run fn(*args) concurrently in N threads, collect results in order."""
    results = [None] * len(args_list)

    def runner(i, a):
        results[i] = fn(*a)

    threads = []
    for i, a in enumerate(args_list):
        t = threading.Thread(target=runner, args=(i, a), daemon=True)
        t.start()
        threads.append(t)
    for t in threads:
        t.join(timeout_s + 5)
    return results


class TeeLog:
    """stdout + file simultaneously."""
    def __init__(self, path):
        self.f = open(path, "w", encoding="utf-8")

    def __call__(self, msg=""):
        print(msg)
        self.f.write(msg + "\n")
        self.f.flush()

    def close(self):
        self.f.close()


def scan(host, port, serials, variants, max_n, log):
    """For each N in 1..max_n, exercise N panels and report per-panel success."""
    log(f"# N-panel concurrent capacity scan")
    log(f"# host={host}:{port} serials={serials} variants={variants}")
    log(f"# start={datetime.datetime.now().isoformat()}")
    log()

    summary = []  # list of (n, powerOn_ok_count, getPanelId_ok_count, sendImage_ok_count, getPanelId2_ok_count)

    for n in range(1, max_n + 1):
        sel_serials = serials[:n]
        sel_variants = variants[:n]
        log(f"== N={n}  serials={sel_serials} ==")

        # Step 1: concurrent cold powerOn
        log(f"  [1/4] powerOn × {n} concurrent ...")
        po_args = [(host, port, {"cmd": "powerOn", "serial": sn, "variant": v}, 60.0)
                   for sn, v in zip(sel_serials, sel_variants)]
        po_results = parallel(send_cmd, po_args, 60.0)
        po_ok = 0
        for sn, (elapsed, ok, resp) in zip(sel_serials, po_results):
            tag = "OK" if ok else "FAIL"
            err = "" if ok else f" err={resp}"
            log(f"    {sn:>16}  powerOn {tag} ({elapsed:.0f} ms){err}")
            if ok:
                po_ok += 1

        # Even if some powerOn failed, continue probing all to see cascade.

        # Step 2: getPanelId per panel (sequential — TCP cmd is short)
        log(f"  [2/4] getPanelId × {n} ...")
        gp_ok = 0
        gp_panels = {}
        for sn in sel_serials:
            elapsed, ok, resp = send_cmd(host, port, {"cmd": "getPanelId", "serial": sn}, 15.0)
            pid = (resp or {}).get("data", {}).get("panelId", "") if ok else ""
            tag = "OK" if ok else "FAIL"
            err = "" if ok else f" err={resp}"
            log(f"    {sn:>16}  getPanelId {tag} ({elapsed:.0f} ms)  id='{pid}'{err}")
            if ok:
                gp_ok += 1
                gp_panels[sn] = pid

        # Step 3: sendImage Black × N (sequential, with small gap).
        # Black has APL=0, so APL_EXCEEDED protection never trips regardless
        # of brightness — isolates pure powerOn/sendImage capacity behavior
        # from any brightness-default issue.
        log(f"  [3/4] sendImage Black × {n} ...")
        si_ok = 0
        for sn in sel_serials:
            elapsed, ok, resp = send_cmd(host, port,
                {"cmd": "sendImageByName",
                 "name": "720x720_Black.png",
                 "serial": sn}, 30.0)
            tag = "OK" if ok else "FAIL"
            err = "" if ok else f" err={resp}"
            log(f"    {sn:>16}  sendImage {tag} ({elapsed:.0f} ms){err}")
            if ok:
                si_ok += 1
            time.sleep(0.05)

        # Step 4: settle 2s, then 2nd getPanelId pass to detect cascade
        log(f"  [4/4] settle 2s, then getPanelId × {n} again (cascade check) ...")
        time.sleep(2.0)
        gp2_ok = 0
        for sn in sel_serials:
            elapsed, ok, resp = send_cmd(host, port, {"cmd": "getPanelId", "serial": sn}, 15.0)
            pid = (resp or {}).get("data", {}).get("panelId", "") if ok else ""
            tag = "OK" if ok else "FAIL"
            err = "" if ok else f" err={resp}"
            log(f"    {sn:>16}  getPanelId-2 {tag} ({elapsed:.0f} ms)  id='{pid}'{err}")
            if ok:
                gp2_ok += 1

        summary.append((n, po_ok, gp_ok, si_ok, gp2_ok))

        # Cleanup: powerOff all so next N starts cold
        log(f"  [cleanup] powerOff × {n} ...")
        for sn in sel_serials:
            _ = send_cmd(host, port, {"cmd": "powerOff", "serial": sn}, 30.0)
        time.sleep(1.0)
        log()

    # Summary
    log("=" * 70)
    log("SUMMARY  (ok-count / N per stage)")
    log(f"{'N':>3}  {'powerOn':>10}  {'getPanelId':>12}  {'sendImage':>11}  {'getPanelId-2':>14}")
    cliff_n = None
    for n, po, gp, si, gp2 in summary:
        marker = ""
        if cliff_n is None and (po < n or gp < n or si < n or gp2 < n):
            cliff_n = n
            marker = "  ← FIRST FAIL"
        log(f"{n:>3}  {po:>4}/{n:<4}  {gp:>6}/{n:<4}  {si:>5}/{n:<4}  {gp2:>8}/{n:<4}{marker}")
    if cliff_n:
        log(f"\n★ CAPACITY CLIFF AT N={cliff_n}")
    else:
        log(f"\nNo cliff detected up to N={max_n} — all panels healthy")


def main():
    ap = argparse.ArgumentParser(formatter_class=argparse.RawDescriptionHelpFormatter,
                                  description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5566)
    ap.add_argument("--serials", required=True,
                    help="comma-separated panel serials (order matters)")
    ap.add_argument("--variants", required=True,
                    help="comma-separated variants 1:1 with --serials "
                         "(e.g. F33RV2,F33LV2,...)")
    ap.add_argument("--max-n", type=int, default=6,
                    help="max number of panels to test concurrently (default 6)")
    ap.add_argument("--out", default="",
                    help="optional log file path; otherwise prints to stdout only")
    args = ap.parse_args()

    serials = [s.strip() for s in args.serials.split(",") if s.strip()]
    variants = [v.strip() for v in args.variants.split(",") if v.strip()]
    if len(serials) != len(variants):
        print(f"ERROR: --serials has {len(serials)} entries but --variants has {len(variants)}",
              file=sys.stderr)
        sys.exit(2)
    if args.max_n > len(serials):
        print(f"ERROR: --max-n={args.max_n} but only {len(serials)} serials supplied",
              file=sys.stderr)
        sys.exit(2)

    if args.out:
        log = TeeLog(args.out)
    else:
        def log(m=""): print(m)

    try:
        scan(args.host, args.port, serials, variants, args.max_n, log)
    finally:
        if args.out:
            log.close()


if __name__ == "__main__":
    main()
