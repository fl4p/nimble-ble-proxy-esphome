// QEMU-only OOM self-test driver, see oom_selftest.cpp. Built only with
// -DNBP_OOM_SELFTEST=1; never part of firmware.
#pragma once

namespace api_server::oom_selftest {
// Takes over app_main: runs every step, then ends in a deliberate abort.
// Deliberately not [[noreturn]]: the rest of app_main must stay reachable
// to the compiler, or nat_router drops out of the link and lwIP's
// nbp_ip4_canforward hook is left undefined.
void run();
}  // namespace api_server::oom_selftest
