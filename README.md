# nimble-ble-proxy

A standalone ESP32-S3 firmware that speaks the [aioesphomeapi](https://github.com/esphome/aioesphomeapi)
plaintext protocol so unmodified Home Assistant treats it as a regular ESPHome
Bluetooth proxy — but with **[NimBLE](https://github.com/h2zero/esp-nimble-cpp)**
as the BLE backend instead of Bluedroid.

## Why

Home Assistant's ESPHome integration is the easy path to extending Bluetooth
coverage: HA connects to an ESPHome device over native aioesphomeapi, asks it
to scan, and pipes raw BLE advertisements + GATT operations through the
device. ESPHome implements this on top of **Bluedroid**
(`CONFIG_BT_BLUEDROID_ENABLED`), which is heavier and less flexible for
raw-advertisement throughput than NimBLE — and locked into ESPHome's whole
YAML/codegen/runtime stack.

This firmware is the minimum needed to look like an ESPHome Bluetooth proxy to
HA, with none of the rest of ESPHome attached. The motivation is a lighter,
more responsive BLE bridge under direct control, with room to layer custom
scan-filtering, batching, or peripheral-specific logic later. To my knowledge
nothing else does this on an ESP32 directly — `aioesphomeserver` (Python,
alpha) and `bleak_esphome`/`habluetooth` (Python middleware) exist, but no
native firmware.

## What it does

Implements just enough of the aioesphomeapi plaintext protocol that HA's
client accepts the device as a Bluetooth proxy:

- **Handshake & housekeeping** — `Hello`, `Connect`, `Ping`, `DeviceInfo`,
  `ListEntities`/`ListEntitiesDone`, `SubscribeLogs` (silently accepted).
- **Bluetooth proxy surface** (~15 messages) —
  - Raw advertisement subscribe/unsubscribe + batched
    `BluetoothLERawAdvertisementsResponse` stream.
  - Connection-slot bookkeeping
    (`SubscribeBluetoothConnectionsFree` / `BluetoothConnectionsFreeResponse`).
  - GATT connect / disconnect with MTU exchange.
  - GATT service / characteristic / descriptor discovery (chunked).
  - GATT read / write / notify (with CCCD subscription).
- **Feature flags** advertised: `PASSIVE_SCAN | ACTIVE_CONNECTIONS | REMOTE_CACHING | RAW_ADVERTISEMENTS` (`0x27`).
- **mDNS** — announces `_esphomelib._tcp` with the TXT records HA's discovery
  flow expects (`platform=ESP32`, `network=wifi`, `mac=…`, `version=…`, etc.).
- **9 concurrent BLE connections**, up to 4 concurrent API clients (e.g. HA
  plus a CLI smoke test at the same time).
- **HTTP OTA** on port 80 — `POST /update` writes the inactive OTA slot via
  `esp_ota_*` and reboots into it.
- **/stats.json** dashboard piggybacks on the OTA listener — adverts/s, GATT
  read/write/notify counts, in-memory log ring.

**Out of scope** (by design): sensors, switches, lights, voice assistant,
Noise encryption, password auth (deprecated in ESPHome 2026.1), pairing,
bonding, cache management. HA never asks for these because the feature flags
don't advertise them.

## Architecture

```
main/                      app_main → wifi → mdns → ota → api_server → ble_backend
components/
├── api_proto/             nanopb-generated bindings for the api.proto subset
├── api_server/            plaintext frame codec, handshake, BT request handlers, /stats
├── ble_backend/           NimBLE wrappers — scanner, connection slots, GATT discovery
├── mdns_announce/         _esphomelib._tcp registration
├── ota/                   HTTP POST /update via esp_ota_*
└── wifi_sta/              STA bring-up from include/wifi_creds.h
include/
├── proxy_config.h         tunables (max_conn, scan timing, feature flags, …)
└── wifi_creds.h.example   SSID/PSK template — real file is gitignored
```

The `api_server` and `ble_backend` components are decoupled — `ble_backend`
publishes via a function-pointer seam (`publish::install`) so it doesn't
depend on the API server.

## Build

ESP-IDF (tested with 5.x). Component manager pulls
[`h2zero/esp-nimble-cpp ^2.5.0`](https://github.com/h2zero/esp-nimble-cpp)
and `nanopb`.

```sh
cp include/wifi_creds.h.example include/wifi_creds.h
# edit SSID / PSK
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

After the first serial flash, subsequent updates can go over WiFi:

```sh
curl --data-binary @build/nimble_ble_proxy.bin http://<device-ip>/update
```

Partitions: dual-OTA on 8 MB flash (`ota_0` + `ota_1`, no factory slot). If
both slots get bricked, re-flash via serial.

## Verify

Run [`scripts/test_proxy_connect.py`](scripts/test_proxy_connect.py) to drive
the device with `aioesphomeapi` directly — it logs in, fetches `device_info`,
subscribes to adverts, and prints the stream.

In HA, the proxy shows up under **Settings → Devices & services → Discovered**
as an ESPHome device. Confirm, and it starts serving as a Bluetooth scanner
allocation (visible via the `bluetooth/subscribe_connection_allocations`
WebSocket call).

## State

End-to-end-verified against live Home Assistant:

- mDNS discovery + handshake + DeviceInfo
- 220–240 raw adverts/s from 10+ unique peripherals
- Multi-client API server (HA + smoke test simultaneously)
- 9-slot scanner allocation registered in HA's bluetooth registry
- Routing wins against another Bluedroid proxy on the same LAN
- HTTP OTA round-trip

Bring-up notes, bugs hit, and resolutions are in [`FINDINGS.md`](FINDINGS.md).

## License

MIT.
