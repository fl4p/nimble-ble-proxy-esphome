#!/usr/bin/env python3
"""Direct CoreBluetooth (bleak) probe against fugu-flat.

Bypasses the nimble-ble-proxy entirely. Used to determine whether
fugu-flat actually pushes data over NUS — independent of the proxy's
notify-RX path. If this script prints notifications, fugu-flat works
and the proxy is broken. If this also prints nothing, fugu-flat may
be wedged.

Usage: ./venv/bin/python bleak_fugu.py [--mac 70:04:1D:A6:AB:32]
                                       [--cmd "uptime\\n"] [--wait 10]
"""

from __future__ import annotations

import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner

NUS_TX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # central -> peripheral
NUS_RX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # peripheral -> central (notify)


async def run(args) -> int:
    # CoreBluetooth uses UUIDs to identify peers, not MAC. Need to
    # discover by name or address. On macOS bleak maps a CoreBluetooth
    # UUID to the MAC after a fresh scan.
    print(f"[+] scanning ~5 s for fugu-flat / {args.mac}…")
    devices = await BleakScanner.discover(timeout=5.0, return_adv=True)
    target = None
    for d, adv in devices.values():
        # On macOS d.address is the CB UUID, not the MAC; bleak's
        # platform layer doesn't expose the MAC. Match by name only.
        name = d.name or adv.local_name or ""
        if name == "fugu-flat":
            target = d
            print(f"    found: name={name!r} cb_uuid={d.address} rssi={adv.rssi}")
            break
    if target is None:
        print("[!] fugu-flat not discovered in 5 s")
        return 2

    notifies: list[bytes] = []

    def on_notify(_sender, data: bytes) -> None:
        notifies.append(bytes(data))
        print(f"    notify({len(data)}): {data!r}  hex={data.hex()}")

    print(f"[+] connecting…")
    async with BleakClient(target) as client:
        print(f"    connected, mtu={client.mtu_size}")
        for svc in client.services:
            print(f"     svc {svc.uuid}")
            for ch in svc.characteristics:
                print(f"        chr {ch.uuid} props={ch.properties}")

        print(f"[+] start_notify on {NUS_RX}")
        await client.start_notify(NUS_RX, on_notify)

        if args.cmd:
            payload = args.cmd.encode("utf-8").decode("unicode_escape").encode("utf-8")
            print(f"[+] write to {NUS_TX}: {payload!r}")
            await client.write_gatt_char(NUS_TX, payload, response=False)

        print(f"[+] waiting {args.wait}s for notifies…")
        await asyncio.sleep(args.wait)

        await client.stop_notify(NUS_RX)

    print(f"[+] total notifies received: {len(notifies)}")
    return 0 if notifies else 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--mac", default="70:04:1D:A6:AB:32")
    p.add_argument("--cmd", default="uptime\\n",
                   help="optional payload to write to NUS TX before listening")
    p.add_argument("--wait", type=int, default=10)
    args = p.parse_args()
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
