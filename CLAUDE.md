# nimble-ble-proxy — project notes

## Runtime / hardware gotchas

- **WiFi power-save causes instability — keep it OFF.** Running with
  `WIFI_PS_MAX_MODEM` (listen_interval > 0) plus esp_pm light sleep made
  the ESP32-S3 intermittently unreachable over HTTP/the api_server and
  *silenced the USB-Serial/JTAG console* in steady state (serial only
  emits during the post-reset boot window). Default is now power-save OFF
  (`DEFAULT_WIFI_LI = 0` in both `components/wifi_sta/wifi_sta.cpp` and
  `components/api_server/stats.cpp`, kept in sync). Opt back in per-device
  via `POST /wifips?li=N` (1..10) only if power draw matters more than
  responsiveness. `li=0` → `WIFI_PS_NONE`, which also keeps esp_pm from
  light-sleeping while WiFi is associated.

- **Debugging the device:** if serial is silent, the device is almost
  certainly in light sleep (power-save on) — disable it live with
  `POST /wifips?li=0` and `POST /cpufreq?mhz=240&ls=0`, or rely on the
  `/log` web-console endpoint while keeping the page polling. Config
  endpoints (`/wifips`, `/cpufreq`, `/txpower`, …) only apply via `POST`;
  a bare GET just reads.

- **NAT builds are heap- and coex-bound — don't over-provision NimBLE.**
  This S3 has *no PSRAM*, so WiFi (APSTA) + NimBLE + httpd + NAPT all share
  ~512 KB internal DRAM. Over-provisioning shows up as a WiFi-layer failure,
  not an obvious OOM: the SoftAP tears clients down in an 802.11w PMF
  **SA-Query → disassoc loop** (`wifi: … STA not responded to 6 SA Query
  attempts` + `reason = 209`, repeating), and HA's api_server connections
  fail with `xTaskCreate … failed`. The chain: `BT_NIMBLE_MAX_CONNECTIONS=9`
  reserved enough host+controller per-link buffers that no *contiguous* 8 KB
  block remained for an api_server connection task stack → HA reconnect storm
  → BLE central scan keeps re-arming → coex steals airtime on the single
  radio → beacon misses → client re-associates → SA-Query loop → every TCP
  conn (incl. the NAT'd client's MQTT/OTA) dies with `select() timeout`.
  Note PMF can't be turned off in IDF 5.5 (`wifi_pmf_config_t.capable` is
  deprecated/forced), so the lever is reducing re-assoc, i.e. the radio
  contention. Fix was sizing NimBLE to what's actually used — 1 clone
  upstream + ~3 HA links: `BT_NIMBLE_MAX_CONNECTIONS 9→4`,
  `BT_CTRL_BLE_MAX_ACT 10→6`, `BT_NIMBLE_ACL_BUF_COUNT 24→12` (freed ~13 KB;
  42→55 KB steady, SA-Query loop gone, port-forwards work). The connection
  count is coupled across **three** files — keep them in sync:
  `BT_NIMBLE_MAX_CONNECTIONS` (`sdkconfig.defaults`), `proxy::MAX_CONNECTIONS`
  (`include/proxy_config.h`, the slot count advertised to HA), and the nanopb
  `BluetoothConnectionsFreeResponse.allocated` cap
  (`components/api_proto/api_subset.options`). Diagnose live with
  `GET /stats.json` (`heap`) + `GET /log` (grep `SA Query` / `xTaskCreate`).

- **Feature opt-ins must live in `sdkconfig.defaults`, not menuconfig-only.**
  `sdkconfig` is gitignored/generated; `rm sdkconfig && idf.py reconfigure`
  (or a fresh checkout) regenerates it *purely* from `sdkconfig.defaults` and
  **silently drops** any flag that was only ever set via menuconfig — e.g. it
  once dropped `NBP_NAT_ROUTER` (compiling the whole NAT router + SoftAP out).
  `NBP_NAT_ROUTER`, `NBP_BLE_AUTO_OFF`, `NBP_WS_PROXY` are now pinned in
  `sdkconfig.defaults` for this reason. IDF saves the previous config as
  `sdkconfig.old` on regenerate — recover dropped values from there.

## There are multiple ESP32 boards on this LAN — verify which one you're on

- `mDNS nimble-proxy.local` resolves to **192.168.1.231** (MAC `94:A9:90:08:BA:3C`),
  which is NOT the board flashed over USB. The USB board (`/dev/cu.usbmodem*`,
  MAC `9C:13:9E:F4:04:98`) gets a different DHCP IP (e.g. 192.168.1.125).
  There's also `esp32-repeater.lan` (192.168.1.173). Flashing the USB board
  and then testing `nimble-proxy.local` silently tests the *wrong* device.
- **Before testing a flashed change, confirm the IP belongs to the board you
  flashed.** Quick checks: `arp -a | grep 9c:13:9e` to find the USB board's IP;
  or read `app_init: Compile time` from the serial boot log; or query the
  api_server over TCP (`:6053`, Hello+DeviceInfo) — `DeviceInfoResponse` carries
  the MAC and compile time. A stale compile time = you're on the wrong/old board.
