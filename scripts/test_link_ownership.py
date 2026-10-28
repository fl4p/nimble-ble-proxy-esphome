#!/usr/bin/env python3
"""Two-client test of per-client BLE link ownership on a live proxy.

Plays the HA + batmon scenario with aioesphomeapi clients against a real
peripheral (one that stops advertising while linked, e.g. a BMS):

  1. A connects to the peripheral.
  2. B's CONNECT to the same address gets error -100, and A sees NO
     connection-state event for it (it would read as "my link dropped").
  3. B's DISCONNECT for A's address answers B with connected=false and
     leaves A's link up (still listed as allocated).
  4. A vanishes abruptly (socket closed, like a restarted add-on). B,
     still connected, sees the slot freed via ConnectionsFree.
  5. C (the "restarted" client) connects to the same peripheral: success.
  6. Mid-connect: D requests a connect and drops its API socket before
     the result; the slot must come back free and C can reconnect.

Usage:
    pip install aioesphomeapi
    python3 scripts/test_link_ownership.py <proxy-ip> <ble-mac> [--addr-type N]

Exit code 0 = all steps passed.
"""

from __future__ import annotations

import argparse
import asyncio
import sys

from aioesphomeapi import APIClient


def mac_to_int(mac: str) -> int:
    return int(mac.replace(":", "").replace("-", ""), 16)


class Peer:
    """One API client plus the connection-state events it received."""

    def __init__(self, name: str, ip: str) -> None:
        self.name = name
        self.api = APIClient(ip, 6053, password="")
        self.events: list[tuple[bool, int, int]] = []
        self.event = asyncio.Event()
        self.free: tuple[int, int, list[int]] | None = None
        self.free_event = asyncio.Event()
        self.flags = 0

    async def open(self) -> None:
        await self.api.connect(login=True)
        self.flags = (await self.api.device_info()).bluetooth_proxy_feature_flags

    def on_state(self, connected: bool, mtu: int, error: int) -> None:
        print(f"    [{self.name}] state connected={connected} mtu={mtu} error={error}")
        self.events.append((connected, mtu, error))
        self.event.set()

    def on_free(self, free: int, limit: int, allocated: list[int] | None = None) -> None:
        self.free = (free, limit, list(allocated or []))
        self.free_event.set()

    async def connect(self, addr: int, addr_type: int, timeout: float) -> None:
        self.event.clear()
        try:
            await self.api.bluetooth_device_connect(
                addr, on_bluetooth_connection_state=self.on_state,
                timeout=timeout, feature_flags=self.flags, has_cache=False,
                address_type=addr_type)
        except Exception as e:  # noqa: BLE001 - report and let the caller check events
            print(f"    [{self.name}] connect raised {type(e).__name__}: {e}")

    async def wait_event(self, timeout: float) -> tuple[bool, int, int] | None:
        try:
            await asyncio.wait_for(self.event.wait(), timeout)
        except asyncio.TimeoutError:
            return None
        return self.events[-1]


async def wait_free(peer: Peer, pred, timeout: float) -> bool:
    deadline = asyncio.get_running_loop().time() + timeout
    while True:
        if peer.free is not None and pred(peer.free):
            return True
        left = deadline - asyncio.get_running_loop().time()
        if left <= 0:
            return False
        peer.free_event.clear()
        try:
            await asyncio.wait_for(peer.free_event.wait(), left)
        except asyncio.TimeoutError:
            return False


async def run(ip: str, mac: str, addr_type: int, timeout: float) -> int:
    addr = mac_to_int(mac)
    fails = 0

    def check(ok: bool, what: str) -> None:
        nonlocal fails
        print(f"[{'PASS' if ok else 'FAIL'}] {what}")
        fails += 0 if ok else 1

    a, b = Peer("A", ip), Peer("B", ip)
    await a.open()
    await b.open()
    b.api.subscribe_bluetooth_connections_free(b.on_free)

    await a.connect(addr, addr_type, timeout)
    ev = await a.wait_event(timeout + 2)
    check(ev is not None and ev[0], "1. A connects")
    if not (ev and ev[0]):
        return 1

    n_a = len(a.events)
    await b.connect(addr, addr_type, timeout)
    ev = await b.wait_event(5)
    check(ev is not None and not ev[0] and ev[2] == -100, "2. B's CONNECT to A's address -> -100")
    await asyncio.sleep(2)
    check(len(a.events) == n_a, "2. A saw no state event from B's attempt")

    await b.api.bluetooth_device_disconnect(addr)
    await asyncio.sleep(2)
    check(len(a.events) == n_a, "3. B's DISCONNECT did not touch A's link")
    check(await wait_free(b, lambda f: addr in f[2], 3), "3. address still allocated")

    await a.api.disconnect(force=True)  # abrupt, like a killed add-on
    check(await wait_free(b, lambda f: addr not in f[2], 10),
          "4. A gone -> B sees the slot freed (A's link dropped)")

    c = Peer("C", ip)
    await c.open()
    ok = False
    for _ in range(3):  # the peer may need a moment to advertise again
        await c.connect(addr, addr_type, timeout)
        ev = await c.wait_event(timeout + 2)
        if ev and ev[0]:
            ok = True
            break
        await asyncio.sleep(2)
    check(ok, "5. reconnecting client C connects")
    if ok:
        await c.api.bluetooth_device_disconnect(addr)
        await c.wait_event(5)
    await c.api.disconnect(force=True)
    check(await wait_free(b, lambda f: addr not in f[2], 10), "5. slot free after C leaves")

    d = Peer("D", ip)
    await d.open()
    d.event.clear()
    task = asyncio.create_task(d.connect(addr, addr_type, timeout))
    await asyncio.sleep(0.3)  # connect is in flight
    await d.api.disconnect(force=True)
    task.cancel()
    check(await wait_free(b, lambda f: addr not in f[2], timeout + 5),
          "6. client gone mid-connect -> slot freed")

    e = Peer("E", ip)
    await e.open()
    await e.connect(addr, addr_type, timeout)
    ev = await e.wait_event(timeout + 2)
    check(ev is not None and ev[0], "6. a new client connects afterwards")
    if ev and ev[0]:
        await e.api.bluetooth_device_disconnect(addr)
        await e.wait_event(5)
    await e.api.disconnect()
    await b.api.disconnect()
    print(f"{'ALL PASSED' if fails == 0 else f'{fails} FAILED'}")
    return 0 if fails == 0 else 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("proxy_ip")
    p.add_argument("mac", help="BLE peripheral MAC (AA:BB:CC:DD:EE:FF)")
    p.add_argument("--addr-type", type=int, default=0)
    p.add_argument("--timeout", type=float, default=10.0)
    args = p.parse_args()
    return asyncio.run(run(args.proxy_ip, args.mac, args.addr_type, args.timeout))


if __name__ == "__main__":
    sys.exit(main())
