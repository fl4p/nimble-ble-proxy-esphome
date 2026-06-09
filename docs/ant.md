
Our proxy (at http://192.168.1.231/) still doesn't connect to ANT-BMS.

We have shown that we can connect to ANT-BMS with:
* Micropython (aiobl): working
* MacOS: working  (trough bleak): working.
* Pi 5 BlueZ (trough bleak): working.
  *  /Users/fab/dev/ha/esphome/esphome (Bluedroid): not working (auth err, 133 or similar)

Then there is this popular library that captures the BMS data and wires it to esphome:
https://github.com/syssi/esphome-ant-bms/tree/main/components/ant_bms_ble
This works on the ESP32(s3), TODO: which stack/backend?.

See [FINDINGS.md](../FINDINGS.md) and [ANT-BMS-BLE.md](../../home-assistant-addons/batmon-ha/doc/ANT-BMS-BLE.md)


Have a closer look at syssi/esphome-ant-bms , what do they do different there than our proxy does?
