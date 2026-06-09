// Lightweight transaction counters + a tiny web UI that renders them
// as live rates. Counters are bumped from bt_handlers; the HTTP routes
// piggyback on the OTA component's httpd handle (one shared listener).

#pragma once

#include "esp_http_server.h"
#include "proxy_config.h"

namespace api_server::stats {

void record_read();
void record_write();
void record_notify();

// Optional provider that lets /stats.json fold a second source of GATT
// activity (the ble_clone GATT image) into reads/writes/notifies/conns
// without api_server depending on ble_clone. The provider is invoked
// from build_stats_json; main wires it up at boot when CONFIG_NBP_CLONE
// is on. Pass nullptr to detach.
struct CloneCounters {
  uint32_t reads;
  uint32_t writes;
  uint32_t notifies;
  uint8_t connected_centrals;
};
using CloneStatsProvider = void (*)(CloneCounters *out);
void set_clone_stats_provider(CloneStatsProvider fn);

// Optional provider that lets /stats.json report NAT-router (SoftAP)
// throughput without api_server depending on nat_router. main wires it up
// at boot under CONFIG_NBP_NAT_ROUTER (forwarding to
// nat_router::get_ap_throughput); pass nullptr to detach. When a provider
// is installed, build_stats_json emits cumulative "ap_rx"/"ap_tx" byte
// counters and the dashboard renders the throughput chart; when absent the
// fields are omitted (BLE-only / non-router builds) and the chart hides.
struct NatThroughput {
  uint64_t ap_rx_bytes;  // octets received from SoftAP clients (their uplink)
  uint64_t ap_tx_bytes;  // octets sent to SoftAP clients (their downlink)
};
using NatThroughputProvider = void (*)(NatThroughput *out);
void set_nat_throughput_provider(NatThroughputProvider fn);

// Build the same JSON body that /stats.json returns into a caller-
// supplied buffer. Returns bytes written (truncated at cap-1 by
// snprintf semantics). Safe to call from any task. The CPU-percent
// fields advance a static window between calls — if two callers poll
// concurrently each sees its own delta window, not a shared one.
size_t build_stats_json(char *buf, size_t cap);

#if CONFIG_NBP_WEB_CONSOLE
// Copy one contiguous slice of the log ring starting at byte position
// `*since_inout` into `buf`. `*since_inout` is clamped on the way in
// (see below) so the caller can derive where the bytes actually came
// from: the returned slice spans [*since_inout, *since_inout + ret).
// `*out_seq` gets the current g_log_seq at snapshot time so the caller
// knows the upper bound.
//
// Clamp behavior:
//   since > seq           -> clamps to seq, returns 0 (client reboot)
//   seq - since > RING    -> clamps to seq - RING (client far behind)
//
// Wrap-around: this call returns at most up to the ring end; if the
// requested range straddles the wrap point, call again with
// `*since_inout` advanced by the returned bytes.
size_t build_log_slice(uint32_t *since_inout, char *buf, size_t cap,
                       uint32_t *out_seq);
#endif

// ---- Endpoint helpers shared by httpd and BLE transports ----
//
// build_* writes the GET response body (JSON) into the caller's buffer
// and returns bytes written.
//
// handle_*_set parses a query string ("key=val&key=val", post-?) and
// either applies the change or returns a short error string. nullptr
// return means success — caller should respond with {"ok":true}. The
// returned error pointer is to a static literal; do not free.

size_t build_level_json(char *buf, size_t cap);
const char *handle_level_set(const char *query);

size_t build_txpower_json(char *buf, size_t cap);
const char *handle_txpower_set(const char *query);

size_t build_cpufreq_json(char *buf, size_t cap);
const char *handle_cpufreq_set(const char *query);

// Peripheral advertising interval in ms. GET returns {"ms":N}; POST takes
// "ms=N". -1 = advertising OFF (broadcaster disabled via
// ble_backend::set_advertising_enabled); 0 = NimBLE host default
// (~30..60 ms range); 20..10240 = explicit interval (BLE-spec bounds).
// Persisted in NVS under "adv_itvl" (0xFFFF = off sentinel); applied live
// via ble_backend::set_adv_interval_ms / set_advertising_enabled. The
// scanner and raw-advert forwarding to HA are unaffected by the off state
// — it only gates the device's own broadcaster role.
#if CONFIG_NBP_BLE
size_t build_advitvl_json(char *buf, size_t cap);
const char *handle_advitvl_set(const char *query);
#endif

// WiFi power-save listen-interval. GET returns {"li":N}; POST takes
// "li=N" (0..10). 0 = PS_NONE, >0 = PS_MAX_MODEM with that many DTIM
// beacons between wake-ups. PS-mode flip is live; listen_interval
// inside wifi_config_t only takes effect on the next AP association,
// so the dashboard pulses the reboot button as an apply hint.
size_t build_wifips_json(char *buf, size_t cap);
const char *handle_wifips_set(const char *query);

// Load persisted listen_interval from NVS into the in-RAM mirror so
// /wifips GET returns the right value before any POST has happened.
// Note: wifi_sta.cpp also reads the same NVS key directly at boot to
// stamp wifi_config_t.sta.listen_interval before esp_wifi_start.
void apply_wifi_ps_from_nvs();

// Hostname (mDNS / WiFi netif / NimBLE GAP name / aioesphomeapi
// DeviceInfo). Persisted in NVS as a UTF-8 string under key "hostname";
// applied at boot via apply_hostname_from_nvs() — runtime POST writes
// NVS but does NOT re-init the radio stack, so the caller should reboot
// for the change to take effect.
//
// Validation: 1..HOSTNAME_MAX chars from [A-Za-z0-9._-], must not start
// or end with '-' or '.'. These mirror RFC 1123 hostname conventions so
// the value is usable as the mDNS local name (`<name>.local`).
size_t build_hostname_json(char *buf, size_t cap);

// Firmware identity from esp_app_get_description(): project name, version,
// build date/time, IDF version, secure-version, and the short ELF SHA256.
// GET-only (/appinfo) — static for the life of the running image. Lets the
// dashboard show "which build/board am I talking to" without serial.
size_t build_appinfo_json(char *buf, size_t cap);
const char *handle_hostname_set(const char *query);

#ifdef CONFIG_NBP_SMP
// Persist + apply the static SMP passkey used by the proxy when an
// upstream peer (BMS, cloned device) demands pairing. Single entry
// point shared with /clone — the standalone /passkey endpoint was
// folded into /clone so the dashboard collects target MAC + passkey
// in one POST. Returns nullptr on success, or a short error literal.
const char *set_passkey(uint32_t pin);
#endif

#if CONFIG_NBP_DEVICES_PANEL
// May truncate if `cap` is smaller than ~120 bytes per device row.
size_t build_devices_json(char *buf, size_t cap);
#endif

// Returns the resulting trace state (true if trace on after call).
bool handle_trace_set(const char *query);

// Fire-and-forget: schedules a reboot ~500 ms in the future via
// esp_timer so the caller can finish sending its response first.
void schedule_reboot();

#if CONFIG_NBP_BLE
// Scan duty cycle: GET returns {"window":W,"interval":I}; POST takes
// "window=N&interval=N" (ms each). window <= interval, both 20..10000.
size_t build_scan_json(char *buf, size_t cap);
const char *handle_scan_set(const char *query);

// Read persisted scan window/interval (NVS keys "scan_win"/"scan_int")
// and apply to the live scanner. Call AFTER ble_backend::start so
// scanner::init has set up the NimBLEScan object.
void apply_scan_from_nvs();
#endif  // CONFIG_NBP_BLE

#if CONFIG_NBP_WEB_CONSOLE
// Install the esp_log vprintf hook so device logs mirror into an
// in-memory ring buffer. Call as early as possible in app_main so we
// catch boot-time logs. The original vprintf (UART/JTAG) is still
// invoked, so console output is preserved.
void install_log_hook();
#endif

// Read persisted NimBLE log level from NVS and apply via
// esp_log_level_set. Must be called AFTER nvs_flash_init and BEFORE
// ble_backend::start() so the level is in effect by the time NimBLE
// initialises. Default if no key present: ESP_LOG_WARN.
void apply_log_overrides_from_nvs();

// Read persisted WiFi + BLE TX power (dBm) from NVS and apply them.
// Call AFTER wifi_sta::start_and_wait_for_ip() AND ble_backend::start()
// — both radios must be up before set-power calls succeed. Defaults if
// no NVS entry: WiFi 20 dBm, BLE +9 dBm (chip maxes).
void apply_tx_power_from_nvs();

// Read persisted CPU clock (80/160/240 MHz) from NVS and apply via
// esp_pm_configure. Safe to call any time after nvs_flash_init; calling
// early (before WiFi/BLE init) makes the radios' init use the chosen
// frequency from the start. Default if no NVS entry: 240 MHz.
void apply_cpu_freq_from_nvs();

#if CONFIG_NBP_BLE
// Read persisted advertising interval from NVS and apply it via
// ble_backend::set_adv_interval_ms. Must run AFTER ble_backend::start
// so NimBLEDevice::init has created the singleton; runs before the
// first g_adv->start() in ble_httpd::activate or clone start_advertising
// so the configured interval lands in the first HCI window.
void apply_adv_interval_from_nvs();
#endif  // CONFIG_NBP_BLE

// Read persisted hostname from NVS into proxy::g_hostname. Must run
// AFTER nvs_flash_init and BEFORE any consumer (wifi_sta, mdns_announce,
// ble_backend, handshake) reads proxy::hostname(). No-op if no NVS key
// or stored value fails validation — buffer keeps the compile-time
// default it was statically initialised with.
void apply_hostname_from_nvs();

// Registers all dashboard URIs on the OTA httpd: /, /favicon.svg,
// /stats.json, /log, /level, /trace, /reboot, /txpower, /cpufreq,
// /scan, /advitvl, /wifips, /hostname, /devices. SMP passkey is
// reached via /clone (gated on CONFIG_NBP_SMP+CONFIG_NBP_CLONE, see
// ble_clone/clone.cpp). Only defined when WiFi (and therefore the
// HTTP server) is in the build.
#if CONFIG_NBP_WIFI
void register_endpoints(httpd_handle_t srv);
#endif

}  // namespace api_server::stats
