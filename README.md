# nimble-ble-proxy

A standalone ESP32-S3 firmware that speaks the [aioesphomeapi](https://github.com/esphome/aioesphomeapi)
plaintext protocol so unmodified Home Assistant treats it as a regular ESPHome
Bluetooth proxy — but with **[NimBLE](https://github.com/h2zero/esp-nimble-cpp)**
as the BLE backend instead of Bluedroid.

I started this little project because the regular esphome proxy didn't connect
to my sketchy Bluetooth BMS, so I [vide-coded](docs/vibe.md) my own. This began as fun-project, and it
has become a more stable, complete and energy saving Wifi/Ble/Repeater/Bridge than other esp32 NAT routers I have used.
Specifically it is or can be:

* Esphome BLE proxy, you can use it outside of esphome env with their client libr (`TODO`)
* BLE cloner/repeater: connect to a device, copy all its ble services/characteristics and start advertising them and
  passthrough all reads/writes/notifies. Useful for ble range extension without wifi.
* BLE scanner: decodes BTHome enconded advertisement packages and looks up vendor detaisls
* SoftAP NAT router with port-mappings
* Many energy saving knobs to explore the power saving capabilities of an esp32/s3 chip
* Boilerplate for any coex WiFi/Bluetooth projects with web-interface

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
- **Optional clone mode** (`CONFIG_NBP_CLONE`) — mirror one BLE peripheral
  as a local GATT server so multiple centrals (HA + a vendor app) share one
  upstream link. Full spec in [`docs/clone.md`](docs/clone.md).
- **Web dashboard** on port 80 — `/` serves an embedded HTML page with two
  live charts (BLE activity, CPU + chip-temperature), a devices-seen table
  with one-click clone, a streaming log console, and tunables for CPU
  frequency, WiFi/BLE TX power, BLE scan duty, BLE advertising interval,
  WiFi power-save listen interval, device hostname, and SMP passkey. Full
  spec in [`docs/web-ui.md`](docs/web-ui.md).
- **BLE-side HTTP transport** (`CONFIG_NBP_BLE_HTTPD`) — same dashboard,
  same endpoints, reachable via a GATT request/response service when WiFi
  isn't built in.

**Out of scope** (by design): sensors, switches, lights, voice assistant,
Noise encryption, password auth (deprecated in ESPHome 2026.1), bonding
replication across clones, cache management. HA never asks for these
because the feature flags don't advertise them. SMP / static-passkey
pairing for paired upstream peripherals (e.g. Victron SmartShunt) is in
scope under `CONFIG_NBP_SMP`.

## Architecture

```
main/                      app_main; orders bring-up of every component
components/
├── api_proto/             nanopb-generated bindings for the api.proto subset
├── api_server/            plaintext frame codec, handshake, BT request handlers,
│                          dashboard endpoints (stats / log / level / txpower /
│                          cpufreq / scan / hostname / wifips / advitvl / trace
│                          / reboot / devices), embedded web/index.html
├── ble_backend/           NimBLE wrappers — scanner, connection slots, GATT
│                          discovery, adv interval state, SMP passkey holder
├── ble_clone/             optional (CONFIG_NBP_CLONE) — supervisor task +
│                          upstream client + local GATT mirror + /clone endpoint
├── ble_httpd/             optional (CONFIG_NBP_BLE_HTTPD) — GATT request/
│                          response service that routes the same dashboard
│                          endpoints over Web Bluetooth
├── mdns_announce/         _esphomelib._tcp registration
├── ota/                   HTTP POST /update via esp_ota_*; owns the shared
│                          httpd that the dashboard piggybacks on
└── wifi_sta/              STA bring-up from include/wifi_creds.h; reads
                           listen_interval from NVS for power save
include/
├── proxy_config.h         tunables (max_conn, scan timing, feature flags,
│                          runtime hostname accessor)
└── wifi_creds.h.example   SSID/PSK template — real file is gitignored
web/
└── index.html             dashboard single-source: HTTP fetch OR Web
                           Bluetooth transport, auto-detected at boot
```

The `api_server` and `ble_backend` components are decoupled — `ble_backend`
publishes via a function-pointer seam (`publish::install`) so it doesn't
depend on the API server. `ble_clone` and `ble_httpd` plug into the same
NimBLE singleton without a circular dep on `api_server`.

## Build profiles

These Kconfig switches under `nimble-ble-proxy` choose what gets compiled in:

| Kconfig                    | Default | What it adds                                                                                                                                                                                                                                                                                                                                                                    |
|----------------------------|---------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `CONFIG_NBP_WIFI`          | `y`     | STA + mDNS + dashboard httpd + aioesphomeapi server + embedded dashboard HTML                                                                                                                                                                                                                                                                                                   |
| `CONFIG_NBP_OTA`           | `y`     | `POST /update` HTTP OTA endpoint. Turn off for a lockdown build where firmware updates require serial.                                                                                                                                                                                                                                                                          |
| `CONFIG_NBP_AP_FALLBACK`   | `n`     | When STA can't associate within `CONFIG_NBP_AP_FALLBACK_SECS`, fall back to a SoftAP at `<hostname>-setup` (192.168.4.1) with a one-page form that POSTs credentials to NVS and reboots. Replaces edit-`wifi_creds.h`-and-reflash for first-time setup.                                                                                                                         |
| `CONFIG_NBP_NAT_ROUTER`    | `n`     | SoftAP + NAPT out the STA uplink, with `/nat` (status + AP creds) and `/portmap` (inbound forwards). `/nat` also lists connected clients — MAC, leased IP, RSSI, and DHCP hostname (captured via a custom lwIP hook; see `components/dhcps_hostname`). Shown in the dashboard NAT panel.                                                                                        |
| `CONFIG_NBP_BLE_HTTPD`     | `n`     | GATT request/response service (Web Bluetooth dashboard)                                                                                                                                                                                                                                                                                                                         |
| `CONFIG_NBP_CLONE`         | `n`     | Clone supervisor, upstream client, local GATT mirror, `/clone` endpoint                                                                                                                                                                                                                                                                                                         |
| `CONFIG_NBP_SMP`           | `n`     | Static-passkey pairing as central (paired upstream peripherals). Passkey is set via `POST /clone?passkey=N`                                                                                                                                                                                                                                                                     |
| `CONFIG_NBP_DEVICES_PANEL` | `y`     | 64-entry devices-seen table + `/devices` + dashboard table                                                                                                                                                                                                                                                                                                                      |
| `CONFIG_NBP_WEB_CONSOLE`   | `y`     | 64 KiB log ring + `/log` + on-page console                                                                                                                                                                                                                                                                                                                                      |
| `CONFIG_NBP_BLE_AUTO_OFF`  | `n`     | Supervisor task that quiesces the BLE radio when idle: pauses the central scan when no API client / GATT link / clone needs it, and suspends advertising when WiFi is up and no central/clone needs the peripheral (kept on when WiFi is down so the BLE dashboard stays reachable). Idle grace `CONFIG_NBP_BLE_AUTO_OFF_IDLE_SECS` (default 30 s); re-activation is immediate. |

Two common profiles:

- **WiFi build** (default `sdkconfig.defaults`) — full dashboard via HTTP,
  HA proxy active. ~1.18 MB.
- **BLE-only build** (`sdkconfig.defaults.bletest`) — `CONFIG_NBP_WIFI=n`,
  `CONFIG_NBP_BLE_HTTPD=y`. Same dashboard over Web Bluetooth. ~0.62 MB.

## Build

ESP-IDF 5.5 (5.x should work). Component manager pulls
[`h2zero/esp-nimble-cpp ^2.5.0`](https://github.com/h2zero/esp-nimble-cpp)
and `nanopb`. A small patch is auto-applied to the managed NimBLE-CPP
header by `components/api_server/CMakeLists.txt` (adds `setNotifyCallback`
so notify registration can skip the CCCD write — see clone pitfall 13.x).

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

Or, for a BLE-only build (no WiFi):

```sh
idf.py -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.bletest' fullclean build
```

Partitions: dual-OTA on 8 MB flash (`ota_0` + `ota_1`, no factory slot). If
both slots get bricked, re-flash via serial. A boot-guard
(`CONFIG_NBP_CLONE_BOOT_GUARD`) disables clone on the next boot if the
previous one panicked early, so a misconfigured clone target can't lock
the device into a reboot loop.

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
- Dashboard live in a phone browser (HTTP) and via a Web Bluetooth Chrome
  page (BLE transport) against the same firmware
- Clone-mode: ANT-BLE20PHUB BMS mirrored to batmon-ha (1:1 NOTIFY fan-out),
  Victron SmartShunt under SMP static-passkey pairing

Bring-up notes, bugs hit, and resolutions are in [`FINDINGS.md`](FINDINGS.md).
Deeper specs:

- [`docs/web-ui.md`](docs/web-ui.md) — dashboard, every HTTP/BLE endpoint,
  the boot order, and per-component cost breakdown.
- [`docs/clone.md`](docs/clone.md) — clone-mode architecture, NimBLE
  constraints discovered during bring-up, verification recipe.

## License

MIT.
