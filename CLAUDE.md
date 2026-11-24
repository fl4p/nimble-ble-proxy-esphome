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
  a bare GET just reads. Every such `POST` needs `-H 'X-NBP-Confirm: yes'`
  (400 without it), see "CSRF guards" below.

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
  contention. (2026-11-15: the .231 unit's eFuses say ESP32-S3R8, 8 MB
  octal PSRAM in the package; the default build still leaves it unused, see
  "PSRAM opt-in" below.) Fix was sizing NimBLE to what's actually used — 1 clone
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

- **OTA command:** `curl -H 'X-NBP-Confirm: yes' --data-binary @build/nimble_ble_proxy.bin
  http://<device>/update`. Without the header the device answers 400
  (`needs header X-NBP-Confirm: yes`) and writes nothing; curl still sends
  the whole body first, httpd discards it. Firmware older than the guard
  ignores the header, so the same command works on every version.
  While the running image is still unverified (see "OTA app rollback"
  below) `/update` answers **409** and writes nothing: the other slot holds
  the only image to roll back to. Wait for `GET /ota` `"phase":"valid"`, or
  `POST /ota?mark=valid` first.

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
  SoftAP` / `scan paused` / `restoring SoftAP` in `/log`. Since app rollback
  is on, every `esp_ota_begin` (bogus or not) also erases the inactive
  otadata sector (`esp_ota_invalidate_inactive_ota_data_slot`,
  esp_ota_ops.c:205-209), so the image in the other slot stops being a
  rollback target. Harmless for a verified running image.

## OTA app rollback (`components/ota/ota_verify.cpp`, 2026-11-22)

- **What it does.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` and
  `CONFIG_BOOTLOADER_PROJECT_VER=2` (`sdkconfig.defaults`, every target and
  profile; the C3 overlay and `.github/build-no-ble.defaults` do not touch
  them). An image installed by `esp_ota_set_boot_partition()` from a
  rollback-enabled app is written with otadata state `NEW`
  (esp_ota_ops.c:115-122, :786). `ota_verify::early_init()` (right after
  `crash_capture::init()`, before the provisioning window and the WiFi
  connect, which can block) starts a worker task (3 KiB stack, only while
  unverified; high-water 1476 B free on .231 with the real network hooks)
  and an esp_timer backstop. The image
  marks itself valid (`esp_ota_mark_app_valid_cancel_rollback`) once, in one
  evaluation: uptime >= `NBP_OTA_VERIFY_MIN_SECS` (60) AND STA has an IP AND
  an uplink proof (a liveness cycle passed this boot, an upstream API
  session >= 30 s old with ACKed sends, the gateway answered :80 with a
  connect or RST, or the gateway is in the ARP table after that attempt; DHCP
  with no router counts as proof too) AND 127.0.0.1:80 and :6053 accept.
  Unmet at `NBP_OTA_VERIFY_DEADLINE_SECS` (900): `request_rollback()`
  records the cause (NVS `exp`, RTC `req_cause`) and restarts; it does not
  call IDF's rollback itself, which needs the RSA stack (see "App image
  signing", Stack). A rollback bootloader then aborts the pending image
  (`last_rollback.from_state` `aborted`, cause `deadline`); under an old
  bootloader the next boot's `early_init` sees `req_cause` and runs
  `esp_ota_mark_app_invalid_rollback_and_reboot` on the main task (`invalid`).
  `POST /ota?mark=invalid` takes the same path. Backstop: 60 s after the deadline a still-pending
  image is `esp_restart()`ed (a stuck worker). The ARP proof is what keeps a
  LAN whose gateway drops :80 (the liveness case) from rolling back good
  images. Builds without WiFi wire null hooks: uptime only.
- **The clocks are boot-relative.** `esp_timer_get_time()` is NOT reset by
  `esp_restart()` (QEMU S3: app_main at 16 s on the boot after a SW reset),
  so `uptime_s()` subtracts the value captured in `early_init()`. A deadline
  on raw esp_timer time fired immediately on the next boot.
