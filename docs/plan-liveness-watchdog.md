# Router self-recovery liveness watchdog (nimble-ble-proxy)

Repo: `/Users/fab/dev/ha/nimble-ble-proxy` (ESP32-S3 WiFi NAT-router + NimBLE BLE-proxy, deployed at **192.168.1.231**).
Plan also mirrored here per repo convention; primary copy is this file.

## Context

On 2026-06-11, OTAing a fugu converter (fry) caused a downstream NAT client to pull a ~1.6 MB
image *through* this router. The router went **fully unresponsive — no ICMP, no web UI — and did
not self-reboot**, cutting fry+flat off the LAN until a **physical power-cycle**. Root cause is a
WiFi/coex network livelock under sustained forwarded throughput on the single radio: tasks keep
running (so the 30 s TWDT never fires) but the IP path is dead.

Two gaps:
1. The coex + OTA-quiesce fixes already in-tree (`ac9b8c7`) **are not on the deployed image** (its
   `/stats.json` lacks the `app` block → stale build), and even when present, OTA-quiesce only
   covers an OTA **of the router itself**, not NAT-forwarded transfers.
2. **No self-recovery.** TWDT/INT_WDT are task-based; a network livelock with live tasks is invisible
   to them. There is no heartbeat that reboots on loss of external reachability.

**Goal of this change:** add a liveness watchdog so any future total wedge **auto-reboots within a
few minutes** without physical access. Flashing current `main` also brings the `ac9b8c7` coex/quiesce
fixes along. Preventing the wedge (throttling forwarded heavy transfers) is **out of scope** here —
recovery first.

## Approach

New component `components/liveness_wdt/` (`liveness_wdt.{h,cpp}` + `CMakeLists.txt`) exposing
`liveness_wdt::start()`. A single low-priority task probes upstream LAN reachability and
`esp_restart()`s after sustained failure.

### Probe logic (robust, never hangs)
- **Multi-target, "all must be unreachable to count as a failure."** Default targets:
  1. STA default **gateway**, port 80 — fetched at runtime via
     `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")` + `esp_netif_get_ip_info()` → `.gw.addr`
     (idiom at `components/nat_router/nat_router.cpp:666`). A TCP **RST (ECONNREFUSED) also counts
     as reachable** — it proves L3/ARP works even with no open port.
  2. MQTT **broker 192.168.1.200:1882** (known-open) — `connect()` success = reachable.
- A target is "reachable" if a **non-blocking `connect()` + `select(timeout=3s)`** completes OR is
  actively refused; only timeout / `EHOSTUNREACH` / `ENETUNREACH` = unreachable. Socket idiom mirrors
  `components/ws_proxy/ws_proxy.cpp:52` but **non-blocking** so the task can never block. One fresh
  socket per target per cycle, always `close()`d.
- Rationale for multi-target: a single gateway probe is unreliable (many gateways silently *drop*
  SYNs to closed ports → false timeouts → reboot loop). Requiring **all** targets unreachable means a
  filtered gateway still passes via the broker, a down broker still passes via gateway RST, and only a
  genuine STA→LAN wedge fails everything.

### Reboot gating (avoid false positives)
- Probe cycle every `interval` s (default **30**). Increment a failure counter when a cycle fails
  (all targets unreachable); **reset to 0** on any success.
- `esp_restart()` only after `threshold` consecutive failed cycles (default **6** → ~3 min sustained).
- **Grace gate:** do nothing until `wifi_sta::is_connected()` has been true at least once and ≥90 s
  uptime (skip boot/first-associate transients). A normal STA drop self-heals via the existing
  reconnect path and the probe recovers before threshold.
- Log each failed cycle at WARN and the reboot at ERROR (`ESP_LOGE`) before restarting.

### Wiring
- `main/main.cpp`: call `liveness_wdt::start()` immediately **after** `wifi_sta::start_and_wait_for_ip()`
  (`main/main.cpp:163`), matching the existing post-STA service-start block. Task: `xTaskCreate`,
  ~4 KiB stack, priority 3 (below app/BLE), unpinned — mirror `components/api_server/api_server.cpp:317`.

