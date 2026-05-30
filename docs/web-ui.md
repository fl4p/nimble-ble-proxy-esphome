# Web UI spec

The proxy ships a small web dashboard. The HTML/JS live in
`web/index.html` — a single file that is the source of truth for both
transports:

- **HTTP mode** (default `CONFIG_NBP_WIFI=y` build): the file is
  embedded via `EMBED_FILES` and served by `stats.cpp::root_get` on
  the OTA httpd at `http://<proxy>/`. All other endpoints below
  (`/stats.json`, `/log`, …) are siblings on the same listener — no
  second HTTP socket.
- **Web Bluetooth mode** (`CONFIG_NBP_BLE_HTTPD=y`, typically with
  `CONFIG_NBP_WIFI=n`): the same file is loaded off-device (open it
  as `file://`, or host it on a static URL) and talks to the device
  over a GATT request/response service. See *BLE transport* below.

The page auto-detects which transport to use at boot: it probes
`GET /level`; if that succeeds it stays in HTTP mode, otherwise it
falls back to BLE and shows a **Connect** button. The query flag
`?ble` forces BLE explicitly when the page is hosted by a server that
happens to answer `/level` with something unrelated.

All endpoints are unauthenticated on the assumption of a trusted LAN.

The layout is responsive: the page declares
`<meta name=viewport content="width=device-width,initial-scale=1">`, uses
`box-sizing: border-box` globally, lays out the control row with
`flex-wrap`, and sizes both uPlot canvases from each chart wrapper's live
`clientWidth` (clamped to 900). A media query at `max-width:640px` tightens
padding and font size for phones. Scrollbars and form controls are dark
to match the chart/console panels.

## Build-time gates

Six Kconfig flags shape what gets compiled in. Edit in
`idf.py menuconfig` → `nimble-ble-proxy`, or set in
`sdkconfig.defaults` / `sdkconfig.defaults.bletest`.

| Kconfig | Default | Cost when ON | What it gates |
|---|---|---|---|
| `CONFIG_NBP_WIFI` | `y` | ~500 KB flash (WiFi stack + httpd + mDNS) | STA bring-up, mDNS, aioesphomeapi server, the HTTP dashboard endpoints, embedded `web/index.html` + `web/favicon.svg` |
| `CONFIG_NBP_OTA` | `y` | code-gated; `app_update` dep always linked | `POST /update` HTTP OTA endpoint. The OTA flash code paths are `#if CONFIG_NBP_OTA` in `ota.cpp` so the linker GCs them when off; turn off for a lockdown build that requires serial flash. The shared httpd listener stays up either way for the dashboard. |
| `CONFIG_NBP_AP_FALLBACK` | `n` | ~15 KB flash, extra netif + httpd | SoftAP fallback when STA can't associate within `CONFIG_NBP_AP_FALLBACK_SECS`. Hosts a single page at `192.168.4.1/` with a form that POSTs `/wifi?ssid=…&psk=…` into NVS namespace `wifi` and reboots. Credential lookup at boot: NVS first, then `wifi_creds.h` if present, then AP fallback. Makes `wifi_creds.h` optional. |
| `CONFIG_NBP_BLE_HTTPD` | `n` | ~4 KB flash | GATT request/response service that lets Web Bluetooth talk to the same handlers |
| `CONFIG_NBP_CLONE` | `n` | ~20 KB flash, ~10-15 KB RAM when active | clone supervisor + local GATT mirror + `/clone` endpoint + dashboard clone button + synthetic clone-target row |
| `CONFIG_NBP_SMP` | `n` | ~2 KB flash | static-passkey pairing as central (paired upstream peripherals); `passkey` field on `/clone` |
| `CONFIG_NBP_DEVICES_PANEL` | `y` | ~12 KB BSS | scanner device table, `/devices`, dashboard table |
| `CONFIG_NBP_WEB_CONSOLE` | `y` | 64 KB BSS (NimBLE DEBUG) / 8 KB BSS | `esp_log_set_vprintf` hook, ring buffer, `/log`, on-page console |

A BLE-only build flips `NBP_WIFI=n` + `NBP_BLE_HTTPD=y` — see
`sdkconfig.defaults.bletest`. With `NBP_WIFI=n` the linker GCs the
entire HTTP handler set, so the BLE-only build is ~75 KB smaller than
the WiFi build even though both expose the same dashboard.

The rate chart, the system chart, the tunables panel and its
`/trace` / `/txpower` / `/cpufreq` / `/scan` / `/advitvl` / `/wifips`
/ `/hostname` endpoints are always compiled in — they cost <100 B BSS
combined.

## Endpoints

