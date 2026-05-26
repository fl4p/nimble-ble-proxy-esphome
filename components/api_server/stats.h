// Lightweight transaction counters + a tiny web UI that renders them
// as live rates. Counters are bumped from bt_handlers; the HTTP routes
// piggyback on the OTA component's httpd handle (one shared listener).

#pragma once

#include "esp_http_server.h"

namespace api_server::stats {

void record_read();
void record_write();
void record_notify();

// Install the esp_log vprintf hook so device logs mirror into an
// in-memory ring buffer. Call as early as possible in app_main so we
// catch boot-time logs. The original vprintf (UART/JTAG) is still
// invoked, so console output is preserved.
void install_log_hook();

// Registers `/`, `/stats.json`, and `/log` on the given httpd.
void register_endpoints(httpd_handle_t srv);

}  // namespace api_server::stats
