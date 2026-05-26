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

Four Kconfig flags shape what gets compiled in. Edit in
`idf.py menuconfig` → `nimble-ble-proxy`, or set in
`sdkconfig.defaults` / `sdkconfig.defaults.bletest`.

| Kconfig | Default | Cost when ON | What it gates |
|---|---|---|---|
| `CONFIG_NBP_WIFI` | `y` | ~500 KB flash (WiFi stack + httpd + mDNS + OTA) | STA bring-up, mDNS, OTA, aioesphomeapi server, the HTTP dashboard endpoints, embedded `web/index.html` |
| `CONFIG_NBP_BLE_HTTPD` | `n` | ~4 KB flash | GATT request/response service that lets Web Bluetooth talk to the same handlers |
| `CONFIG_NBP_DEVICES_PANEL` | `y` | ~12 KB BSS | scanner device table, `/devices`, dashboard table |
| `CONFIG_NBP_WEB_CONSOLE` | `y` | 64 KB BSS (NimBLE DEBUG) / 8 KB BSS | `esp_log_set_vprintf` hook, ring buffer, `/log`, on-page console |

A BLE-only build flips `NBP_WIFI=n` + `NBP_BLE_HTTPD=y` — see
`sdkconfig.defaults.bletest`. With `NBP_WIFI=n` the linker GCs the
entire HTTP handler set, so the BLE-only build is ~75 KB smaller than
the WiFi build even though both expose the same dashboard.

The rate chart, the system chart, the tunables panel (NimBLE log,
WiFi TX, BLE TX, CPU MHz, BLE scan duty, light sleep, reboot), and
`/trace` / `/txpower` / `/cpufreq` / `/scan` endpoints are always
compiled in — they cost <100 B BSS combined.

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
4. **Tunables row** — wrapping flex row with `<select>` dropdowns for
   NimBLE log, WiFi TX, BLE TX, CPU MHz, and BLE scan duty; a "light
   sleep" checkbox; and a red "reboot device" button (`confirm()`
   guard). WiFi TX is auto-disabled when the device reports `wifi:0`
   (either runtime off via `?wifi=0`, or the BLE-only build where
   WiFi is compiled out). On first connect the page issues one GET
   per endpoint to preselect every control from the live state, then
   wires onchange handlers that POST single-field updates.
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

### `GET /scan` &nbsp;·&nbsp; `POST /scan?window=<ms>&interval=<ms>`

Get / set the NimBLE scanner duty cycle. `interval` is the epoch
length (when the next scan starts); `window` is how much of that
epoch the radio is on. Both in ms, both writable. Lowering the duty
is the biggest single thermal lever: a 30 ms window inside a 1000 ms
interval drops the chip temperature by ~10 °C versus a 30/60 (50 %)
duty.

Stored in NVS namespace `stats`, keys `scan_w` / `scan_i` (uint16).
Applied at boot by `apply_scan_from_nvs()` after `ble_backend::start()`
(scanner::set\_duty is a no-op before scanner::init runs). The
underlying `scanner::set_duty` calls NimBLE's `setInterval` /
`setWindow`, then stop+restart the scan so the new timings take
effect immediately.

GET returns `{"window":N,"interval":M}`. Defaults from
`proxy_config.h`. The dashboard exposes four presets:
`30/60` (50 %), `30/120` (25 %), `30/300` (10 %), `30/1000` (3 %).

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

### `GET /passkey`, `POST /passkey` *(gated by `CONFIG_NBP_SMP`)*

Owned by the SMP feature, not by this UI work. See
`CONFIG_NBP_SMP` in `main/Kconfig.projbuild`.

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
NimBLE refusing GATT mutations while the scanner is active.

## Boot wiring

`main/main.cpp` orders initialisation as follows. Steps marked
*(WiFi)* run only when `CONFIG_NBP_WIFI=y`.

1. `api_server::stats::install_log_hook()` — first, so NVS / WiFi / mDNS
   init logs are captured. (Gated on `CONFIG_NBP_WEB_CONSOLE`.)
2. `nvs_flash_init()`.
3. `esp_event_loop_create_default()`.
4. `api_server::stats::apply_log_overrides_from_nvs()` — must run before
   any NimBLE component initialises.
5. `api_server::stats::apply_cpu_freq_from_nvs()` — early so WiFi/BLE
   init runs at the chosen clock.
6. *(WiFi)* `wifi_sta::start_and_wait_for_ip()`.
7. *(WiFi)* `mdns_announce::start()`.
8. *(WiFi)* `ota::start()` — creates the shared httpd.
9. *(WiFi)* `api_server::stats::register_endpoints(ota::handle())` —
   adds all dashboard URIs to the OTA httpd.
10. *(WiFi)* `ble_backend::publish::install(...)`.
11. `ble_backend::start()`.
12. *(WiFi)* `api_server::start()` — aioesphomeapi listener on port 6053.
13. `ble_httpd::start()` — gated on `CONFIG_NBP_BLE_HTTPD`. Registers the
    GATT service after the NimBLE host is up, pausing the scanner across
    `server->start()` to avoid `BLE_HS_EBUSY`.
14. `api_server::stats::apply_tx_power_from_nvs()` — both radios must be
    up before set-power calls succeed.
15. `api_server::stats::apply_scan_from_nvs()` — last; `scanner::set_duty`
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
