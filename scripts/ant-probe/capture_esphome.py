#!/usr/bin/env python3
"""Capture an esphome-ant-bms session via the ESPHome native API.

After the proxy is OTA-flashed with ant-probe.bin, this connects to the
running esphome firmware via aioesphomeapi, subscribes to logs, and
records everything to a file for the configured window.

Usage:
    ./venv/bin/python capture_esphome.py [--host ant-probe.local]
                                         [--wait 60] [--out esphome-trace.log]
"""

from __future__ import annotations

import argparse
import asyncio
import sys

from aioesphomeapi import APIClient, LogLevel


async def run(args) -> int:
    client = APIClient(args.host, 6053, password="")
    print(f"[+] aioesphomeapi connect to {args.host}:6053")
    await client.connect(login=True)

    info = await client.device_info()
    print(f"[+] device: name={info.name!r} esphome={info.esphome_version}")

    with open(args.out, "wb") as f:
        def on_log(msg):  # BluetoothLERawAdvertisementsResponse-like obj
            text = msg.message
            if isinstance(text, bytes):
                f.write(text + b"\n")
            else:
                f.write((text + "\n").encode("utf-8", errors="replace"))
            f.flush()

        print(f"[+] subscribe_logs (VERY_VERBOSE) for {args.wait}s -> {args.out}")
        client.subscribe_logs(on_log, log_level=LogLevel.LOG_LEVEL_VERY_VERBOSE,
                              dump_config=True)
        await asyncio.sleep(args.wait)

    await client.disconnect()
    print(f"[+] saved {args.out}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--host", default="ant-probe.local")
    p.add_argument("--wait", type=int, default=60)
    p.add_argument("--out", default="esphome-trace.log")
    args = p.parse_args()
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
