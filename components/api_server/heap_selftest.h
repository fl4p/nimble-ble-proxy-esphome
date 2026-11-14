// QEMU-only heap self-test driver, see heap_selftest.cpp. Built only with
// -DNBP_HEAP_SELFTEST=1; never part of firmware.
#pragma once

#include <cstdint>

namespace api_server::heap_selftest {
// GetServices for this address runs bulk_burst() on the requesting client's
// connection task instead of a discovery (bt_handlers, self-test only).
inline constexpr uint64_t kBulkAddress = 0x0000DEADBEEF0002ULL;
void bulk_burst(uint32_t client_id);

// Takes over app_main and never returns in practice (not [[noreturn]], for
// the same link reason as oom_selftest::run).
void run();
}  // namespace api_server::heap_selftest
