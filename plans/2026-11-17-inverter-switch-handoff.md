*this document is an LLM generated placeholder*

# Handoff: inverter switch, sensors, Modbus on .231 (2026-11-17)

## State
- .231 (nimble-proxy, Waveshare ESP32-S3-DEV-KIT-N8R8) runs 10a78d6 (PSRAM + switch_out), OTA'd 2026-11-16 15:36 UTC.
  ELF archived as `nimble-proxy-231` (ota 0.2.5).
- GPIO14 output is ON, boot policy `last`. Lock `nimble-proxy-231` released.
- Reach .231 from the Mac via havan only: `ssh -o HostKeyAlias=havan.local 100.64.0.10` (macOS Local Network privacy).
  Writes: `curl -X POST -H "X-NBP-Confirm: yes" "http://192.168.1.231/switch?on=0|1"`.
- Notify c4 (fugu-mppt-firmware-c4) before and after any .231 restart.

## Open
1. Inverter ext-switch port must be pulled UP to BAT+, so the low-side N-FET alone cannot switch it.
   Planned high-side stage:
   - Q2 TP2104N3 (TO-92 pins 1-2-3 = S-G-D, -40 V, Vgs ±20 V, 175 mA): S to BAT+, D to port.
   - R1 10k G-S; R2 10k (or 20k) from Q2 gate to Q1 drain.
   - Q1 TK6R7A10PL: S to GND, G to GPIO14, R3 10k gate pull-down.
   - Logic unchanged (GPIO high = inverter on). Not for 60 V (TP2104 is -40 V).
   - Diode-test first: red on D, black on S should read about 0.6-0.8 V. Front rocker switch must be off.
2. Re-test the switch with Fab watching the inverter. The 2026-11-16 load analysis was inconclusive, and HA has no inverter power data.
3. Choose the boot policy (`last` now; `off` is the safe default).
4. The restart at 2026-11-16 16:50:05 UTC is unexplained (earlier restarts were Fab's power cycles).
   - The `/log` ring overwrites the "reset reason" line within seconds.
   - Get it over USB serial (host unknown), or persist the reset reason and previous-boot tail in `/stats.json`.
5. Not deployed: env_sensors (a9f63ed). When the DFR0845 and sensors arrive:
   - OTA the PSRAM + modbus + sensors + switch build.
   - Run `scripts/modbus_bridge_test.py`.
   - Set up HA config.
6. Fan controller (low-side buck, 50 µH/10 A coil, 25 kHz) is waiting on Fab's answers:
   - what it cools
   - fan V/I
   - fail on or off
   - tach yes/no
   - HA or local control
7. Optional: force the RGB LED off at boot (needs its GPIO from Waveshare's pinout).
