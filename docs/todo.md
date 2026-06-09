# nimble-ble-proxy — TODO

* remove softap kconfig gateway
  put Web UI: decode BTHome v2 service-data adver under Web UI: live device table

## Active-scan: OR the web toggle with "HA is listening"

Make the effective active-scan mode the OR of two inputs:

1. **Web toggle** — `POST /scan?active=1`, persisted in NVS `scan_act`,
   applied at boot via `apply_scan_from_nvs()`, wired to
   `ble_backend::scanner::set_active()`. *(Already implemented.)*
2. **HA is subscribed to advertisements** — `g_sub_adv_count > 0` in
   `components/api_server/bt_handlers.cpp`.

So: `effective_active = web_toggle || (g_sub_adv_count > 0)`. Drive
`scanner::set_active(effective_active)` from both `handle_subscribe_adv`
and `handle_unsubscribe_adv` (and keep the web `/scan` handler OR-ing in
the subscription state, not overwriting it).

**Important correction / not a blocker:** there is **no** active-scan
request in the ESPHome protocol. `SubscribeBluetoothLEAdvertisementsRequest.flags`
is only `BLUETOOTH_PROXY_SUBSCRIPTION_FLAG_RAW_ADVERTISEMENTS` — it does
**not** carry scan mode. So the OR's second operand is "HA is listening
at all", not a scan-mode field from HA. Scan mode is otherwise a
device-side setting (compile-time `g_active_scan` default + the
`PASSIVE_SCAN` capability bit the device declares in
`bluetooth_proxy_feature_flags`).

See the `TODO(active-scan)` marker in
`components/api_server/bt_handlers.cpp` (`handle_subscribe_adv`).




```

     ────────────────────────────┬─────────────────────────────┬───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐    
    │       Proposed gate        │  Cost saved / capability    │                                                                        Why it matters                                                                         │ 
    │                            │            added            │                                                                                                                                                               │ 
    ├────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤ 
    │ NBP_OTA (opt-out, default  │ ~12 KB flash + a            │ OTA over plain HTTP is a write primitive for anyone on the LAN. A "lockdown" build for production deployments where firmware updates happen only via serial   │ 
    │ y)                         │ hot-pluggable security risk │ would meaningfully reduce blast radius. Currently ota::start() is always called when NBP_WIFI=y.                                                              │ 
    ├────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤ 
    │ NBP_HTTP_AUTH (opt-in,     │ ~2 KB flash                 │ HTTP Basic on every dashboard endpoint, credentials in NVS. The README explicitly assumes a trusted LAN — making "trust-the-LAN" optional opens the door to   │ 
    │ default n)                 │                             │ running this on a shared network. POST routes (/reboot, /clone, /cpufreq, /txpower, /hostname) are the obvious priority.                                      │ 
    ├────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤ 
    │ NBP_AP_FALLBACK (opt-in,   │ ~15 KB flash, +1 SoftAP     │ If STA can't associate within N seconds (bad creds, AP down), bring up a SoftAP + minimal captive page that exposes only /wifi?ssid=…&psk=…, write to NVS,    │ 
    │ default n)                 │ code path                   │ reboot. Replaces the current "edit wifi_creds.h + re-flash" workflow. High value for distributing prebuilt binaries.                                          │ 
    ├────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤ 
    │ NBP_RUNTIME_STATS          │ A few µs per context switch │ Pulls CONFIG_FREERTOS_USE_TRACE_FACILITY + …_GENERATE_RUN_TIME_STATS only when CPU% is wanted. Lets a latency-sensitive build (clone + high notify rate) drop │ 
    │ (opt-out, default y)       │                             │  the kernel overhead. The dashboard would have to report cpu0/cpu1 as null.                                                                                   │ 
    ├────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤ 
    │ NBP_PANIC_DUMP (opt-in,    │ ~3 KB flash + 4 KiB NVS     │ Tee esp_core_dump_to_flash (or just the last N ESP_LOGE lines) into NVS so the dashboard can show "last crash backtrace" after the next reboot. Way more      │ 
    │ default n)                 │                             │ useful than ESP-IDF's "panic dropped on UART" given that the dashboard is a UART-less interface for many users. Pairs nicely with the existing boot-guard.    │ 
    ├────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤ 
    │ NBP_ADV_ALLOWLIST (opt-in, │ ~1 KB flash + N×8 B NVS     │ MAC allowlist for raw-advert forwarding. With 30+ nearby devices the proxy forwards everything; HA discards most. A MAC filter at the proxy cuts WiFi TX duty │ 
    │  default n)                │                             │  proportionally. Hands the user the same lever ESPHome's active: true / passive: true does, but per-MAC.                                                      │ 
    └────────────────────────────┴─────────────────────────────┴───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘ 
                                                                                                                                                                                                                                 
                     
                     
```