- `idf.py flash`'s RTS hard-reset over USB-Serial/JTAG often does NOT reboot the
  app (it keeps running the old image). After flashing, reboot explicitly via
  `POST /reboot` (or an esptool reset) and re-check the compile time.

## BLE serves two independent roles — don't conflate them when "turning BLE off"

- **The web dashboard is reachable over BLE, not just WiFi.** `ble_httpd`
  (`CONFIG_NBP_BLE_HTTPD`) exposes an HTTP-style request/response transport over
  a NimBLE *peripheral* GATT service (advertises a dashboard UUID; `dispatch()`
  serves `/advitvl`, clone config, web-console, etc.). It's the out-of-band admin
  path **when WiFi is off/down** — i.e. BLE is a recovery channel, the same way a
  provisioning radio is. (WiFi provisioning fallback itself is SoftAP+web, see
  `NBP_AP_FALLBACK` in `wifi_sta.cpp` — a *separate* recovery path.)
- So BLE has two separable roles: **central/observer** (scan adverts → forward to
  HA; GATT client to proxied peers — gated by "API client subscribed" +
  "cloning active") and **peripheral** (ble_httpd dashboard + ble_clone mirrors —
  needed whenever WiFi is down or a BLE dashboard session is live).
- **Therefore any "power BLE completely off" logic must require BOTH roles idle:**
  no ESPHome API client AND cloning inactive AND WiFi connected AND no active/recent
  ble_httpd GATT session. The WiFi-connected condition is correct *because* of
  ble_httpd (WiFi up ⇒ dashboard reachable another way). A live ble_httpd dashboard
  connection is NOT an "API client" — gate on it separately or you'll cut off the
  Web Bluetooth UI. Prefer powering down just the (expensive, duty-cycled) central
  scan while leaving the cheap connectable advert up; full controller deinit only
  when the peripheral side is also idle. WiFi-down must reliably + quickly re-init
  BLE and re-`activate()` advertising (debounce flaps) or the device is unreachable
  during the very outage the BLE dashboard exists for.
- **Implemented** as `CONFIG_NBP_BLE_AUTO_OFF` (default n): a supervisor task
  in `ble_backend.cpp` (`power::init`, predicates injected from `main.cpp`)
  pauses the central scan when no API client + no GATT links + clone inactive,
  and auto-suspends advertising when WiFi is up + no central connected + clone
  inactive (composed with the user `/advitvl` master switch via
  `set_advertising_auto_suspend`). Idle→off waits `NBP_BLE_AUTO_OFF_IDLE_SECS`
  (default 30); re-activation is immediate. It does *radio quiesce*, not full
  `NimBLEDevice::deinit()` — deinit would tear down the one-shot clone/ble_httpd
  GATT DB and is deliberately avoided. `wifi_sta::is_connected()` is the WiFi gate.

## `CONFIG_NBP_BLE` — compile-time master gate (vs the runtime auto-off above)

- `NBP_BLE` (default y, `main/Kconfig.projbuild`) is the **compile-time**
  switch that builds BLE in or out entirely — distinct from
  `NBP_BLE_AUTO_OFF`, which only *quiesces the radio at runtime*. Every
  BLE option (`NBP_BLE_HTTPD`, `NBP_CLONE`, `NBP_SMP`, `NBP_BLE_AUTO_OFF`,
  and the `NBP_DEVICES_PANEL` table) nests in an `if NBP_BLE … endif`
  block. `NBP_BLE` `select`s `BT_ENABLED`; off ⇒ no scanner, no
  BluetoothProxy API, NimBLE stack excluded (~300 KB smaller, WiFi/NAT/
  dashboard-only). Keep at least one of `NBP_WIFI` / `NBP_BLE` on or the
  device has no remote transport.
- **`sdkconfig.defaults` must NOT pin `CONFIG_BT_ENABLED`** — the `select`
  is authoritative. The NimBLE host/role/tuning lines stay there but are
  inert when the BT_HOST choice is hidden (BT off).
- **`esp-nimble-cpp` is an *optional* dependency** in all four manifests
  (`api_server`, `ble_backend`, `ble_clone`, `ble_httpd`):
  `rules: - if: "$CONFIG{NBP_BLE} == True"`. Two gotchas: (1) gate on
  `NBP_BLE`, **not** `BT_NIMBLE_ENABLED` — the latter vanishes from the
  Kconfig model when BT is off and trips IDF's "missing kconfig after
  retry" fatal; (2) the component manager does **not** auto-add an
  *optional* dep to `REQUIRES`, so `ble_backend` and `api_server` link
  `nimble_lib` explicitly in their CMakeLists (guarded on `CONFIG_NBP_BLE`)
  the same way `ble_clone`/`ble_httpd` always have.
