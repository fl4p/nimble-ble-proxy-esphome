#pragma once

// Panic capture that survives the reboot (see Kconfig NBP_CRASH_CAPTURE).
//
// A wrapper around esp_panic_handler copies the exception into RTC_NOINIT
// memory; init() reports it on the next boot. With a flash core dump
// enabled, init() also makes sure there is a coredump partition (registering
// the free flash after the last partition when the table has none).

#include <cstddef>

#include "esp_http_server.h"

namespace crash_capture {

// Call once, early in app_main, after the /log hook is installed so the
// report lands in the web console. Logs the previous panic(s), if any.
void init();

// GET /crash (JSON), POST /crash?erase=1, GET /coredump (raw ELF core),
// and, with CONFIG_NBP_CRASH_TEST, POST /crash-test.
void register_endpoints(httpd_handle_t srv);

// Adds `"ota":{…}` to GET /crash: fn writes one JSON object into buf and
// returns its length (0 = nothing). Wired by main (ota_verify::write_json),
// so crash_capture keeps no dependency on the ota component.
void set_ota_json_provider(size_t (*fn)(char *buf, size_t len));

}  // namespace crash_capture
