#!/usr/bin/env python3
"""Improv-over-BLE end-to-end check against a device in provisioning mode.

Uses py-improv-ble-client, the library behind Home Assistant's improv_ble
integration:

    python3 -m venv /tmp/iv && /tmp/iv/bin/pip install py-improv-ble-client bleak
    /tmp/iv/bin/python scripts/improv_provision.py                 # find, info, scan
    /tmp/iv/bin/python scripts/improv_provision.py --ssid X --psk Y # also provision
    /tmp/iv/bin/python scripts/improv_provision.py --ssid X --psk wrongpass --expect-fail

--name picks a device by advertised name when several are in range. The
password is never printed.
"""

import argparse
import asyncio
import sys

from bleak import BleakScanner
from improv_ble_client import ImprovBLEClient, ImprovServiceData, SERVICE_DATA_UUID, device_filter
from improv_ble_client.errors import ProvisioningFailed


async def find(name, timeout):
    found = {}

    def cb(dev, adv):
        if device_filter(adv) and (not name or (adv.local_name or dev.name or "") == name):
            found.setdefault(dev.address, (dev, adv))

    async with BleakScanner(cb):
        for _ in range(int(timeout * 10)):
            if found:
                break
            await asyncio.sleep(0.1)
    return next(iter(found.values()), None)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", help="advertised device name")
    ap.add_argument("--ssid")
    ap.add_argument("--psk", default="")
    ap.add_argument("--expect-fail", action="store_true",
                    help="provisioning must end in UNABLE_TO_CONNECT")
    ap.add_argument("--timeout", type=float, default=20)
    a = ap.parse_args()

    hit = await find(a.name, a.timeout)
    if not hit:
        print("FAIL no Improv device in provisioning mode found")
        return 1
    dev, adv = hit
    sd = ImprovServiceData.from_bytes(adv.service_data[SERVICE_DATA_UUID])
    print("found %s (%s) rssi %s state %s caps %r" % (
        adv.local_name or dev.name, dev.address, adv.rssi, sd.state.name, sd.capabilities))

    c = ImprovBLEClient(dev, adv)
    rc = 0
    try:
        print("need authorization:", await c.need_authorization())
        if c.can_get_device_info:
            i = await c.get_device_info()
            print("device info:", i.firmware_name, i.firmware_version, i.hardware_chip, i.device_name)
        if c.can_scan_wifi:
            nets = (await c.scan_wifi()).networks
            print("networks (%d):" % len(nets))
            for ssid, rssi, auth in nets:
                print("  %-32s %5s %s" % (ssid.decode(errors="replace"), rssi.decode(), auth.decode()))
        if a.ssid:
            try:
                url = await c.provision(a.ssid, a.psk, lambda s: print("state:", s.name))
                print("provisioned, next url:", url)
                rc = 1 if a.expect_fail else 0
            except ProvisioningFailed as exc:
                print("provisioning failed:", exc)
                rc = 0 if a.expect_fail else 1
    finally:
        # The client has no public disconnect (it drops the link after 90 s
        # idle); close the bleak link directly.
        bc = getattr(c, "_client", None)
        if bc is not None and bc.is_connected:
            await bc.disconnect()
    print("PASS" if rc == 0 else "FAIL")
    return rc


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