In HTTP mode the paths below are at `http://<proxy>/<path>` (port 80,
ESP-IDF httpd default). In Web Bluetooth mode the same `<method>
<path>?<query>` line is sent as a single GATT write (see *BLE
transport*); responses arrive on the notify characteristic. The
client-side `apiText` / `apiJson` helpers in `web/index.html` paper
over the difference.

### `GET /` — dashboard HTML

Single self-contained page sourced from `web/index.html`, embedded
into the WiFi build via `EMBED_FILES` in
`components/api_server/CMakeLists.txt` and served by `root_get`. The
embed is conditional on `CONFIG_NBP_WIFI` so BLE-only builds don't
carry the ~21 KB of HTML they can't serve. Pulls uPlot 1.6.31 CSS+JS
and ansi\_up v5 from jsdelivr.

Layout (top → bottom):

1. **Rate chart** — uPlot, width auto-fits up to 900 × 320, 120-sample
   rolling window (~2 min). Series: reads/s, writes/s, notifies/s,
   adverts/s, active connections, free heap (KB, right axis with explicit
   `size:52` so the labels aren't clipped, dashed). Per-second deltas
   computed client-side from cumulative counters returned by
   `/stats.json`. Negative deltas (counter reset after reboot) are clamped
   to `null` so uPlot draws a gap instead of a spike.
2. **System chart** — uPlot, width-matched to the rate chart, height 200.
   Per-core CPU% (`cpu0` red, `cpu1` orange) on a 0–100 left axis, plus a
   10-sample moving average of chip temperature (`temp °C`, cyan) on a
   separate right scale with `size:42`. The raw `s.temp_c` feeds the MA
   buffer but isn't itself drawn — the sensor jitter looks like noise.
3. **Devices table** *(if `CONFIG_NBP_DEVICES_PANEL`)* — every unique MAC
   the scanner has seen. Columns: MAC, name, RSSI, adv/s, total adverts,
   age. Sorted by total adverts descending. Rows whose `age > 10 s` get
   the `.stale` class (40 % opacity). Wrapped in `<div class=devwrap>`
   with `overflow-x:auto` so the table can scroll horizontally on phones
   without forcing the rest of the page wider.
4. **Tunables panel** — vertical stack of four labelled groups (a flex-
   column with one flex-wrap row per group, separated by hairline rules
   so the surface stays scannable as it grows):
   - **CPU** — `freq` (80 / 160 / 240 MHz), `light sleep` checkbox.
   - **WiFi** — `TX` (off / 8 / 11 / 14 / 17 / 20 / 21 dBm), `PS` (off /
     1×DTIM / 3 / 5 / 10).
   - **BLE** — `TX` (-12..+9 dBm in 3 dB steps, plus **`off`**), `adv`
     (default / 100 / 200 / 500 / 1000 / 2000 ms — annotated with the
     resulting rate), `scan` duty preset (50 % / 25 % / 10 % / 3 %),
     `active scan` checkbox, `log` (NimBLE log level 0–5). The `off`
     entry (`POST /txpower?ble=off`) **fully shuts BLE down** —
     `NimBLEDevice::deinit`, host + controller — not just a radio
     quiesce: the scanner/proxy, the BLE dashboard (`ble_httpd`) and any
     clone mirror all go away. It is runtime-only with **no live
     re-enable** (a reboot brings BLE back at the stored dBm), so the UI
     `confirm()`s first and reverts the dropdown on cancel. `/txpower`
     reports the state as `ble_off`; while off, a dBm POST is rejected
     ("reboot to re-enable").
   - **device** — `name` (hostname text field with validation and a
     blue/green/red border state), `reboot device` button (red, with a
     `confirm()` guard; turns blue and re-labels "reboot to apply" after
     a hostname or WiFi-PS save that needs an association restamp).

   WiFi TX is auto-disabled when the device reports `wifi:0` (runtime
   off via `?wifi=0`, or the BLE-only build where WiFi is compiled out).
   On first connect the page issues one GET per endpoint to preselect
   every control from the live state, then wires `onchange` handlers
   that POST single-field updates. "BLE scan" controls the central-role
   scanner (advertisement ingest + clone-upstream discovery); the proxy
   advertises as a peripheral too, but those parameters (interval) live
   on `BLE → adv`.
5. **Console pane** *(if `CONFIG_NBP_WEB_CONSOLE`)* — ~900 × 300
   monospace pane, `width:100%; max-width:920px`. ansi\_up renders
   ESP-IDF color escapes. New bytes from `/log` are appended via
   `Range.createContextualFragment` (avoids `innerHTML` to stay clear of
   the security hook). Trimmed to 3000 DOM nodes; auto-scrolls only if
   the user was already at the bottom.
6. **Footer** — OTA `curl` hint.

Both charts re-fit on viewport resize / orientation change: a `resize`
listener calls `u.setSize` / `u2.setSize` with `chartW(el)`, where
`chartW(el) = min(900, el.clientWidth - 16)` reads the wrapper's actual
inner width.

### `GET /stats.json`

Cumulative counters and instantaneous state. Always available.

```json
{
  "reads": 1234,
  "writes": 5,
  "notifies": 9876,
  "adverts": 542109,
  "connections": 2,
  "heap": 168432,
  "notify_rx": 9876,
  "last_notify_handle": 18,
  "cpu0": 7,
  "cpu1": 1,
  "temp_c": 44.4
}
```

- `reads` / `writes` / `notifies` — bumped from `bt_handlers.cpp` on
  successful GATT operations.
- `adverts` — `ble_backend::scanner::adv_count()`. Incremented at the top
  of every `onResult`, before the forwarding gate, so it reflects radio
  activity regardless of whether HA is subscribed.
- `connections` — `MAX_CONNECTIONS - free_slots()` from the connection
  pool.
- `heap` — `esp_get_free_heap_size()` (bytes). The chart label says "heap"
  but the value is free bytes.
- `notify_rx`, `last_notify_handle` — diagnostic counters from
  `ble_backend`.
- `cpu0`, `cpu1` — per-core busy% over the inter-poll window. Computed
  from `uxTaskGetSystemState`'s `ulRunTimeCounter` for tasks named
  `IDLE` / `IDLE1`, against `esp_timer_get_time()` as the wall-clock
  denominator. Requires `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` +
  `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` (set in
  `sdkconfig.defaults`). First sample after boot reports 0/0 (no prior
  delta).
- `temp_c` — ESP32-S3 internal silicon temperature sensor, °C, one
  decimal. ±1-2 °C absolute accuracy, fine for trend visibility. Sensor
  is lazy-installed on first `/stats.json` hit.

### `GET /devices` *(gated by `CONFIG_NBP_DEVICES_PANEL`)*

Snapshot of the scanner's live device table.

```json
{
  "devices": [
    {
      "addr": "20:A1:11:02:23:45",
      "type": 0,
      "rssi": -69,
      "count": 4231,
      "age": 142,
      "name": "ANT-BMS"
    }
  ]
}
```

- `addr` — MSB-first hex, matching aioesphomeapi formatting.
- `type` — NimBLE `BLE_ADDR_*` enum (0 = public, 1 = random).
- `rssi` — dBm from the most recent advert.
- `count` — cumulative adverts seen from this MAC since boot.
- `age` — ms since last sighting (FreeRTOS tick × `portTICK_PERIOD_MS`).
- `name` — last non-empty Complete/Shortened Local Name. Names found only
  in scan responses persist across plain adv packets. JSON-escaped: `"`
  and `\` get backslashed, control chars get `\u00XX`.

Internal table holds up to 64 entries; when full, the row with the oldest
`last_ms` is evicted (LRU by sighting). Response is built into a 6 KiB
static buffer; httpd serializes requests so the single buffer is safe.

#### Dashboard rendering

The dashboard fetches `/devices` and `/clone` in parallel each tick. For
each device row it builds a clone button:

- **Clone**: not currently the clone target. Click prompts for the
  upstream SMP passkey (when `CONFIG_NBP_SMP` is built), then POSTs
  `/clone?addr=…&type=…&enabled=1[&passkey=N]` in one request — the
  `/passkey` endpoint was folded into `/clone` so a paired upstream
  can be configured in one gesture.
- **Unclone**: this row IS the active clone target. The row gets a
  `.cloned` class (blue inset border, tinted background) and the
  button text/style flips. Click POSTs `/clone?enabled=0`.

If the configured clone target isn't in the `/devices` response (e.g.
the peripheral is silent during an active connection — BLE peripherals
suppress advertising while in a connection), a **synthetic row** is
prepended for it. The row's name field is derived from the upstream
supervisor state in `/clone` so a successfully-cloned peer reads as
"(connected to upstream)" rather than "(not in range)". State → label
mapping:

| `/clone state` | synthetic row label |
|---|---|
| `Ready` | (connected to upstream) |
| `Discovering` | (discovering upstream…) |
| `Connecting` | (connecting to upstream…) |
| `Reconnecting` | (reconnecting to upstream…) |
| `Scanning` | (searching for upstream…) |
| `Idle` | (clone target, idle) |
| `Disabled` | (clone target, disabled) |
| anything else | (clone target, not in range) |

The synthetic row carries a `.synthetic` class (italic, dim) so it's
visually distinct from a live-scanned row. RSSI / adv/s / age all show
`—` since the scanner isn't seeing the peripheral.

### `GET /log?since=<seq>` *(gated by `CONFIG_NBP_WEB_CONSOLE`)*

Chunked stream of the slice `[since, current_seq)` of the log ring.
Response header `X-Log-Seq: <new_seq>` tells the client what to pass next
time. If `since` is behind the oldest byte still in the ring, the response
starts from the oldest resident byte and the client's local counter is
implicitly reset on the next poll.

`Content-Type: text/plain; charset=utf-8`. Body is the raw line bytes
including ANSI escapes. Streamed in 1 KiB chunks through a static bounce
buffer with the ring mutex released across each socket send.

Ring size: 64 KiB when `CONFIG_BT_NIMBLE_LOG_LEVEL_DEBUG=y` (the default
for this project — required so `/trace` produces useful captures), 8 KiB
otherwise. Indexed by a monotonic `g_log_seq` (total bytes ever written).

The mirror is a tee: `esp_log_set_vprintf` is hooked, but the previous
vprintf (UART/JTAG) is still invoked so serial output is unaffected.

### `GET /level` &nbsp;·&nbsp; `POST /level?nimble=<0..5>`

Get / set the NimBLE log level for the NimBLE-Cpp tags
(`NimBLE`, `NimBLEDevice`, `NimBLEClient`, `NimBLERemoteCharacteristic`,
`NimBLEScan`, `NimBLEAdvertisedDevice`). Values: 0 = NONE, 1 = ERROR,
2 = WARN (default), 3 = INFO, 4 = DEBUG, 5 = VERBOSE.

GET returns `{"nimble":N}`. POST persists to NVS namespace `stats`, key
`nimble_lvl` (int8). Applied immediately, and reapplied at boot by
`apply_log_overrides_from_nvs()` before `ble_backend::start()`.

The scanner tags (`NimBLEScan` + `NimBLEAdvertisedDevice`) are
**capped at WARN** in `apply_level()` regardless of the picked value —
INFO+ would flood the console with "New advertiser: \<mac\>" on every
advert. They only become more verbose via `/trace ON`'s explicit
override.

### `GET /txpower` &nbsp;·&nbsp; `POST /txpower?wifi=<dBm|0>&ble=<dBm>`

Get / set the WiFi and BLE TX power (dBm). Either query param may be
omitted; only the supplied one is updated. Persisted to NVS namespace
`stats`, keys `wifi_tx` / `ble_tx` (int8). Applied at boot by
`apply_tx_power_from_nvs()` after both radios are up.

GET returns `{"wifi":N,"ble":M}`.

- WiFi maps to `esp_wifi_set_max_tx_power(dbm × 4)` (chip wants
  0.25 dBm units). Dashboard exposes off / 8 / 11 / 14 / 17 / 20 / 21
  dBm. Lower values (2, 5) were removed after observing that 2 dBm
  doesn't drop the AP association but kills throughput — the device
  becomes reachable-but-unusable.

  `wifi=0` is the **off** sentinel: it calls `esp_wifi_stop()`, marks
  the radio off for the rest of the boot, and is **not** persisted to
  NVS — a reboot restores WiFi at the previously stored dBm. This
  keeps a BLE-only client from being able to brick the device. While
  off, the GET response reports `wifi:0` and further `?wifi=<n>` POSTs
  return `"wifi off; reboot to re-enable"`. When `CONFIG_NBP_WIFI` is
  not set, GET also reports `wifi:0` (compile-time absent).
- BLE maps to `esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, lvl)`,
  with `lvl` from a `dbm → ESP_PWR_LVL_*` lookup in 3 dBm steps.
  Dashboard exposes -12/-9/-6/-3/0/3/6/9 dBm.

Defaults if no NVS entry: WiFi 20 dBm, BLE +9 dBm (chip maxes).

### `GET /cpufreq` &nbsp;·&nbsp; `POST /cpufreq?mhz=<80|160|240>&ls=<0|1>`

Get / set the CPU clock and `esp_pm` light-sleep gate. Either query
param may be omitted. Pinned `max_freq = min_freq` so the clock is
clamped exactly; `ls=1` enables `light_sleep_enable` so the SoC can
coast between bursts of work — useful when scan duty is low.

Light sleep is only effective with `CONFIG_FREERTOS_USE_TICKLESS_IDLE`
on; otherwise the 1 ms FreeRTOS tick wakes both cores every ms and
the longest nap is ~1 ms. The bletest defaults turn that on plus BT
controller modem sleep (`CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1`); the
default WiFi build leaves them off because the active TCP listener
makes the wake-up overhead not worth it.

Stored in NVS namespace `stats`: `cpu_freq` (int8 = MHz/10) and
`cpu_ls` (int8 boolean). Applied at boot by
`apply_cpu_freq_from_nvs()` early in `app_main` (before WiFi/BLE
init) so the radios initialise at the chosen clock.

GET returns `{"mhz":N,"ls":bool}`. Defaults if no NVS entry:
240 MHz, light sleep off.

Requires `CONFIG_PM_ENABLE=y` (set in `sdkconfig.defaults`). Dropping
to 80 MHz visibly slows GATT discovery — fine for a quiet observer,
maybe not for fast pairing.

### `GET /scan` &nbsp;·&nbsp; `POST /scan?window=<ms>&interval=<ms>&active=<0|1>`

Get / set the NimBLE scanner duty cycle and active/passive mode.
`interval` is the epoch length (when the next scan starts); `window`
is how much of that epoch the radio is on. Both in ms, both writable.
Lowering the duty is the biggest single thermal lever: a 30 ms window
inside a 1000 ms interval drops the chip temperature by ~10 °C versus
a 30/60 (50 %) duty.

`active=1` makes the scanner send `SCAN_REQ` to each advertiser and
receive a `SCAN_RSP`, which is where many devices (e.g. Victron
SmartShunt) put their Complete Local Name. Passive scan (`active=0`)
only ingests the legacy adv packet, so names from scan-response-only
devices never appear in `/devices`. Cost: roughly doubles per-advert
radio time, mildly increases consumption, and adds congestion that
can starve a clone supervisor's connect window when many devices are
nearby.

All three params are optional on POST — at least one must be set.
Stored in NVS namespace `stats`, keys `scan_win` / `scan_int`
(uint16) and `scan_act` (int8). Applied at boot by
`apply_scan_from_nvs()` after `ble_backend::start()`
(scanner::set\_duty is a no-op before scanner::init runs). The
underlying `scanner::set_duty` / `set_active` call NimBLE's
`setInterval` / `setWindow` / `setActiveScan`, then stop+restart the
scan so the new settings take effect immediately.

GET returns `{"window":N,"interval":M,"active":true|false}`.
Defaults from `proxy_config.h`. The dashboard exposes four duty
presets (`30/60` 50 %, `30/120` 25 %, `30/300` 10 %, `30/1000` 3 %)
and an `active scan` checkbox.

### `GET /advitvl` &nbsp;·&nbsp; `POST /advitvl?ms=<N>`

Get / set the peripheral advertising interval (in ms). 0 = NimBLE host
default (~30–60 ms range for connectable undirected adv, ~17–33 adv/s);
otherwise 20..10240 ms. Persisted to NVS namespace `stats`, key
`adv_itvl` (uint16). Applied **live** via
`ble_backend::set_adv_interval_ms` — the call updates the singleton
`NimBLEAdvertising` params, and hot-restarts adv via `stop() / start()`
if it's currently running, so a dropdown change takes effect within one
HCI window.

GET returns `{"ms":N}`. Dashboard exposes 0 (default), 100, 200, 500,
1000, 2000 ms — each labelled with the resulting per-second rate.

Reapplied at boot by `apply_adv_interval_from_nvs()` right after
`ble_backend::start()` so the first `g_adv->start()` (in
`ble_httpd::activate` or `clone_mirror::start_advertising`) already uses
the configured interval. `clone_mirror::start_advertising` calls
`g_adv->reset()` before populating the adv data, so it re-reads
`ble_backend::adv_interval_units()` after reset to restore the value.

Effective rate at the controller is the configured interval plus a
spec-mandated 0..10 ms random delay per adv event; the dropdown labels
reflect the rough midpoint.

**Note — auto-suspend (`CONFIG_NBP_BLE_AUTO_OFF`):** `/advitvl` only sets
*user intent* (the master on/off + interval). When the BLE auto-off
supervisor is compiled in, advertising actually broadcasts only when the
user switch is on **AND** the supervisor hasn't auto-suspended it. The
supervisor suspends advertising while WiFi (STA) is connected and no
central/clone needs the peripheral, and restores it the moment WiFi drops
or a central connects — see the "BLE serves two independent roles" note in
`CLAUDE.md`. So the dashboard's "advertising on" state reflects intent, not
necessarily what's on air right now; `ble_backend::advertising_enabled()`
reports the *combined* state, and the supervisor flips the suspend half via
`set_advertising_auto_suspend()`. The two compose and don't fight: re-enabling
either half resumes the same payload without a clone reconnect.

### `GET /wifips` &nbsp;·&nbsp; `POST /wifips?li=<0..10>`

Get / set the WiFi STA power-save listen interval (DTIM beacons between
RX wake-ups). 0 → `WIFI_PS_NONE` (radio always on, lowest RX latency,
highest energy). N > 0 → `WIFI_PS_MAX_MODEM` with N×DTIM sleeps.

Persisted to NVS namespace `stats`, key `wifi_li` (int8). The PS-mode
flip is live (`esp_wifi_set_ps`), but the `listen_interval` field
inside `wifi_config_t.sta` is read only at association time —
`wifi_sta.cpp::start_and_wait_for_ip` reads the NVS slot **before**
`esp_wifi_start`, so the value lands in the initial association. A
runtime change therefore needs a reboot (or a disconnect/reconnect) to
fully take effect; the dashboard pulses the reboot button blue and
re-labels it "reboot to apply PS" as an apply hint.

GET returns `{"li":N}`. Dashboard exposes 0 (off), 1, 3, 5, 10.

### `GET /hostname` &nbsp;·&nbsp; `POST /hostname?val=<name>`

Get / set the device hostname. Drives mDNS (`<name>.local`), the WiFi
netif hostname, the NimBLE GAP name advertised by ble_httpd, and
`aioesphomeapi DeviceInfoResponse.name`. Persisted in NVS namespace
`stats`, key `hostname` (UTF-8 string). Applied at boot by
`apply_hostname_from_nvs()` which loads into the mutable
`proxy::g_hostname[31]` buffer that all consumers read via
`proxy::hostname()`.

Validation: 1..30 chars from `[A-Za-z0-9._-]`, no leading/trailing `-`
or `.`. The 30-char cap keeps the value (+ NUL) fitting the smallest
consumer — `proxyapi_HelloResponse.name` is 31 bytes — without
tripping `-Werror=format-truncation`.

GET returns `{"hostname":"…","default":"nimble-proxy"}`. POST persists
but does **not** re-initialise mDNS / WiFi netif / NimBLE — a reboot is
required for the change to take effect across all consumers. The
dashboard reflects this: the text field gains a `.dirty` (blue) border
while editing, flips to `.ok` (green) after a successful POST, and
restyles the reboot button to "reboot to apply name".

### `GET /nat` &nbsp;·&nbsp; `POST /nat` *(gated by `CONFIG_NBP_NAT_ROUTER`)*

SoftAP/NAT router status + control. `GET` returns:

```json
{"enabled":true,"ssid":"nimble-nat","open":false,
 "ap_ip":"192.168.4.1","sta_ip":"192.168.1.50","clients":2,"max":4,
 "stations":[
   {"mac":"aa:bb:cc:dd:ee:ff","ip":"192.168.4.2","rssi":-54,"hostname":"pixel-7"},
   {"mac":"11:22:33:44:55:66","ip":"192.168.4.3","rssi":-71,"hostname":""}],
 "portmaps":[{"proto":"tcp","mport":8080,"daddr":"192.168.4.2","dport":80}]}
