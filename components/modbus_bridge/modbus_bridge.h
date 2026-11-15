// Transparent RS485 <-> TCP byte bridge (Modbus RTU over TCP, ser2net-style).
// Moves bytes only: no CRC, no framing beyond line-idle detection. One TCP
// client on the STA side; a new connection replaces the old one. Plain UART
// with the transceiver's direction pin (DE and /RE tied) driven as a GPIO.

#pragma once

#include <cstddef>

#include "sdkconfig.h"

#ifdef CONFIG_NBP_MODBUS_BRIDGE

namespace modbus_bridge {

// Load NVS overrides, set up UART + direction pin, spawn the bridge task.
// Call after the STA is up. Idempotent.
void start();

// GET /modbus (config + client) and POST /modbus on the dashboard httpd.
void register_endpoints(void *httpd_handle);

// Appends ,"mb_*":n pairs for /stats.json. Returns what snprintf returns.
int append_stats_json(char *buf, size_t cap);

}  // namespace modbus_bridge

#endif  // CONFIG_NBP_MODBUS_BRIDGE
