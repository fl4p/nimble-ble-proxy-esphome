# Web UI spec

The proxy ships a small web dashboard alongside the OTA endpoint. Both share
the single `httpd_handle_t` exposed by `ota::handle()` — there is no second
HTTP listener. All endpoints are unauthenticated on the assumption of a
trusted LAN.

## Build-time gates

Two Kconfig flags toggle the heavier optional panels. Both default `y`. Edit
in `idf.py menuconfig` → `nimble-ble-proxy`, or set in `sdkconfig.defaults`.

| Kconfig | Default | Cost when ON | What it gates |
|---|---|---|---|
| `CONFIG_NBP_DEVICES_PANEL` | `y` | ~12 KB BSS | scanner device table, `/devices`, dashboard table |
| `CONFIG_NBP_WEB_CONSOLE` | `y` | 64 KB BSS (NimBLE DEBUG) / 8 KB BSS | `esp_log_set_vprintf` hook, ring buffer, `/log`, on-page console |

The rate chart, control row (NimBLE log level, reboot), and `/trace` endpoint
are always compiled in — they cost <100 B BSS combined.

## Endpoints

All under `http://<proxy>/`. Default port: 80 (ESP-IDF httpd default).

### `GET /` — dashboard HTML

Single self-contained page. Pulls uPlot 1.6.31 CSS+JS and ansi\_up v5 from
jsdelivr. CSS, HTML, and inline JS are embedded as one C++ string literal in
`stats.cpp`. Sections conditional on `CONFIG_NBP_*` are wrapped with `#if`
inside the literal.

Layout (top → bottom):

1. **Rate chart** — uPlot, 900 × 320, 120-sample rolling window (~2 min).
   Series: reads/s, writes/s, notifies/s, adverts/s, active connections,
   free heap (KB, right axis, dashed). Per-second deltas computed
   client-side from cumulative counters returned by `/stats.json`. Negative
   deltas (counter reset after reboot) are clamped to `null` so uPlot draws
   a gap instead of a spike.
2. **Devices table** *(if `CONFIG_NBP_DEVICES_PANEL`)* — every unique MAC
   the scanner has seen. Columns: MAC, name, RSSI, adv/s, total adverts,
   age. Sorted by total adverts descending. Rows whose `age > 10 s` get
   the `.stale` class (40 % opacity).
3. **Control row** — NimBLE log-level `<select>` (NONE / ERROR / WARN / INFO
   / DEBUG / VERBOSE) and a red "reboot device" button with a `confirm()`
   guard.
4. **Console pane** *(if `CONFIG_NBP_WEB_CONSOLE`)* — 900 × 300 monospace
   pane. ansi\_up renders ESP-IDF color escapes. New bytes from `/log` are
   appended via `Range.createContextualFragment` (avoids `innerHTML` to
   stay clear of the security hook). Trimmed to 3000 DOM nodes; auto-
   scrolls only if the user was already at the bottom.
5. **Footer** — OTA `curl` hint.

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
  "last_notify_handle": 18
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

### `GET /level`

Returns the currently applied NimBLE log level.

```json
{ "nimble": 2 }
```

Values: 0 = NONE, 1 = ERROR, 2 = WARN (default), 3 = INFO, 4 = DEBUG,
5 = VERBOSE.

### `POST /level?nimble=<0..5>`

Persists the value to NVS namespace `stats`, key `nimble_lvl` (int8) and
applies it immediately to all known NimBLE-Cpp tags
(`NimBLE`, `NimBLEDevice`, `NimBLEClient`, `NimBLERemoteCharacteristic`,
`NimBLEScan`, `NimBLEAdvertisedDevice`).

Returns `{"ok":true}` on success. The persisted value is read back by
`apply_log_overrides_from_nvs()` early in `app_main`, before
`ble_backend::start()`, so the level is in effect from the first scan
callback.

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
5. `wifi_sta::start_and_wait_for_ip()`.
6. `mdns_announce::start()`.
7. `ota::start()` — creates the shared httpd.
8. `api_server::stats::register_endpoints(ota::handle())` — adds all
   dashboard URIs to the OTA httpd.
9. `ble_backend::publish::install(...)` and `ble_backend::start()`.
10. `api_server::start()` — opens the aioesphomeapi listener on port 6053.

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

## Costs at a glance

| Component | Flash | BSS RAM |
|---|---|---|
| Dashboard skeleton (chart + controls + `/stats.json`) | ~3 KB | <100 B |
| `CONFIG_NBP_DEVICES_PANEL` | ~3 KB | ~12 KB |
| `CONFIG_NBP_WEB_CONSOLE` | ~2 KB | 64 KB (or 8 KB without NimBLE DEBUG) |
| `/trace` + `/reboot` + `/level` (always on) | ~1.5 KB | <100 B |

CDN payload (per page load, not on-device): uPlot ≈ 50 KB gzipped + ansi\_up
≈ 8 KB gzipped. Both cached aggressively by jsdelivr; no payload cost when
the LAN client has them cached.