```

`stations` lists the currently-associated SoftAP clients. `mac` + `rssi`
come from `esp_wifi_ap_get_sta_list`; `ip` from the DHCP server's MAC↔IP
pairing (`esp_wifi_ap_get_sta_list_with_ip`); `hostname` is the client's
DHCP **option 12** (Host Name), empty when the client didn't send one.

IDF 5.5's DHCP server parses past option 12 and discards it, so we recover
it with a custom lwIP hook (`LWIP_HOOK_DHCPS_POST_STATE`, wired via
`ESP_IDF_LWIP_HOOK_FILENAME` in the root `CMakeLists.txt`): on every
incoming DHCP packet the hook reads option 12 and stores it keyed by MAC in
a small registry (`components/dhcps_hostname`), which `/nat` looks up per
station. The built-in server is left fully intact — no fork, no
`CONFIG_LWIP_DHCPS` change. The dashboard NAT panel renders the array as a
"hostname / IP / MAC / RSSI" table.

`POST /nat?enabled=0|1`, `?ssid=…`, `?psk=…` toggle/reconfigure the AP;
`/portmap` (separate endpoint) adds/deletes inbound forwards.

### `POST /trace?on=<0|1>`

Diagnostic capture mode. `on=1`:

- silences `NimBLEScan` and `NimBLEAdvertisedDevice` (set to `ESP_LOG_ERROR`),
- raises `NimBLE`, `NimBLEDevice`, `NimBLEClient`, `NimBLERemoteCharacteristic`
  to `ESP_LOG_DEBUG`,
- pauses the scan via `ble_backend::scanner::pause()` so the radio stops
  delivering adv callbacks,
- resets `g_log_seq` *(when `CONFIG_NBP_WEB_CONSOLE`)* so the next
  `GET /log?since=0` returns a clean trace from the moment trace was
  enabled.

`on=0` restores the persisted NimBLE log level and resumes scanning.

Returns `{"trace":true}` or `{"trace":false}`.

### `POST /reboot`

Returns `rebooting\n`, waits 500 ms for the TCP FIN, then calls
`esp_restart()`. The page's reboot button is a thin wrapper around this
endpoint with a `confirm()` guard.

### `GET /favicon.svg`

Tiny inline SVG (Bluetooth glyph on a blue rounded square) embedded
into the WiFi build via `EMBED_FILES`. `Cache-Control: public,
max-age=86400` so browsers stop refetching it. ~300 B.

### Passkey (folded into `/clone`)

The static SMP passkey used when the proxy is the initiator and an
upstream peer demands MITM pairing (Victron SmartShunt, some BMS
variants) is now part of the `/clone` payload — there is no
standalone `/passkey` HTTP endpoint anymore. Both the dashboard's clone
prompt and any external scripting use:

```
GET  /clone                   → {"passkey":NNNNNN,...} (when CONFIG_NBP_SMP)
POST /clone?addr=…&passkey=N&enabled=1
```

The `api_server::stats::set_passkey()` C++ helper is the single source
of truth for NVS persistence (`stats / ble_passkey`) and apply
(`ble_backend::connection::set_passkey`). Boot-time replay still runs
inside `apply_log_overrides_from_nvs()` so the passkey is loaded
before any GATT pairing can be triggered. See `docs/clone.md` §6.

## BLE transport (`CONFIG_NBP_BLE_HTTPD`)

A NimBLE peripheral service with three characteristics:

| UUID suffix | Property | Direction | Purpose |
|---|---|---|---|
| `…0001` | WRITE | client → device | REQUEST: one `<METHOD> <PATH>?<QUERY>` line per write |
| `…0002` | NOTIFY | device → client | RESPONSE: fragmented body of the matching request |
| `…0003` | READ | client → device | INFO: `[u8 version][u8 reserved][u16 max_frag_le]` |

Base UUID: `6e627062-7072-7879-0001-000000000000` (`'nbpb' 'prxy'` +
slot bytes).

Both REQUEST and RESPONSE are fragmented with a 2-byte header per
fragment:

```
byte 0   flags  bit0 = FIN (last fragment), bit1 = ERR
byte 1   reqId  echoed by server so the client can multiplex
bytes 2+ payload
```

The server allocates a 4 KiB response buffer reused across requests
(single-connection serialization is fine for a dashboard). Fragment
size is `MTU − 3 − 2` bytes; with the chip-wide MTU set to 247 this
is 242 payload bytes per fragment.

The httpd dispatcher inside `ble_httpd.cpp` is a thin router that
calls the same `build_*_json` / `handle_*_set` helpers used by the
HTTP handlers in `stats.cpp`. The only protocol-level transform is
`/log`: the BLE response prepends a 10-digit-decimal sequence number
plus newline so the client can reassemble across fragments without
needing a separate header channel. The HTTP path synthesizes the
same envelope on the client side (from the `X-Log-Seq` header) so
`apiText` returns identical bytes either way.

Once the GATT server is registered, `NimBLEDevice::getAdvertising()`
gets the service UUID added and starts advertising. A scan
pause/resume guards the registration to dodge a `BLE_HS_EBUSY` from
NimBLE refusing GATT mutations while the scanner is active. When
`CONFIG_NBP_CLONE` is also on, `ble_httpd::activate()` is called by
`clone_mirror::finalize_server` instead of running independently —
there must be exactly one `NimBLEServer::start()` per session and the
clone path owns the timing. See clone pitfall 13.15.

### Web Bluetooth Connect button

The dashboard's `Connect` button calls `navigator.bluetooth.
requestDevice` with `acceptAllDevices: true` (not a service-UUID
filter) and adds the dashboard SVC to `optionalServices`. Reason: in
clone mode the cloned upstream's service UUIDs can fill the 31+31 B
main+scan-response budget, so `clone_mirror::start_advertising` drops
the dashboard SVC from the adv payload — a UUID-filtered picker would
then show an empty list. `acceptAllDevices` lists every nearby BLE
device; `getPrimaryService(SVC)` fails fast if the wrong one is
chosen.

Web Bluetooth requires a secure context (HTTPS or localhost) and is
not available on plain `http://<ip>/`. The button is disabled while
the HTTP transport probe is in flight at boot; if the probe fails AND
`navigator.bluetooth` is undefined, the page shows the "unsupported"
banner. Clicking Connect anyway reprobes HTTP once before erroring —
useful for the post-OTA reboot window where `/level` is transiently
unreachable.

