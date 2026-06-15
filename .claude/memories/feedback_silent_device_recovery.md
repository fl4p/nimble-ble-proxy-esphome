---
name: silent-device-recovery
description: "If the ESP32 goes silent on serial mid-session, send an esptool reset on both /dev/cu.usbmodem* ports to recover"
metadata: 
  type: feedback
---

When the serial monitor stops producing output mid-session (no `I (…)` lines arriving) and you suspect the device is stuck or crashed, try an esptool reset on **both** `/dev/cu.usbmodem*` ports — the board exposes two and either may be the one whose DTR/RTS lines drive the reset.

**Why:** The board has two USB endpoints — `/dev/cu.usbmodem12101` (USB-Serial-JTAG, the one that works for `idf.py flash`) and a second one with a long serial like `/dev/cu.usbmodem59720648061`. Hard resets via control-line toggles only land on one of them; trying both avoids guessing.

**How to apply:** Stop the monitor (it holds the port), then for each port run something like:

```bash
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX --before default_reset chip_id
```

(`chip_id` is a no-op other than `--before default_reset` which toggles the reset lines.) Restart the monitor afterwards.

**Caveats:** Don't do this if the port has multiple holders — check `lsof /dev/cu.usbmodem*` first.
