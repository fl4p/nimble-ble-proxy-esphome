#!/usr/bin/env python3
"""Two-client ESPHome-API bench for nimble-ble-proxy (plaintext API, raw frames).

Client A: healthy. Subscribes adverts, connects a BLE peer, discovers services,
enables every notify characteristic (NotifyRequest + CCCD write), counts
NotifyData frames, times one GATT read per --read-every seconds, and re-sends
GetServices every --gs-every seconds (bulk bursts).
Client B: subscribes adverts, then stops reading (tiny SO_RCVBUF). It pings
every 2 s so a reap shows up as a send error; it then reconnects and stalls
again. Counts its reaps.
A poller reads /stats.json at 1 Hz. Everything goes to OUT/*.jsonl, and a
summary JSON is printed at the end.

usage: apibench.py OUT --host IP [--peer MAC] [--dur 180] [--b-start 15]
                   [--no-b] [--read-every 1.0] [--gs-every 0]
"""
import argparse, json, os, socket, struct, threading, time, urllib.request

# ---- protobuf helpers ----
def venc(n):
    out = bytearray()
    while True:
        b = n & 0x7F; n >>= 7
        if n: out.append(b | 0x80)
        else: out.append(b); return bytes(out)

def vdec(buf, i):
    n = s = 0
    while True:
        b = buf[i]; i += 1
        n |= (b & 0x7F) << s; s += 7
        if not b & 0x80: return n, i

def fv(f, v): return venc(f << 3) + venc(v)
def fb(f, b): return venc(f << 3 | 2) + venc(len(b)) + b

def pdec(buf):
    """-> list of (field, wiretype, value)"""
    i, out = 0, []
    while i < len(buf):
        k, i = vdec(buf, i); f, w = k >> 3, k & 7
        if w == 0: v, i = vdec(buf, i)
        elif w == 2:
            l, i = vdec(buf, i); v = buf[i:i + l]; i += l
        elif w == 1: v = buf[i:i + 8]; i += 8
        elif w == 5: v = buf[i:i + 4]; i += 4
        else: raise ValueError(w)
        out.append((f, w, v))
    return out

def pget(fields, f, default=None):
    for ff, _, v in fields:
        if ff == f: return v
    return default

def frame(t, payload=b""):
    return b"\x00" + venc(len(payload)) + venc(t) + payload

class Conn:
    def __init__(s, host, rcvbuf=None):
        s.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf: s.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        s.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.sock.settimeout(10)
        s.sock.connect((host, 6053))
        s.buf = b""
    def send(s, t, p=b""): s.sock.sendall(frame(t, p))
    def recv(s, timeout=None):
        s.sock.settimeout(timeout)
        while True:
            if len(s.buf) >= 3:
                try:
                    if s.buf[0] != 0: raise IOError("bad preamble")
                    l, i = vdec(s.buf, 1); t, j = vdec(s.buf, i)
                    if len(s.buf) >= j + l:
                        p = s.buf[j:j + l]; s.buf = s.buf[j + l:]
                        return t, p
                except IndexError:
                    pass
            d = s.sock.recv(65536)
            if not d: raise EOFError("peer closed")
            s.buf += d
    def handshake(s, name):
        s.send(1, fb(1, name.encode()) + fv(2, 1) + fv(3, 10))
        s.send(3, b"")
        s.send(9, b"")
        info = {}
        while "dev" not in info:
            t, p = s.recv(10)
            if t == 10:
                f = pdec(p)
                info["dev"] = {"mac": pget(f, 3, b"").decode(),
                               "compile": pget(f, 5, b"").decode(),
                               "name": pget(f, 2, b"").decode()}
        return info["dev"]

def mac_int(m): return int(m.replace(":", ""), 16)

