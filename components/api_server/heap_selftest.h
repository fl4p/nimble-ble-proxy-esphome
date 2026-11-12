// QEMU-only heap self-test driver, see heap_selftest.cpp. Built only with
// -DNBP_HEAP_SELFTEST=1; never part of firmware.
#pragma once

namespace api_server::heap_selftest {
// Takes over app_main and never returns in practice (not [[noreturn]], for
// the same link reason as oom_selftest::run).
void run();
}  // namespace api_server::heap_selftest
