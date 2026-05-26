// Lightweight transaction counters + a tiny web UI that renders them
// as live rates. Counters are bumped from bt_handlers; the HTTP routes
// piggyback on the OTA component's httpd handle (one shared listener).

#pragma once

#include "esp_http_server.h"

namespace api_server::stats {

void record_read();
void record_write();
void record_notify();

// Registers `/` (HTML dashboard) and `/stats.json` on the given httpd.
void register_endpoints(httpd_handle_t srv);

}  // namespace api_server::stats
