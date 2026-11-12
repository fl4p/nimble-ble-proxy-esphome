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
  42→55 KB steady, SA-Query loop gone, port-forwards work). 2026-11: further
  to 2 links (batmon-ha's BMS + one HA link), `MAX_ACT 4`, `ACL_BUF_COUNT 8`
  (see "Heap budget" below). The connection
  count is coupled across **three** files — keep them in sync:
  `BT_NIMBLE_MAX_CONNECTIONS` (`sdkconfig.defaults`), `proxy::MAX_CONNECTIONS`
  (`include/proxy_config.h`, the slot count advertised to HA), and the nanopb
  `BluetoothConnectionsFreeResponse.allocated` cap
  (`components/api_proto/api_subset.options`). Diagnose live with
  `GET /stats.json` (`heap`) + `GET /log` (grep `SA Query` / `xTaskCreate`).

- **Coex arbitration: bias to WiFi while the SoftAP is up.** The single radio's
  WiFi/BLE coexistence preference defaults to `ESP_COEX_PREFER_BALANCE`; under
  APSTA + a BLE scan that lets BLE steal enough airtime that the SoftAP's PMF
  SA-Query exchange times out (the loop above). `nat_router::ap_up()` calls
  **`esp_coex_preference_set(ESP_COEX_PREFER_WIFI)`** (header `esp_coexist.h`,
  the **only** API call into esp_coex — add `esp_coex` to the component's
  `REQUIRES`, it is *not* pulled in transitively by `esp_wifi`), and `ap_down()`
  restores `BALANCE` so a STA-only / BLE-proxy build regains BLE's share. This
  trades some BLE scan/connection throughput for WiFi/AP stability — the right
  call for a repeater, where BLE is best-effort. `esp_coex_preference_set` is
  current in IDF 5.5 (not deprecated); SW coexist (`ESP_COEX_SW_COEXIST_ENABLE`)
  must be on (it is by default). Note the BLE scan is *also* already duty-cut to
  ~3–50% (`proxy::SCAN_INTERVAL_MS`/`SCAN_WINDOW_MS`, runtime `/duty`).

- **WiFi driver log spam is silenced at WARN.** Under coex-induced AP-client
  flapping the IDF WiFi driver floods both serial and `/log` with I-level
  state-machine churn (`wifi:state: …`, `<ba-add>`, the SA-Query/disassoc
  lines) plus DHCP-server "assigned IP" notices. `wifi_sta::start_and_wait_for_ip()`
  sets `esp_log_level_set("wifi", ESP_LOG_WARN)` + `esp_log_level_set(
  "esp_netif_lwip", ESP_LOG_WARN)` (global per-tag — one call covers the AP side
  too). WARN keeps genuine driver warnings/errors and our own "disconnected;
  retrying". To debug a WiFi/coex issue, raise it back to INFO.

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
- **The dashboard BLE-TX "off" option DOES do the full deinit** the auto-off
  supervisor avoids. The BLE TX-power dropdown has an `off` entry → `POST
  /txpower?ble=off` → `ble_backend::power_off()` → `NimBLEDevice::deinit(true)`
  (host + controller). It's the explicit "I really want BLE gone" switch, so it
  accepts the consequences the auto-off path won't: ble_httpd + clone are torn
  down, and there is **no live re-enable** — like WiFi-off (`?wifi=0`) it is
  runtime-only and **a reboot brings BLE back** at the stored dBm (so it can't
  lock you out the way it can't brick). `powered_off()` latches the state;
  `build_txpower_json` surfaces it as `"ble_off":true` (the dropdown shows
  "off"); the auto-off supervisor early-`continue`s on it so it never touches
  the freed singletons. The dashboard `confirm()`s before sending it.

## OTA over a loaded device — quiesce the radio during the upload

- **A firmware upload competes for the single radio and gets starved.** On a
  busy NAT build (`/update` POST of ~1.3 MB) the SoftAP forward path + BLE scan
  + coex leave so little airtime for the upload's TCP stream that it crawls to a
  few KB/s and times out — the same SA-Query/heap death-spiral as above, now
  triggered by the OTA's own traffic. Symptom: `curl --data-binary @fw.bin …`
  uploads ~80–90% then `(28) timed out`. (An aborted OTA is **harmless** — it
  writes the *inactive* slot and only switches on a fully-validated image, so
  the device keeps running the old firmware.)
- **Fix: the OTA handler frees the radio for the transfer's duration.**
  `ota::set_quiesce_hooks(begin, end)` (wired in `main.cpp`, same provider-
  callback pattern as the stats providers so `ota` keeps no dep on
  `nat_router`/`ble_backend`). `begin` fires once the upload starts:
  `nat_router::pause_for_ota()` drops the SoftAP via `ap_down()` (**STA link
  stays up** — that's what carries the upload + dashboard, so the OTA survives
  it) and `ble_backend::quiesce_for_ota()` pauses the central scan + suspends
  advertising. `end` fires **only on failure** to restore both (success
  reboots, which restores everything). Two gotchas: (1) the BLE auto-off
  supervisor would re-resume the scan within a tick ("central needed" — an API
  client is attached during OTA), so `quiesce_for_ota` sets a flag the
  supervisor early-`continue`s on; (2) `pause_for_ota` only restores the AP if
  it had been up (`g_ota_resume_ap`) — don't resurrect a user-disabled SoftAP.
- **Manual recovery / forced quiesce** if a device is already too saturated to
  flash: `POST /nat?enabled=0` sheds the SoftAP load live (verified: heap
  20→77 KB, temp 57→44 °C, page 60 s→0.7 s in seconds), then OTA flies. Re-enable
  with `?enabled=1` (NVS-persisted, so it also survives reboot — remember to turn
  it back on). Diagnose the quiesce live (non-destructively) by POSTing a short
  bogus image: it fails validation without rebooting and leaves `OTA: dropping
  SoftAP` / `scan paused` / `restoring SoftAP` in `/log`.

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

## NAPT table size — lwIP asserts if it can't allocate it (ESP32-C3 boot loop)

- On the first `esp_netif_napt_enable`, lwIP `mem_calloc`s the whole NAPT
  table: `IP_NAPT_MAX` × 32 B. The default of 512 is 16 KB contiguous.
  `ip_napt_init` **asserts** on failure instead of returning an error, so a
  device that can't spare the block panics and then boot-loops, because the
  SoftAP comes back up from NVS on every boot. Issue #2: v0.2.0 on an
  ESP32-C3 (WiFi + BLE + httpd + api_server leave no 16 KB block).
- Fixed in 0.2.1 in two ways. First, `CONFIG_NBP_NAT_TABLE_SIZE` (128 on
  C3/C2, 512 elsewhere) is passed to the lwip target as `-DIP_NAPT_MAX` in
  the root `CMakeLists.txt`. When the table is full, lwIP evicts the oldest
  flow. Second, `nat_router::ap_up()` checks the heap before every enable
  (`napt_heap_ok`) and leaves the radio STA-only if the check fails. Verify
  the define with `compile_commands.json` (look for `-DIP_NAPT_MAX=` on
  `ip4_napt.c`). The mirrored entry size (32 B) must be re-checked on an IDF
  bump.
- **The C3 needs its own feature profile, `sdkconfig.defaults.esp32c3`.**
  IDF loads `<defaults>.<target>` automatically. With the full default set,
  v0.2.1 on a C3 booted with 16 KB free and a 7.5 KB largest block, so every
  api_server connection (8 KB stack) failed with `xTaskCreate … failed`.
  On the C3, IRAM and DRAM share one SRAM, so IRAM code is lost heap. The
  overlay moves WiFi hot paths to flash (+19.4 KB) and turns off clone, NAT
  and the WS bridge (+6.2 KB static). The 12 KB clone-supervisor stack is
  not a real saving on that board: its `xTaskCreate` had silently failed
  there (now logged). Clone-free builds take the SMP passkey via
  `POST /passkey?val=N`. The no-BLE overlay turns NAT back on for the C3
  BLE-off image.
  The next lever is `BT_CTRL_RUN_IN_FLASH_ONLY` (+22.3 KB). It is held back
  because BLE may misbehave during flash erase. `/stats.json` reports
  `heap_largest` and `heap_min`, and the boot-complete log line prints both.
  Watch `largest`: it must stay above 8 KB.
- **lwIP frees the tables on every disable.** `ip_napt_enable_netif(…, 0)`
  calls `ip_napt_deinit()` when no other netif has NAPT on, so every
  `ap_down()` frees them. Every later enable (OTA-failure resume, a
  dashboard re-toggle) allocates again. Don't cache "tables exist" past an
  `ap_down()`.

## SoftAP throughput (NBP_NAT_THROUGHPUT) — a second lwIP hook

- `/nat`'s sibling feature: the dashboard's **repeater throughput chart**
  (down/up KB/s) comes from cumulative `ap_rx`/`ap_tx` byte counters in
  `/stats.json`. Gated on `CONFIG_NBP_NAT_THROUGHPUT` (default y, `depends on
  NBP_NAT_ROUTER`); off ⇒ the whole thing compiles out and lwIP's forward
  path is untouched. The dashboard chart (`#chart3`, `.nat-only` in
  `web/index.html`) self-reveals only when `ap_rx` appears in `/stats.json`,
  so non-router / gate-off builds show nothing.
- **Don't reach for lwIP MIB2 / `netif->mib2_counters` here — it's a dead
  end.** Those `ifinoctets`/`ifoutoctets` fields exist (under a global
  `MIB2_STATS=1`, which itself needs `LWIP_STATS=1` or opt.h force-zeroes it)
  but lwIP core only ever increments them on the loopback/ppp/lowpan paths —
  **never** the ESP32 WiFi ethernet netif, and esp_netif exposes no byte
  counters either. They read a flat 0. (Verified the hard way.)
- Mechanism instead: an **observe-only `LWIP_HOOK_IP4_CANFORWARD` hook**,
  declared alongside the dhcps hook in `nbp_lwip_hooks.h` and implemented in
  `nat_router.cpp` (`nbp_ip4_canforward`, `extern "C"`). `ip4_canforward()`
  calls it for **every** forwarded datagram — both NAT directions, since
  inbound packets are de-NAT'd to the client IP and then re-forwarded. It
  adds `p->tot_len` to a portMUX-guarded up/down tally (classified by whether
  the host-order dest is in the AP subnet, published by `ap_up()`), then
  returns **-1** ("no decision") so the normal forward-eligibility check runs
  unchanged. `get_ap_throughput()` reads the tally; `main.cpp` bridges it into
  `/stats.json` via a `set_nat_throughput_provider` callback (same
  edge-component pattern as the clone-stats provider — `api_server` keeps no
  dependency on `nat_router`).
- **Same IDF-coupling caveats as the dhcps hook.** The counter symbol is
  pulled into the link exactly like `nbp_dhcps_post_state`: lwIP's `ip4.c`
  references it `U`, `nat_router` defines it `T`, IDF's `--start-group`
  resolves the order. On an IDF bump re-verify it still fires:
  `nm build/.../ip4.c.obj | grep nbp_ip4_canforward` should show `U`. The hook
  is injected via the same `ESP_IDF_LWIP_HOOK_FILENAME` wiring (root
  `CMakeLists.txt`, gated on `NBP_NAT_ROUTER`).
- **Gotcha when adding the Kconfig option itself:** a plain `idf.py build`
  did **not** write the new `CONFIG_NBP_NAT_THROUGHPUT` into an already-
  generated `sdkconfig` — it silently compiled the feature out (gate
  undefined → treated as 0). Took an explicit `idf.py reconfigure` to
  materialise the `default y`. Watch for this whenever adding a Kconfig symbol
  to a configured tree.

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
  **As of 2026-11 `~/dev/esp/idf5.5` is gone** and `get_idf` fails; main
  is built with IDF 6.0.3 instead: `export IDF_PYTHON_ENV_PATH=
  ~/.espressif/python_env/idf6.0_py3.14_env; . ~/dev/esp/idf6.0.3/export.sh`
  (the env var is needed because system `python3` is 3.15, which has no
  IDF env).
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

## api_server socket teardown + retained log (v0.2.3, issue #2)

- **Close dead API sockets abortively.** A reaped (send-stalled) HA socket
  closed gracefully stays in FIN-WAIT while lwIP retransmits its unsent data
  for minutes, holding up to `TCP_SND_BUF` (~5.7 KB) of pbufs each. On a C3
  a few stall/reconnect rounds drained the heap to `heap_min` 744 B. Now
  `set_abortive_close()` (SO_LINGER {1,0}) runs before every reap
  `shutdown(SHUT_RDWR)` and before the connection_task's close, except
  after a protocol close (DisconnectResponse / Hello reject), whose last
  frame is still unacked and would be RST away. lwIP only aborts when data
  is still queued, so quiet disconnects still close normally. **Requires `CONFIG_LWIP_SO_LINGER=y`** (pinned in
  `sdkconfig.defaults`; default n, and lwIP silently ignores SO_LINGER
  without it).
- **`CLIENT_SEND_DEADLINE_MS` is 20 s, not 5 s.** Each reap drops HA *and*
  releases that client's BLE links (`connection::release_owner`), so a 5 s
  WiFi stall became a BMS disconnect/reconnect loop (batmon-ha #391). The
  cost: a stalled client can hold `g_tx_mutex`, and so the NimBLE host task,
  for up to 20 s. A broadcast shares one 20 s budget across clients (each
  later client gets at least `CLIENT_SEND_MIN_SHARE_MS`), because a WiFi
  stall stalls every client at once and N × 20 s would exceed peers' 30 s
  ATT timeout. The real fix would be no blocking socket I/O on the host
  task (per-client queue).
- **The disconnect log line says why** (`reason=peer closed | read error |
  send failed | send failed or stalled (reaped) | closed by protocol`, plus peer IP).
  Compare it with the `connection: release …` lines to see which side
  dropped first.
- **The `/log` ring is `__NOINIT_ATTR`** (with a magic word). After any
  reset but power-on, `install_log_hook` re-logs the last 1.5 KB of the
  previous boot under tag `prevboot`, plus `reset reason: …`. To diagnose
  an unexplained reboot, read `/log` after it comes back. Panic/WDT
  backtraces go through `esp_rom_printf`, not the hook, so the ring does
  not hold them; `crash_capture` (next section) records them instead.
- **`NimBLEClient: unknown handle: N` is a dropped notification, not noise —
  don't mute it.** The proxy forwards notifications via
  `chr->setNotifyCallback` on NimBLEClient's own attribute cache
  (`bt_handlers.cpp`); the GAP listener in `ble_backend.cpp` only counts.
  NimBLE-cpp returns before any callback when the handle isn't in that
  cache, so the notification never reaches HA (issue #2, JK BMS handle 18
  during discovery). Open question: why is the handle missing (notifies
  arriving before discovery finishes, or HA skipping discovery with cached
  services)?

## Crash capture: panic info and core dumps over HTTP (`components/crash_capture`)

- **Two layers, both read over HTTP** (no serial needed):
  1. **RTC record** (`CONFIG_NBP_CRASH_CAPTURE`, default y). `-Wl,--wrap=
     esp_panic_handler` runs `__wrap_esp_panic_handler` before IDF's handler:
     it copies reason, abort/assert text, PC, EXCVADDR/MTVAL, cause, task name,
     a 16-entry walked backtrace, a random nonce and the first 8 bytes of the
     ELF SHA (read at panic time, so a panic before `init()` still names its
     image) into `RTC_NOINIT` memory (last 3 panics, ~1 KB RTC slow,
     CRC-checked). The next boot logs new records to `/log` (tag `crash`, IDF
     `Backtrace:` format). Needs no partition-table change. On RISC-V (C3/C6)
     there is no unwinder: entries after mepc/ra are a stack scan for code
     addresses (`stack_scan`).
  2. **ELF core dump to flash** (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`,
     `sdkconfig.defaults`). Partition `coredump` at 0x3E0000 (128 KB, after
     `ota_1`, all older offsets unchanged). **An app OTA never rewrites the
     table at 0x8000**, so devices flashed before it was added have no such
     partition: `crash_capture::init()` then registers the free flash after
     the last partition with `esp_partition_register_external()` (rejects any
     overlap with the running table, bounded by the header flash size) and
     re-runs `esp_core_dump_init()`. Boot shows `esp_core_dump_flash: No core
     dump partition found!` twice (IDF's own init, before app_main), then
     `crash: partition table has no coredump partition; using free flash
     0x3e0000+0x20000`. That is expected.
- **A panic during capture must not cost the records.** The record is built
  in a DRAM scratch copy and committed to RTC last (slot, count, then CRC);
  a nested entry (fault inside capture or inside IDF's handler/core dump, or
  the other core panicking) finishes the commit and flags the record
  `nested`, so the CRC always matches before the reset. Ownership is an
  `esp_cpu_compare_and_set`. The old code bumped `count` before the CRC, and
  a fault in between made the next boot wipe all records silently.
- **The boot reports what it could not capture.** `init()` logs at E level
  when the store failed its CRC (`store_state` `corrupt`), at W when an OTA
  changed the layout (`old_layout`; the version word is `kLayout << 16 |
  sizeof(Store)`, so bump `kLayout` on any Record/Store change), and
  `crash reset (<reason>), no record captured` when `esp_reset_reason()` is a
  panic/WDT/lockup but no new record arrived. `GET /crash` carries
  `last_reset_reason`, `store_was_valid`, `store_state` and
  `crash_without_record`.
- **Core dump ↔ record tie.** `capture()` writes seq/uptime/nonce into
  `g_core_tag` (`COREDUMP_DRAM_ATTR`, so it is in every core; magic
  `NBPCTAG1`) and the nonce into the record. `crash_decode.py` refuses a core
  whose tag is not the newest record's (exit 2: stale, or nonce 0 = a panic
  that recorded nothing) or that has no record to compare against (exit 3);
  `--allow-stale` / `--allow-unverified` print it anyway. PC and task alone
  are not enough: the same bug crashes at the same PC every time. Cores from
  firmware before the tag fall back to task+PC and say so.
- **Endpoints:** `GET /crash` (JSON, chunked, no size cap: records newest
  first, `running_elf`, the boot fields above, `coredump.{region,state,size,
  error}`), `GET /coredump` (raw core, 404 unless its checksum is valid),
  `GET /coredump?raw=1` (the region whatever its state; header
  `X-Coredump-State`), `POST /crash?erase=1` (clears records and the core
  dump). `coredump.state` is `none` (blank), `ok`, `invalid` (header
  unreadable, with the IDF error) or `bad_checksum`; only `none` means there
  is nothing there.
- **Destructive endpoints need the header `X-Crash-Confirm: yes`** (erase and
  crash-test). It is not CORS-safelisted and the server answers no preflight,
  so a page on another origin cannot fire them from a browser. It is not
  authentication: anyone on the LAN can still send it.
- **`POST /crash-test?kind=null|abort` is compiled out by default**
  (`CONFIG_NBP_CRASH_TEST` default n since 2026-11-12). For a test build use
  `-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.crashtest"`.
  Never put a crash-test build on a deployed device; test panics belong in
  QEMU (below). A test panic within `NBP_CLONE_BOOT_GUARD_SECS` of boot trips
  the clone boot guard.
- **Symbolicate:** `scripts/crash_decode.py --host <ip>` (or
  `scripts/crash_decode.py crash.json coredump.bin`) runs addr2line on the
  RTC backtraces and `esp-coredump info_corefile` on the core, picking the ELF
  from `elf-archive/` by SHA. Needs the IDF env (addr2line, esp-coredump) and
  `zstd`. `app_elf_sha256` is the SHA-256 of the ELF file, so every ELF is
  hashed before use: an archive blob that is not the build its metadata
  names, a metadata SHA that differs beyond the 8-hex blob key, or an
  `--elf` that is not the crashed build all fail (exit 2) instead of
  decoding garbage. **Archive the ELF of every image you flash**: `idf.py
  flash` does it via `idf_ext.py`; after an OTA run `python3
  etc/idf-devtools/elf_archive.py archive <device> --method ota --build-dir
  <dir>`. `esp-coredump` only decodes against the exact ELF
  (`CONFIG_APP_RETRIEVE_LEN_ELF_SHA=64`). `elf_archive.py` matches the core's
  SHA as 64 hex chars or 32 raw bytes, and guesses from the flash history only
  when given `--device/--at`, with a warning.
  After a fresh clone: `git submodule update --init etc/idf-devtools`.
- **Build guard:** `components/crash_capture/check_panic_wrap.py` runs after
  every link (deferred `POST_BUILD` on the ELF) and fails the build, deleting
  the ELF, unless the map's cross-reference table shows `panic_handler.c.obj`
  referencing `__wrap_esp_panic_handler` and nothing but crash_capture
  calling the real `esp_panic_handler`. `--wrap` only rewrites references
  between objects, so an IDF refactor would otherwise make the capture dead
  code without a warning. Costs ~0.05 s.
- **`CONFIG_ESP_PANIC_HANDLER_IRAM` is incompatible** (`#error`): the wrapper
  and what it calls run from flash. **Task-WDT expiry is not captured**:
  `CONFIG_ESP_TASK_WDT_PANIC` is off, so the TWDT only prints and no panic
  happens. Turning it on would reboot the device on every TWDT trigger, a
  runtime behaviour change, so it is left off.
- **RAM cost** (IDF 5.5, measured with `idf.py size`): S3 −4.3 KB DIRAM,
  C3 −3.2 KB DRAM vs. both off. Most of it is the core dump (1.8 KB own stack,
  `CONFIG_ESP_COREDUMP_STACK_SIZE=1792`, plus ~1 KB buffers) and ~1.1 KB of
  FreeRTOS data it pulls in. The RTC record costs no heap (RTC slow memory);
  the scratch record adds ~0.4 KB DRAM .bss.
- **QEMU** (`idf.py qemu`, IDF 6.0.3, esp32s3) reaches app_main and is enough to
  test the capture path. IDF 5.5 images hang in QEMU after `coexist:` before
  app_main (unchanged v0.2.3 does too), so they cannot be boot-tested there.
  **Self-test:** build with `idf.py -B build-qemu-st -DNBP_CRASH_SELFTEST=1
  build` (compile define, never a firmware build). Each boot runs one step
  and prints the `/crash` JSON between `SELFTEST-JSON-BEGIN/END n` lines:
  plain panic; fault while committing; fault while building; corrupted CRC +
  restart; panic with capture skipped. Every step logs `SELFTEST step n:
  PASS|FAIL`. Run QEMU without the download strap (`idf.py qemu` adds none;
  `strap_mode=0x07` waits for UART download). IDF logs land in the stdout JSON
  dump, so strip `ESC[…m` log lines before parsing it.

## Heap exhaustion is an error, not an abort (`components/oom_guard`)

- **Without C++ exceptions a failed throwing `new` aborts**, and so does
  `new (std::nothrow)` with stock libstdc++: since GCC 9 the nothrow form is
  `try { return ::operator new(n); } catch (...) { return nullptr; }`, and
  IDF's `__wrap___cxa_allocate_exception` stub aborts before the catch. The
  field crash of 2026-11-12 (`std::make_unique<ServicesEncodeCtx>` in
  `gatt_discovery`, heap_min 244 B) was this; its `if (ctx == nullptr)` was
  dead code. Exceptions stay off (`CONFIG_COMPILER_CXX_EXCEPTIONS=n`): they
  would cost flash and an emergency pool, and NimBLE-cpp is not
  exception-safe anyway.
- **`oom_guard.cpp` replaces the global `operator new` family** (linked
  `WHOLE_ARCHIVE`). Nothrow new/new[] = `malloc`, nullptr on failure. The
  throwing forms, which only library code reaches (NimBLE-cpp `new
  NimBLEClient`/remote attributes, libstdc++ containers), wait up to
  `CONFIG_NBP_OOM_NEW_WAIT_MS` (2 s, 10 ms steps, task context only) for the
  heap to come back, then abort with `OOM: operator new(N) failed after M ms
  wait; free F largest L`, which `/crash` shows. They must never return
  nullptr: callers do not check and GCC assumes non-null.
  `check_new_override.py` fails the build (and deletes the ELF) unless the
  link map shows all four symbols defined by `oom_guard.cpp.obj`.
- **Our code allocates only through `oom_guard::make<T>()` /
  `oom_guard::make_unique<T>()` or `malloc`, and handles nullptr.** No
  `std::string`/`std::vector`/`std::make_unique` in runtime paths; read
  advert fields with `ble_backend/ad_parse.h`, not NimBLE's `std::string`
  getters (per advert, on the host task). Init-time and clone-session
  NimBLE-cpp calls that allocate internally remain (see the commit message).
- **`/stats.json`: `oom`** = failed heap allocations since boot, every caller
  (heap_caps failed-alloc hook), handled or not; `oom_last_size`; and
  `oom_new_waits` = throwing-new calls that waited and then got memory. The
  `oom` log tag reports new failures every 2 s.
- **QEMU test:** `idf.py -B build-qemu-oom -DIDF_TARGET=esp32s3
  -DNBP_OOM_SELFTEST=1 build`, then `idf.py -B build-qemu-oom qemu` (IDF
  6.0.3; it rebuilds first, so edit nothing in between). It runs the real
  api_server on lwIP loopback, takes the heap away for real, and prints
  `OOM-SELFTEST <step>: PASS|FAIL` for GetServices (error -3 reply),
  nothrow new, throwing new recovering after a wait, an API send that
  recovers, and one that fails and reaps the client. The last step must end
  in the `OOM: operator new(2048)` abort. Compile define only, never a
  firmware build.

## Heap budget on the NAT S3 (2026-11, after the GetServices OOM)

- **A stalled API client pins ~25 KiB, not 5.7 KiB.** IDF builds lwIP with
  `LWIP_NETIF_TX_SINGLE_PBUF`, so `tcp_write` allocates a full-MSS pbuf
  (~1.55 KiB measured) for every new segment however small the frame, and
  with `TCP_NODELAY` each notify/advert batch is its own segment until ACKed.
  `TCP_SND_QUEUELEN` (16) is the only bound: QEMU measured 16 × 60 B frames
  to a non-ACKing peer = 24 972 B of heap. `TCP_SND_BUF` (5760 B) counts
  payload only and says nothing about heap.
- **`NBP_API_CLIENT_TXQ_CAP` (8192)** caps it. `send_capped()` in
  `api_server.cpp` measures the heap of the pcb's unsent + unacked segments
  (`heap_caps_get_containing_block_size` per seg/pbuf, read in the tcpip
  thread via `tcpip_api_call`; lwIP has `MEMP_MEM_MALLOC=1`, so all of it is
  heap). At or over the cap, advert batches for that client are dropped
  (they never block the host/flush task any more), other frames wait for the
  queue to drain, and a client over the cap for 20 s is reaped. A healthy LAN
  client holds 1 segment (1.55 KiB). `/stats.json`: `api_txq` ([id, last,
  max] per client), `api_txq_max`, `api_txq_drops`, `api_txq_reaps`,
  `api_refused`.
- **`NBP_API_MAX_CLIENTS` (2)**: each client is ~9.3 KiB at rest (8 KiB task
  stack, TCB, socket; QEMU). A further client is accepted and reset at once
  (`no client slot … refusing`).
- **GetServices sends one service per response** (`api_subset.options`
  services `max_count:1`): the encode ctx is 2936 B instead of 23 376 B. The
  23 KiB contiguous block was exactly the allocation that failed in the field
  OOM of 2026-11-12.
- **`/log` ring `NBP_WEB_CONSOLE_RING_SIZE` 4096** (was 8192, `.noinit`,
  comes out of the heap region). The magic word mixes in the size, so an OTA
  that changes it does not replay a misread ring.
- **The clone supervisor (12 KiB stack) only starts for an enabled target**
  (`ble_clone::start_if_enabled`, also called from `POST /clone`). Before, a
  device with clone compiled in but disabled carried an idle 12 KiB task.
- **Clone vs 2 slots:** NimBLE's connection limit covers every role, and each
  proxy slot keeps its `NimBLEClient`, so once HA has used both slots the
  clone's `createClient()` fails until reboot (it retries with backoff), and a
  clone with a local central connected leaves no link for HA. Raise
  `BT_NIMBLE_MAX_CONNECTIONS` above `proxy::MAX_CONNECTIONS` on a device that
  clones and proxies at once.
- **Stats endpoints register after `ble_backend::start()`**, and every
  `connection::` accessor is null-safe: a `GET /stats.json` in the boot window
  asserted in `free_slots()` (crash 2026-11-12T1519). `/crash` stays early.
- **QEMU:** `idf.py -B build_qemu_heap -DIDF_TARGET=esp32s3
  -DNBP_HEAP_SELFTEST=1 build`, then `… qemu` (IDF 6.0.3). Prints
  `HEAP-SELFTEST <step>: PASS|FAIL` for the boot race, the 3rd-client
  refusal, a stalled client under lossy and non-lossy traffic, and
  `HEAP-MEASURE` numbers. The stall is real: a test-only `LWIP_HOOK_IP4_INPUT`
  (`nbp_lwip_hooks.h`) drops every segment to the stalled client's port, so
  data is sent and never ACKed. Add `CONFIG_NBP_API_CLIENT_TXQ_CAP=0` via an
  extra defaults file to measure the uncapped case.
- **Not changed, deliberately** (send-stall risk), the next levers with
  estimates: `ESP_WIFI_STATIC_RX_BUFFER_NUM` 10→6 (~6.4 KiB permanent);
  `DYNAMIC_TX_BUFFER_NUM` 32→16 and `DYNAMIC_RX_BUFFER_NUM` 32→16 (only
  allocated under load, cut the burst peak by up to ~25 KiB each);
  `TCP_SND_BUF`/`TCP_WND` 5760→2880 (`TCP_SND_QUEUELEN` 16→8, so an
  uncapped socket pins ~12 KiB instead of ~25 KiB). Other levers, not
  tried: `NBP_NAT_TABLE_SIZE` 512→256 (8 KiB, one 16 KiB block → 8 KiB),
  api_client stack 8→6 KiB (QEMU high-water leaves 4.6 KiB free, the OOM
  core 4.8 KiB at GetServices depth; 2 KiB per client), httpd task stack
  (1.1 KiB of 7 KiB used in the core; ~3 KiB), NimBLE `MSYS_2_BLOCK_COUNT`
  24→12 (~3.8 KiB), freeing a proxy slot's remote attribute cache on
  disconnect (a few KiB per peer; needs care: discovery may be walking it).

## Liveness watchdog targets (v0.2.4, issue #2)

- The probe used to hard-code Fab's lab broker `192.168.1.200:1882` as the
  second target. On other LANs it never answered, the gateway (`:80`) was the
  only target, and a slow or filtered gateway rebooted a reporter's C3 every
  ~3 min. Now: gateway `:80`, plus `CONFIG_NBP_LIVENESS_EXTRA_TARGET`
  ("ip:port", empty in the repo; set it in a local sdkconfig for the lab
  repeater). Only a handshake or a RST counts as reachable; local socket
  errors count as failures.
- Frames from an *upstream* API client within 60 s also pass a cycle
  (`api_server::last_rx_us`; loopback = WS bridge and SoftAP-subnet peers
  don't count, they would hide a dead uplink).
- Reboots arm only after a probe target answered once this boot (`"armed"`
  in `GET /liveness`). A liveness reboot carries the armed state across
  `esp_restart` (`__NOINIT` magic), so a wedge that recurs before the first
  probe still reboots. Each carried reboot without a probe success doubles
  the threshold (cap 32x), so a board moved to a LAN whose gateway ignores
  `:80` backs off instead of looping.
- Panic records already survive the reset: `GET /crash` (crash_capture).
  Ask for it before guessing at a `panic` reset reason.
- On-demand GATT discovery (`ensure_discovered`, bt_handlers.cpp) retries
  while a link's last discovery failed part-way (`g_partial`). Known
  pre-existing risk: any `discoverAttributes()` after notifications are on
  (GetServices, or that retry) deletes the service list on the connection
  task while the host task may walk it for a notification.
