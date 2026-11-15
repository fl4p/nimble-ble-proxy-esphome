#!/usr/bin/env python3
"""Read-only Modbus RTU-over-TCP probe through the modbus_bridge (port 8899).

Sends FC04 (read input registers 0x3108..0x310F) and FC01 (read coil 0x0F)
to an EPEVER IP-Plus inverter, raw RTU frames with CRC16, and decodes them.
Only read function codes are ever built; nothing is written.

  scripts/modbus_bridge_test.py <ip> [--port 8899] [--slave 3] [--timeout 2]
"""
import argparse
import socket
import struct
import sys
import time

READ_FCS = (0x01, 0x02, 0x03, 0x04)


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def frame(slave: int, fc: int, addr: int, count: int) -> bytes:
    assert fc in READ_FCS, "read-only tool"
    pdu = struct.pack(">BBHH", slave, fc, addr, count)
    return pdu + struct.pack("<H", crc16(pdu))


def recv_reply(sock: socket.socket, timeout: float) -> bytes:
    sock.settimeout(timeout)
    buf = b""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            chunk = sock.recv(512)
        except socket.timeout:
            break
        if not chunk:
            raise ConnectionError("bridge closed the connection")
        buf += chunk
        if len(buf) >= 5 and buf[1] & 0x80:
            return buf[:5]
        if len(buf) >= 3 and len(buf) >= 5 + buf[2]:
            return buf[:5 + buf[2]]
        sock.settimeout(max(0.05, end - time.monotonic()))
    return buf


def transact(sock, slave, fc, addr, count, timeout):
    req = frame(slave, fc, addr, count)
    t0 = time.monotonic()
    sock.sendall(req)
    rep = recv_reply(sock, timeout)
    dt = (time.monotonic() - t0) * 1000
    print(f"  tx {req.hex(' ')}")
    print(f"  rx {rep.hex(' ') or '(nothing)'}  [{dt:.0f} ms]")
    if len(rep) < 5:
        raise TimeoutError("no/short reply")
    if crc16(rep[:-2]) != struct.unpack("<H", rep[-2:])[0]:
        raise ValueError("CRC mismatch")
    if rep[0] != slave or (rep[1] & 0x7F) != fc:
        raise ValueError(f"unexpected slave/fc {rep[0]}/{rep[1]:#x}")
    if rep[1] & 0x80:
        raise ValueError(f"exception code {rep[2]}")
    return rep[3:3 + rep[2]]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--slave", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=2.0)
    a = ap.parse_args()

    ok = True
    with socket.create_connection((a.host, a.port), timeout=5) as s:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"FC04 slave {a.slave} input registers 0x3108..0x310F")
        try:
            d = transact(s, a.slave, 0x04, 0x3108, 8, a.timeout)
            r = struct.unpack(">8H", d)
            u32 = lambda lo, hi: (r[hi] << 16) | r[lo]  # low word first
            print(f"  DC  {r[0] / 100:.2f} V  {r[1] / 100:.2f} A  {u32(2, 3) / 100:.2f} W")
            print(f"  AC  {r[4] / 100:.2f} V  {r[5] / 100:.2f} A  {u32(6, 7) / 100:.2f} W")
        except Exception as e:  # noqa: BLE001
            ok = False
            print(f"  FAIL: {e}")
        time.sleep(0.05)
        print(f"FC01 slave {a.slave} coil 0x000F")
        try:
            d = transact(s, a.slave, 0x01, 0x000F, 1, a.timeout)
            print(f"  coil 0x0F = {d[0] & 1}")
        except Exception as e:  # noqa: BLE001
            ok = False
            print(f"  FAIL: {e}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