## Boot wiring

`main/main.cpp` orders initialisation as follows. Steps marked
*(WiFi)* run only when `CONFIG_NBP_WIFI=y`; *(BLE httpd)* when
`CONFIG_NBP_BLE_HTTPD=y`; *(clone)* when `CONFIG_NBP_CLONE=y`.

1. `api_server::stats::install_log_hook()` — first, so NVS / WiFi /
   mDNS init logs are captured. (Gated on `CONFIG_NBP_WEB_CONSOLE`.)
2. `nvs_flash_init()`.
3. `esp_event_loop_create_default()`.
4. `api_server::stats::apply_hostname_from_nvs()` — must run before any
   consumer reads `proxy::hostname()` (mDNS, esp_netif, NimBLE GAP name,
   aioesphomeapi DeviceInfo).
5. `api_server::stats::apply_log_overrides_from_nvs()` — must run before
   any NimBLE component initialises. Also loads the SMP passkey into
   `ble_backend::connection`.
6. `api_server::stats::apply_cpu_freq_from_nvs()` — early so WiFi/BLE
   init runs at the chosen clock.
7. *(WiFi)* `wifi_sta::start_and_wait_for_ip()` — reads
   `stats / wifi_li` for PS listen_interval BEFORE `esp_wifi_start`.
