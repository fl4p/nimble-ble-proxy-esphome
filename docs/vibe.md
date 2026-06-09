# How to vibe

* this is 100% LLM-coded, didnt wrote a single line
* the trick: bring the LLM agent in the full loop (end2end)
* the better the full coverage, the better the results
* its obvious that LLM make mistakes, let them find them and adjust
* Let it write small tests for basic assumptions, then build on that
* Get a second chip and let the agent control it too, to be able to instantly test wifi connection
* then you can watch sipping your coffee
* API can be obscure and buggy, the LLM with struggle with the same problems as humans
* take your time to chat with the LLM, ask what the agent did, if it checked the documentation
* LLM have limited attention and they get biased
* My prompt before closing a topic was mostly: tests? docs? commits?
* Ask the LLM if the agent harness is compatible with our tooling, and let it write new tools or change existing to speedup
  iterations. E.g. `idf.py monitor` is made for humans and didn't work well inside the agent harness. I just told the agent to
  code their own. Otherwise you will see it trying idf.py monitor over and over again, noticing that it doesn't work as
  it expected.


# Some of my prompts

I captured some of my early prompts, because I thought I would need them later to build a specification or so.
Then I noticed, clean code is actual enough and far more expressive and complete than a specification. I could
turn the code at any time into a spec. In the end I spent most of the time watching the claude agent getting things
fixed.


```


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
Write a static html page with a BLE device selector, that connects and fills an iframe or such with the HTML (something
like HTTP-over-BLE)

ok lets make it more systematic. you keep a browser window open on http://192.168.1.231/,
so it will constantly fetch the http server. set BLE debug to DEBUG.
Fix errors first, then dig into warnings.
If the device becomes silent on the serial port, try to send reset sequence with esptool on both serial ports
/dev/cu.usbmodem*.

If no more issues, try the clone feature with SmartShunt (PSK PIN is 123456). Make sure it is still cloning, because
cloning disables on failed boots.
Then watch out of errors and warnings again.
You are on full autopilot.

/plan
under a Kconfig gate implement: the device is already scanning and receiving advertisements. create a new component that
uses https://bthome.io/ to parse all recognizable advertisement frames, and display them on the web  
page

#  

under a Kconfig gate add the following feature:
The http server accepts a websocket connection that wraps the esphome tcp protocol, so the bluetooth proxy service
can be used from a browser

# nat router

under a Kconfig gate add a simple WiFi Nat router
we are already connected to Wifi. now we just need to create an AP.
for reference you can use https://github.com/martin-ger/esp32_nat_router
and https://github.com/dchristl/esp32_nat_router_extended (i have a local copy.
Port mappings can be configured through the website.
create a new component for that.

```