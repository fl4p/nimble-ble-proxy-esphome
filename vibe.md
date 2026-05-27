I want to build a standalone ESP32-S3 firmware that speaks the aioesphomeapi protocol over plaintext TCP so unmodified
Home Assistant treats it as a regular ESPHome Bluetooth proxy, but with NimBLE as the BLE backend instead of Bluedroid (
which ESPHome uses). Scope is BLE proxy only — no sensors, switches, OTA, or other ESPHome features — implementing just
the minimum protocol surface (Hello/Connect/Ping/DeviceInfo/ListEntitiesDone plus the ~15 Bluetooth* messages) and the
basic GATT operations: scan/advertise, connect, discover services, read/write characteristics, and notifications.


# mirror
I'd like to add (under a build flag gate) a BLE clone/mirror/relay:
take a look at /Users/fab/dev/pv/micropython-blebms/clone.py

* a given device is cloned, all characteristics, advertisements 
* acts as a pass-through proxy, connections can be multiplexed (similar to bleak, where multiple processes/clients can
  simultaneously connect to the same peripheral)
* still lets the esphome proxy work
* keep it simple, no need to implement every GATT feature, just the essentials

Give an estimate on code size an impact on flash/mem usage.



write a BLE services that can server the website code/endpoints.
Write a static html page with a BLE device selector, that connects and fills an iframe or such with the HTML (something like HTTP-over-BLE)


ok lets make it more systematic. you keep a browser window open on http://192.168.1.231/, 
so it will constantly fetch the http server. set BLE debug to DEBUG.
Fix errors first, then dig into warnings.
If the device becomes silent on the serial port, try to send reset sequence with esptool on both serial ports /dev/cu.usbmodem*.

If no more issues, try the clone feature with SmartShunt (PSK PIN is 123456). Make sure it is still cloning, because cloning disables on failed boots.
Then watch out of errors and warnings again.
You are on full autopilot. 