// QEMU-only WebSocket Origin / POST confirm self-test driver, see
// ws_selftest.cpp. Built only with -DNBP_WS_SELFTEST=1; never part of
// firmware.
#pragma once

namespace ws_selftest {
// Takes over app_main: runs every step, prints a summary, then idles.
// Not [[noreturn]], for the same link reason as api_server::oom_selftest.
void run();
}  // namespace ws_selftest
