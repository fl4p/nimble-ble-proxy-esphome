# Web UI spec

The proxy ships a small web dashboard alongside the OTA endpoint. Both share
the single `httpd_handle_t` exposed by `ota::handle()` — there is no second
HTTP listener. All endpoints are unauthenticated on the assumption of a
trusted LAN.

The layout is responsive: the page declares
`<meta name=viewport content="width=device-width,initial-scale=1">`, uses
`box-sizing: border-box` globally, lays out the control row with
`flex-wrap`, and sizes both uPlot canvases from each chart wrapper's live
`clientWidth` (clamped to 900). A media query at `max-width:640px` tightens
padding and font size for phones.

## Build-time gates

Two Kconfig flags toggle the heavier optional panels. Both default `y`. Edit
in `idf.py menuconfig` → `nimble-ble-proxy`, or set in `sdkconfig.defaults`.

| Kconfig | Default | Cost when ON | What it gates |
|---|---|---|---|
| `CONFIG_NBP_DEVICES_PANEL` | `y` | ~12 KB BSS | scanner device table, `/devices`, dashboard table |
| `CONFIG_NBP_WEB_CONSOLE` | `y` | 64 KB BSS (NimBLE DEBUG) / 8 KB BSS | `esp_log_set_vprintf` hook, ring buffer, `/log`, on-page console |

The rate chart, the system chart, the control row (NimBLE log, WiFi TX, BLE
TX, CPU MHz, reboot), and `/trace` / `/txpower` / `/cpufreq` endpoints are
always compiled in — they cost <100 B BSS combined.

## Endpoints

All under `http://<proxy>/`. Default port: 80 (ESP-IDF httpd default).

### `GET /` — dashboard HTML

Single self-contained page. Pulls uPlot 1.6.31 CSS+JS and ansi\_up v5 from
jsdelivr. CSS, HTML, and inline JS are embedded as one C++ string literal in
`stats.cpp`. Sections conditional on `CONFIG_NBP_*` are wrapped with `#if`
inside the literal.

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
4. **Control row** — wrapping flex row with four `<select>` dropdowns
   (NimBLE log, WiFi TX, BLE TX, CPU MHz) and a red "reboot device"
   button (`confirm()` guard).
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

### `GET /txpower` &nbsp;·&nbsp; `POST /txpower?wifi=<dBm>&ble=<dBm>`

Get / set the WiFi and BLE TX power (dBm). Either query param may be
omitted; only the supplied one is updated. Persisted to NVS namespace
`stats`, keys `wifi_tx` / `ble_tx` (int8). Applied at boot by
`apply_tx_power_from_nvs()` after both radios are up.

GET returns `{"wifi":N,"ble":M}`.

- WiFi maps to `esp_wifi_set_max_tx_power(dbm × 4)` (chip wants
  0.25 dBm units). Dashboard exposes 8/11/14/17/20/21 dBm. Lower
  values (2, 5) were removed after observing that 2 dBm doesn't drop
  the AP association but kills throughput — the device becomes
  reachable-but-unusable.
- BLE maps to `esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, lvl)`,
  with `lvl` from a `dbm → ESP_PWR_LVL_*` lookup in 3 dBm steps.
  Dashboard exposes -12/-9/-6/-3/0/3/6/9 dBm.

Defaults if no NVS entry: WiFi 20 dBm, BLE +9 dBm (chip maxes).

### `GET /cpufreq` &nbsp;·&nbsp; `POST /cpufreq?mhz=<80|160|240>`

Get / set the CPU clock via `esp_pm_configure` (max_freq = min_freq,
light_sleep_enable = false — a continuously-active BLE scanner makes
sleep entry/exit overhead not worth the modest savings).

Stored in NVS namespace `stats`, key `cpu_freq` (int8 = MHz/10).
Applied at boot by `apply_cpu_freq_from_nvs()` early in `app_main`
(before WiFi/BLE init) so the radios initialise at the chosen clock.

GET returns `{"mhz":N}`. Default if no NVS entry: 240 MHz.

Requires `CONFIG_PM_ENABLE=y` (set in `sdkconfig.defaults`). Dropping
to 80 MHz visibly slows GATT discovery — fine for a quiet observer,
maybe not for fast pairing.

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

## Boot wiring

`main/main.cpp` orders the dashboard initialisation as follows:

1. `api_server::stats::install_log_hook()` — first, so NVS / WiFi / mDNS
   init logs are captured. (Gated on `CONFIG_NBP_WEB_CONSOLE`.)
2. `nvs_flash_init()`.
3. `esp_event_loop_create_default()`.
4. `api_server::stats::apply_log_overrides_from_nvs()` — must run before
   any NimBLE component initialises.
5. `api_server::stats::apply_cpu_freq_from_nvs()` — early so WiFi/BLE
   init runs at the chosen clock.
6. `wifi_sta::start_and_wait_for_ip()`.
7. `mdns_announce::start()`.
8. `ota::start()` — creates the shared httpd.
9. `api_server::stats::register_endpoints(ota::handle())` — adds all
   dashboard URIs to the OTA httpd.
10. `ble_backend::publish::install(...)` and `ble_backend::start()`.
11. `api_server::start()` — opens the aioesphomeapi listener on port 6053.
12. `api_server::stats::apply_tx_power_from_nvs()` — last; both radios
    must be up before set-power calls succeed.

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
| Dashboard skeleton (charts + controls + `/stats.json`) | ~4 KB | <100 B |
| `CONFIG_NBP_DEVICES_PANEL` | ~3 KB | ~12 KB |
| `CONFIG_NBP_WEB_CONSOLE` | ~2 KB | 64 KB (or 8 KB without NimBLE DEBUG) |
| `/trace` + `/reboot` + `/level` + `/txpower` + `/cpufreq` (always on) | ~2.5 KB | <200 B |

CDN payload (per page load, not on-device): uPlot ≈ 50 KB gzipped + ansi\_up
≈ 8 KB gzipped. Both cached aggressively by jsdelivr; no payload cost when
the LAN client has them cached.
