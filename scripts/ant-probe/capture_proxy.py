#!/usr/bin/env python3
"""Capture a full ANT-BMS bring-up session against the nimble-ble-proxy.

Sequence:
  1. POST /trace?on=1 to silence scanner + reset log_seq
  2. Stream /log?since=N to a file in the background
  3. Drive the proxy via aioesphomeapi:
       connect -> get_services -> start_notify(FFE1) -> write device-info query
  4. Sleep the capture window so post-CCCD silence is recorded
  5. Disconnect, POST /trace?on=0
  6. Report notify_rx counter delta from /stats.json

Usage (defaults match the dev setup):
    pip install aioesphomeapi
    python3 capture_proxy.py [--proxy 192.168.1.231] [--mac 20:A1:11:02:23:45]
                             [--wait 30] [--out proxy-trace.log]
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
import time
import urllib.error
import urllib.request

from aioesphomeapi import APIClient

ANT_SVC_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb"
ANT_CHR_UUID = "0000ffe1-0000-1000-8000-00805f9b34fb"

# Device-info query that aioble + esphome-ant-bms both use:
#   0x7e 0xa1 0x02 0x6c 0x02 0x20 0x58 0xc4 0xaa 0x55
DEVICE_INFO_QUERY = bytes.fromhex("7ea1026c022058c4aa55")


def mac_to_int(mac: str) -> int:
    return int(mac.replace(":", "").replace("-", ""), 16)


def http_post(url: str) -> bytes:
    req = urllib.request.Request(url, method="POST")
    with urllib.request.urlopen(req, timeout=5) as r:
        return r.read()


def http_get(url: str) -> tuple[bytes, dict]:
    with urllib.request.urlopen(url, timeout=5) as r:
        return r.read(), dict(r.headers)


async def stream_log(proxy_ip: str, out_path: str, stop: asyncio.Event) -> int:
    """Poll /log?since=N continuously, append to out_path, return final seq."""
    since = 0
    with open(out_path, "wb") as f:
        while not stop.is_set():
            try:
                body, hdrs = http_get(f"http://{proxy_ip}/log?since={since}")
                if body:
                    f.write(body)
                    f.flush()
                new_seq = int(hdrs.get("X-Log-Seq", since))
                since = new_seq
            except (urllib.error.URLError, ConnectionError, TimeoutError) as e:
                f.write(f"\n[stream_log] {e!r}\n".encode())
                f.flush()
            try:
                await asyncio.wait_for(stop.wait(), timeout=0.3)
            except asyncio.TimeoutError:
                pass
    return since


CCCD_UUID = "00002902-0000-1000-8000-00805f9b34fb"


def find_chr_handle(services, svc_uuid: str, chr_uuid: str) -> int | None:
    """Locate the characteristic handle in a get_services response."""
    for svc in services.services:
        if str(svc.uuid).lower() != svc_uuid.lower():
            continue
        for chr in svc.characteristics:
            if str(chr.uuid).lower() == chr_uuid.lower():
                return chr.handle
    return None


def find_cccd_handle(services, svc_uuid: str, chr_uuid: str) -> int | None:
    """Locate the CCCD descriptor handle for a characteristic."""
    for svc in services.services:
        if str(svc.uuid).lower() != svc_uuid.lower():
            continue
        for chr in svc.characteristics:
            if str(chr.uuid).lower() != chr_uuid.lower():
                continue
            for d in getattr(chr, "descriptors", []) or []:
                if str(d.uuid).lower() == CCCD_UUID:
                    return d.handle
    return None


async def run(args) -> int:
    proxy_url = f"http://{args.proxy}"
    addr_int = mac_to_int(args.mac)

    print(f"[+] /trace on={1}")
    print("    ", http_post(f"{proxy_url}/trace?on=1").decode())

    stop = asyncio.Event()
    log_task = asyncio.create_task(stream_log(args.proxy, args.out, stop))

    stats_before = json.loads(http_get(f"{proxy_url}/stats.json")[0])
    print(f"    notify_rx pre = {stats_before['notify_rx']}")

    client = APIClient(args.proxy, 6053, password="")
    try:
        print(f"[+] aioesphomeapi connect to {args.proxy}:6053")
        await client.connect(login=True)

        state: dict[str, object] = {"connected": False, "event": asyncio.Event()}

        def on_state(connected: bool, mtu: int, error: int) -> None:
            state["connected"] = connected
            state["mtu"] = mtu
            state["error"] = error
            state["event"].set()
            print(f"[+] gatt connect state: connected={connected} mtu={mtu} "
                  f"error={error}")

        print(f"[+] bluetooth_device_connect {args.mac}")
        cancel = await client.bluetooth_device_connect(
            addr_int,
            on_bluetooth_connection_state=on_state,
            timeout=12.0,
            feature_flags=0x27,  # PASSIVE_SCAN|ACTIVE|RAW|REMOTE_CACHING
            has_cache=False,
            address_type=0,
        )
        try:
            await asyncio.wait_for(state["event"].wait(), timeout=14.0)
        except asyncio.TimeoutError:
            print("[!] no connect event in 14s")
            cancel()
            return 2
        if not state["connected"]:
            print(f"[!] connect failed: error={state.get('error')}")
            return 3

        print("[+] get_services")
        services = await client.bluetooth_gatt_get_services(addr_int)
        print(f"    services: {len(services.services)}")
        for svc in services.services:
            chs = ", ".join(f"{c.uuid}@h{c.handle}" for c in svc.characteristics)
            print(f"     {svc.uuid}@h{svc.handle}: [{chs}]")

        handle = find_chr_handle(services, ANT_SVC_UUID, ANT_CHR_UUID)
        if handle is None:
            print(f"[!] FFE1 not found in services")
            return 4
        cccd_handle = find_cccd_handle(services, ANT_SVC_UUID, ANT_CHR_UUID)
        if cccd_handle is None:
            # Proxy's get_services doesn't always include descriptors.
            # FFE1 value is at handle 16 — its CCCD is at handle 17.
            cccd_handle = handle + 1
            print(f"[+] FFE1 handle={handle}; CCCD descriptor not in proto, "
                  f"defaulting to h={cccd_handle}")
        else:
            print(f"[+] FFE1 handle={handle}, CCCD handle={cccd_handle}")

        notify_count = 0

        def on_notify(handle: int, data: bytes) -> None:
            nonlocal notify_count
            notify_count += 1
            print(f"    notify[{notify_count}] h={handle} len={len(data)} "
                  f"hex={data.hex()}")

        # Our proxy's start_notify only registers the callback; the actual
        # CCCD write must come from a separate write_descriptor request
        # (matches what bleak-esphome does in HA's normal flow).
        print(f"[+] start_notify h={handle} (registers callback only)")
        await client.bluetooth_gatt_start_notify(
            addr_int, handle, on_notify
        )

        print(f"[+] write_descriptor CCCD h={cccd_handle} = 0x0001 "
              f"(enable notifications)")
        await client.bluetooth_gatt_write_descriptor(
            addr_int, cccd_handle, b"\x01\x00", wait_for_response=True
        )

        print(f"[+] write FFE1 device-info query "
              f"({DEVICE_INFO_QUERY.hex()})")
        await client.bluetooth_gatt_write(
            addr_int, handle, DEVICE_INFO_QUERY, response=False
        )

        print(f"[+] waiting {args.wait}s for notifies...")
        for i in range(args.wait):
            await asyncio.sleep(1)
            if i % 5 == 4:
                s = json.loads(http_get(f"{proxy_url}/stats.json")[0])
                print(f"    t={i+1}s notify_rx={s['notify_rx']} "
                      f"last_handle={s['last_notify_handle']} "
                      f"api_notifies={notify_count}")

        print("[+] disconnecting")
        await client.bluetooth_device_disconnect(addr_int)
        await asyncio.sleep(1)

    finally:
        try:
            await client.disconnect()
        except Exception:
            pass

        stop.set()
        try:
            final_seq = await asyncio.wait_for(log_task, timeout=3.0)
            print(f"[+] log capture done, final_seq={final_seq}")
        except asyncio.TimeoutError:
            print("[!] log task didn't stop in time")

        stats_after = json.loads(http_get(f"{proxy_url}/stats.json")[0])
        print(f"    notify_rx post = {stats_after['notify_rx']} "
              f"(delta {stats_after['notify_rx'] - stats_before['notify_rx']})")

        print("[+] /trace on=0")
        print("    ", http_post(f"{proxy_url}/trace?on=0").decode())

    print(f"[+] log saved to {args.out}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--proxy", default="192.168.1.231")
    p.add_argument("--mac", default="20:A1:11:02:23:45")
    p.add_argument("--wait", type=int, default=30,
                   help="seconds to wait for notifies after CCCD write")
    p.add_argument("--out", default="proxy-trace.log")
    args = p.parse_args()
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
