// SHT3x (I2C 0x44/0x45) and DS18B20 (one 1-Wire bus over RMT) sampled by a
// low-priority task. A reading carries its age, or null with the reason.

#pragma once

#include <cstddef>

#include "sdkconfig.h"

#ifdef CONFIG_NBP_ENV_SENSORS

namespace env_sensors {

// Load NVS overrides, validate and claim the pins. Call right after
// nvs_flash_init. Idempotent.
void early_init();

// early_init() if not done, open the buses, spawn the sampling task.
// Refuses (logs) on disallowed pins. Idempotent.
void start();

// GET /sensors (readings + config) and POST /sensors on the dashboard httpd.
void register_endpoints(void *httpd_handle);

// Appends ,"env_*":n pairs for /stats.json. Returns what snprintf returns.
int append_stats_json(char *buf, size_t cap);

}  // namespace env_sensors

#endif  // CONFIG_NBP_ENV_SENSORS
