// One switched GPIO output (e.g. a MOSFET closing an inverter's remote
// on/off contact). The pad is held, so its level survives software resets.
// Boot state: off, on or the last commanded one (NVS).

#pragma once

#include <cstddef>

#include "sdkconfig.h"

#ifdef CONFIG_NBP_SWITCH_OUT

namespace switch_out {

// Load NVS, drive the boot state and hold the pad. Call right after
// nvs_flash_init. Refuses (logs) a disallowed pin. Idempotent.
void early_init();

// GET /switch and POST /switch on the dashboard httpd.
void register_endpoints(void *httpd_handle);

// Appends ,"sw_*":n pairs for /stats.json. Returns what snprintf returns.
int append_stats_json(char *buf, size_t cap);

}  // namespace switch_out

#endif  // CONFIG_NBP_SWITCH_OUT
