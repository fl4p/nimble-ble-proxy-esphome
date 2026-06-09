#!/usr/bin/env python3
"""
Bounded serial reader for the ESP32-S3 over USB-Serial/JTAG.

Unlike `idf.py monitor` (which never returns and blocks the agent harness),
this reads for a fixed window and exits, printing whatever it captured. It
auto-detects the usbmodem port and can optionally pulse a reset first so you
catch the post-reset boot log (the only window serial emits in once WiFi
power-save / light sleep is on — see CLAUDE.md).

Self-bootstraps: homebrew python3 has no pyserial, so if `import serial`
fails we re-exec under an ESP-IDF env python (~/.espressif/python_env/*),
which ships pyserial 3.5.

Run:
  scripts/serlog.py                      # single port: auto; multiple: errors
  scripts/serlog.py --auto                # pick first port even if several exist
  scripts/serlog.py -s 20                 # 20s window
  scripts/serlog.py --reset               # pulse DTR/RTS, then capture boot log
  scripts/serlog.py --until "app_init"    # stop early when a line matches regex
  scripts/serlog.py -p /dev/cu.usbmodem1201 -b 115200

With several ESP32s plugged in, pass -p explicitly (see CLAUDE.md — picking
the wrong board silently tests stale firmware). --auto opts into guessing.
"""

from __future__ import annotations

import glob
import os
import re
import sys
import time

try:
    import serial  # type: ignore
except ModuleNotFoundError:
    # Re-exec under an ESP-IDF env python that has pyserial.
    cands = sorted(
        glob.glob(os.path.expanduser("~/.espressif/python_env/*/bin/python")),
        reverse=True,  # prefer newest idf/py
    )
    for py in cands:
        if os.path.realpath(py) != os.path.realpath(sys.executable):
            os.execv(py, [py, os.path.abspath(__file__), *sys.argv[1:]])
    sys.exit("pyserial not found and no ESP-IDF python_env available "
             "(pip install pyserial)")

import argparse


def autodetect_port(allow_multi: bool) -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no /dev/cu.usbmodem* port found (board not plugged in?)")
    if len(ports) > 1 and not allow_multi:
        sys.exit(f"multiple ports {ports}; pass -p <port> to choose, "
                 "or --auto to use the first")
    if len(ports) > 1:
        print(f"# --auto: multiple ports {ports}; using {ports[0]}",
              file=sys.stderr)
    return ports[0]


def main() -> int:
    ap = argparse.ArgumentParser(description="Bounded ESP32 serial reader.")
    ap.add_argument("-p", "--port",
                    help="serial port (default: the sole usbmodem port)")
    ap.add_argument("--auto", action="store_true",
                    help="if several ports exist, pick the first instead of erroring")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-s", "--seconds", type=float, default=10.0,
                    help="capture window (default 10)")
    ap.add_argument("--reset", action="store_true",
                    help="pulse DTR/RTS to reboot before capturing")
    ap.add_argument("--until", metavar="REGEX",
                    help="stop early once an output line matches REGEX")
    args = ap.parse_args()

    port = args.port or autodetect_port(args.auto)
    pat = re.compile(args.until) if args.until else None

    print(f"# {port} @ {args.baud} for {args.seconds:g}s"
          + (" (reset)" if args.reset else ""), file=sys.stderr)

    with serial.Serial(port, args.baud, timeout=0.2) as ser:
        if args.reset:
            # ESP boot strapping: classic esptool reset sequence.
            ser.dtr = False
            ser.rts = True
            time.sleep(0.1)
            ser.rts = False
            time.sleep(0.05)
            ser.reset_input_buffer()

        deadline = time.monotonic() + args.seconds
        buf = b""
        while time.monotonic() < deadline:
            try:
                chunk = ser.read(4096)
            except serial.SerialException:
                # S3 native USB-CDC quirk: select() flags the port readable
                # but read returns 0 bytes when the device is idle / in light
                # sleep (steady-state silence — see CLAUDE.md). Not fatal;
                # keep waiting out the window.
                time.sleep(0.2)
                continue
            if not chunk:
                continue
            buf += chunk
            *lines, buf = buf.split(b"\n")
            for ln in lines:
                text = ln.decode("utf-8", "replace").rstrip("\r")
                print(text, flush=True)
                if pat and pat.search(text):
                    return 0
        if buf:
            print(buf.decode("utf-8", "replace").rstrip("\r"), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