8. *(WiFi)* `mdns_announce::start()`.
9. *(WiFi)* `ota::start()` — creates the shared httpd (URI handler cap
   bumped to 32 to fit the growing endpoint set; see clone pitfall 13.12).
10. *(WiFi)* `api_server::stats::register_endpoints(ota::handle())` —
    adds all dashboard URIs to the OTA httpd.
11. *(WiFi)* `ble_backend::publish::install(...)`.
12. `ble_backend::start()` — NimBLE host up; `getAdvertising()` valid.
13. `api_server::stats::apply_adv_interval_from_nvs()` — applies the
    persisted advertising interval to the singleton before any
    `g_adv->start()` call below.
14. *(WiFi)* `api_server::start()` — aioesphomeapi listener on port 6053.
15. *(BLE httpd)* `ble_httpd::start()` — only **creates** the dashboard
    GATT service (no `server->start()`, no `adv->start()`). See clone
    pitfall 13.15 for the one-call constraint.
16. *(clone)* `ble_clone::config::load()` + `ble_clone::init()` —
    spawns the supervisor. The supervisor eventually calls
    `clone_mirror::finalize_server()` which calls `ble_httpd::activate()`
    so the dashboard service and any cloned services start together.
17. *(BLE httpd, !clone)* `ble_httpd::activate()` — fallback that runs
    when clone is compiled out so the dashboard still comes up.
