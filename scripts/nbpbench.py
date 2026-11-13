#!/usr/bin/env python3
"""WiFi buffer benchmark against the nimble-ble-proxy bench build.

Runs on havan (LAN side). Phases run one after another; during every phase a
thread polls /stats.json at 1 Hz and `ping -i 0.2` runs. Output: JSON lines in
OUTDIR/{stats,phases}.jsonl.

Needs a CONFIG_NBP_BENCH image (components/bench), curl, ping, and a POST
body at /tmp/nbp-bench/blob.bin (head -c 4000000 /dev/urandom > ...).
Results and method: docs/wifi-buffer-throughput.md.

usage: nbpbench.py OUTDIR [--host 192.168.1.231] [--runs 5] [--bytes 4000000]
                   [--no-bench]   (only idle + ping, for a non-bench image)
                   [--floods N]   (only idle + N 100 B UDP floods)
"""
import argparse
import json
import os
import re
import signal
import socket
import subprocess
import threading
import time
import urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("outdir")
ap.add_argument("--host", default="192.168.1.231")
ap.add_argument("--runs", type=int, default=5)
ap.add_argument("--bytes", type=int, default=4_000_000)
ap.add_argument("--idle", type=int, default=60)
ap.add_argument("--no-bench", action="store_true")
ap.add_argument("--floods", type=int, default=0, help="only: idle, then N small-packet floods")
ap.add_argument("--udp-floor", type=int, default=6000,
                help="stop the UDP ramp once the heap low-water drops below this")
args = ap.parse_args()
os.makedirs(args.outdir, exist_ok=True)
H = args.host
STATS = f"http://{H}/stats.json"
B = f"http://{H}:8081"

phase = "init"
stop = False
f_stats = open(os.path.join(args.outdir, "stats.jsonl"), "a", buffering=1)
f_phase = open(os.path.join(args.outdir, "phases.jsonl"), "a", buffering=1)


def get_json(url, timeout=3.0):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read())


def poller():
    while not stop:
        t0 = time.time()
        rec = {"t": round(t0, 3), "phase": phase}
        try:
            rec["s"] = get_json(STATS)
        except Exception as e:  # noqa: BLE001
            rec["err"] = repr(e)
        rec["lat_ms"] = round((time.time() - t0) * 1000, 1)
        f_stats.write(json.dumps(rec) + "\n")
        time.sleep(max(0.0, 1.0 - (time.time() - t0)))


def ping_start():
    return subprocess.Popen(["ping", "-n", "-i", "0.2", H], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)


def ping_stop(p):
    p.send_signal(signal.SIGINT)
    out, _ = p.communicate(timeout=10)
    res = {}
    m = re.search(r"(\d+) packets transmitted, (\d+) received", out)
    if m:
        res["tx"], res["rx"] = int(m.group(1)), int(m.group(2))
    m = re.search(r"= ([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+) ms", out)
    if m:
        res["min"], res["avg"], res["max"], res["mdev"] = map(float, m.groups())
    rtts = [float(x) for x in re.findall(r"time=([\d.]+) ms", out)]
    if rtts:
        rtts.sort()
        res["p50"] = rtts[len(rtts) // 2]
        res["p95"] = rtts[min(len(rtts) - 1, int(len(rtts) * 0.95))]
    return res


def benchmin(reset=False):
    return get_json(B + "/benchmin" + ("?reset=1" if reset else ""), timeout=5)


def record(name, **kw):
    kw.update({"phase": name, "t": round(time.time(), 3)})
    f_phase.write(json.dumps(kw) + "\n")
    print(json.dumps(kw), flush=True)


def run_phase(name, fn):
    global phase
    phase = name
    p = ping_start()
    if not args.no_bench:
        benchmin(reset=True)
    t0 = time.time()
    res = fn()
    dur = time.time() - t0
    time.sleep(1.0)  # let the heap settle before reading the low-water mark
    bm = None if args.no_bench else benchmin()
    pg = ping_stop(p)
    record(name, dur=round(dur, 3), res=res, bench=bm, ping=pg)
    phase = "gap"
    time.sleep(3)
    return res, bm


def curl(cmd):
    out = subprocess.run(["curl", "-sS", "--max-time", "60", "-o", "/dev/null", "-w",
                          "%{http_code} %{size_download} %{size_upload} %{time_total} "
                          "%{time_starttransfer}"] + cmd,
                         capture_output=True, text=True)
    parts = out.stdout.split()
    if out.returncode != 0 or len(parts) < 5:
        return {"err": (out.stderr or out.stdout).strip(), "rc": out.returncode}
    code, dl, ul, tt, ts = parts
    nbytes = int(ul) if int(ul) else int(dl)
    return {"code": int(code), "bytes": nbytes, "s": float(tt), "ttfb": float(ts),
            "MBps": round(nbytes / float(tt) / 1e6, 3)}


def udp_flood(rate, size, dur):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payload = b"u" * size
    n = 0
    t0 = time.perf_counter()
    end = t0 + dur
    if rate <= 0:
        while time.perf_counter() < end:
            for _ in range(64):
                try:
                    s.sendto(payload, (H, 5009))
                    n += 1
                except OSError:
                    pass
    else:
        gap = 1.0 / rate
        nxt = t0
        while True:
            now = time.perf_counter()
            if now >= end:
                break
            if now < nxt:
                time.sleep(min(nxt - now, 0.001)) if nxt - now > 0.0005 else None
                continue
            try:
                s.sendto(payload, (H, 5009))
                n += 1
            except OSError:
                pass
            nxt += gap
    el = time.perf_counter() - t0
    return {"sent": n, "rate": rate, "size": size, "pps": round(n / el), "Mbps": round(n * size * 8 / el / 1e6, 2)}


th = threading.Thread(target=poller, daemon=True)
th.start()
try:
    run_phase("idle", lambda: time.sleep(args.idle) or {})
    if args.floods:
        for i in range(args.floods):
            run_phase(f"flood100_{i}", lambda: udp_flood(0, 100, 2))
            time.sleep(10)
    elif not args.no_bench:
        for i in range(args.runs):
            run_phase(f"tcp_get_{i}", lambda: curl([f"{B}/bench?bytes={args.bytes}"]))
        for i in range(args.runs):
            run_phase(f"tcp_post_{i}", lambda: curl(
                ["-H", "Content-Type: application/octet-stream", "--data-binary",
                 "@/tmp/nbp-bench/blob.bin", f"{B}/bench"]))
        for rate, size, dur in [(500, 1400, 3), (1000, 1400, 3), (2000, 1400, 3),
                                (4000, 1400, 3), (8000, 1400, 3), (0, 1400, 2), (0, 100, 2)]:
            res, bm = run_phase(f"udp_{rate or 'max'}_{size}",
                                lambda r=rate, s=size, d=dur: udp_flood(r, s, d))
            if bm and bm["heap_min"] < args.udp_floor:
                record("udp_ramp_stopped", heap_min=bm["heap_min"])
                break
        run_phase("idle_after", lambda: time.sleep(20) or {})
finally:
    stop = True
    th.join(timeout=5)