- To flip a previously-on `sdkconfig` to BLE-free you must clear **both**
  `CONFIG_NBP_BLE` and the stale `CONFIG_BT_ENABLED=y` (a `select` sets a
  symbol but can't un-set an existing `y` on reconfigure), then
  reconfigure. A fresh `rm sdkconfig && reconfigure` does it cleanly.
- **Known gap:** `web/index.html` still calls `/scan`, `/advitvl`,
  `/devices` and reads `adverts`/`bthome` — those endpoints/fields are
  absent in a BLE-free build, so the dashboard's device table + BLE
  controls 404/empty. Not yet gated client-side.

## DHCP client hostnames (NAT/SoftAP) — captured via a custom lwIP hook

- `/nat` lists connected SoftAP clients with MAC, leased IP, RSSI, and
  **DHCP hostname**. IDF 5.5's DHCP server parses past option 12 (Host Name)
  and discards it, and there's no public accessor — so we recover it with a
  custom lwIP hook rather than forking the server (a whole-file fork is
  *unworkable*: esp_netif's AP-DHCP wiring is gated by `ESP_DHCPS` ==
  `CONFIG_LWIP_DHCPS`, so disabling the built-in to substitute symbols just
  makes esp_netif skip starting any AP DHCP server).
- Mechanism: `components/dhcps_hostname` defines `LWIP_HOOK_DHCPS_POST_STATE`
  in `nbp_lwip_hooks.h`, injected into lwIP's `dhcpserver.c` via
  `ESP_IDF_LWIP_HOOK_FILENAME` (set on the lwip component in the **root
  `CMakeLists.txt`**, gated on `CONFIG_NBP_NAT_ROUTER`). The hook reads
  option 12 from each incoming packet and stores MAC→hostname in a small
  spinlock-guarded registry; `nat_router` reads it back via
  `dhcps_hostname_lookup()`. The implementation symbol is pulled into the
  link by `nat_router`'s `REQUIRES dhcps_hostname`.
- **IDF-version coupled.** The hook relies on `struct dhcps_msg` layout
  (`chaddr`, `options`) and the `LWIP_HOOK_DHCPS_POST_STATE` /
  `ESP_IDF_LWIP_HOOK_FILENAME` contract. On an IDF bump, re-verify the hook
  still fires: `nm build/.../dhcpserver.c.obj | grep nbp_dhcps_post_state`
  should show it as `U` (undefined → the hook is being called). IDF 6 has
  this natively (`CONFIG_LWIP_DHCPS_REPORT_CLIENT_HOSTNAME`) — prefer that
  if/when the project moves to IDF 6 and drop the hook.

## WebSocket bridge (NBP_WS_PROXY)

- Gated off by default. When on, `GET ws://<host>/api` tunnels the plaintext
  aioesphomeapi protocol to the local api_server over a 127.0.0.1:6053 loopback.
  A browser JS client must treat inbound WS data as a **continuous ESPHome byte
  stream** (device→browser is chunked at arbitrary boundaries; one WS frame ≠
  one ESPHome message).

## ESP-IDF workflow (for the agent)

- **Activate the IDF environment with `get_idf`** before any `idf.py`
  command (it's a shell alias for `. $HOME/dev/esp/idf5.5/export.sh` —
  use this, not the old `./idf-export`). Because it sources an absolute
  path it works from any directory — this repo or any other repo with
  the same ESP-IDF setup. Shell state doesn't persist between Bash
  calls, so chain it: `get_idf && idf.py build`.
- `idf.py build` and `idf.py flash` terminate and return — run them as
  normal Bash calls. No MCP server needed.
- `idf.py monitor` is the only blocking command (streams serial forever,
  never returns). Don't run it foreground and wait. **Instead use
  `scripts/serlog.py`** — a bounded pyserial reader that captures for a
  fixed window and returns. Use it any time you'd reach for `idf.py monitor`
  (including when the user says "use idf.py monitor"):
    - `scripts/serlog.py -s 10` — 10s capture, auto-picks the sole usbmodem port.
    - `scripts/serlog.py --reset --until "app_init"` — pulse reset, then stop
      as soon as the boot banner appears (good for checking compile time).
    - `scripts/serlog.py -p /dev/cu.usbmodem1201` — **required when several
      boards are plugged in**; bare auto-pick errors on multiple ports (pass
      `--auto` to override). See the multi-board warning above.
  It self-bootstraps pyserial from `~/.espressif/python_env/*` (homebrew
  python3 lacks it) and tolerates the S3 native-USB-CDC idle quirk where a
  readable port returns 0 bytes. Fall back to `timeout 10 idf.py monitor`
  only if you need IDF's panic/backtrace decoding.
- **Reset difference vs `idf.py monitor`:** `idf.py monitor` resets the chip
  on attach by default, so it always shows a fresh boot log. `serlog.py` does
  **not** reset unless you pass `--reset`; without it you capture only
  steady-state output — which can be empty if the device is in light sleep.
  So when you reach for monitor *to see boot/init output*, use
  `serlog.py --reset`. (Verified: the classic DTR/RTS sequence in `--reset`
  does reboot this board's native USB-Serial/JTAG peripheral.)
- **Prefer the `/log` web-console endpoint over serial monitor** for
  observing the device: it's pollable over HTTP, survives light sleep,
  and avoids USB port contention. Serial only emits in the post-reset
  boot window anyway (see power-save note above).
- Skip the ESP-IDF serial-monitor MCP server — it only wraps the blocking
  `monitor` case, which the options above already cover.
