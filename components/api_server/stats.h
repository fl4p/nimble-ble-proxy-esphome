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

// Registers `/`, `/stats.json`, `/log`, `/level`, `/reboot`.
void register_endpoints(httpd_handle_t srv);

}  // namespace api_server::stats
