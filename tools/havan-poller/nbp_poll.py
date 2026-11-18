#!/usr/bin/python3
"""Poll the nimble-proxy's GET /stats.json, /wifidisc and /wifihist and send
InfluxDB line protocol to a UDP relay. Read-only on the device (GETs only).

Measurements:
  nimble_proxy         one point per period (stats.json scalars, wifidisc, newest
                       /wifihist sample as sta_rssi/ap_rssi/wd_rtt_ms/sta_count)
  nimble_proxy_wifi    every /wifihist sample (the device's liveness-probe ring,
                       ~1 h at 30 s), including the ones recorded while the poller
                       could not reach the device; backfilled on recovery.
                       Timestamps are ESTIMATED (no per-sample time on the
                       device): new samples spread evenly since the last one sent.
  nimble_proxy_reboot  one point per detected reboot (uptime went backwards),
                       with the reset reason from /crash. The new boot's /log
                       (which still holds the previous boot's tail for a few
                       minutes) and /crash are saved under NBP_REBOOT_DIR.
"""
import datetime
import json
import os
import socket
import sys
import time

import requests

DEV = os.environ.get("NBP_HOST", "192.168.1.231")
RELAY = (os.environ.get("NBP_RELAY_HOST", "127.0.0.1"), int(os.environ.get("NBP_RELAY_PORT", "8086")))
PERIOD = float(os.environ.get("NBP_PERIOD_S", "30"))
TIMEOUT = float(os.environ.get("NBP_TIMEOUT_S", "5"))
MEAS = "nimble_proxy"
TAGS = "host=" + os.environ.get("NBP_TAG_HOST", "nimble-proxy-231")
MAX_DGRAM = 1400
REBOOT_DIR = os.path.expanduser(os.environ.get("NBP_REBOOT_DIR", "~/nbp-reboots"))
REBOOT_TRIES = 10  # cycles to keep retrying the /log + /crash snapshot after a reboot
FLOAT_FIELDS = {"temp_c", "latency_ms", "latency_wd_ms", "latency_wh_ms"}

_types = {}  # field -> first-seen type, so a field's type never changes in this process