### Config (Kconfig + runtime off-switch)
- `main/Kconfig.projbuild`: `NBP_LIVENESS_WDT` (bool, default y, `depends on NBP_WIFI`),
  `NBP_LIVENESS_INTERVAL_SECS` (default 30), `NBP_LIVENESS_THRESHOLD` (default 6). Pin all three in
  `sdkconfig.defaults` (per the project rule that opt-ins must live there, CLAUDE.md).
- **Runtime control + safety valve** (this is a live, load-bearing router): minimal `/liveness`
  GET/POST in `components/api_server/stats.cpp`, mirroring the `/wifips` handler+registration pattern
  (`stats.cpp:459`, registration `stats.cpp:1526`), NVS-persisted in the existing `"stats"` namespace.
  - `GET /liveness` → `{"enabled":bool,"interval":N,"threshold":N,"failures":N,"last_ok_s":N}`
  - `POST /liveness?enabled=0|1&interval=N&threshold=N` → live enable/disable + retune (lets us kill it
    instantly if it ever misbehaves, without a reflash).
  - `POST /liveness?test=1` → force the failure counter to `threshold` to **prove the reboot path**
    end-to-end on demand.

## Files
- **new** `components/liveness_wdt/liveness_wdt.h` — `void start();` + small runtime API
  (`set_enabled/set_params/get_state`) for the endpoint.
- **new** `components/liveness_wdt/liveness_wdt.cpp` — task, probe, gating, reboot.
- **new** `components/liveness_wdt/CMakeLists.txt` — `REQUIRES esp_netif lwip esp_wifi nvs_flash`
  (+ `wifi_sta` for `is_connected()`).
- **edit** `main/main.cpp` — `#include` + `liveness_wdt::start()` after STA-up (`:163`).
- **edit** `main/Kconfig.projbuild` — 3 config entries.
- **edit** `sdkconfig.defaults` — pin the 3 flags.
- **edit** `components/api_server/stats.cpp` — `/liveness` GET/POST handlers + registration + NVS;
  optionally surface `failures`/`last_ok_s` in `/stats.json`.

## Deploy (OTA, user-supervised)
1. Build S3: `cd /Users/fab/dev/ha/nimble-ble-proxy && idf.py build` → `build/nimble_ble_proxy.bin`.
2. Pre-flash sanity: confirm current state `curl -s http://192.168.1.231/stats.json` (heap, routing).
3. OTA: `curl --data-binary @build/nimble_ble_proxy.bin http://192.168.1.231/update` (reboots on
   success; `ota.cpp:143`).
4. **Brick risk is low:** OTA writes the *inactive* slot (`ota_0`/`ota_1`, `partitions.csv`); a failed
   or wedged-mid-upload flash leaves the **old image bootable** — worst case is a power-cycle back onto
   the current image, then retry. Do it while you can reach the unit physically.
   - Caveat: the **currently-running stale image lacks OTA-quiesce**, so the upload itself shares the
     radio with the BLE scan and *could* wedge mid-transfer. It's a direct (not NAT-forwarded) upload
     to the router's own httpd, lower contention than today's event; supervise and power-cycle if it
     stalls. After this image is on, future OTAs self-recover.

## Verification
- After reboot: `curl -s http://192.168.1.231/appinfo` → new `version`/`elf_sha256`
  (`stats.cpp:964`); `curl -s http://192.168.1.231/liveness` → `enabled:true`.
- Steady state: `curl -s http://192.168.1.231/stats.json` heap/temp unchanged within noise; `/log`
  shows the watchdog task started, no spurious WARN failure cycles (proves no false positives against
  the real gateway/broker).
- **Prove the reboot path:** `curl -X POST 'http://192.168.1.231/liveness?test=1'` → device reboots
  within one cycle; confirm it comes back and re-serves `/appinfo`. Then confirm fry/flat reappear on
  the broker (live subscribe to `pv/log/#`).
- Regression: confirm fry+flat keep converting across the router reboot (they ride it on the AP).

## Out of scope (future)
- Extending OTA-quiesce / throttling to **NAT-forwarded** heavy transfers (prevention, not recovery).
- Heap-floor reboot trigger. Note as a possible second liveness signal but not implemented here.
