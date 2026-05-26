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

// Registers /, /stats.json, /log, /level, /reboot, /trace, /devices,
// /passkey, /txpower, /cpufreq.
void register_endpoints(httpd_handle_t srv);

}  // namespace api_server::stats