18. `api_server::stats::apply_tx_power_from_nvs()` — both radios must be
    up before set-power calls succeed.
19. `api_server::stats::apply_scan_from_nvs()` — last; `scanner::set_duty`
    is a no-op before `scanner::init` runs.

## Concurrency

- All HTTP handlers run on the single httpd worker task; no inter-request
  contention.
- `/devices` snapshots the scanner's device table under
  `ble_backend::scanner::g_mutex`, then formats the JSON outside the
  critical section. Holding the mutex during socket IO would stall the
  NimBLE host task on every advert callback.
- `/log` copies up to 1 KiB at a time into a static bounce buffer under
  the log mutex, then releases the mutex across each `httpd_resp_send_chunk`
  call.
- `record_read/write/notify` (from `bt_handlers.cpp`) use
  `std::atomic<uint32_t>` with `memory_order_relaxed` — counters are
  monotonic; ordering between them and other state doesn't matter.
- `sample_cpu_pct()` keeps its prev-sample state in function-static vars;
  safe because httpd serializes requests.

## Costs at a glance

| Component | Flash | BSS RAM |
|---|---|---|
| Embedded `web/index.html` (charts + tunables + controls JS) | ~21 KB | 0 |
| `/stats.json` + `/level` + `/txpower` + `/cpufreq` + `/scan` + `/trace` + `/reboot` handlers | ~3 KB | <200 B |
| `CONFIG_NBP_DEVICES_PANEL` | ~3 KB | ~12 KB |
| `CONFIG_NBP_WEB_CONSOLE` | ~2 KB | 64 KB (or 8 KB without NimBLE DEBUG) |
| `CONFIG_NBP_BLE_HTTPD` (GATT service + dispatcher) | ~4 KB | ~4 KB |

CDN payload (per page load, not on-device): uPlot ≈ 50 KB gzipped + ansi\_up
≈ 8 KB gzipped. Both cached aggressively by jsdelivr; no payload cost when
the LAN client has them cached.

Reference binary sizes on ESP32-S3 with the full WiFi build vs. the
BLE-only `sdkconfig.defaults.bletest`:

| Build | Size | Notes |
|---|---|---|
| WiFi (`CONFIG_NBP_WIFI=y`) | ~1.18 MB | WiFi stack + httpd + OTA + aioesphomeapi + embedded HTML |
| BLE-only (`CONFIG_NBP_WIFI=n`, `CONFIG_NBP_BLE_HTTPD=y`) | ~0.62 MB | GATT dashboard transport only; HTTP handlers GC'd by the linker because `register_endpoints` is `#if CONFIG_NBP_WIFI` |
