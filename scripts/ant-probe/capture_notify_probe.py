#!/usr/bin/env python3
"""Generic notify-RX probe against the nimble-ble-proxy.

Connects to any peripheral via aioesphomeapi, finds a characteristic
matching a given UUID, subscribes to notifications + writes the CCCD
explicitly (matching bleak-esphome's flow), and waits to see whether
any notify_rx events reach the proxy's host-level counter. Used to
answer "does the proxy's notify-RX path work at all, on any
peripheral?".

Defaults target fugu-fry's Nordic UART Service RX char (notify
direction = peripheral->host).

Usage:
    ./venv/bin/python capture_notify_probe.py
        [--proxy 192.168.1.231] [--mac 70:04:1D:A6:AB:32]
        [--chr-uuid 6E400003-B5A3-F393-E0A9-E50E24DCCA9E]
        [--wait 30] [--out notify-probe.log]
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
import urllib.error
import urllib.request

from aioesphomeapi import APIClient


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
    since = 0
    with open(out_path, "wb") as f:
        while not stop.is_set():
            try:
                body, hdrs = http_get(f"http://{proxy_ip}/log?since={since}")
                if body:
                    f.write(body)
                    f.flush()
                since = int(hdrs.get("X-Log-Seq", since))
            except (urllib.error.URLError, ConnectionError, TimeoutError) as e:
                f.write(f"\n[stream_log] {e!r}\n".encode())
                f.flush()
            try:
                await asyncio.wait_for(stop.wait(), timeout=0.3)
            except asyncio.TimeoutError:
                pass
    return since


def find_chars_with_notify(services, target_uuid: str | None):
    """Return list of (svc_uuid, chr_uuid, chr_handle, cccd_handle?) for
    each characteristic that has the notify (0x10) or indicate (0x20)
    property bit set. If target_uuid is given, only return matches."""
    out = []
    NOTIFY_OR_INDICATE = 0x10 | 0x20
    for svc in services.services:
        for chr in svc.characteristics:
            if chr.properties & NOTIFY_OR_INDICATE == 0:
                continue
            if target_uuid is not None and str(chr.uuid).lower() != target_uuid.lower():
                continue
            cccd_handle = None
            for d in getattr(chr, "descriptors", []) or []:
                if str(d.uuid).lower().startswith("00002902"):
                    cccd_handle = d.handle
                    break
            if cccd_handle is None:
                cccd_handle = chr.handle + 1  # typical layout
            out.append((str(svc.uuid), str(chr.uuid), chr.handle, cccd_handle,
                        chr.properties))
    return out


async def run(args) -> int:
    proxy_url = f"http://{args.proxy}"
    addr_int = mac_to_int(args.mac)

    print(f"[+] /trace on=1")
    http_post(f"{proxy_url}/trace?on=1")

    stop = asyncio.Event()
    log_task = asyncio.create_task(stream_log(args.proxy, args.out, stop))

    stats_before = json.loads(http_get(f"{proxy_url}/stats.json")[0])
    print(f"    notify_rx pre = {stats_before['notify_rx']}")

    notify_handles: dict[int, int] = {}
    client = APIClient(args.proxy, 6053, password="")
    try:
        await client.connect(login=True)

        state = {"event": asyncio.Event(), "connected": False, "error": 0}

        def on_state(connected, mtu, error):
            state["connected"] = connected
            state["mtu"] = mtu
            state["error"] = error
            state["event"].set()
            print(f"[+] gatt state: connected={connected} mtu={mtu} error={error}")

        print(f"[+] connect {args.mac}")
        cancel = await client.bluetooth_device_connect(
            addr_int,
            on_bluetooth_connection_state=on_state,
            timeout=12.0,
            feature_flags=0x27,
            has_cache=False,
            address_type=0,
        )
        try:
            await asyncio.wait_for(state["event"].wait(), timeout=14.0)
        except asyncio.TimeoutError:
            print("[!] connect timeout")
            return 2
        if not state["connected"]:
            return 3

        print("[+] get_services")
        services = await client.bluetooth_gatt_get_services(addr_int)
        for svc in services.services:
            chs = ", ".join(
                f"{c.uuid}@h{c.handle} prop=0x{c.properties:02x}"
                for c in svc.characteristics
            )
            print(f"     {svc.uuid}@h{svc.handle}: [{chs}]")

        targets = find_chars_with_notify(services, args.chr_uuid)
        if not targets:
            print(f"[!] no characteristic matching {args.chr_uuid} with notify/indicate")
            return 4
        print(f"[+] {len(targets)} notify-able char(s) to subscribe:")
        for t in targets:
            print(f"     {t[1]} h={t[2]} cccd_h={t[3]} prop=0x{t[4]:02x}")

        notify_count = 0

        def on_notify(handle, data):
            nonlocal notify_count
            notify_count += 1
            notify_handles[handle] = notify_handles.get(handle, 0) + 1
            print(f"    notify[{notify_count}] h={handle} len={len(data)} "
                  f"hex={data[:20].hex()}{'…' if len(data) > 20 else ''}")

        for (_svc, _chr, chr_h, cccd_h, _prop) in targets:
            print(f"[+] start_notify h={chr_h}")
            await client.bluetooth_gatt_start_notify(addr_int, chr_h, on_notify)
            cccd_value = b"\x02\x00" if (_prop & 0x20) and not (_prop & 0x10) \
                else b"\x01\x00"
            print(f"[+] write_descriptor CCCD h={cccd_h} = {cccd_value.hex()}")
            try:
                await client.bluetooth_gatt_write_descriptor(
                    addr_int, cccd_h, cccd_value, wait_for_response=True
                )
            except Exception as e:
                print(f"    write_descriptor failed: {e}")

        if args.write_to_handle and args.write_bytes:
            wb = bytes.fromhex(args.write_bytes)
            print(f"[+] write h={args.write_to_handle} bytes={wb.hex()}")
            await client.bluetooth_gatt_write(
                addr_int, args.write_to_handle, wb, response=False
            )

        print(f"[+] waiting {args.wait}s for notifies...")
        for i in range(args.wait):
            await asyncio.sleep(1)
            if i % 5 == 4:
                s = json.loads(http_get(f"{proxy_url}/stats.json")[0])
                print(f"    t={i+1}s notify_rx={s['notify_rx']} "
                      f"last_handle={s['last_notify_handle']} "
                      f"api_notifies={notify_count}")

        print("[+] disconnect")
        await client.bluetooth_device_disconnect(addr_int)
        await asyncio.sleep(1)

    finally:
        try:
            await client.disconnect()
        except Exception:
            pass
        stop.set()
        try:
            await asyncio.wait_for(log_task, timeout=3.0)
        except asyncio.TimeoutError:
            pass
        stats_after = json.loads(http_get(f"{proxy_url}/stats.json")[0])
        print(f"    notify_rx post = {stats_after['notify_rx']} "
              f"(delta {stats_after['notify_rx'] - stats_before['notify_rx']})")
        if notify_handles:
            print(f"    api notifies per handle: {notify_handles}")
        http_post(f"{proxy_url}/trace?on=0")

    print(f"[+] log saved to {args.out}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--proxy", default="192.168.1.231")
    p.add_argument("--mac", default="70:04:1D:A6:AB:32")
    p.add_argument("--chr-uuid", default="6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
                   help="characteristic UUID to subscribe (default: NUS RX). "
                        "Pass 'all' to subscribe every notify-able char.")
    p.add_argument("--write-to-handle", type=int, default=None,
                   help="optional: after subscribe, write hex to this handle")
    p.add_argument("--write-bytes", type=str, default=None,
                   help="hex bytes to write (no spaces)")
    p.add_argument("--wait", type=int, default=30)
    p.add_argument("--out", default="notify-probe.log")
    args = p.parse_args()
    if args.chr_uuid.lower() == "all":
        args.chr_uuid = None
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
