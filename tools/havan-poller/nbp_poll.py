#!/usr/bin/python3
"""Poll the nimble-proxy's GET /stats.json and GET /wifidisc and send one
InfluxDB line-protocol point per period to a UDP relay. Read-only on the device."""
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
FLOAT_FIELDS = {"temp_c", "latency_ms", "latency_wd_ms"}

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


def fetch(path):
    t0 = time.monotonic()
    try:
        # no keep-alive: don't hold one of the device's few HTTP sockets between polls
        r = requests.get("http://%s%s" % (DEV, path), timeout=TIMEOUT, headers={"Connection": "close"})
        ms = (time.monotonic() - t0) * 1e3
        r.raise_for_status()
        d = r.json()
        return (d if isinstance(d, dict) else None), ms
    except Exception as e:
        print("%s: %s" % (path, e), file=sys.stderr, flush=True)
        return None, (time.monotonic() - t0) * 1e3


def collect():
    st, lat = fetch("/stats.json")
    wd, lat_wd = fetch("/wifidisc")
    f = {"ok": int(st is not None and wd is not None), "ok_stats": int(st is not None),
         "ok_wd": int(wd is not None), "latency_ms": round(lat, 1), "latency_wd_ms": round(lat_wd, 1)}
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
    return f


def lines(fields, ts_ms):
    head, tail = "%s,%s " % (MEAS, TAGS), " %d" % ts_ms
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
            send(sock, lines(collect(), ts_ms))
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
