"""Concurrent-load cascade reproduction.

Targets the 2026-05-25 LEA failure pattern:

  15:49:10  panel 393 powerOn starts
  15:49:11  panel 376 sendImage starts (concurrent)
  15:49:11  panel 393 init NACK @ 0x6a then 0x65 → init_ok=False
  15:49:11  panel 393 powerOn reply success=false

bench_n_panel_scan.py does not trigger this because its phases are
sequential — all-powerOn then all-sendImage then all-getPanelId. The
real LEA flow runs them interleaved: while one panel is mid-init,
others are doing sendImage / setBrightness / getTemperature.

This bench runs two passes per (K, trial), where K = the number of
"stressor" panels that hammer sendImage in tight loop:

  STRESSED:  cold-powerOn 1..K  →  threads 1..K spam sendImage Black
                              ↓
                              thread T fires powerOn(K+1)
                              ↓
             record powerOn(K+1) outcome + 1..K sendImage rates

  CONTROL:   cold-powerOn 1..K  →  1..K idle
                              ↓
                              thread T fires powerOn(K+1)
                              ↓
             record outcome (baseline for comparison)

If STRESSED fails more than CONTROL, the cascade is concurrency-induced
(libusb / GIL / USB-host-controller contention). If both fail equally,
something else is at play.

Usage:
  python bench_concurrent_cascade.py \\
      --serials LITE20240330,LITE20240337,LITE20240338,LITE20240343,LITE20240345,LITE20240359 \\
      --variants F33L,F33L,F33L,F33L,F33L,F33L \\
      --k-stressors 2,3,4 \\
      --trials 3 \\
      --stress-duration 10 \\
      --out cascade_$(date +%Y%m%d_%H%M%S).log
"""
import argparse
import datetime
import json
import socket
import sys
import threading
import time


BLACK_IMAGE = "720x720_Black.png"


def send_cmd(host, port, cmd_json, timeout_s):
    """One-shot TCP send; returns (elapsed_ms, ok, response)."""
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


class TeeLog:
    def __init__(self, path):
        self.f = open(path, "w", encoding="utf-8")

    def __call__(self, msg=""):
        print(msg)
        self.f.write(msg + "\n")
        self.f.flush()

    def close(self):
        self.f.close()


def cold_power_on(host, port, serial, variant, log):
    """Synchronous cold powerOn; useful as a baseline measurement too."""
    elapsed, ok, resp = send_cmd(host, port,
        {"cmd": "powerOn", "serial": serial, "variant": variant}, 60.0)
    tag = "OK" if ok else "FAIL"
    err = "" if ok else f" err={resp}"
    log(f"    {serial:>16}  powerOn {tag} ({elapsed:.0f} ms){err}")
    return ok


def cold_power_off(host, port, serial):
    """Best-effort cleanup between trials. Returns nothing — we don't
    care if it fails; the next cold-powerOn will rebuild anyway."""
    send_cmd(host, port, {"cmd": "powerOff", "serial": serial}, 30.0)


def stress_loop(host, port, serial, stop_evt, stats):
    """Tight-loop sendImage Black. Records each call's outcome into
    `stats` (a dict updated in place — thread-safe enough because each
    thread writes to its own keys)."""
    sent = 0
    fail = 0
    fail_examples = []
    while not stop_evt.is_set():
        elapsed, ok, resp = send_cmd(host, port,
            {"cmd": "sendImageByName", "name": BLACK_IMAGE, "serial": serial}, 15.0)
        sent += 1
        if not ok:
            fail += 1
            if len(fail_examples) < 3:
                fail_examples.append((round(elapsed), str(resp)[:200]))
        # No sleep: matches the worst-case "tight loop" mentioned in the
        # 2026-05-25 evidence where sendImage requests were back-to-back.
    stats["sent"] = sent
    stats["fail"] = fail
    stats["fail_examples"] = fail_examples


