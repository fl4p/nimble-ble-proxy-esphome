#!/usr/bin/env python3
"""
nimble-ble-proxy dashboard probe over Web Bluetooth's GATT protocol.

Same wire format as web/index.html:
  - REQUEST  char  (write):   "<METHOD> <PATH>?<query>" framed as
                              [u8 flags][u8 reqId][payload], flags bit0=FIN.
  - RESPONSE char (notify):   fragments [u8 flags][u8 reqId][payload],
                              reassembled on FIN. bit1=ERR turns the body
                              into an exception message.

Run:
  pip install bleak
  python3 scripts/ble_dashboard_probe.py           # canned probe set
  python3 scripts/ble_dashboard_probe.py /stats.json /clone   # specific calls
  python3 scripts/ble_dashboard_probe.py --watch /stats.json  # poll forever
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time
from typing import Optional

from bleak import BleakClient, BleakScanner

SVC  = "6e627062-7072-7879-0001-000000000000"
REQ  = "6e627062-7072-7879-0001-000000000001"
RESP = "6e627062-7072-7879-0001-000000000002"

FRAME_FIN = 0x01
FRAME_ERR = 0x02


class Dashboard:
    def __init__(self, client: BleakClient):
        self.client = client
        self._next_id = 0
        self._pending: dict[int, asyncio.Future] = {}
        self._chunks: dict[int, list[bytes]] = {}

    def _on_notify(self, _char, data: bytearray) -> None:
        if len(data) < 2:
            return
        flags = data[0]
        rid = data[1]
        self._chunks.setdefault(rid, []).append(bytes(data[2:]))
        if flags & FRAME_FIN:
            payload = b"".join(self._chunks.pop(rid, []))
            fut = self._pending.pop(rid, None)
            if fut and not fut.done():
                if flags & FRAME_ERR:
                    fut.set_exception(RuntimeError(
                        payload.decode("utf-8", errors="replace")))
                else:
                    fut.set_result(payload)

    async def start(self) -> None:
        await self.client.start_notify(RESP, self._on_notify)

    async def call(self, line: str, timeout: float = 8.0) -> bytes:
        self._next_id = (self._next_id + 1) & 0xFF
        rid = self._next_id
        frame = bytes([FRAME_FIN, rid]) + line.encode("utf-8")
        fut: asyncio.Future = asyncio.get_event_loop().create_future()
        self._pending[rid] = fut
        await self.client.write_gatt_char(REQ, frame, response=True)
        try:
            return await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            self._pending.pop(rid, None)
            self._chunks.pop(rid, None)
            raise


def fmt_preview(body: bytes, n: int = 120) -> str:
    # Strip a trailing NUL terminator that snprintf may have left behind.
    body = body.rstrip(b"\x00")
    s = body[:n].decode("utf-8", errors="replace")
    if len(body) > n:
        s += "…"
    return s


async def find_device(name_hint: Optional[str], scan_seconds: float):
    print(f"scanning for service {SVC[:8]}… (timeout {scan_seconds}s)")
    devices = await BleakScanner.discover(
        timeout=scan_seconds, service_uuids=[SVC])
    if not devices:
        print("  no devices found", file=sys.stderr)
        return None
    for d in devices:
        print(f"  {d.address}  name={d.name!r}")
    if name_hint:
        for d in devices:
            if d.name and name_hint.lower() in d.name.lower():
                return d
        print(f"no match for {name_hint!r}; picking first", file=sys.stderr)
    return devices[0]


async def run(args) -> int:
    device = await find_device(args.name, args.scan)
    if device is None:
        return 1
    print(f"\nconnecting to {device.name!r} {device.address}")
    async with BleakClient(device, services=[SVC]) as client:
        dash = Dashboard(client)
        await dash.start()
        print("subscribed to RESPONSE; running calls…\n")

        lines = args.endpoints or [
            "GET /stats.json",
            "GET /level",
            "GET /txpower",
            "GET /cpufreq",
            "GET /scan",
            "GET /clone",
            "GET /devices",
            "GET /log?since=0",
            "GET /log?since=99999999",
        ]
        # Allow bare paths: prefix GET by default.
        lines = [l if " " in l else f"GET {l}" for l in lines]

        cycle = 0
        while True:
            for line in lines:
                t0 = time.perf_counter()
                try:
                    body = await dash.call(line, timeout=args.timeout)
                    dt = (time.perf_counter() - t0) * 1000
                    print(f"  {dt:6.1f}ms  {line:30s}  {len(body):5d}B  "
                          f"{fmt_preview(body)}")
                except Exception as e:
                    dt = (time.perf_counter() - t0) * 1000
                    print(f"  {dt:6.1f}ms  {line:30s}  ERR: {e}")
            if not args.watch:
                break
            cycle += 1
            print(f"--- cycle {cycle} done, sleep {args.watch}s ---")
            await asyncio.sleep(args.watch)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("endpoints", nargs="*",
                    help="endpoint lines (default: canned probe set). "
                         "Use 'METHOD /path?args' or just '/path' for GET.")
    ap.add_argument("--name", default=None,
                    help="prefer device whose name contains this substring "
                         "(default: pick the first match)")
    ap.add_argument("--scan", type=float, default=5.0,
                    help="scan duration seconds")
    ap.add_argument("--timeout", type=float, default=8.0,
                    help="per-call timeout seconds")
    ap.add_argument("--watch", type=float, default=0,
                    help="repeat the call set every N seconds (0=once)")
    args = ap.parse_args()
    try:
        sys.exit(asyncio.run(run(args)))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
