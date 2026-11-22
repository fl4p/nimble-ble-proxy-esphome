#!/usr/bin/env python3
"""Cross-check components/improv_ble/improv_proto against py-improv-ble-client.

The client library (what Home Assistant's improv_ble integration uses) encodes
the RPCs, the firmware's protocol code decodes them, and the client parses
the results the firmware encodes. Runs on the host, no device needed:

    python3 -m venv /tmp/iv && /tmp/iv/bin/pip install py-improv-ble-client
    /tmp/iv/bin/python scripts/test_improv_client.py

For a device: scripts/improv_provision.py.
"""

import os
import subprocess
import sys
import tempfile

from improv_ble_client import protocol as prot

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMP = os.path.join(ROOT, "components", "improv_ble")

fails = 0


def check(ok, what):
    global fails
    fails += 0 if ok else 1
    print(("PASS " if ok else "FAIL ") + what)


def build():
    exe = os.path.join(tempfile.mkdtemp(), "improv_pipe")
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", COMP,
                    os.path.join(COMP, "improv_proto.cpp"),
                    os.path.join(COMP, "host_test", "test_improv_proto.cpp"), "-o", exe],
                   check=True)
    return exe


class Pipe:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe, "--pipe"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)

    def ask(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().split()


def hx(b):
    return b.hex() if b else "-"


def main():
    pipe = Pipe(build())
    ms = 0
    cases = [("MyWirelessAP", "mysecurepassword"), ("open-net", ""),
             ("x" * 32, "p" * 63), ("Café ☕", "pässwörd!"), ("a", "0123456789abcdef" * 4)]
    for ssid, psk in cases:
        data = prot.WiFiSettingsCmd(ssid.encode(), psk.encode()).as_bytes()
        # One write, and 20-byte writes as on a 23-byte ATT MTU.
        for size in (len(data), 20):
            ms += 10000  # past the assembly timeout: every case starts clean
            out = None
            for i in range(0, len(data), size):
                out = pipe.ask("F %s %d" % (data[i:i + size].hex(), ms))
                ms += 5
            ok = (out[0] == "PKT" and bytes.fromhex(out[1]) == data and out[2] == "WIFI"
                  and bytes.fromhex(out[3]) == ssid.encode()
                  and (out[4] == "-" and not psk or bytes.fromhex(out[4]) == psk.encode()))
            check(ok, "WiFiSettingsCmd %r/%d-char psk in %d-byte writes" % (ssid, len(psk), size))

    for cmd in (prot.DeviceInfoCmd(), prot.ScanWifiCmd(), prot.IdentifyCmd()):
        ms += 10000
        data = cmd.as_bytes()
        out = pipe.ask("F %s %d" % (data.hex(), ms))
        check(out[0] == "PKT" and bytes.fromhex(out[1]) == data, "%s decodes" % type(cmd).__name__)

    def result(cmd, cap, strings):
        out = pipe.ask("R %d %d %s" % (cmd, cap, " ".join(hx(s.encode()) for s in strings)))
        return bytes.fromhex(out[1]), int(out[2])

    raw, _ = result(1, 244, ["http://192.168.1.77/"])
    res = prot.parse_result(raw)
    check(isinstance(res, prot.WiFiSettingsRes) and res.redirect_url == b"http://192.168.1.77/",
          "WiFiSettingsRes with URL parses")
    raw, _ = result(1, 244, [])
    res = prot.parse_result(raw)
    check(isinstance(res, prot.WiFiSettingsRes) and res.redirect_url is None,
          "WiFiSettingsRes without URL parses")
    info = ["nimble-ble-proxy", "0.2.8", "esp32s3", "nimble-proxy"]
    raw, n = result(3, 244, info)
    res = prot.parse_result(raw)
    check(n == 4 and isinstance(res, prot.DeviceInfoRes) and res.firmware_name == b"nimble-ble-proxy"
          and res.device_name == b"nimble-proxy", "DeviceInfoRes parses")
    scan = ["HomeNet", "-48", "WPA2", "Guest", "-71", "NO", "Office", "-80", "WPA2/WPA3"]
    raw, n = result(4, 244, scan)
    res = prot.parse_result(raw)
    nets = getattr(res, "networks", None)
    check(n == 9 and isinstance(res, prot.ScanWifiRes) and nets is not None and len(nets) == 3,
          "ScanWifiRes parses (%s)" % nets)
    # On a 23-byte MTU a result is capped at 20 bytes. A device info cut to
    # fewer than 4 strings is invalid for the client, so the firmware answers
    # with an error instead (improv_ble.cpp send_device_info); scan results
    # keep whole triples and stay valid.
    raw, n = result(3, 20, info)
    try:
        prot.parse_result(raw)
        check(False, "capped device info would be rejected by the client")
    except Exception:  # noqa: BLE001
        check(n < 4, "capped device info is rejected by the client (firmware sends an error)")
    raw, n = result(4, 20, scan[:3] + ["Other", "-50", "WPA2"])
    try:
        res = prot.parse_result(raw)
        check(len(raw) <= 20 and n == 3 and len(res.networks) == 1,
              "scan result capped to a 23-byte MTU keeps whole triples and parses")
    except Exception as exc:  # noqa: BLE001
        check(False, "capped scan result parses: %s" % exc)

    out = pipe.ask("S 2 6")
    sd = prot.ImprovServiceData.from_bytes(bytes.fromhex(out[1]))
    check(sd.state == prot.State.AUTHORIZED and sd.capabilities ==
          prot.Capabilities.DEVICE_INFO | prot.Capabilities.SCAN_WIFI, "service data parses")

    print("ALL PASSED" if not fails else "FAILED (%d)" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