def esc_str(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def fmt(k, v):
    if isinstance(v, bool):
        t = bool
    elif isinstance(v, (int, float)):
        t = float if (k in FLOAT_FIELDS or isinstance(v, float)) else int
    elif isinstance(v, str):
        t = str
    else:
        return None
    t = _types.setdefault(k, t)
    try:
        if t is bool:
            return "true" if v else "false"
        if t is int:
            return "%di" % int(round(v))
        if t is float:
            return repr(float(v))
        return esc_str(v)
    except (TypeError, ValueError):
        return None


def fetch(path, text=False):
    t0 = time.monotonic()
    try:
        # no keep-alive: don't hold one of the device's few HTTP sockets between polls
        r = requests.get("http://%s%s" % (DEV, path), timeout=TIMEOUT, headers={"Connection": "close"})
        ms = (time.monotonic() - t0) * 1e3
        r.raise_for_status()
        if text:
            return r.text, ms
        d = r.json()
        return (d if isinstance(d, dict) else None), ms
    except Exception as e:
        print("%s: %s" % (path, e), file=sys.stderr, flush=True)
        return None, (time.monotonic() - t0) * 1e3


class State:
    boot_epoch = None    # wall-clock time of the current boot (from the first uptime seen)
    last_uptime = None
    prev_hist = None     # last /wifihist sample list seen (this boot)
    last_hist_ts = None  # estimated timestamp of the newest sample already sent
    reboot_pending = 0   # cycles left to retry the reboot snapshot
    reboot_info = None


def hist_samples(wh):
    """/wifihist arrays -> [fields], oldest first; ([], 30) if malformed."""
    try:
        n = int(wh["count"])
        iv = int(wh.get("interval") or 30)
        cols = [wh[k] for k in ("rssi", "ap", "rtt", "sta")]
    except (KeyError, TypeError, ValueError):
        return [], 30
    if n <= 0 or any(not isinstance(c, list) or len(c) != n for c in cols):
        return [], iv
    out = []
    for rssi, ap, rtt, sta in zip(*cols):
        f = {"wd_probe_ok": int(rtt is not None)}
        if rssi is not None:
            f["sta_rssi"] = rssi
        if ap is not None:
            f["ap_rssi"] = ap
        if rtt is not None:
            f["wd_rtt_ms"] = rtt
        if sta is not None:
            f["sta_count"] = sta
        out.append(f)
    return out, iv


def new_tail(prev, cur):
    """Number of samples in `cur` not already in `prev`: the ring only appends
    (and drops from the front once full), so find the smallest shift k with
    cur[:-k] == prev[k':] tail overlap. No overlap -> everything is new."""
    if not prev:
        return len(cur)
    for k in range(0, len(cur) + 1):
        keep = len(cur) - k
        if keep == 0:
            return len(cur)
        if keep <= len(prev) and cur[:keep] == prev[len(prev) - keep:]:
            return k
    return len(cur)


def snapshot_reboot(sock, now):
    """Save /log + /crash after a reboot; True when done."""
    crash, _ = fetch("/crash")
    log, _ = fetch("/log", text=True)
    if crash is None and log is None:
        return False
    stamp = datetime.datetime.utcfromtimestamp(now).strftime("%Y%m%dT%H%M%SZ")
    d = os.path.join(REBOOT_DIR, stamp)
    os.makedirs(d, exist_ok=True)
    if log is not None:
        with open(os.path.join(d, "log.txt"), "w") as fh:
            fh.write(log)
    if crash is not None:
        with open(os.path.join(d, "crash.json"), "w") as fh:
            json.dump(crash, fh, indent=1)
    info = dict(State.reboot_info or {})
    info["saved_log"] = int(log is not None)
    info["saved_crash"] = int(crash is not None)
    if crash is not None:
        info["reset_reason"] = str(crash.get("last_reset_reason", ""))
        if isinstance(crash.get("count"), int):
            info["crash_count"] = crash["count"]
    send(sock, lines(info, int(now * 1000), meas="nimble_proxy_reboot"))
    print("reboot snapshot saved to %s (%s)" % (d, info.get("reset_reason", "no /crash")),
          file=sys.stderr, flush=True)
    return log is not None and crash is not None


def collect(sock=None):
    now = time.time()
    st, lat = fetch("/stats.json")
    wd, lat_wd = fetch("/wifidisc")
    wh, lat_wh = fetch("/wifihist")
    f = {"ok": int(st is not None and wd is not None), "ok_stats": int(st is not None),
         "ok_wd": int(wd is not None), "ok_wh": int(wh is not None),
         "latency_ms": round(lat, 1), "latency_wd_ms": round(lat_wd, 1),
         "latency_wh_ms": round(lat_wh, 1)}
    if st:
        for k, v in st.items():
            if isinstance(v, (bool, int, float)) and k not in f:
                f[k] = v
    if wd:
        for k in ("uptime_s", "sta_down_ms", "total"):
            if isinstance(wd.get(k), (int, float)):
                f[k] = wd[k]
        ev = [e for e in wd.get("events") or [] if isinstance(e, dict) and "t_s" in e]
        if ev:
            e = max(ev, key=lambda e: e["t_s"])
            f["last_evt_t_s"] = e["t_s"]
            f["last_evt_reason"] = e.get("reason", 0)
            f["last_evt_if"] = str(e.get("if", ""))

    up = wd.get("uptime_s") if wd else None
    if isinstance(up, (int, float)):
        if State.last_uptime is not None and up < State.last_uptime:
            State.reboot_info = {"prev_uptime_s": State.last_uptime, "uptime_s": up}
            State.reboot_pending = REBOOT_TRIES
            State.boot_epoch = None
            State.prev_hist = None
            State.last_hist_ts = None
        if State.boot_epoch is None:
            State.boot_epoch = now - up
        State.last_uptime = up
    if State.reboot_pending and sock is not None:
        State.reboot_pending = 0 if snapshot_reboot(sock, now) else State.reboot_pending - 1

    if wh:
        samples, iv = hist_samples(wh)
        if samples:
            f.update(samples[-1])
            k = new_tail(State.prev_hist, samples)
            if sock is not None and k:
                # Timestamps are estimates: the device keeps no per-sample time.
                # New samples are spread evenly over (last sent, now]; with no
                # previous anchor they are spaced `iv` back from now, never
                # earlier than boot + 90 s (the liveness grace period).
                if State.last_hist_ts is not None and State.last_hist_ts < now:
                    step = (now - State.last_hist_ts) / k
                    t0 = State.last_hist_ts + step
                else:
                    step = iv
                    t0 = now - (k - 1) * iv
                    if State.boot_epoch is not None:
                        t0 = max(t0, State.boot_epoch + 90)
                        step = min(iv, (now - t0) / max(k - 1, 1)) if k > 1 else iv
                out = []
                new = samples[len(samples) - k:]
                for i, sf in enumerate(new):
                    out += lines(sf, int((t0 + i * step) * 1000), meas="nimble_proxy_wifi")
                send(sock, out)
                State.last_hist_ts = t0 + (k - 1) * step
            State.prev_hist = samples
    return f


def lines(fields, ts_ms, meas=MEAS):
    head, tail = "%s,%s " % (meas, TAGS), " %d" % ts_ms
    out, cur = [], []
    for k, v in fields.items():
        p = fmt(k, v)
        if p is None:
            continue
        kv = "%s=%s" % (k.replace(" ", "\\ ").replace(",", "\\,").replace("=", "\\="), p)
        if cur and len(head) + len(",".join(cur + [kv])) + len(tail) > MAX_DGRAM:
            out.append(head + ",".join(cur) + tail)
            cur = []
        cur.append(kv)
    if cur:
        out.append(head + ",".join(cur) + tail)
    return out


def send(sock, ls):
    buf = ""
    for l in ls:
        if buf and len(buf) + 1 + len(l) > MAX_DGRAM:
            sock.sendto(buf.encode(), RELAY)
            buf = ""
        buf = buf + "\n" + l if buf else l
    if buf:
        sock.sendto(buf.encode(), RELAY)


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if "--print" in sys.argv:
        print("\n".join(lines(collect(), int(time.time() * 1000))))
        return
    nxt = time.monotonic()
    while True:
        ts_ms = int(time.time() * 1000)
        try:
            send(sock, lines(collect(sock), ts_ms))
        except Exception as e:
            print("cycle: %r" % e, file=sys.stderr, flush=True)
        nxt += PERIOD
        d = nxt - time.monotonic()
        if d < 0:
            nxt = time.monotonic()
            d = 0
        time.sleep(d)


if __name__ == "__main__":
    main()
