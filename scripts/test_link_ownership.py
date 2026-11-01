#!/usr/bin/env python3
"""Two-client test of per-client BLE link ownership on a live proxy.

Plays the HA + batmon scenario with aioesphomeapi clients against a real
peripheral (one that stops advertising while linked, e.g. a BMS). B is a
bystander that stays connected throughout and watches ConnectionsFree.

  1. A connects to the peripheral.
  2. A re-sends CONNECT for its own link: answered connected=true (+MTU),
     and the link stays up (no connected=false).
  3. B's CONNECT to A's address gets error -100; A sees no state event.
  4. B's GATT read and get-services on A's link are rejected with a GATT
     error, and A sees nothing.
  5. B's DISCONNECT for A's address answers B with connected=false and
     leaves A's link up (still allocated).
  6. A vanishes abruptly (socket closed, like a restarted add-on). B sees
     the slot freed.
  7. C (the "restarted" client) connects to the same peripheral; then C
     too vanishes while holding it, and B sees the slot freed again.
  8. Mid-connect (BEST EFFORT): D requests a connect and drops its socket
     right after. The slot must be freed and E can connect afterwards.
     The script cannot tell from outside whether the GAP connect was
     still in flight or had already completed when the proxy released it
     (both paths must free the slot). It reports INCONCLUSIVE if it never
     saw the slot allocated. To confirm the mid-connect path, check the
     device log (/log) for "release <addr> (owner=N gone, state=1)" or
     "owner gone, terminating".

"Freed" always means: the address was seen ALLOCATED in a ConnectionsFree
update after the step began, and later absent. A slot that was never
allocated does not count as freed.

Usage:
    pip install aioesphomeapi
    python3 scripts/test_link_ownership.py <proxy-ip> <ble-mac> [--addr-type N]

Exit code 0 = all steps passed (INCONCLUSIVE counts as not passed).
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
        self.free_log: list[tuple[int, int, list[int]]] = []
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
        self.free_log.append((free, limit, list(allocated or [])))
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


async def wait_free_log(peer: Peer, pred, start: int, timeout: float) -> int | None:
    """Index of the first free_log entry >= start matching pred, or None."""
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    i = start
    while True:
        while i < len(peer.free_log):
            if pred(peer.free_log[i]):
                return i
            i += 1
        left = deadline - loop.time()
        if left <= 0:
            return None
        peer.free_event.clear()
        try:
            await asyncio.wait_for(peer.free_event.wait(), left)
        except asyncio.TimeoutError:
            return None


async def wait_freed(peer: Peer, addr: int, start: int, timeout: float) -> str:
    """'freed' | 'never-allocated' | 'still-allocated' since free_log[start]."""
    held = await wait_free_log(peer, lambda f: addr in f[2], start, timeout)
    if held is None:
        return "never-allocated"
    gone = await wait_free_log(peer, lambda f: addr not in f[2], held + 1, timeout)
    return "freed" if gone is not None else "still-allocated"


async def run(ip: str, mac: str, addr_type: int, timeout: float) -> int:
    addr = mac_to_int(mac)
    fails = 0

    def check(ok: bool, what: str) -> None:
        nonlocal fails
        print(f"[{'PASS' if ok else 'FAIL'}] {what}")
        fails += 0 if ok else 1

    def check_freed(res: str, what: str) -> None:
        nonlocal fails
        tag = {"freed": "PASS", "never-allocated": "INCONCLUSIVE"}.get(res, "FAIL")
        print(f"[{tag}] {what} ({res})")
        fails += 0 if res == "freed" else 1

    a, b = Peer("A", ip), Peer("B", ip)
    await a.open()
    await b.open()
    b.api.subscribe_bluetooth_connections_free(b.on_free)
    await wait_free_log(b, lambda f: True, 0, 3)  # initial snapshot

    # 1
    mark = len(b.free_log)
    await a.connect(addr, addr_type, timeout)
    ev = await a.wait_event(timeout + 2)
    check(ev is not None and ev[0], "1. A connects")
    if not (ev and ev[0]):
        return 1
    check(await wait_free_log(b, lambda f: addr in f[2], mark, 3) is not None,
          "1. B sees the address allocated")

    # 2
    n_a = len(a.events)
    await a.connect(addr, addr_type, timeout)
    ev = await a.wait_event(5)
    # aioesphomeapi keeps step 1's state callback registered and adds a second
    # one, so the proxy's single reply shows up here twice (verified in /log:
    # one "re-confirming" line). Only require that every new event is connected.
    check(ev is not None and ev[0] and len(a.events) > n_a,
          "2. A's re-CONNECT of its own link -> connected=true")
    await asyncio.sleep(1)
    check(all(e[0] for e in a.events[n_a:]), "2. no connected=false to A afterwards")

    # 3
    n_a = len(a.events)
    await b.connect(addr, addr_type, timeout)
    ev = await b.wait_event(5)
    check(ev is not None and not ev[0] and ev[2] == -100, "3. B's CONNECT to A's address -> -100")
    await asyncio.sleep(2)
    check(len(a.events) == n_a, "3. A saw no state event from B's attempt")

    # 4
    svcs = await a.api.bluetooth_gatt_get_services(addr)
    handles = [c.handle for s in svcs.services for c in s.characteristics]
    check(bool(handles), f"4. A discovered {len(handles)} characteristic handles")
    if handles:
        try:
            await b.api.bluetooth_gatt_read(addr, handles[0], timeout=5)
            check(False, "4. B's GATT read on A's link rejected (it succeeded!)")
        except Exception as e:  # noqa: BLE001
            check("GATT" in type(e).__name__ or "error=-1" in str(e),
                  f"4. B's GATT read on A's link rejected ({type(e).__name__})")
    try:
        await b.api.bluetooth_gatt_get_services(addr)
        check(False, "4. B's get-services on A's link rejected (it succeeded!)")
    except Exception as e:  # noqa: BLE001
        check(True, f"4. B's get-services on A's link rejected ({type(e).__name__})")
    await asyncio.sleep(1)
    check(len(a.events) == n_a, "4. A's link unaffected (no state event)")

    # 5
    await b.api.bluetooth_device_disconnect(addr)
    await asyncio.sleep(2)
    check(len(a.events) == n_a, "5. B's DISCONNECT did not touch A's link")
    last = b.free_log[-1] if b.free_log else (0, 0, [])
    check(addr in last[2], "5. address still allocated")

    # 6
    mark = max(0, len(b.free_log) - 1)  # the last snapshot shows it allocated
    await a.api.disconnect(force=True)  # abrupt, like a killed add-on
    check_freed(await wait_freed(b, addr, mark, 10), "6. A gone -> B sees the slot freed")

    # 7
    c = Peer("C", ip)
    await c.open()
    ok = False
    for _ in range(3):  # the peer may need a moment to advertise again
        mark = len(b.free_log)
        await c.connect(addr, addr_type, timeout)
        ev = await c.wait_event(timeout + 2)
        if ev and ev[0]:
            ok = True
            break
        await asyncio.sleep(2)
    check(ok, "7. reconnecting client C connects")
    if ok:
        await c.api.disconnect(force=True)  # leaves while holding the link
        check_freed(await wait_freed(b, addr, mark, 10),
                    "7. C gone while holding the link -> slot freed")
    else:
        await c.api.disconnect(force=True)
    await asyncio.sleep(2)  # let the peer start advertising again

    # 8 (best effort, see docstring)
    d = Peer("D", ip)
    await d.open()
    mark = len(b.free_log)
    task = asyncio.create_task(d.connect(addr, addr_type, timeout))
    await asyncio.sleep(0.05)  # CONNECT is on the wire; GAP connect takes longer
    await d.api.disconnect(force=True)
    task.cancel()
    check_freed(await wait_freed(b, addr, mark, timeout + 5),
                "8. client gone right after CONNECT -> slot freed")

    e = Peer("E", ip)
    await e.open()
    ok = False
    for _ in range(3):
        await e.connect(addr, addr_type, timeout)
        ev = await e.wait_event(timeout + 2)
        if ev and ev[0]:
            ok = True
            break
        await asyncio.sleep(2)
    check(ok, "8. a new client connects afterwards")
    if ok:
        await e.api.bluetooth_device_disconnect(addr)
        await e.wait_event(5)
    await e.api.disconnect()
    await b.api.disconnect()
    print(f"{'ALL PASSED' if fails == 0 else f'{fails} NOT PASSED'}")
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