class Bench:
    def __init__(s, a):
        s.a = a; s.t0 = time.time(); s.stop = False
        os.makedirs(a.out, exist_ok=True)
        s.f_ev = open(os.path.join(a.out, "events.jsonl"), "a", buffering=1)
        s.f_st = open(os.path.join(a.out, "stats.jsonl"), "a", buffering=1)
        s.lock = threading.Lock()
        s.A = dict(notifies=0, notify_by_handle={}, adverts=0, reads=0, read_err=0,
                   read_lat_ms=[], gaps_ms=[], max_gap_ms=0, disconnected=None,
                   gs_bursts=0, gs_ms=[], gs_frames=0, peer_connected=False,
                   peer_drops=0, seq_lost=0, seq_breaks=0, last_seq={})
        s.B = dict(sessions=0, reaps=0, reap_after_s=[], err=[])
    def ev(s, **k):
        k["t"] = round(time.time() - s.t0, 3)
        s.f_ev.write(json.dumps(k) + "\n")

    # ---- stats poller ----
    def poller(s):
        url = f"http://{s.a.host}/stats.json"
        while not s.stop:
            t = time.time()
            try:
                with urllib.request.urlopen(url, timeout=3) as r:
                    st = json.loads(r.read())
                s.f_st.write(json.dumps({"t": round(t - s.t0, 3), "s": st}) + "\n")
            except Exception as e:
                s.f_st.write(json.dumps({"t": round(t - s.t0, 3), "err": str(e)}) + "\n")
            time.sleep(max(0, 1 - (time.time() - t)))

    # ---- client B ----
    def client_b(s):
        time.sleep(s.a.b_start)
        while not s.stop:
            try:
                c = Conn(s.a.host, rcvbuf=1024)
                c.handshake("bench-B")
                c.send(66, fv(1, 1)); c.send(80)
                if s.a.b_flood:
                    c.sock.sendall(frame(9) * s.a.b_flood)
                s.B["sessions"] += 1
                start = time.time(); s.ev(who="B", what="stalling")
                # stop reading; shrink further after the handshake
                c.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                c.sock.settimeout(5)
                while not s.stop:
                    time.sleep(2)
                    c.sock.sendall(frame(7))
            except Exception as e:
                dt = round(time.time() - start, 1) if 'start' in dir() else None
                s.B["reaps"] += 1; s.B["reap_after_s"].append(dt)
                s.B["err"].append(str(e)[:60]); s.ev(who="B", what="dead", err=str(e), after=dt)
                try: c.sock.close()
                except Exception: pass
                time.sleep(1)
                continue
        try: c.sock.close()
        except Exception: pass

    # ---- client A ----
    def client_a(s):
        a = s.a
        c = Conn(a.host)
        dev = c.handshake("bench-A"); s.ev(who="A", what="dev", **dev); s.dev = dev
        c.send(66, fv(1, 1)); c.send(80)
        addr = mac_int(a.peer) if a.peer else None
        addr_type = None
        pending_read = {}
        services = []
        notify_handles, read_handle = [], None
        state = "scan"; next_read = 0; next_gs = 0; gs_t0 = None; last_n = None
        scan_deadline = time.time() + 30
        cccd_pending = []
        while not s.stop:
            now = time.time()
            if state == "scan" and addr and addr_type is not None:
                c.send(68, fv(1, addr) + fv(2, 4) + fv(3, 1) + fv(4, addr_type))
                state = "connecting"; s.ev(who="A", what="connect_req", at=addr_type)
                conn_deadline = now + 30
            if state == "scan" and now > scan_deadline and addr_type is None:
                s.ev(who="A", what="peer_not_seen"); state = "adv_only"; s.A["peer_seen"] = False
            if state == "connecting" and now > conn_deadline:
                s.ev(who="A", what="connect_timeout"); state = "adv_only"
            if state == "run":
                if read_handle and now >= next_read and not pending_read:
                    c.send(73, fv(1, addr) + fv(2, read_handle)); pending_read[read_handle] = now
                    next_read = now + a.read_every
                if a.gs_every and now >= next_gs and gs_t0 is None:
                    c.send(70, fv(1, addr)); gs_t0 = now; next_gs = now + a.gs_every
            if state == "adv_only" and a.gs_every and addr and now >= next_gs:
                pass
            try:
                t, p = c.recv(0.05)
            except socket.timeout:
                continue
            except Exception as e:
                s.A["disconnected"] = f"{round(time.time()-s.t0,1)}s: {e}"
                s.ev(who="A", what="dead", err=str(e)); return
            if t == 93:
                for f, _, adv in pdec(p):
                    if f != 1: continue
                    af = pdec(adv); s.A["adverts"] += 1
                    if a.peer_name and addr is None and a.peer_name.encode() in pget(af, 4, b""):
                        addr = pget(af, 1); s.ev(who="A", what="peer_by_name", addr="%012X" % addr)
                    if addr and pget(af, 1) == addr and addr_type is None:
                        addr_type = pget(af, 3, 0); s.A["peer_seen"] = True
            elif t == 69:
                f = pdec(p)
                conn, err = pget(f, 2, 0), pget(f, 4, 0)
                s.ev(who="A", what="conn_state", connected=conn, mtu=pget(f, 3, 0), err=err)
                if conn and state == "connecting":
                    state = "discover"; c.send(70, fv(1, addr)); gs_t0 = time.time()
                elif not conn and state in ("run", "discover"):
                    s.A["peer_drops"] += 1; s.A["peer_connected"] = False
                    state = "scan"; addr_type = None; scan_deadline = time.time() + 15
                    pending_read.clear()
            elif t == 71:
                s.A["gs_frames"] += 1
                if state == "discover":
                    for f, _, sv in pdec(p):
                        if f == 2: services.append(pdec(sv))
            elif t == 72:
                if gs_t0 is not None:
                    s.A["gs_ms"].append(round((time.time() - gs_t0) * 1000, 1)); gs_t0 = None
                    s.A["gs_bursts"] += 1
                if state == "discover":
                    for sv in services:
                        for f, _, ch in sv:
                            if f != 3: continue
                            cf = pdec(ch); h = pget(cf, 2, 0); props = pget(cf, 3, 0)
                            if props & 0x02 and read_handle is None and (not a.read_handle or a.read_handle == h):
                                read_handle = h
                            if props & 0x30 and (not a.notify_handle or a.notify_handle == h):
                                cccd = None
                                for ff, _, d in cf:
                                    if ff == 4:
                                        df = pdec(d)
                                        if pget(df, 3, 0) == 0x2902: cccd = pget(df, 2)
                                notify_handles.append((h, cccd, props))
                    if a.read_handle: read_handle = a.read_handle
                    s.ev(who="A", what="discovered", notify=notify_handles, read=read_handle,
                         services=len(services))
                    for h, cccd, props in notify_handles:
                        c.send(78, fv(1, addr) + fv(2, h) + fv(3, 1))
                        if cccd:
                            val = b"\x01\x00" if props & 0x10 else b"\x02\x00"
                            c.send(77, fv(1, addr) + fv(2, cccd) + fb(3, val))
                    state = "run"; s.A["peer_connected"] = True
                    s.A["run_since"] = round(time.time() - s.t0, 1)
            elif t == 74 or t == 82:
                f = pdec(p); h = pget(f, 2, 0)
                if h in pending_read:
                    dt = (time.time() - pending_read.pop(h)) * 1000
                    if t == 74: s.A["reads"] += 1; s.A["read_lat_ms"].append(round(dt, 1))
                    else: s.A["read_err"] += 1; s.ev(who="A", what="gatt_err", h=h, e=pget(f, 3))
                elif t == 82:
                    s.ev(who="A", what="gatt_err", h=h, e=pget(f, 3))
            elif t == 79:
                now = time.time(); nf = pdec(p); h = pget(nf, 2, 0); d = pget(nf, 3, b"")
                s.A["notifies"] += 1
                if len(d) >= 4:
                    sq = struct.unpack_from("<I", d)[0]
                    ls = s.A["last_seq"].get(h)
                    if ls is not None and sq != ls + 1:
                        s.A["seq_lost"] += max(0, sq - ls - 1); s.A["seq_breaks"] += 1
                        s.ev(who="A", what="seq_gap", h=h, prev=ls, got=sq)
                    s.A["last_seq"][h] = sq
                s.A["notify_by_handle"][h] = s.A["notify_by_handle"].get(h, 0) + 1
                if last_n is not None:
                    g = (now - last_n) * 1000; s.A["gaps_ms"].append(round(g, 1))
                last_n = now
            elif t == 7:
                c.send(8)
            elif t == 5:
                c.send(6); s.A["disconnected"] = "server DisconnectRequest"; return
        try:
            if addr: c.send(68, fv(1, addr) + fv(2, 1)); time.sleep(0.5)
            c.send(5); c.sock.close()
        except Exception: pass

    def run(s):
        th = [threading.Thread(target=s.poller, daemon=True),
              threading.Thread(target=s.client_a, daemon=True)]
        if not s.a.no_b: th.append(threading.Thread(target=s.client_b, daemon=True))
        for t in th: t.start()
        time.sleep(s.a.dur); s.stop = True
        for t in th[1:]: t.join(5)
        time.sleep(1.2)
        try:
            with urllib.request.urlopen(f"http://{s.a.host}/stats.json", timeout=3) as r:
                s.f_st.write(json.dumps({"t": round(time.time() - s.t0, 3), "s": json.loads(r.read()), "final": 1}) + "\n")
        except Exception as e:
            s.ev(who="poll", what="final_err", err=str(e))

