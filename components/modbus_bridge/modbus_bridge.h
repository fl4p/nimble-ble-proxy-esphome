// RS485 <-> TCP bridge (Modbus RTU over TCP, ser2net-style). Requests must
// be valid RTU frames (length, CRC); replies pass through as bytes, framed by
// line idle. One TCP client on the STA side. Plain UART with the
// transceiver's direction pin (DE and /RE tied) driven as a GPIO.

#pragma once

#include <cstddef>

#include "sdkconfig.h"

#ifdef CONFIG_NBP_MODBUS_BRIDGE

namespace modbus_bridge {

// Load NVS overrides, drive DE low and pull TX up. Call right after
// nvs_flash_init, before WiFi. Idempotent.
void early_init();

// early_init() if not done, set up the UART, spawn the bridge task. Refuses
// (logs) on disallowed pins. Call after the STA is up. Idempotent.
void start();

// GET /modbus (config + client) and POST /modbus on the dashboard httpd.
void register_endpoints(void *httpd_handle);

// Appends ,"mb_*":n pairs for /stats.json. Returns what snprintf returns.
int append_stats_json(char *buf, size_t cap);

}  // namespace modbus_bridge

#endif  // CONFIG_NBP_MODBUS_BRIDGE