def run_trigger(host, port, serial, variant, results):
    """Fire one powerOn against the (K+1)th panel — this is the cascade
    trigger. Lives in its own thread so the main thread can correlate
    its start time with the stressor activity."""
    t0 = time.monotonic()
    elapsed, ok, resp = send_cmd(host, port,
        {"cmd": "powerOn", "serial": serial, "variant": variant}, 60.0)
    results["start_mono"] = t0
    results["elapsed_ms"] = elapsed
    results["ok"] = ok
    results["resp"] = resp


def run_phase(host, port, stressors, trigger_serial, trigger_variant,
              stress_duration, log, stressed):
    """One pass: with stressed=True, the K stressor panels spam sendImage.
    With stressed=False, they sit idle. Trigger fires after a 1-second
    warm-up so the stressor threads are definitely active when it lands.
    """
    mode = "STRESSED" if stressed else "CONTROL "
    log(f"  [{mode}] K={len(stressors)} stressors={stressors} "
        f"trigger={trigger_serial} variant={trigger_variant}")

    stop_evt = threading.Event()
    stress_stats = {}
    threads = []
    if stressed:
        for sn in stressors:
            stats = {"sent": 0, "fail": 0, "fail_examples": []}
            stress_stats[sn] = stats
            t = threading.Thread(target=stress_loop,
                                 args=(host, port, sn, stop_evt, stats),
                                 daemon=True, name=f"stress-{sn}")
            t.start()
            threads.append(t)
        time.sleep(1.0)  # let stressors ramp up

    trigger_result = {}
    trig = threading.Thread(target=run_trigger,
                            args=(host, port, trigger_serial, trigger_variant,
                                  trigger_result),
                            daemon=True, name=f"trigger-{trigger_serial}")
    trig.start()
    trig.join(stress_duration + 60.0)

    # Let stressors run a bit longer to see post-trigger fall-out.
    time.sleep(0.5)
    stop_evt.set()
    for t in threads:
        t.join(5.0)

    elapsed = trigger_result.get("elapsed_ms", -1)
    ok = trigger_result.get("ok", False)
    resp = trigger_result.get("resp")
    tag = "OK" if ok else "FAIL"
    err = "" if ok else f" err={resp}"
    log(f"    trigger powerOn({trigger_serial})  {tag} ({elapsed:.0f} ms){err}")
    if stressed:
        for sn, st in stress_stats.items():
            rate = (st["sent"] - st["fail"]) / st["sent"] * 100.0 if st["sent"] else 0.0
            log(f"    stressor {sn}  sent={st['sent']:>4}  fail={st['fail']:>4}  "
                f"ok_rate={rate:5.1f}%")
            for ex in st["fail_examples"]:
                log(f"      fail example: t={ex[0]}ms  resp={ex[1]}")
    log()
    return {
        "mode": "STRESSED" if stressed else "CONTROL",
        "trigger_ok": ok,
        "trigger_elapsed_ms": elapsed,
        "stress_stats": stress_stats,
    }


def cleanup_all(host, port, serials, log):
    log("  [cleanup] powerOff all ...")
    for sn in serials:
        cold_power_off(host, port, sn)
    time.sleep(1.0)