- **Bootloaders in the field have no rollback, and an app OTA never
  replaces the bootloader.** IDF 6.0.3 (and 5.5.4, same lines) with rollback
  off: the bootloader neither turns `NEW` into `PENDING_VERIFY`
  (bootloader_utility.c:445-451) nor `PENDING_VERIFY` into `ABORTED` (:395-404,
  both `#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). It boots a `NEW` image
  like a valid one (bootloader_common_loader.c:78-86 treats only
  `INVALID`/`ABORTED`/blank as invalid) and keeps booting it after every
  reset. What still works there: `esp_ota_get_state_partition` reports `NEW`
  (esp_ota_ops.c:1198-1230, reads otadata as is);
  `esp_ota_mark_app_valid_cancel_rollback` writes `VALID` (:1095-1112, no
  bootloader involvement); `esp_ota_mark_app_invalid_rollback_and_reboot`
  writes `INVALID` and the old bootloader honours it, because
  `bootloader_common_get_active_otadata` (unconditional,
  bootloader_utility.c:426) skips `INVALID` entries. What does not: rollback on
  a crash/reset (bootloader-only). Covered in the app instead: an unverified
  `NEW` image counts its own starts in `RTC_NOINIT` (power-on resets the
  count) and rolls back at `NBP_OTA_VERIFY_MAX_BOOTS` (3). Not covered: a crash
  before app_main, which then loops (as before this feature). `esp_ota_begin`
  refuses only `PENDING_VERIFY` (esp_ota_ops.c:166-171), not `NEW`, so
  `/update` refuses itself while unverified.
- **The first rollback-capable image is not protected.** It is installed
  by an app without rollback, which writes state `UNDEFINED`
  (`set_new_state_otadata`), so it boots as verified. Only updates *from* it
  are verified.
- **Which bootloader?** Observed on the first boot of an image (`PENDING_
  VERIFY` = rollback bootloader, `NEW` = none), else from the bootloader's
  `esp_bootloader_desc_t.version` (`esp_ota_get_bootloader_description`):
  2 = built with rollback, 1 = every earlier build of this project (IDF
  default). Boot log: `ota_verify: running ota_1, otadata state
  pending_verify; bootloader v2 (v6.0.3), rollback yes (observed)`.
- **A rollback is recorded.** `/update` (`note_update`) and every unverified
  boot write NVS `otarb/exp` (slot, ELF SHA prefix, version, cause). A boot
  that runs another slot than `exp` names logs `ROLLED BACK from ota_0
  (0.2.9, elf …): <cause>` and keeps it in `otarb/rb`. Causes:
  `reset_while_pending` (bootloader `ABORTED` it; `reset_reason` says why,
  e.g. `panic`), `deadline`, `unverified_starts`, `manual`,
  `backstop_restart`, `not_booted` (the bootloader skipped the image),
  `marked_invalid`.
- **Endpoints.** `GET /ota`: `running`, `state`, `phase`
  (`pending|valid|rolling_back|not_pending`), `bootloader{ver,idf,rollback,
  source}`, `verify{uptime_s,min_s,deadline_s,left_s,starts,hooks,sta,uplink,
  via,service,valid_at_s,valid_via}`, `other{label,state,version,elf}`,
  `last_rollback{this_boot,from,version,elf,cause,from_state,reset_reason}`.
  The same object is `"ota"` in `GET /crash`. `POST /ota?mark=valid|invalid`
  (`X-NBP-Confirm: yes`; 409 unless pending) for manual tests. A pending image
  with no valid other slot (`esp_ota_check_rollback_is_possible` false) is
  accepted at once (`valid_via: no_rollback_target`). That check runs once, in
  `early_init`; when a later rollback attempt fails (IDF re-runs the check,
  an RSA verify that allocates heap and can fail under exactly the low heap
  that failed the criterion) the image is NOT accepted: `rollback()`
  restarts, so a rollback bootloader aborts the pending image and an old
  bootloader counts one more start. Only a boot-time start-counter rollback
  that keeps failing past `MAX_BOOTS + 2` starts keeps the image
  (`valid_via: rollback_failed`), so it cannot loop. Review 2026-11-23: the
  first OTA after a USB flash IS protected: every bootloader writes
  `otadata[0]` on the first boot of a flashed image (`set_actual_ota_seq`,
  `bootloader_utility.c:501`, not rollback-gated), so the OTA leaves two
  valid entries (QEMU self-test starts from a serial flash). The dashboard
  install follows `GET /ota` until `valid` or a rollback and explains the 409.
- **Any reset of a pending image on a rollback bootloader is a rollback**,
  including `/reboot`, a liveness reboot or a power cut in the first minute.
  That is IDF's design; retry the update.
- **RAM cost** (`idf.py size`, IDF 6.0.3, vs 3dd7a29): S3 DIRAM +224 B
  (.bss +192, .data +32), RTC slow +16 B, flash +9.5 KB; C3 DRAM +216 B,
  RTC +16 B, flash +11 KB. Bootloader +80 B (S3) / +128 B (C3); the ESP32
  bootloader is 0x66c0 of 0x7000 (8 % free). Steady state on a verified
  image: no task, no timer.
- **QEMU test** (S3, IDF 6.0.3): `idf.py -B build-qemu-ota -DIDF_TARGET=esp32s3
  -DNBP_OTA_SELFTEST=1 build`, then `… qemu` (~2 min, kill it after `OTA-SELFTEST
  done`). It adds `sdkconfig.defaults.otatest` (min 3 s, deadline 20 s), and
  each boot prints `OTA-SELFTEST <step>: PASS|FAIL`: a real OTA install of a
  copy of itself, `first_boot` + `bootloader_observed`,
  `valid_after_criterion`, `rollback_on_panic`, `rollback_on_deadline`.
  **Old-bootloader run:** take the bootloader of a build without rollback
  (main before this change, or `sdkconfig.defaults.oldbl`), merge a fresh
  image (`cd build-qemu-ota && python -m esptool --chip=esp32s3 merge-bin
  --output=/tmp/f.bin --pad-to-size=4MB @flash_args`), overwrite 0x0..0x8000
  with 0xFF + that bootloader.bin, and run `idf.py … qemu --flash-file
  /tmp/f.bin`. Use a fresh merge: QEMU writes to the flash file, so an old
  `qemu_flash.bin` carries otadata from the last run. Expected there: state
  `new`, rollback on panic after 3 starts (`unverified_starts`), deadline
  rollback honoured. **Known-bad:** `-DNBP_OTA_SELFTEST_BREAK=1` (mark-valid
  writes nothing) fails `valid_after_criterion`. Host test of the policy:
  `components/ota/host_test/test_ota_verify_policy.cpp` (build line inside).
  `esp_image_verify()` on the running partition hung in QEMU; the self-test
  uses `esp_image_get_metadata()` for the image length.
- **Not verified on hardware yet:** the ARP/gateway proof, the loopback
  checks against the real listeners, and the `/update` 409.

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

## Improv Wi-Fi over BLE (`NBP_IMPROV`, `components/improv_ble`)

- **Lives only in the AP provisioning window.** `wifi_sta::enter_ap_provisioning`
  blocks `app_main` before `ble_backend::start()`, so the `entered` hook inits
  NimBLE itself with just the Improv service. Nothing is allocated, advertised
  or started in normal mode (static cost < 100 B; flash 12 KB, 16.5 KB on C3,
  measured IDF 6.0.3 vs main 3dd7a29 with the same `dependencies.lock`).
  Every exit is a reboot, so there is no teardown path.
- **One radio owner: the provisioning loop.** The GATT write callback only
  queues (`wifi_sta::request_join` / `request_scan`, one in-flight slot
  `g_req_busy`); the loop runs the join or scan between its saved-network
  retries. The SoftAP form takes the same slot, so a form submit and a BLE
  join cannot race; `g_prov_closed` refuses both once the saved network is
  back or a join succeeded. A BLE join switches AP to APSTA and restores the
  previous STA config and mode when it fails.
- **Results never exceed one notification on the requester's MTU** (clients
  parse the notification as is). A device-info that does not fit answers
  `ERR_UNKNOWN`; scan results keep whole triples; a URL that does not fit is
  dropped. The device also starts an MTU exchange on connect.
- **Tests:** `components/improv_ble/host_test/test_improv_proto.cpp`
  (assembler incl. the 2026-09-17 Codex review's continuation and staleness
  cases) and `scripts/test_improv_client.py` (cross-check against
  py-improv-ble-client). On a device: `scripts/improv_provision.py`.

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

## Static SoftAP DHCP leases — same hook, plus a second parse

- IDF's DHCP server has no reservations: it assigns in join order, so the
  NAT S3's fixed-IP port-forwards (fry/flat telnet + scope) pointed at the
  wrong board after reboots (2026-11-13: flat got .2, fry .3/.4). Leases
  are configured with `POST /nat?lease_mac=…&lease_ip=…` (`&lease_del=1`
  removes; header `X-Nat-Confirm: yes`, see Crash capture), persisted in NVS `nat`/`leases`, listed in `GET /nat` `leases`.
  The .231 table: fry `70:04:1d:a6:ab:30` → .2, flat `70:04:1d:a4:ea:34` →
  .4 (WiFi STA MAC = BLE MAC − 2 on both, checked against the lease table).
- Mechanism (`components/dhcps_hostname/dhcps_static.c`): the server first
  looks the client's MAC up in its private IP-sorted lease list and the
  allocator skips every IP in that list, so an always-present entry
  (static MAC, static IP, lease timer `UINT32_MAX`) IS a static lease. The
  `LWIP_HOOK_DHCPS_POST_STATE` macro expands inside `handle_dhcp()` and
  uses its locals `dhcps`/`tlen` and the file-static `parse_msg()`: after
  each parse it pins the entries (dropping a static MAC's other entry and
  anyone else holding a reserved IP) and, if this packet was answered from
  the unpinned list, parses it once more. Pinning per packet also survives
  `dhcps_stop/start` (every `set_ap_dns()`), RELEASE and expiry.
- **IDF-coupled**: mirrors the private `list_node`, relies on `struct
  dhcps_pool` and the local names `dhcps`, `tlen`, `parse_msg` (a rename is
  a compile error, not a silent no-op). Same in IDF 5.5 and 6.0.3. Verify
  with `nm …/dhcpserver.c.obj | grep nbp_dhcps_static` (`U`).
- A change deauths associated clients whose address now disagrees
  (`kick_mismatched`), else they keep it until renewal (half the 120 min
  lease). The IP must lie in the derived pool (.2 .. .101).
- **QEMU:** `idf.py -B build-qemu-dhcps -DIDF_TARGET=esp32s3
  -DNBP_DHCPS_SELFTEST=1 build`, then `… qemu` (IDF 6.0.3). Runs the real
  server on lwIP loopback and prints `DHCPS-SELFTEST <step>: PASS|FAIL`
  (join order, INIT-REBOOT NAK, renew of a reserved IP, server restart,
  RELEASE, table cleared).

## WiFi disconnect reasons (`wifi_sta`)

- `WIFI_EVENT_STA_DISCONNECTED` is logged under tag `wifi_ev` (the `wifi`
  tag is held at WARN for the driver) with reason code + name, the RSSI the
  event carries and the BSSID; `STA back after N ms (M failed reconnect
  attempts)` on the next IP. Failed retries log only on a reason change and
  every 10th attempt. SoftAP `AP_STADISCONNECTED` logs reason + MAC, and
  joins log the MAC.
- `/stats.json`: `sta_disc`, `sta_disc_reason`, `sta_disc_rssi`,
  `sta_disc_down_ms`, `ap_disc`, `ap_disc_reason`, `ap_disc_mac`. `GET
  /wifidisc`: the last 8 events with names and uptime stamps (160 B ring),
  since the 4 KB `/log` ring rolls over within minutes.

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

## BLE bonding (NBP_SMP) — two paths, and why the second one exists

- The proxy can bond as a **central**: `NimBLEDevice::setSecurityAuth(bond,
  mitm, sc)` + `IO_CAP=KEYBOARD_ONLY`, with a static passkey injected from
  `ClientCb::onPassKeyEntry` (`/bond?passkey=`, NVS `stats/ble_passkey`).
  `CONFIG_BT_NIMBLE_NVS_PERSIST` keeps the resulting bond across reboots.
- **Path 1 — HA asks.** `BT_PROXY_FEATURE_FLAGS` now sets bit 3 (`PAIRING`,
  gated on `CONFIG_NBP_SMP`) and bit 4 (`CACHE_CLEARING`), so
  `bleak-esphome` lets `BleakClient.pair()` through as
  `BLUETOOTH_DEVICE_REQUEST_TYPE_PAIR`. `bt_handlers` answers with
  `BluetoothDevicePairingResponse` (85) / `…UnpairingResponse` (86) /
  `…ClearCacheResponse` (88). CLEAR_CACHE is a truthful no-op — we
  re-discover on every connect, so "cache cleared" is already the case.
  **The ESPHome protocol carries no passkey**, which is the one real gap
  vs a local BlueZ adapter: the PIN has to be on the device beforehand.
- **Path 2 — the auto-bond list, and it is the one that matters.** HA only
  sends PAIR *after* a successful connect **and** service discovery. Peers
  that refuse discovery on an unauthenticated link (Felicity/SolarB packs,
  batmon-ha#415) drop the connection before that, so path 1 can never
  trigger for exactly the devices that need it. For an address on the
  auto-bond list (`/bond?add=MAC`, NVS `stats/auto_bond`, cap
  `AUTO_BOND_MAX` = `CONFIG_BT_NIMBLE_MAX_BONDS` = 4) `ClientCb::onConnect`
  **withholds the ConnectionResponse**, runs SMP, and only then tells HA the
  link is up — so discovery happens over an encrypted link. A failed bond
  still reports `connected=true` (the link is real, just unauthenticated):
  that keeps HA's retry behaviour identical to a build without the list.
- **`secureConnection()` blocks with no timeout of its own** (it waits for
  `BLE_GAP_EVENT_ENC_CHANGE` or a disconnect — bounded in practice by the
  30 s SMP timeout). It must not run on the api_server client task (HA's
  pings would stall) nor on the NimBLE host task (which delivers the very
  event it waits for), so it runs on a lazily-created `ble_bond` worker
  (3 KB stack, spawned on first use, deletes itself after 30 s idle, so the
  stack is heap only while bonding happens; creation, enqueue and idle exit
  are serialised by `g_bond_start`). A failed `xTaskCreate` falls back to
  reporting the link unauthenticated / answering PAIR with -102. The async variant
  (`secureConnection(true)`) is NOT used: NimBLE-cpp only calls
  `onAuthenticationComplete` on **success**, so async gives no failure edge.
- **Lock ordering: never call into NimBLE while holding `connection.cpp`'s
  `g_mutex`.** `getConnInfo()`/`ble_gap_conn_find` take the `ble_hs` lock,
  and the host task calls our callbacks; taking them in the other order
  inverts. `onConnect` therefore reads `getMTU()` + `getConnInfo()` *before*
  the mutex, and `pair()` snapshots the client pointer, releases, then asks.
- **Slot recycling is handled with a generation counter.** A bonding job
  carries `{slot, gen}`; `alloc_locked()` bumps `Slot::gen`. A job that
  finishes after the peer dropped sees the mismatch (or `state != Connected`)
  and stays silent, so HA never gets two connect results for one attempt —
  `onDisconnect` already sent `connected=false` in that case.
- **Ownership (merged onto per-client links, 2026-11).** PAIR is
  owner-only like every other per-link request (`pair(addr, owner, cb)`;
  another client's link answers `paired=false, error=-102`), and the
  pairing, unpairing and clear-cache responses go to the requester alone
  (`send_to`). The auto-bond's deferred connect result goes to the slot's
  owner at finish time; if the owner left, `release_owner` has already moved
  the slot to Disconnecting and the job stays silent. While a result is
  withheld, a re-CONNECT from the owner answers "in progress"
  (`OwnInProgress`), not `connected=true`. UNPAIR has no owner check: the
  bond store is device-wide. When a job finishes on a slot that was freed
  under it, `bond_finish` fires the free-change callback, because only then
  does the slot count as free again. No connection slot was added: bonding
  runs on the proxy's 2 existing slots.
- **RAM cost** (IDF 6.0.3, `idf.py size` vs main 3f5d6ad): +64 B static
  `.bss` on both S3 (DIRAM) and C3 (DRAM). NBP_SMP's NimBLE SM/store was
  already on in main, so the feature adds no NimBLE RAM. Heap: the job
  queue and start mutex (~0.3 KB) from `connection::init()`; the worker
  stack (3 KB + TCB) only while a bond is in progress and for 30 s after.
  Under the ~2 KB threshold, so not Kconfig-gated beyond `NBP_SMP`.
- **`POST /bond` needs `X-Bond-Confirm: yes`** over HTTP (same CSRF guard as
  `X-Nat-Confirm`); the dashboard and `scripts/test_settings.py` send it. The
  BLE dashboard transport carries no headers and is not gated.
- `NimBLEDevice::isBonded()/deleteBond()` match on the **full NimBLEAddress
  including type**, but HA only hands us 48 bits (and the adv address type
  isn't necessarily the identity type in the store). `find_bond()` enumerates
  `getNumBonds()`/`getBondedAddress(i)` and compares the 48 bits instead.
- **The bond store is a host facility — guard every read on
  `ble_backend::powered_off()`.** `POST /txpower?ble=off` does a full
  `NimBLEDevice::deinit(true)`, so `getNumBonds()` afterwards walks a
  torn-down `ble_hs`. The dashboard polls `/bond` every 5 s, which makes this
  a *reliable* abort rather than a rare one. `find_bond`, `bonded_addresses`
  and `pair()` all check first; `/bond` surfaces `"ble_off":true` so the panel
  can say why the list is empty instead of implying the keys were deleted.
  (This is the same guard `build_txpower_json` already used — the pattern is
  called out in `ble_backend.h`'s comment on `powered_off()`.)
- **A slot with a bonding job in flight is not allocatable.** The worker
  holds the slot's `NimBLEClient*` across `secureConnection()`, and slots
  *reuse* their client object — so re-tenanting the slot in that window would
  run SMP against the wrong peer. `alloc_locked()`/`free_slots()` share
  `slot_available_locked()` (`state == Free && !bond_busy`), and
  `onDisconnect`/`onConnectFail` deliberately clear only `bond_holds_connect`,
  leaving `bond_busy` to `bond_finish()`. The generation counter alone does
  *not* cover this: it detects a recycled slot after the fact, but by then
  SMP has already run on the wrong link.
- **Sending the deferred connect result is not atomic with the state check.**
  The peer can drop between `bond_finish()` observing `Connected` and the
  worker's `connect_cb(connected=true)` — and `onDisconnect` would then have
  pushed its `connected=false` *first*, leaving HA holding a link that no
  longer exists. The worker re-checks with `job_link_live()` after the send
  and emits a corrective `connected=false`. A duplicate disconnect is
  harmless; a phantom connection is not.

## WebSocket bridge (NBP_WS_PROXY)

- Gated off by default. When on, `GET ws://<host>/api` tunnels the plaintext
  aioesphomeapi protocol to the local api_server over a 127.0.0.1:6053 loopback.
  A browser JS client must treat inbound WS data as a **continuous ESPHome byte
  stream** (device→browser is chunked at arbitrary boundaries; one WS frame ≠
  one ESPHome message).
- **The handshake checks `Origin` (2026-11-13).** Browsers apply no CORS to
  WebSockets, so before it any page open on the LAN could open the bridge and
  drive GATT writes and PAIR/UNPAIR. Rules (`components/ws_proxy/origin_check.h`):
  no/empty `Origin` → accept (aioesphomeapi, curl); `Origin` host[:port] ==
  `Host` → accept (on-device dashboard; default port and case ignored);
  exact entry in `CONFIG_NBP_WS_ORIGIN_ALLOWLIST` (default empty — the
  dashboard does not use the bridge, no off-device page does) → accept;
  anything else, `null` included, → 403 before the 101, `ws_origin_refused`
  in `/stats.json`. UNPAIR over the native TCP API with no ownership check is
  stock ESPHome behaviour and stays. Not covered: DNS rebinding.
- **esp_http_server no longer calls the URI handler for the handshake GET**
  (IDF commit 222d470312, 2026-02; in release/v5.5 and v6.0.3, not in tag
  v5.5.3). The old `if (req->method == HTTP_GET)` set-up branch therefore
  never ran: the first data frame found no session and closed the socket, so
  the bridge was dead on current IDF builds (read from the IDF source, not
  run on hardware). The checks now live in
  `ws_pre_handshake_cb` (Origin, session cap → 503) and the bridge set-up in
  `ws_post_handshake_cb`; NBP_WS_PROXY selects both
  `HTTPD_WS_{PRE,POST}_HANDSHAKE_CB_SUPPORT` and ws_proxy.cpp `#error`s
  without them. A refusal returns non-OK from the pre-callback, so nothing is
  allocated and httpd closes the socket.
- **QEMU test:** `idf.py -B build-qemu-ws -DIDF_TARGET=esp32s3
  -DNBP_WS_SELFTEST=1 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wstest"
  build`, then `… qemu`. Prints `WS-SELFTEST <step>: PASS|FAIL`: forged,
  null, look-alike and port-mismatch origins get 403 and close; no-origin and
  same-origin get 101 and a real HelloResponse through the bridge; the third
  bridge gets 503; 90 refused handshakes cost no heap or task; POST without
  `X-NBP-Confirm` gets 400. Host test of the policy alone:
  `components/ws_proxy/host_test/test_origin_check.cpp` (build line inside).

## CSRF guards on the HTTP surface (2026-11-13)

- **Every state-changing `POST` needs a non-safelisted header.** `/level`,
  `/txpower`, `/cpufreq`, `/scan`, `/advitvl`, `/wifips`, `/hostname`,
  `/passkey`, `/liveness`, `/trace`, `/reboot`, `/clone`, `/update` need
  `X-NBP-Confirm: yes` (`include/http_confirm.h`, one shared header for all
  new guards); `/nat`, `/portmap` (`X-Nat-Confirm`), `/bond`
  (`X-Bond-Confirm`), `/crash`, `/crash-test` (`X-Crash-Confirm`) keep their
  older headers. A cross-origin page cannot send a custom header without a
  preflight and httpd answers no `OPTIONS`. Not authentication.
- Callers that send it: `web/index.html` `confirmHeaders()` (every POST),
  `scripts/test_settings.py` `http()`, `scripts/ant-probe/capture_*.py`. The
  BLE transport (`ble_httpd`) carries no headers and is not gated.
- **Left unguarded:** `POST /wifi` on the SoftAP provisioning httpd (open AP,
  plain HTML form for JS-less captive-portal views; anyone on that AP owns
  the device anyway) and the test-only raw-socket `/bench`
  (`CONFIG_NBP_BENCH`, discards the body).

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
- **No socket I/O on the NimBLE host task (TX ring + `api_tx`).** Every
  frame to a client is encoded once into `g_tx_buf` (`g_tx_mutex`) and copied
  whole into that client's static ring (`NBP_API_CLIENT_TX_RING`, 4096; 3296
  on the C3, the minimum). The `api_tx` task (prio 6, 2.5 KiB stack; QEMU
  high-water ~1.7 KiB incl. the no-memory log, 868 B free on the S3 and
  1416 B on the C3 in QEMU on 2026-11-17) drains every ring with
  `MSG_DONTWAIT` `sendmsg()`, many frames per call, gated by the heap cap
  below. Ring admission has four levels: advert batches up to half the
  ring (dropped beyond); GATT notifications up to `RING - RING_CTRL_RESERVE`
  (160 B, four max-size control frames), **dropped beyond, client kept**
  (`api_notify_drops`, see "API send path on the C3"); frames from the
  client's **own** connection task (handshake, GetServices, GATT replies) up
  to the notify limit minus `RING_RESERVE` (two max-size notifies, 1076 B)
  and otherwise they wait; everything else (connection states,
  ConnectionsFree, GATT errors and pairing replies from the host task, other
  clients' broadcasts) up to the whole ring, else the client is marked
  reaped (`TX ring full`). The
  reserve is what keeps a GetServices burst on link 2 from reaping the
  client over link 1's notifies (batmon #391); it holds two notifies, so it
  assumes `api_tx` gets data into lwIP at least every ~100 ms while notifies
  run at max size. A waiting producer holds no lock and no copy: it releases
  `g_tx_mutex`, waits for room (≤ 20 s), then **calls the encoder again**,
  so every `EncodeFn` must be a pure function of its ctx. While it waits,
  the slot is marked `bulk_waiting`: advert batches for that client are
  refused (counted in `api_txq_drops`) and `api_tx` skips its coalescing
  hold, so the ring actually empties. On the C3 a 2060 B frame fits only
  an (almost) empty ring (bulk limit 3296 − 160 − 1076 = 2060 B); without the mark,
  adverts kept it non-empty and the waiter reaped a healthy client at the
  20 s deadline. A connection task never waits on another client's ring. Producers never `shutdown()`: they
  mark the slot and `api_tx` does the abortive shutdown and discards the
  ring. A ring that holds data and moves nothing for `CLIENT_SEND_DEADLINE_MS`
  (20 s; 5 s caused the batmon-ha #391 reconnect loop) is reaped by
  `api_tx`; the clock starts when data arrives after an idle spell (it once
  started at the last progress before the idle, which reaped at once). With
  `MSG_DONTWAIT` lwIP reports a failed pbuf allocation as `EWOULDBLOCK`; the
  `no memory` log fires when the queue query showed room and slots but the
  send still would-block (once per episode, ≤ 1 per 5 s). Teardown:
  `unregister_client` marks the slot retired and waits for `api_tx`'s
  per-slot `io_busy`, so a reissued fd never gets the old client's bytes
  (QEMU `fd_reuse`; the ring is reset at reap, unregister and register, and
  any one of the three alone passes it). Coalescing: with one segment in
  flight and < 1 MSS queued, `api_tx` holds data up to 20 ms
  (`TX_COALESCE_MS`); with `TX_HOLD_SEGS` (2) or more queued in lwIP it
  holds until an ACK frees one or a full MSS has gathered, and then sends
  whole segments only; lwIP's Nagle stays off. API sockets send with a
  536 B MSS (`NBP_API_CLIENT_TX_MSS`). See "API send path on the C3" below.
- **The disconnect log line says why** (`reason=peer closed | read error |
  send failed | TX ring full (reaped) | send stalled 20 s (reaped) | send
  failed (reaped) | closed by protocol`, plus peer IP). A protocol close
  waits up to 2 s for its last frame to leave the ring, then closes
  gracefully (abortively if it did not).
  Compare it with the `connection: release …` lines to see which side
  dropped first.
- **The `/log` ring is `__NOINIT_ATTR`** (with a magic word). After any
  reset but power-on, `install_log_hook` re-logs the last 1.5 KB of the
  previous boot under tag `prevboot`, plus `reset reason: …`. To diagnose
  an unexplained reboot, read `/log` after it comes back. Panic/WDT
  backtraces go through `esp_rom_printf`, not the hook, so the ring does
  not hold them; `crash_capture` (next section) records them instead.
- **Where `notify_rx` goes** (`/stats.json`, 2026-11-14): `notifies`
  (forwarded; includes the clone image's), `notify_unknown` (handle not in
  the link's NimBLE-cpp attribute cache, counted in the GAP listener with
  the same lookup NimBLE-cpp does), `notify_unowned` (link has no owning
  API client) and `notify_no_callback` (the rest: a known characteristic
  with no notify callback, i.e. not subscribed yet, or a clone upstream
  link). Without the clone they sum to `notify_rx` (S3 bench: 2233 =
  2233+0+0+0). The non-zero buckets have not been provoked on hardware yet.
- **BLE link losses (`/stats.json`, 2026-11-14):** `ble_disc` (every
  established link that ended) split into `ble_disc_timeout` (0x208,
  supervision timeout: peer went silent), `ble_disc_remote` (0x213..0x215),
  `ble_disc_local` (0x216, this host closed it), `ble_disc_fail_est`
  (0x23e), `ble_disc_other`; `ble_disc_reason` and `ble_disc_up_ms` of the
  last one; `ble_conn_fail` / `ble_conn_fail_reason` (onConnectFail).
  Reasons are NimBLE codes, decimal (520 = 0x208). The `onDisconnect` log
  line carries `up=Ns`. Counted in `connection.cpp` under a portMUX. S3
  bench: killing the Mac peripheral app gave 531 (remote), an API client
  leaving gave 534 (local); 520 and onConnectFail not provoked on purpose.
- **httpd under low heap (`components/http_guard`).** `-Wl,--wrap=
  httpd_register_uri_handler` puts a trampoline in front of every handler
  (slot table, 56 entries; the handler's own `user_ctx` is swapped back
  into the request): a failing handler logs `GET <uri> failed (<err>);
  free F largest L` (httpd's own line names no URI) and gets SO_LINGER
  {1,0}, so httpd's close is abortive. `http_guard::bulk_ok()` answers 503
  + `Retry-After: 10` for `/`, `/devices` and `/coredump` while free heap
  < 16 KiB or the largest block < 2 KiB; `/stats.json`, `/log`, `/crash`
  stay served. `/coredump` streams through `send_chunk_paced()` (≤ 2
  pbufs queued in lwIP; 128 KiB raw in 2.7 s on the S3 over WiFi).
  Verified on the S3 2026-11-14 (aborted download logs the URI; a build
  with the threshold at 400 KiB returned 503 on all three). A handler
  registered past the 56 slots is registered unwrapped (logged).
- **`NimBLEClient: unknown handle: N` is a dropped notification, not noise —
  don't mute it.** (It is rate-limited in the `/log` hook to one line per
  5 s plus a count of the lines left out; `notify_unknown` counts every
  one.) The proxy forwards notifications via
  `chr->setNotifyCallback` on NimBLEClient's own attribute cache
  (`bt_handlers.cpp`); the GAP listener in `ble_backend.cpp` only counts.
  NimBLE-cpp returns before any callback when the handle isn't in that
  cache, so the notification never reaches HA (issue #2, JK BMS handle 18
  during discovery). Open question: why is the handle missing (notifies
  arriving before discovery finishes, or HA skipping discovery with cached
  services)?

## API send path on the C3 under notify load (2026-11-16, issue #2 / batmon-ha #391)

- **Symptom (v0.2.5, reporter's C3, batmon + two JK BMS, ~15 notifies/s):**
  `send queue full: 9320 B heap (6 segs, 751 B unacked or unsent), cap 8192`
  about every second, then `TX ring full (reaped)` every 20-60 s, both BMS
  links released; heap_min 656, 255 failed 1700 B allocations (WiFi
  buffers), `send … no memory (… largest 1472)`.
- **Cause:** `api_tx` sent each notify as soon as the 20 ms coalescing hold
  ran out, so a notify stream became one TCP segment per notify (or burst),
  and every segment is a full-MSS heap pbuf (`LWIP_NETIF_TX_SINGLE_PBUF`,
  `tcp_out.c` `tcp_pbuf_prealloc`: alloc = max_length): ~1.55 KiB for 40-150
  B. With ACKs ~250 ms late, 6 segments in flight hit the 8 KiB cap with
  < 2 KB of payload, `api_tx` then moved nothing, and the 3136 B ring was the
  only buffer left: ~1.5-2 s of notifies. Any ACK gap longer than that (an
  RTO after a lost ACK; the WiFi RX buffers fail to allocate at this heap
  level) reaped the client. The pbufs themselves pushed the heap to the edge
  and could not be allocated once the largest block was < 1.5 KiB.
- **Fix (api_server.cpp):** (1) with `TX_HOLD_SEGS` (2) or more segments
  queued in lwIP, `api_tx` holds less than an MSS in the ring until an ACK
  frees a segment, and sends only whole segments (a window or cap limit may
  still cut a write short); (2) API sockets send with a 536 B MSS
  (`NBP_API_CLIENT_TX_MSS`, `pcb->mss` lowered in the tcpip thread right
  after `accept()`; lwIP has no TCP_MAXSEG), ~0.65 KiB per segment;
  loopback peers (the WS bridge) keep lwIP's MSS, see below; (3) a write is
  bounded by the pbuf slots left (lwIP fails the whole write past
  `TCP_SND_QUEUELEN`) and by the cap; (4) advert batches are refused while
  the client is `congested` (queue full or no memory) so the ring keeps its
  room for notifies; (5) the "unknown handle" log is rate-limited (above).
- **Evidence (QEMU C3 heap self-test, `stall_strict`, stalled peer, 63 B
  notifies every 50 ms):** before (10a78d6) 55 frames / 2.8 s until the
  reap, 378 B of them in lwIP in 9316 B of heap; after: 141 frames / 7.1 s,
  5760 B in lwIP in 8528 B. The step now fails unless lwIP holds >= 4 KiB
  under an 8 KiB cap; known-bad: the 10a78d6 sender fails it. With the MSS
  clamp off (`NBP_API_CLIENT_TX_MSS=0`) the hold alone passes it too
  (5760 B in 9312 B). Not tested on C3 hardware or against batmon/HA.
- **lwIP receivers and a small MSS:** an lwIP socket queues at most
  `TCP_RECVMBOX_SIZE` (6) segments and refuses the rest until its 250 ms
  timer; 6 x 536 B is less than the 5760 B window, so a slow lwIP reader
  stalls. In QEMU this reaped the `bulk_wait` client (GetServices burst to
  a slow reader); with 16 mbox slots it passed (218 ms burst, 377 ms
  before the change). Hence the loopback exemption in firmware, and
  `sdkconfig.defaults.heaptest` (`TCP_RECVMBOX_SIZE=16`), which the root
  CMakeLists adds to every `-DNBP_HEAP_SELFTEST=1` build: its clients are
  loopback lwIP sockets standing in for Linux ones. It is read when the
  build dir's sdkconfig is created, so use a fresh build dir.

- **v0.2.6 still reaped (reporter's C3, 2026-11-17):** `send queue full:
  7244 B heap (11 segs, 5760 B unacked or unsent), cap 8192, for 1000 ms;
  1494 B wait in its ring`, ~1 s later `TX ring full (reaped); 3037 B
  dropped`; `api_ring_reaps` 15, oom 5 (was 255), heap_min 3356 (was 656).
  Packing worked; the limit hit is `TCP_SND_BUF` (5760 B in flight), not the
  cap. The ring filled at ~1.5 KB/s (1494 B in the first 1000 ms, 1543 B in
  the next 1010 ms), so it bridged ~2 s. Filling 5760 B of in-flight data
  at that rate takes ~3-4 s with (almost) no cumulative ACK, so the ACK
  clock was stopped for **≥ 5 s**, more than one RTO: lwIP's RTO on a LAN
  sits at 3-4 slow ticks (1.5-2 s; `LWIP_TCP_RTO_TIME` 1500 seeds `sv` = 3
  ticks and `m - (sv >> 2)` never lowers it with 0-tick RTT samples,
  `tcp_in.c` RTT estimator; the timer fires on the 500 ms slow tick) and
  doubles per retransmission (`tcp.c` `tcp_slowtmr`,
  `tcp_backoff`), so ≥ 5 s means a lost retransmission too, or a multi-
  second WiFi/coex outage. Not the peer application: the host kernel ACKs
  5.7 KB whatever batmon does. Either way the proxy cannot shorten it.
- **Fix (2026-11-17): drop notifications instead of reaping.** A
  notification that does not fit under `RING - RING_CTRL_RESERVE` is
  dropped (`api_notify_drops` in `/stats.json`, `fd=N ring at notify
  limit: K notifications (B B) dropped, client kept`, once at the start and then at
  most every 5 s per client); frames that must not be lost still reap when
  the whole ring is full. The newest frame is dropped (tail drop): the
  oldest is often half in lwIP and `api_tx` reads the ring outside the lock,
  so evicting old frames needs an index and compaction for no gain; tail
  drop keeps "a frame is queued whole or not at all", so the wire stays
  framed. The `send queue full` log line also prints lwIP's `rto`,
  retransmit count, `cwnd` and peer window, to tell an RTO backoff from a
  closed window in the next field log. QEMU `notify_stall`: 2 s stall
  under ~6 KB/s of notifies, S kept, 36 notifies dropped, connection state
  and PingResponse delivered, stream framed and in order; known-bad (the
  0.2.6 sender): S reaped, connection state refused, ping reply lost.
- **Not done, deliberately:** a bigger C3 ring (2 s at 2 KB/s = 4 KiB would
  cost 2 × 960 B of static RAM against heap_min 3356 B with 1700 B WiFi RX
  allocations already failing, and still not cover a ≥ 5 s gap) and a lower
  RTO floor (`LWIP_TCP_RTO_TIME` 1000 gives `sv` 2 ticks, first retransmit
  after 0.5-1 s instead of 1-1.5 s; global to every TCP socket and needs a
  hardware check; decide after a field log with the rto/retransmit fields).

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
- **The same guard on the NAT router (2026-11-13): every `POST /nat`
  (enable, AP creds, static leases) and `POST /portmap` needs `X-Nat-Confirm:
  yes`**, else 400. A lease change deauths fry/flat, so a drive-by no-cors
  fetch from any LAN page must not reach it. The dashboard's `apiText()`
  adds the header for those two paths; `ble_httpd` does not route them.
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
- **`NBP_API_CLIENT_TXQ_CAP` (8192; was 16384 on the S3 until the 2026-11-13 ring bench, `docs/bench-api-tx-ring-s3.md`)** caps it. `api_tx` measures the heap
  of the pcb's unsent + unacked segments (`heap_caps_get_containing_block_size`
  per seg/pbuf, read in the tcpip thread via `tcpip_api_call`; lwIP has
  `MEMP_MEM_MALLOC=1`, so all of it is heap) and moves nothing from the
  client's ring into lwIP while it is at the cap (or lwIP has no payload room
  or pbuf slots). Over WiFi a healthy batmon/HA client held 6 one-frame
  segments (9.3 KiB for ~200 B of notifies: ACKs ~250 ms late, .231, before
  the ring); `api_tx` now packs queued frames into one segment. An over-cap
  episode is logged only after 1 s. `/stats.json`: `api_txq` ([id, last,
  max] per client), `api_txq_max`, `api_txq_drops` (advert batches dropped,
  ring half full or the send queue full), `api_tx_mss`, `api_txq_reaps` (20 s stall), `api_ring_reaps` (a frame
  that must not be lost did not fit), `api_ring` ([id, fill, high-water]),
  `api_ring_size`, `api_tx_frames`/`api_tx_sends` (frames per send),
  `api_enq_us_max` (longest send call that did not wait), `api_refused`.
  `api_txq_busy` is always false now.
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
  refusal, a stalled client under lossy and non-lossy traffic, stall
  isolation (host-priority send calls stay short, only the stalled client
  goes), `fd_reuse` (a reissued fd gets no stale bytes), `bulk_wait` (a
  GetServices-sized burst from the client's own connection task, via a
  test-only GetServices address in `bt_handlers`, next to max-size notifies:
  nobody reaped), `bulk_reserve` (the same with W stalled: the waiting burst
  leaves room for exactly two max-size notifies; deterministic),
  `bulk_adverts` (the burst to a slow reader while advert batches are
  broadcast every tick at priority 7; a soak, it also passes without the
  `bulk_waiting` mark on loopback, only slower), `bulk_gate` (deterministic:
  W stalled with a few notifies in its ring and the burst waiting; adverts
  broadcast then must leave W's ring unchanged and still reach B; fails with
  the mark disabled on S3 and C3), `notify_stall` (2 s stall under notify
  load: notifies dropped, client kept, control replies delivered; fails on
  the 0.2.6 sender), coalescing,
  and `HEAP-MEASURE` numbers. Known-bad calibrations (2026-11-13):
  `RING_RESERVE = 0` fails `bulk_reserve` on S3 and C3 (`bulk_wait` is
  timing-dependent: with the reserve at 0 it failed in 3 of 4 runs and passed
  once, on the C3); no ring reset
  fails `fd_reuse`; the old stall clock fails the OOM self-test's
  `send_heap_exhausted` (it idles 21 s first).
- **QEMU on the C3** (`-DIDF_TARGET=esp32c3`, build dir of its own) runs the
  heap self-test too. Needs `qemu-riscv32` (`idf_tools.py install
  qemu-riscv32`; IDF's Python 3.14 failed TLS verification, so the archive
  was fetched with curl into `~/.espressif/dist` first) and its `bin` on
  `PATH` by hand (`export.sh` did not add it). Two self-test-only shims in
  the root `CMakeLists.txt`: QEMU models no ADC, so IDF's boot-time ADC
  self-calibration (a constructor) spins forever in `read_cal_channel`
  before app_main; it is `--wrap`ped to a no-op. And the lwIP input hook is
  wired without the NAT router (the C3 profile has none), with the dhcps
  hook macro gated on `CONFIG_NBP_NAT_ROUTER` in `nbp_lwip_hooks.h`. The stall is real: a test-only `LWIP_HOOK_IP4_INPUT`
  (`nbp_lwip_hooks.h`) drops every segment to the stalled client's port, so
  data is sent and never ACKed. Add `CONFIG_NBP_API_CLIENT_TXQ_CAP=0` via an
  extra defaults file to measure the uncapped case.
- **Not changed, deliberately** (send-stall risk), the next levers with
  estimates: `ESP_WIFI_STATIC_RX_BUFFER_NUM` 10→6 (~6.4 KiB permanent);
  `DYNAMIC_TX_BUFFER_NUM` 32→16 and `DYNAMIC_RX_BUFFER_NUM` 32→16 (only
  allocated under load; **measured 2026-11-13, `docs/wifi-buffer-throughput.md`:
  no throughput cost, but a 100 B UDP flood still takes the heap to
  1.4–5 KB — it only turns the failed 1700 B allocs into driver drops**);
  `TCP_SND_BUF`/`TCP_WND` 5760→2880 (`TCP_SND_QUEUELEN` 16→8, so an
  uncapped socket pins ~12 KiB instead of ~25 KiB). Other levers, not
  tried: `NBP_NAT_TABLE_SIZE` 512→256 (8 KiB, one 16 KiB block → 8 KiB),
  api_client stack 8→6 KiB (QEMU high-water leaves 4.6 KiB free, the OOM
  core 4.8 KiB at GetServices depth; 2 KiB per client), httpd task stack
  (1.1 KiB of 7 KiB used in the core; ~3 KiB), NimBLE `MSYS_2_BLOCK_COUNT`
  24→12 (~3.8 KiB), freeing a proxy slot's remote attribute cache on
  disconnect (a few KiB per peer; needs care: discovery may be walking it).

## PSRAM opt-in (ESP32-S3R8, `sdkconfig.defaults.psram`)

- **A fragment, not a Kconfig switch** (the PSRAM mode and speed are Kconfig
  choices, which `select` cannot set). S3 only:
  `idf.py -B build-psram -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-psram/sdkconfig
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.psram" build`.
  The plain build is unchanged. Octal PSRAM, 80 MHz, `SPIRAM_IGNORE_NOTFOUND`
  (boots without it), `SPIRAM_USE_MALLOC`, `MALLOC_ALWAYSINTERNAL=2048`,
  `MALLOC_RESERVE_INTERNAL=32768`, `TRY_ALLOCATE_WIFI_LWIP`, NimBLE
  `MEM_ALLOC_MODE_DEFAULT`. Reasons are in the fragment. Task stacks stay
  internal (`FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=n`, the renamed
  `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`). The bootloader is unchanged, so an
  app OTA is enough.
- **Not NimBLE `MEM_ALLOC_MODE_EXTERNAL`**: it allocates PSRAM-only and
  asserts on the first host allocation when PSRAM is absent.
- **GPIO 33-37 belong to the octal PSRAM** on an S3R8 (26-32 to flash). The
  firmware drives no GPIO itself except the optional RS485 bridge and env
  sensors below (console UART0 on 43/44).
- **Every heap figure and guard reads internal RAM**
  (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`), not `esp_get_free_heap_size()` /
  `MALLOC_CAP_DEFAULT`, which include PSRAM once it is in the heap. Without
  PSRAM both select the same heaps on S3, C3 and ESP32, so nothing changes
  there. `/stats.json` `heap`/`heap_largest`/`heap_min` are internal;
  `psram_free`/`psram_min` are new (0 without PSRAM). Exceptions, on
  purpose: `oom_guard`'s throwing-new retry gate keeps `MALLOC_CAP_DEFAULT`
  (it predicts `malloc`, which may use PSRAM); the API send-queue cap sums
  the segments' actual blocks wherever they live; `napt_heap_ok()` drops the
  table size from the internal requirement when the tables fit in PSRAM
  (lwIP prefers it). New code: never guard on `esp_get_free_heap_size()`.
- **IDF 6.0.3 changed the reserve pool**: it now carries `MALLOC_CAP_DEFAULT`
  at the lowest priority, so small `malloc()`s use it after the other
  internal heaps and before PSRAM (IDF commit d3c628c759). The kb note on
  the pool (`~/dev/kb/esp32/memory-and-display-dma.md`) describes 5.5.1.
- **QEMU** (heap self-test with the fragment, `build-qemu-heap-psram`): two
  emulator traps. `idf.py qemu` passes `-m 32M`; 32 MB of PSRAM fills the
  data address space and the app aborts in `esp_mmu_map` (partition table).
  And with the octal-PSRAM flag the bootloader hangs writing otadata after
  `Loaded app` (no write, no hang: copy otadata 0xf000+0x2000 from a booted
  non-PSRAM `qemu_flash.bin`). Run `qemu-system-xtensa` by hand with `-m 8M`
  and `-global driver=ssi_psram,property=is_octal,value=true`. 2026-11-15:
  boots, 8 MB found and added, all steps PASS; internal free 232→222 KB at
  boot (PSRAM driver IRAM/DRAM ~10 KB), largest 172→131 KB (the 32 KiB
  reserve is carved from it). QEMU has no WiFi/BLE, so the WiFi-buffer
  saving is unmeasured.
- **Flash writes with the cache off** (OTA, NVS, core dump) are safe for
  PSRAM data: `esp_flash_write` bounces non-DRAM source buffers, and nothing
  of ours touches heap memory from an ISR (oom_guard's IRAM hook writes
  `DRAM_ATTR` words only).

## RS485 <-> TCP bridge (`NBP_MODBUS_BRIDGE`, `components/modbus_bridge`)

- **Build** (PSRAM unit): `idf.py -B build-psram-mb -DIDF_TARGET=esp32s3
  -DSDKCONFIG=build-psram-mb/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.psram;sdkconfig.defaults.modbus" build`.
  Default n; the fragment turns it on (UART1, TX 4, RX 5, 115200 8N1, TCP 8899).
- ser2net-style, for HA `modbus` `type: rtuovertcp`, no register knowledge.
  A request goes to RS485 only as a valid RTU frame (4..256 B, CRC16; FC 1-6
  = 8 B, FC 15/16 = 9 + byte count, other FCs complete on a matching CRC or a
  3 ms TCP gap); anything else counts `mb_bad_req`, drops the client and sends
  nothing. A half-closed client still gets its request sent and the reply.
  The request goes out as one burst after an RX flush; UART data goes back as one `send()` at line idle
  (UART RX timeout = Modbus t3.5, 1.75 ms floor). A reply after
  `NBP_MB_REPLY_TIMEOUT_MS` still reaches the client; it only counts `mb_timeouts`.
- **Direction pin optional** (`NBP_MB_DE_GPIO`, default -1 = none, for
  auto-direction modules such as DFRobot DFR0845). With a DE pin
  (MIKROE-989 R/T = 6): plain UART, *not* `UART_MODE_RS485_HALF_DUPLEX`
  (received only 0x00 on an S3, `~/dev/kb/esp32/rs485-half-duplex-mode-received-nothing-on-s3.md`);
  the task raises itself to priority 19, DE high, `uart_write_bytes`,
  `uart_wait_tx_done`, DE low, two character times, flushes RX.
  `modbus_bridge::early_init()` right after `nvs_flash_init` (before WiFi,
  also in AP provisioning) drives DE low and pulls TX up, so DE is low from
  boot on except the ROM/bootloader window. Pins are checked against
  `esp_gpio_is_reserved()` (flash/PSRAM), the UART0 console and, on the S3,
  26-37, straps and USB; a disallowed Kconfig/NVS pin set leaves the bridge
  off (`GET /modbus` `running:false`), never a crash.
- `CONFIG_UART_ISR_IN_IRAM=y` and `CONFIG_LWIP_MAX_SOCKETS=20` in the fragment.
- **STA side only:** listener on INADDR_ANY, but a connection must arrive on
  the STA address from the STA subnet (or the one allowlisted IP) and never
  from the SoftAP subnet; else RST + `mb_refused`. One client; a new one
  replaces it only if it is the allowlisted IP or the current client's IP,
  else it is refused. TCP keepalive (60 s + 3 x 10 s) frees a dead client's slot.
- `GET /modbus` (config, client, `stack_free` = task stack high-water);
  `POST /modbus?baud=&parity=n|e|o&allow=<ip>|none&tx=&rx=&de=<gpio>|none|-1`
  with `X-NBP-Confirm: yes`, NVS namespace `mbridge`. Line settings apply at
  once, pins after a reboot. `/stats.json`: `mb_client`, `mb_tx_frames`,
  `mb_rx_frames`, `mb_tx_bytes`, `mb_rx_bytes`, `mb_timeouts`, `mb_refused`,
  `mb_bad_req`. POST applies to RAM only after every NVS write and the commit
  succeeded.
- Host check (read-only, FC04 0x3108.. + FC01 coil 0x0F, slave 3):
  `scripts/modbus_bridge_test.py <ip>`.

## Environment sensors (`NBP_ENV_SENSORS`, `components/env_sensors`)

- **Build** (PSRAM unit, with the bridge): `idf.py -B build-psram-mb-env
  -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-psram-mb-env/sdkconfig
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.psram;sdkconfig.defaults.modbus;sdkconfig.defaults.sensors" build`.
  Default n. Builds only so far, not hardware-tested.
- **Sensors:** SHT3x (SHT30/SHT31-F) at I2C 0x44 and/or 0x45, single shot
  0x2400, both CRC8s checked; DS18B20/DS1822 on one 1-Wire bus
  (`espressif/onewire_bus`, RMT, pulled in only with the option): ROM search,
  convert all (skip ROM), 750 ms, read each scratchpad by match ROM, CRC and
  config byte checked (a bus held low reads zeros with a valid CRC), 85.0 C
  power-on value = `por`. Up to `NBP_ENV_DS_MAX` (6) probes; slots of
  vanished ROMs are reused once the table is full.
- **Pins** (Kconfig, NVS namespace `envsens`): SDA 8, SCL 9, 1-Wire 7; -1 turns
  a bus off. Chosen free on the S3R8: not flash/PSRAM 26-37, straps 0/3/45/46,
  USB 19/20, UART0 43/44, JTAG 39-42, the bridge's 4/5 (+DE 6) or the DevKit
  RGB LED 38/48. Checked by `nbp_gpio::allowed()` (`include/nbp_gpio.h`,
  shared with the modbus bridge): console, `esp_gpio_is_reserved()`, S3
  ranges, and pins claimed by the other peripheral (`nbp_gpio::claim`,
  bridge first). A disallowed set leaves the sensors off
  (`running:false`). Internal pull-ups are on; **the 1-Wire bus needs an
  external 4.7 kOhm pull-up to 3V3** and the probes their own VDD (3-wire;
  parasite power is not supported, it reads `por`). I2C breakouts usually
  carry their own pull-ups.
- **Task** `env`, priority 1, 3 KiB internal stack (`stack_free` = high-water),
  every `NBP_ENV_PERIOD_S` (30 s). The fragment moves the RMT/I2C driver code
  out of IRAM; neither ISR is cache-safe either way, so a transfer hit by a
  flash write fails its CRC and reads null for that cycle.
- **Readings are never stale-as-fresh:** a value is shown only if the last
  attempt succeeded and it is younger than 3 periods, with `age_s`; otherwise
  `t`/`rh` are null with `err` (`crc`, `i2c`, `absent`, `bus`, `por`,
  `stale`) and `fails` (consecutive). An SHT address never seen is omitted;
  one that vanished stays as `absent`.
- `GET /sensors` (worst case 2 SHT + 6 DS < 744 B, stack buffer):
  `{"sht":[{"addr":68,"t":21.40,"rh":55.20,"age_s":12}],"ds":[{"id":"28ff…","t":19.81,"age_s":12}],"err":{"sht_crc":0,"sht_i2c":0,"ds_crc":0,"ds_miss":0,"ds_bus":0,"ds_por":0,"ds_full":0},"cfg":{"sda":8,"scl":9,"ow":7,"period_s":30,"running":true,"stack_free":1234,"cycles":5}}`.
  The DS id is the ROM in bus order, family byte first.
  `POST /sensors?period=5..3600&sda=&scl=&ow=` (`-1` = off, `kconfig` = drop
  the override) with `X-NBP-Confirm: yes`; period at once, pins after a
  reboot. `/stats.json`: `env_cycles`, `env_sht_err`, `env_ds_err`,
  `env_stack_free` (~100 B).
- Host test (CRCs, conversions, JSON bound and freshness):
  `c++ -std=c++20 -Wall -Wextra -Werror -I components/env_sensors
  components/env_sensors/host_test/test_env_core.cpp -o /tmp/t && /tmp/t`.
- **Home Assistant** (`rest`, no decoding; replace the ROM id and IP):

```yaml
rest:
  - resource: http://192.0.2.10/sensors
    scan_interval: 30
    sensor:
      - name: Caravan temperature
        unique_id: caravan_sht_44_t
        device_class: temperature
        unit_of_measurement: "°C"
        value_template: >
          {% set s = value_json.sht | selectattr('addr', 'eq', 68) | first %}
          {{ s.t if s is defined and s.t is not none else none }}
        json_attributes_path: "$.sht[?(@.addr==68)]"
        json_attributes: [age_s, err, fails]
      - name: Caravan humidity
        unique_id: caravan_sht_44_rh
        device_class: humidity
        unit_of_measurement: "%"
        value_template: >
          {% set s = value_json.sht | selectattr('addr', 'eq', 68) | first %}
          {{ s.rh if s is defined and s.rh is not none else none }}
      - name: Water tank temperature
        unique_id: caravan_ds_28ff0123456789ab
        device_class: temperature
        unit_of_measurement: "°C"
        value_template: >
          {% set d = value_json.ds | selectattr('id', 'eq', '28ff0123456789ab') | first %}
          {{ d.t if d is defined and d.t is not none else none }}
        json_attributes_path: "$.ds[?(@.id=='28ff0123456789ab')]"
        json_attributes: [age_s, err, fails]
```

  A `none` state shows as unknown in HA, so a failed or stale reading never
  carries the last value forward.

## Switch output (`NBP_SWITCH_OUT`, `components/switch_out`)

- **Built into every image since 2026-11-22** (`NBP_SWITCH_OUT` default y,
  needs `NBP_WIFI`), so a device can run a stock release and keep its
  switch. The pin is runtime config: NVS `switch`/`gpio` (i8, -1 = none),
  else `CONFIG_NBP_SWITCH_GPIO` (default **-1**: release images touch no
  pin until one is set). A build with a Kconfig pin writes it to NVS on the
  first boot that finds no `gpio` key (migration), so a later stock image
  keeps using it. `sdkconfig.defaults.switch` only presets GPIO14 for .231:
  `idf.py -B build-psram-sw -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-psram-sw/sdkconfig
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.psram;sdkconfig.defaults.switch" build`.
  Running on .231 since 2026-11-18 (on/off, boot policy, held across OTA
  reboots). Not yet run on hardware: a stock image picking the pin up from
  NVS, and the live pin move/disable path.
- On .231, GPIO14 drives an N-MOSFET gate across the EPEVER IP3000
  remote-switch contact: high = closed = inverter on. It feeds a fridge,
  so the output must not glitch. Checked by `nbp_gpio::allowed()` and
  claimed first (before the bridge and sensors); a disallowed pin is left
  untouched (`running:false`). On ESP32/C3/C6 the switch also refuses
  strapping and USB pins (`nbp_gpio::strap_or_usb`: ESP32 0/2/5/12/15, C3
  2/8/9/18/19, C6 8/9/12/13/15): the gate's pull-down held through a reset
  would pick download mode or the flash voltage (the S3 excludes its straps
  in `allowed()` for every peripheral).
- `GET /switch` → `{"gpio":14,"name":"inverter","on":true,"boot":"last","changes":0,"since_s":12,"running":true}`
  (`changes` since boot, `since_s` since the last change or boot; `gpio`
  -1 = not configured).
  `POST /switch?on=0|1`, `?boot=off|on|last`, `?gpio=N|none`, `?name=TEXT`
  (combinable) with `X-NBP-Confirm: yes`,
  **STA side only** (request on the STA address from the STA subnet, never
  from a SoftAP client; IPv6 peers refused; `getsockname`/`getpeername` on
  `httpd_req_to_sockfd`, IPv4-mapped on the dual-stack listener), else 403.
  `gpio` is checked with `nbp_gpio::allowed(p, own pin)` (400 if refused)
  and **applies at once**: the old pin is driven off and stays held off
  (the safe level; nothing else drives it until power-off), the new pin
  starts off and `on=0` is stored; `on=1` together with a new gpio is
  refused. `name`: 1-24 printable ASCII after %XX/+ decoding, no `"` or
  `\` (it goes into the JSON unescaped). on/boot without a working pin:
  400. NVS namespace `switch` (`on`, `boot` u8, `gpio` i8, `name` str),
  written only when a value differs, before the pin changes. Default boot
  policy `last`; no stored state = off. `/stats.json`: `sw_on`,
  `sw_changes`, `sw_cfg` (1 = pin active). Dashboard: Switches tab, name
  and pin under "configure".
- **Boot:** `switch_out::early_init()` right after `nvs_flash_init`, before
  WiFi: level into the output register, `gpio_config` (in+out, no pulls),
  `gpio_sleep_sel_dis` (startup isolates every pad in light sleep,
  `sleep_gpio.c:266-275`), then `gpio_hold_dis` + `gpio_hold_en`. A change
  does the same: register first, then release and re-take the hold.
- **What the hold covers (IDF 6.0.3 source, S3):** GPIO14 is an RTC pad, so
  `gpio_hold_en` (`gpio.c:752-754`) sets `RTC_CNTL_TOUCH_PAD14_HOLD` in
  `RTC_CNTL_PAD_HOLD_REG` (`rtc_io_ll.h:298-301`, `rtc_io_periph.c:81`), an
  RTC-domain register that only power-on, chip (EN) and RTC resets clear.
  Startup unholds pads only after a deep-sleep reset (`cpu_start.c:849-852`).
  - Kept: `esp_restart()` incl. `/reboot`, OTA and liveness reboots
    (`esp_restart_noos` resets the CPUs, `system_internal.c:153/159`; its
    1 s RWDT fallback is a system reset, `:83`), panics and task/int WDT
    (`panic_restart` → `esp_restart_noos`, `panic_handler.c:304-313`; int WDT
    stage 1 is a system reset, `int_wdt.c:94`), brownout
    (`esp_rom_software_reset_system`, `brownout.c:80`), light sleep.
  - Lost (pad at its reset state, so the gate needs an external pull-down to
    read off): power-on, EN/chip reset, any RTC-WDT RTC reset: bootloader
    hang > 9 s (`bootloader_init.c:93`), panic handler (core dump) > 10 s
    (`panic_handler.c:219`, `panic.c:203`), stage 2 of the
    `esp_restart_noos` RWDT (`system_internal.c:84`). The pin then stays
    there until `early_init` restores it.
  - An image without this feature (rollback, older OTA) leaves the pad held
    at the last level until power-off and cannot change it.

## Liveness watchdog targets (v0.2.4, issue #2)

- The probe used to hard-code Fab's lab broker `192.168.1.200:1882` as the
  second target. On other LANs it never answered, the gateway (`:80`) was the
  only target, and a slow or filtered gateway rebooted a reporter's C3 every
  ~3 min. Now: gateway `:80`, plus `CONFIG_NBP_LIVENESS_EXTRA_TARGET`
  ("ip:port", empty in the repo; set it in a local sdkconfig for the lab
  repeater). Only a handshake or a RST counts as reachable; local socket
  errors count as failures.
- Frames from an *upstream* API client within 60 s also pass a cycle
  (`api_server::stable_rx_us`; loopback = WS bridge and SoftAP-subnet peers
  don't count, they would hide a dead uplink), but only from a session open
  for at least max(60 s, 2 × interval). 2026-11-16, .231 at -80 dBm: batmon
  reconnected every ~40 s and each handshake kept the watchdog from firing.
- Reboots arm only after a probe target answered once this boot (`"armed"`
  in `GET /liveness`). A liveness reboot carries the armed state across
  `esp_restart` (`__NOINIT` magic), so a wedge that recurs before the first
  probe still reboots. Each carried reboot without a probe success doubles
  the threshold (cap 32x), so a board moved to a LAN whose gateway ignores
  `:80` backs off instead of looping.
- Panic records already survive the reset: `GET /crash` (crash_capture).
  Ask for it before guessing at a `panic` reset reason.
- **The peer is walked over the air only when the attribute cache is
  empty** (`gatt_discovery.cpp`: `walk_empty`, `ensure_discovered`,
  `fill_gaps`, `g_partial`; bt_handlers uses the same helpers). Every
  connect clears the cache, so a non-empty one belongs to this connection:
  a repeat GetServices answers from it (`services from cache`), and after a
  walk that failed part-way only the gaps are filled. Before (2026-11-14),
  every GetServices ran `discoverAttributes()`, whose `deleteServices()`
  freed the characteristics, and their notify callbacks, under the host
  task: during the ~4.5 s walk notifications were `unknown handle`, after
  it they were dropped silently (new objects, no callback), and HA never
  re-subscribes. S3 bench `ABgs` (GetServices every 10 s, 20 notifies/s):
  before, A got 5 notifies in 120 s; after, 2240/2246 sent, no sequence
  gap, repeat GetServices 20–400 ms. A Service Changed indication is not
  handled (the cache is not invalidated by it).

## Dashboard is embedded gzipped (`components/api_server/CMakeLists.txt`)

- The build gzips `web/index.html` (level 9, `mtime=0` so the image stays
  reproducible) into `index.html.gz` and embeds that: 119,766 → ~38 KB,
  −78 KB of C6 image (flash budget: `docs/flash-budget.md`). `root_get`
  sends it with `Content-Encoding: gzip` + `Vary: Accept-Encoding`; a client
  without `gzip` in `Accept-Encoding` gets **406** and a hint. Use `curl
  --compressed http://<device>/`; browsers always send it. Verified on .231
  2026-11-24 (decompressed bytes == `web/index.html`, headless Chrome renders it).

## Dashboard update check (`web/index.html`, last script block)

- **The browser checks and downloads, the device does not.** The page reads
  `https://fl4p.github.io/nimble-ble-proxy-esphome/updates.json` and the
  image from `firmware/<tag>/nimble_ble_proxy-<variant>-ota.bin` next to it,
  checks size + sha256 (pure-JS SHA-256: `crypto.subtle` is missing on a
  plain-http page) and the image header (chip id, app descriptor), then
  POSTs it to the unchanged `/update` with `X-NBP-Confirm: yes`, waits for
  the reboot and requires `/appinfo` `elf_sha256` == the manifest's.
  Release-asset downloads send no `Access-Control-Allow-Origin` (Pages and
  api.github.com send `*`), so the images must be on Pages:
  `pages.yml` stages them and writes `updates.json` with
  `scripts/make_updates_manifest.py` (newest stable + newer prereleases,
  never drafts; fails on a size/digest mismatch). Without `updates.json`
  the page falls back to the releases API, display-only.
- **Variant identity.** `GET /appinfo` `variant` is the CI label
  (`-DNBP_VARIANT=<target>-ble-on|ble-off`, `build.yml`) or `custom`; the
  root CMakeLists.txt refuses a stock label on a non-stock config. Only an
  image of exactly that variant is offered. `custom`, or firmware before
  the field (<= 0.2.8), gets no offer, only the expert override (pick a
  variant of the same target + type its name). Prereleases only with
  `?debug-releases=1`. Install is HTTP-only (no `/update` over BLE).
  `?upd-base=<url>` overrides the source on a localhost-served page only.

## App image signing (OTA accepts only images signed with the running key)

- **What is on** (`sdkconfig.defaults`, S3/C3/C6): `SECURE_SIGNED_APPS_NO_SECURE_BOOT`
  + `SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, RSA scheme (Secure Boot V2 block,
  RSA-3072 PSS), `SECURE_BOOT_BUILD_SIGNED_BINARIES=n`. No secure boot, no
  flash encryption, no eFuse is written. Release checklist, key handling,
  recovery and rotation: `docs/signing.md`.
- **Where it is checked:** only in the running app. `esp_ota_end()` →
  `ota_verify_partition` → `esp_image_verify` (IDF 6.0.3
  `app_update/esp_ota_ops.c:554-566,593-626`); `esp_ota_set_boot_partition`
  re-validates (`:790-797`). The app build gets `SECURE_BOOT_CHECK_SIGNATURE=1`
  from `SECURE_SIGNED_ON_UPDATE`, the bootloader build does not
  (`bootloader_support/src/esp_image_format.c:36-48`); bootloader-side checking
  without secure boot exists only for the V1/ECDSA scheme
  (`bootloader/Kconfig.projbuild:619-629`). Verification itself is PSA RSA-PSS
  (`secure_boot_v2/secure_boot_rsa_signature.c:88-189`).
- **The trusted key is the running image's own signature block 0** — not
  eFuse, not a compiled-in key: `get_secure_boot_key_digests` takes the
  digests from the running app when secure boot is off
  (`secure_boot_v2/secure_boot_signatures_app.c:154-160`), and with
  `SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` only index 0 is compared on both
  sides (`:233-237`, loop `:239-283`). Consequences: (1) a dev-signed device
  accepts only dev-signed images and a release device only release-signed
  ones; switching keys = USB flash; (2) **key rotation over OTA is
  impossible**, it is a USB reflash of every device; (3) **a running image
  without a valid signature has no trusted key and refuses every OTA**
  (`no_trusted_key`) until a USB flash.
- **Field transition:** firmware up to 0.2.8 has no check, so it accepts the
  next image signed or not (and the extra signature sector is ignored). A
  pre-signing bootloader boots a signed image: it verifies only the appended
  SHA-256 (`esp_image_format.c:236-241`), the 4 KiB signature sector sits past
  the image length (`:1034-1055` only in signing builds), and `--secure-pad-v2`
  padding is a segment inside the image. QEMU 2026-11-22: the baseline
  bootloader (main 3dd7a29) booted the release-style signed S3 app. So: give
  field devices the **signed** release first; if they get the unsigned CI
  image they lock themselves out of OTA. App OTA cannot replace the
  bootloader, and nothing here needs it to.
- **USB:** `idf.py flash`/esptool check nothing about signatures and the
  bootloader does not either, so USB flashing is always the escape hatch.
  Secure boot would add a bootloader-checked chain and refuse unsigned USB
  images; flash encryption would stop reading/patching flash. Both burn
  eFuses irreversibly and a lost key bricks the board: out of scope.
- **Classic ESP32: off** (`sdkconfig.defaults.esp32`). Its RSA scheme needs chip
  rev >= 3 (`Kconfig.projbuild:470-474`; `ets_rsa_pss_verify` is only in
  `esp32.rom.eco3.ld`), and V1/ECDSA would need a second key and a build-time
  embedded public key. The release script skips `esp32` assets.
- **Local builds are signed with the dev key** by the root `CMakeLists.txt`
  (`scripts/nbp_sign.py build-sign`, after IDF's `.bin` step):
  `build/nimble_ble_proxy.bin` is signed in place, the unsigned image stays as
  `nimble_ble_proxy-unsigned.bin`. Key: `-DNBP_SIGNING_KEY=` / env
  `NBP_SIGNING_KEY` / `~/.config/nbp-signing/dev-key.pem`; `none` or a missing
  key builds UNSIGNED with a CMake warning. The release key is refused there.
  IDF still prints "App built but not signed" (its own build-time signing is
  off); the next line says it was signed. ~2 s per relink. `idf.py flash` and
  the OTA curl in "OTA over a loaded device" work unchanged on a dev-key
  device; the first dev-signed OTA onto an unsigned-era device is accepted.
- **Device side** (`components/ota/ota_sig.cpp`): `GET /update` →
  `{"sig_check","scheme","running","running_signed","key_sha256","rejects","last_reject","last_reject_key"}`;
  `key_sha256` equals `scripts/nbp_sign.py fingerprint <pem>`. A failed
  `esp_ota_end` answers 403 (`unsigned`, `untrusted_key`, `bad_signature`,
  `no_trusted_key`) or 400 (`corrupt`, `not_an_image`) with header
  `X-OTA-Refused`, logs `OTA REFUSED: …` (image key vs trusted key), never
  switches the boot partition, and erases the target slot's first sector
  (the bootloader checks no signature, so a fallback must not boot it). The
  classification only explains IDF's verdict; it cannot turn a failure into a
  success. It returns ESP_OK after the 4xx: with ESP_FAIL http_guard's
  abortive close ate the body. `sig::init` runs on the httpd task (queued by
  `ota::start`): IDF's running-app signature read needs ~2 KiB of stack and
  overflowed the 3.5 KiB main task into the heap (QEMU, pm-lock deadlock).
- **QEMU test:** `idf.py -B build-qemu-sig -DIDF_TARGET=esp32s3
  -DSDKCONFIG=build-qemu-sig/sdkconfig -DNBP_SIG_SELFTEST=1 build`, then
  `… qemu` (needs the dev key; QEMU must not be a background job reading the
  tty, or it stops on SIGTTIN: redirect stdin or use `-serial file:`). Test
  vectors from `nbp_sign.py selftest-blob` go to the last 8 KiB of the
  coredump partition (0x3FE000). Prints `SIG-SELFTEST <step>: PASS|FAIL`:
  fingerprint, unsigned / wrong key / tampered (one byte of the app
  description flipped, checksum and SHA-256 fixed up) → 403, boot partition
  unchanged, slot header erased; signed → 200, boots from ota_1, same key.
  Known-bad: add `sdkconfig.defaults.sigtest-nocheck` (own build dir); the
  three refusals and the counter FAIL.
- **Stack: every IDF image check is an RSA verify now** (~2 KiB of stack):
  `esp_ota_check_rollback_is_possible`, `esp_ota_mark_app_invalid_rollback*`,
  `esp_ota_get_last_invalid_partition`, `esp_ota_set_boot_partition`. With the
  3584 B main task, `ota_verify::early_init` overflowed it on the first boot
  of every OTA'd image (panic in `intr_alloc`, then rolled back; QEMU
  2026-11-23, only with rollback + signing merged). `ESP_MAIN_TASK_STACK_SIZE`
  is 6144 (the task exits after boot; the boot-complete log line prints the
  unused stack). Only `early_init` (main task) calls them; the `ota_verify`
  worker hands a rollback to the next boot (`request_rollback`), so it
  keeps a 3 KiB stack (was 6 KiB, which cost a C3 heap while unverified).
  Any new caller of those IDF functions needs a task with that headroom.
- **Release = draft until signed.** `build.yml`'s `release` job (after the
  whole matrix) creates the tag's release as a **draft** with the unsigned CI
  assets, and refuses to upload to an already published release.
  `release_sign.py sign <tag> --upload` replaces the S3/C3/C6 images, then
  `gh release edit --draft=false`, then dispatches `pages.yml`.
- **Publish gate:** `scripts/nbp_sign.py check-release IMG…` passes an
  `*-ota.bin` or merged `*-flash.bin` (app found via the partition table at
  0x8000) only if block 0 is signed with `RELEASE_KEY_SHA256`, the digest
  matches and RSA-PSS verifies; classic ESP32 passes unsigned; no pinned key
  = everything else refused, except releases up to `LAST_PRE_SIGNING_RELEASE`
  (v0.2.8; firmware that checks no signature) whose image version equals the
  tag and whose length is not 64 KiB-aligned (signing builds are padded to
  the MMU page; a signing build under an old tag is refused). The tag comes
  from the staging path `firmware/<tag>/` or `--tag`. `pages.yml` runs it on every staged file and deletes
  refused ones, `make_updates_manifest.py` re-checks and leaves them out,
  and `pages.yml` is no longer triggered by tag builds:
  `release_sign.py --upload` dispatches it after uploading. The dashboard
  install also refuses an unsigned image and one whose key differs from the
  device's `GET /update` `key_sha256` (key comparison only; the RSA check is
  the device's).
- **Release flow:** CI stays unsigned. `scripts/release_sign.py sign <tag>`
  (dry run; `--upload` asks for the tag, then `gh release upload --clobber`)
  signs `*-ota.bin` with the Keychain key, rebuilds `*-flash.bin` after
  checking the CI parts reproduce the CI merged image, verifies with
  espsecure against `keys/release.pub.pem` and the fingerprint pinned in
  `scripts/nbp_sign.py`. Never run it against a real release from an agent.
