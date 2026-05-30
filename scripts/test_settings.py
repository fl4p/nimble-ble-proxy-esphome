#!/usr/bin/env python3
"""Integration tests for the nimble-ble-proxy control surface.

Drives a *live* device over HTTP (the dashboard endpoints) and over the
WebSocket bridge at ws://<ip>/api (NBP_WS_PROXY). Covers the runtime-tunable
settings and their persistence, the ls/pdcpu interaction, input validation,
and the esphome-over-WebSocket handshake + round-trip.

Stdlib only (urllib + socket) — no pip deps.

Usage:
    python3 scripts/test_settings.py <proxy-ip> [--reboot]

  --reboot   also verify cpufreq settings survive a real reboot. Disruptive:
             the device reboots and the script waits for it to come back.

Exit code 0 = all passed, non-zero = at least one failure.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import sys
import time
import urllib.error
import urllib.request

BASE = ""  # "<ip>", set in main()

_PASS = 0
_FAIL = 0


def check(name: str, cond: bool, detail: str = "") -> None:
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}" + (f"  -- {detail}" if detail else ""))


# ---- HTTP helpers ---------------------------------------------------------

def http(method: str, path: str, timeout: float = 5.0):
    req = urllib.request.Request(f"http://{BASE}{path}", method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def get_json(path: str) -> dict:
    st, body = http("GET", path)
    if st != 200:
        raise RuntimeError(f"GET {path} -> {st}: {body}")
    return json.loads(body)


def post(path: str):
    return http("POST", path)


def wait_up(timeout: float = 45.0) -> bool:
    end = time.time() + timeout
    while time.time() < end:
        try:
            st, _ = http("GET", "/cpufreq", timeout=3)
            if st == 200:
                return True
        except Exception:
            pass
        time.sleep(2)
    return False


# ---- minimal WebSocket client (stdlib) ------------------------------------

def _recvn(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed mid-frame")
        buf += chunk
    return buf


def ws_connect(ip: str, path: str = "/api", timeout: float = 5.0) -> socket.socket:
    sock = socket.create_connection((ip, 80), timeout=timeout)
    sock.settimeout(timeout)
    key = "dGhlIHNhbXBsZSBub25jZQ=="
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {ip}\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"Sec-WebSocket-Key: {key}\r\n\r\n"
    )
    sock.sendall(req.encode())
    # Read response headers (up to blank line).
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += sock.recv(256)
    status = resp.split(b"\r\n", 1)[0].decode(errors="replace")
    if "101" not in status:
        sock.close()
        raise RuntimeError(f"WS handshake not 101: {status!r}")
    return sock


def ws_send_binary(sock: socket.socket, payload: bytes) -> None:
    # Client frames MUST be masked (RFC 6455 5.3).
    hdr = bytearray([0x82])  # FIN + opcode=2 (binary)
    n = len(payload)
    if n < 126:
        hdr.append(0x80 | n)
    elif n < 65536:
        hdr.append(0x80 | 126)
        hdr += struct.pack(">H", n)
    else:
        hdr.append(0x80 | 127)
        hdr += struct.pack(">Q", n)
    mask = os.urandom(4)
    hdr += mask
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    sock.sendall(bytes(hdr) + masked)


def ws_recv_payload(sock: socket.socket) -> bytes:
    b0 = _recvn(sock, 2)
    ln = b0[1] & 0x7F
    if ln == 126:
        ln = struct.unpack(">H", _recvn(sock, 2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", _recvn(sock, 8))[0]
    # Server->client frames are never masked.
    return _recvn(sock, ln) if ln else b""


# ---- esphome plaintext frame helpers --------------------------------------

def esphome_frame(msg_type: int, payload: bytes = b"") -> bytes:
    def varint(n: int) -> bytes:
        out = bytearray()
        while True:
            b = n & 0x7F
            n >>= 7
            out.append(b | 0x80 if n else b)
            if not n:
                break
        return bytes(out)
    return b"\x00" + varint(len(payload)) + varint(msg_type) + payload


def parse_esphome_types(buf: bytes) -> list[int]:
    """Return the list of message-type IDs found in a concatenated stream."""
    types, i = [], 0
    while i < len(buf):
        if buf[i] != 0x00:
            break
        i += 1
        size = shift = 0
        while i < len(buf):
            b = buf[i]; i += 1
            size |= (b & 0x7F) << shift
            if not b & 0x80:
                break
            shift += 7
        mtype = shift = 0
        while i < len(buf):
            b = buf[i]; i += 1
            mtype |= (b & 0x7F) << shift
            if not b & 0x80:
                break
            shift += 7
        if i + size > len(buf):
            break
        types.append(mtype)
        i += size
    return types


# ---- test cases -----------------------------------------------------------

def test_cpufreq_roundtrip():
    print("[cpufreq round-trip + ls/pdcpu semantics]")
    post("/cpufreq?mhz=160")
    post("/cpufreq?ls=1")
    post("/cpufreq?pdcpu=1")
    f = get_json("/cpufreq")
    check("mhz set to 160", f.get("mhz") == 160, str(f))
    check("ls=true after ls=1", f.get("ls") is True, str(f))
    check("pdcpu=true after pdcpu=1", f.get("pdcpu") is True, str(f))

    # pdcpu is independent of ls (turning ls off must not clear pdcpu)
    post("/cpufreq?ls=0")
    f = get_json("/cpufreq")
    check("ls=false after ls=0", f.get("ls") is False, str(f))
    check("pdcpu retained when ls toggled off", f.get("pdcpu") is True, str(f))

    # restore both off (safe default)
    post("/cpufreq?ls=0")
    post("/cpufreq?pdcpu=0")


def test_cpufreq_validation():
    print("[cpufreq input validation]")
    st, body = post("/cpufreq?mhz=123")
    check("invalid mhz rejected (4xx)", 400 <= st < 500, f"got {st}: {body}")
    st, body = post("/cpufreq")
    check("empty cpufreq POST rejected (4xx)", 400 <= st < 500, f"got {st}: {body}")


def test_txpower_roundtrip():
    print("[txpower round-trip]")
    post("/txpower?wifi=13")
    f = get_json("/txpower")
    check("wifi tx set to 13", f.get("wifi") == 13, str(f))
    post("/txpower?wifi=20")
    f = get_json("/txpower")
    check("wifi tx set to 20", f.get("wifi") == 20, str(f))
    post("/txpower?wifi=13")  # leave at the cool value


def test_ble_txpower():
    print("[ble txpower round-trip + off field]")
    f = get_json("/txpower")
    # BLE-free builds report ble_off:false and a static ble dBm; the field is
    # always present so the dashboard can render the "off" state.
    check("txpower exposes ble_off field", "ble_off" in f, str(f))
    if f.get("ble_off"):
        # BLE already shut down (someone ran the off test without rebooting);
        # nothing more to round-trip until a reboot brings BLE back.
        print("  ble_off already set — skipping dBm round-trip")
        return
    post("/txpower?ble=-6")
    check("ble tx set to -6", get_json("/txpower").get("ble") == -6, "")
    post("/txpower?ble=-9")
    check("ble tx set to -9", get_json("/txpower").get("ble") == -9, "")
    # Validation: out-of-range dBm and a junk value are both rejected (4xx),
    # and BLE must NOT have been shut down as a side effect.
    st, body = post("/txpower?ble=99")
    check("ble tx 99 rejected (4xx)", 400 <= st < 500, f"got {st}: {body}")
    check("ble still on after bad value", not get_json("/txpower").get("ble_off"), "")


def test_ble_off_reboot():
    # Destructive: ble=off fully deinits the NimBLE stack; only a reboot brings
    # BLE back. Gated behind --reboot for that reason.
    print("[ble off → reboot recovery]  (disruptive)")
    if get_json("/txpower").get("ble_off"):
        print("  ble already off; rebooting to restore first")
        post("/reboot"); time.sleep(4); wait_up(60)
    post("/txpower?ble=off")
    f = get_json("/txpower")
    check("ble_off true after ?ble=off", f.get("ble_off") is True, str(f))
    # A dBm set is refused while off (reboot-to-re-enable contract).
    st, _ = post("/txpower?ble=0")
    check("ble dBm set refused while off (4xx)", 400 <= st < 500, f"got {st}")
    print("  rebooting to bring BLE back...")
    post("/reboot"); time.sleep(4)
    if not wait_up(60):
        check("device back after reboot", False, "did not return in 60s")
        return
    check("ble_off cleared after reboot",
          get_json("/txpower").get("ble_off") is False, "")


def test_wifips_roundtrip():
    print("[wifips round-trip]")
    post("/wifips?li=0")
    f = get_json("/wifips")
    check("wifips li=0 (PS off)", f.get("li") == 0, str(f))


def test_ws_bridge():
    print("[websocket bridge: handshake + esphome round-trip]")
    try:
        sock = ws_connect(BASE)
    except Exception as e:
        check("WS /api handshake -> 101", False, str(e))
        return
    check("WS /api handshake -> 101", True)
    try:
        ws_send_binary(sock, esphome_frame(1))   # HelloRequest
        ws_send_binary(sock, esphome_frame(9))   # DeviceInfoRequest
        acc = b""
        types: set[int] = set()
        deadline = time.time() + 5
        while time.time() < deadline and not ({2, 10} <= types):
            try:
                acc += ws_recv_payload(sock)
            except (socket.timeout, ConnectionError):
                break
            types = set(parse_esphome_types(acc))
        check("HelloResponse (type 2) over WS", 2 in types, f"types={sorted(types)}")
        check("DeviceInfoResponse (type 10) over WS", 10 in types,
              f"types={sorted(types)}")
    finally:
        sock.close()


def test_cpufreq_persistence():
    print("[cpufreq persistence across reboot]  (disruptive)")
    post("/cpufreq?mhz=160")
    post("/cpufreq?ls=1")
    post("/cpufreq?pdcpu=1")
    before = get_json("/cpufreq")
    check("pre-reboot ls/pdcpu set",
          bool(before.get("ls") and before.get("pdcpu")), str(before))
    print("  rebooting...")
    post("/reboot")
    time.sleep(4)
    if not wait_up(60):
        check("device back after reboot", False, "did not return in 60s")
        return
    after = get_json("/cpufreq")
    check("mhz persisted", after.get("mhz") == before.get("mhz"), str(after))
    check("ls persisted across reboot", after.get("ls") is True, str(after))
    check("pdcpu persisted across reboot", after.get("pdcpu") is True, str(after))
    # restore safe defaults
    post("/cpufreq?ls=0")
    post("/cpufreq?pdcpu=0")


def main() -> int:
    global BASE
    p = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    p.add_argument("ip", help="proxy IP, e.g. 192.168.1.125")
    p.add_argument("--reboot", action="store_true",
                   help="also run the (disruptive) reboot-persistence test")
    args = p.parse_args()
    BASE = args.ip

    if not wait_up(15):
        print(f"[!] {BASE} not reachable")
        return 2

    test_cpufreq_roundtrip()
    test_cpufreq_validation()
    test_txpower_roundtrip()
    test_ble_txpower()
    test_wifips_roundtrip()
    test_ws_bridge()
    if args.reboot:
        test_cpufreq_persistence()
        test_ble_off_reboot()

    print(f"\n{_PASS} passed, {_FAIL} failed")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