def run_one_k(host, port, all_serials, all_variants, k, trials,
              stress_duration, log):
    """Run `trials` STRESSED+CONTROL pairs for a given K (number of
    stressor panels). The trigger panel is always all_serials[k] (the
    K-th, 0-indexed) — same panel across trials for repeatability."""
    if k + 1 > len(all_serials):
        log(f"  [skip K={k}] not enough panels supplied")
        return []

    stressors = all_serials[:k]
    trigger_serial = all_serials[k]
    trigger_variant = all_variants[k]
    log(f"== K={k} stressors={stressors}  trigger={trigger_serial} ({trigger_variant}) ==")

    results = []
    for trial in range(1, trials + 1):
        log(f"  -- trial {trial}/{trials} --")
        # Fresh state each trial: powerOff all then powerOn the K stressors.
        cleanup_all(host, port, all_serials, log)
        log(f"  [prep] cold-powerOn stressors {stressors}")
        for sn, var in zip(stressors, all_variants[:k]):
            cold_power_on(host, port, sn, var, log)
        # Make sure trigger panel is OFF before the trigger phase fires.
        cold_power_off(host, port, trigger_serial)
        time.sleep(0.5)

        ctrl = run_phase(host, port, stressors, trigger_serial, trigger_variant,
                         stress_duration, log, stressed=False)
        # Re-power-off the trigger panel before STRESSED run (CONTROL left it on).
        cold_power_off(host, port, trigger_serial)
        time.sleep(0.5)
        stressed = run_phase(host, port, stressors, trigger_serial, trigger_variant,
                              stress_duration, log, stressed=True)
        results.append({"trial": trial, "control": ctrl, "stressed": stressed})

    cleanup_all(host, port, all_serials, log)
    return results


def main():
    ap = argparse.ArgumentParser(formatter_class=argparse.RawDescriptionHelpFormatter,
                                  description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5566)
    ap.add_argument("--serials", required=True,
                    help="comma-separated panel serials in stable order")
    ap.add_argument("--variants", required=True,
                    help="comma-separated variants 1:1 with --serials")
    ap.add_argument("--k-stressors", default="2,3,4",
                    help="comma-separated K values to sweep (stressor count). "
                         "Default 2,3,4 covers the 5-panel boundary user reported.")
    ap.add_argument("--trials", type=int, default=3,
                    help="repeats per K (random transient may need >1)")
    ap.add_argument("--stress-duration", type=float, default=10.0,
                    help="seconds the stressor sendImage loop runs alongside trigger")
    ap.add_argument("--out", default="",
                    help="optional log file path; otherwise stdout only")
    args = ap.parse_args()

    serials = [s.strip() for s in args.serials.split(",") if s.strip()]
    variants = [v.strip() for v in args.variants.split(",") if v.strip()]
    if len(serials) != len(variants):
        print(f"ERROR: --serials has {len(serials)} entries but --variants has {len(variants)}",
              file=sys.stderr)
        sys.exit(2)
    ks = [int(x) for x in args.k_stressors.split(",") if x.strip()]

    if args.out:
        log = TeeLog(args.out)
    else:
        def log(m=""): print(m)

    try:
        log("# Concurrent cascade reproduction bench")
        log(f"# host={args.host}:{args.port}")
        log(f"# serials={serials}")
        log(f"# variants={variants}")
        log(f"# k_stressors={ks}  trials={args.trials}  stress_duration={args.stress_duration}s")
        log(f"# start={datetime.datetime.now().isoformat()}")
        log()

        all_results = {}
        for k in ks:
            results = run_one_k(args.host, args.port, serials, variants,
                                k, args.trials, args.stress_duration, log)
            all_results[k] = results

        # Summary: control vs stressed trigger success rate per K.
        log("=" * 76)
        log("SUMMARY  (trigger powerOn pass-rate per K, CONTROL vs STRESSED)")
        log(f"{'K':>3}  {'CONTROL pass':>13}  {'STRESSED pass':>14}  {'verdict':>20}")
        for k, results in all_results.items():
            if not results: continue
            ctrl_ok = sum(1 for r in results if r["control"]["trigger_ok"])
            stress_ok = sum(1 for r in results if r["stressed"]["trigger_ok"])
            n = len(results)
            if stress_ok < ctrl_ok:
                verdict = "concurrency-induced"
            elif ctrl_ok == n and stress_ok == n:
                verdict = "no cascade observed"
            else:
                verdict = "both modes failing"
            log(f"{k:>3}  {ctrl_ok:>5}/{n:<5}     {stress_ok:>5}/{n:<5}        {verdict}")
    finally:
        if args.out:
            log.close()


if __name__ == "__main__":
    main()