def pct(v, q):
    if not v: return None
    v = sorted(v); return v[min(len(v) - 1, int(q * len(v)))]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out"); ap.add_argument("--host", required=True)
    ap.add_argument("--peer"); ap.add_argument("--peer-name"); ap.add_argument("--dur", type=float, default=180)
    ap.add_argument("--b-start", type=float, default=15); ap.add_argument("--no-b", action="store_true")
    ap.add_argument("--read-every", type=float, default=1.0)
    ap.add_argument("--read-handle", type=int); ap.add_argument("--notify-handle", type=int)
    ap.add_argument("--gs-every", type=float, default=0)
    ap.add_argument("--b-flood", type=int, default=0, help="B sends N DeviceInfoRequests, then stops reading")
    a = ap.parse_args()
    b = Bench(a); b.run()
    st = []
    for line in open(os.path.join(a.out, "stats.jsonl")):
        r = json.loads(line)
        if "s" in r: st.append(r)
    A = b.A
    keys = ["heap", "heap_min", "heap_largest", "notify_rx", "api_txq_max", "api_txq_drops",
            "api_txq_reaps", "api_ring_reaps", "api_tx_frames", "api_tx_sends",
            "api_enq_us_max", "api_tx_stack_free", "api_refused", "oom"]
    first, last = (st[0]["s"], st[-1]["s"]) if st else ({}, {})
    run_stats = [r["s"] for r in st if "run_since" in A and r["t"] >= A["run_since"] + 2]
    summ = {
        "dev": getattr(b, "dev", None),
        "A": {k: v for k, v in A.items() if k not in ("read_lat_ms", "gaps_ms", "gs_ms")},
        "A_read_ms": {"n": len(A["read_lat_ms"]), "p50": pct(A["read_lat_ms"], .5),
                      "p95": pct(A["read_lat_ms"], .95), "max": max(A["read_lat_ms"] or [None])},
        "A_gap_ms": {"p50": pct(A["gaps_ms"], .5), "p99": pct(A["gaps_ms"], .99),
                     "max": max(A["gaps_ms"] or [None])},
        "A_gs_ms": A["gs_ms"],
        "B": b.B,
        "stats_first": {k: first.get(k) for k in keys},
        "stats_last": {k: last.get(k) for k in keys},
        "heap_min_seen": min((r["s"].get("heap", 1 << 30) for r in st), default=None),
        "heap_largest_min_seen": min((r["s"].get("heap_largest", 1 << 30) for r in st), default=None),
        "ring_max_seen": max((max([x[2] for x in r["s"].get("api_ring", [])] or [0]) for r in st), default=None),
        "txq_max_seen": max((max([x[2] for x in r["s"].get("api_txq", [])] or [0]) for r in st), default=None),
        "poll_errors": sum(1 for line in open(os.path.join(a.out, "stats.jsonl")) if '"err"' in line),
    }
    if "notify_rx" in first and "notify_rx" in last:
        summ["notify_rx_delta"] = last["notify_rx"] - first["notify_rx"]
        summ["notify_lost"] = summ["notify_rx_delta"] - A["notifies"]
    json.dump(summ, open(os.path.join(a.out, "summary.json"), "w"), indent=1)
    print(json.dumps(summ, indent=1))

if __name__ == "__main__":
    main()
