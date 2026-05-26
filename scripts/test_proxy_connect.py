#!/usr/bin/env python3
"""Smoke-test the nimble-ble-proxy by driving it through aioesphomeapi.

Connects to the proxy over TCP (port 6053), watches BLE advertisements
for a few seconds, then asks the proxy to GATT-connect to the given
peripheral and prints the connection-state callbacks it sends back.

Usage:
    pip install aioesphomeapi
    python3 scripts/test_proxy_connect.py <proxy-ip> <ble-mac>

Example:
    python3 scripts/test_proxy_connect.py 192.168.1.42 AA:BB:CC:DD:EE:FF
"""

from __future__ import annotations

import argparse
import asyncio
import sys

from aioesphomeapi import APIClient, BluetoothProxyFeature


def mac_to_int(mac: str) -> int:
    return int(mac.replace(":", "").replace("-", ""), 16)


async def run(proxy_ip: str, mac: str, adv_window_s: float,
              connect_timeout_s: float) -> int:
    addr_int = mac_to_int(mac)
    client = APIClient(proxy_ip, 6053, password="")

    print(f"[+] connecting to proxy at {proxy_ip}:6053")
    await client.connect(login=True)

    info = await client.device_info()
    print(f"[+] device: name={info.name!r} esphome={info.esphome_version} "
          f"mac={info.mac_address}")
    feats = info.bluetooth_proxy_feature_flags
    print(f"[+] bluetooth_proxy_feature_flags=0x{feats:x}")
    if not (feats & BluetoothProxyFeature.ACTIVE_CONNECTIONS):
        print("[!] proxy doesn't advertise ACTIVE_CONNECTIONS — "
              "GATT connect will fail", file=sys.stderr)

    # mac -> (rssi, addr_type)
    seen: dict[str, tuple[int, int]] = {}
    target_advs: list[bytes] = []

    def note(addr_int: int, rssi: int, atype: int, data: bytes = b"") -> None:
        a = f"{addr_int:012X}"
        a_fmt = ":".join(a[i:i + 2] for i in range(0, 12, 2))
        if a_fmt not in seen:
            print(f"    adv {a_fmt} rssi={rssi} type={atype}")
        seen[a_fmt] = (rssi, atype)
        if a_fmt.upper() == mac.upper() and data:
            target_advs.append(data)

    use_raw = bool(feats & BluetoothProxyFeature.RAW_ADVERTISEMENTS)
    if use_raw:
        def on_raw(resp) -> None:
            for adv in resp.advertisements:
                note(adv.address, adv.rssi, adv.address_type, bytes(adv.data))
        print(f"[+] subscribing to RAW advertisements for {adv_window_s:.0f}s")
        unsub = client.subscribe_bluetooth_le_raw_advertisements(on_raw)
    else:
        def on_adv(adv) -> None:
            note(adv.address, getattr(adv, "rssi", 0),
                 getattr(adv, "address_type", 0))
        print(f"[+] subscribing to advertisements for {adv_window_s:.0f}s")
        unsub = client.subscribe_bluetooth_le_advertisements(on_adv)
    try:
        await asyncio.sleep(adv_window_s)
    finally:
        unsub()
    print(f"[+] saw {len(seen)} unique peripherals")
    if target_advs:
        print(f"[+] target adv samples ({len(target_advs)}):")
        for i, d in enumerate(target_advs[:3]):
            print(f"    [{i}] len={len(d)} {d.hex()}")
    tgt = seen.get(mac.upper())
    tgt_atype = 0
    if tgt is not None:
        tgt_rssi, tgt_atype = tgt
        print(f"[+] target {mac} was visible in scan, rssi={tgt_rssi} dBm, "
              f"addr_type={tgt_atype}")
    else:
        print(f"[!] target {mac} not seen during scan window "
              "— connect may still work if the device is advertising")

    state: dict[str, object] = {"connected": False, "mtu": 0, "error": 0,
                                "event": asyncio.Event()}

    def on_state(connected: bool, mtu: int, error: int) -> None:
        state["connected"] = connected
        state["mtu"] = mtu
        state["error"] = error
        state["event"].set()
        print(f"[+] connection state: connected={connected} mtu={mtu} "
              f"error={error}")

    print(f"[+] requesting GATT connect to {mac} ({addr_int:#014x}) "
          f"addr_type={tgt_atype}")
    cancel = await client.bluetooth_device_connect(
        addr_int,
        on_bluetooth_connection_state=on_state,
        timeout=connect_timeout_s,
        feature_flags=feats,
        has_cache=False,
        address_type=tgt_atype,
    )

    try:
        await asyncio.wait_for(state["event"].wait(),
                               timeout=connect_timeout_s + 2)
    except asyncio.TimeoutError:
        print(f"[!] no connection-state event within "
              f"{connect_timeout_s:.0f}s", file=sys.stderr)
        cancel()
        await client.disconnect()
        return 2

    ok = bool(state["connected"])
    if ok:
        print(f"[+] CONNECTED (mtu={state['mtu']}); disconnecting")
        await client.bluetooth_device_disconnect(addr_int)
    else:
        print(f"[!] connect FAILED (error={state['error']})",
              file=sys.stderr)

    await client.disconnect()
    return 0 if ok else 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("proxy_ip", help="IP of the nimble-ble-proxy")
    p.add_argument("mac", help="BLE peripheral MAC (AA:BB:CC:DD:EE:FF)")
    p.add_argument("--adv-window", type=float, default=4.0,
                   help="seconds to watch advertisements (default 4)")
    p.add_argument("--timeout", type=float, default=10.0,
                   help="GATT connect timeout (default 10)")
    args = p.parse_args()
    try:
        return asyncio.run(run(args.proxy_ip, args.mac,
                               args.adv_window, args.timeout))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
