#pragma once

#include "sdkconfig.h"

// Network/heap benchmark server (CONFIG_NBP_BENCH, default n). Test builds
// only: it opens a TCP listener and a UDP sink that anyone on the LAN can
// drive at line rate. See docs/wifi-buffer-throughput.md.
namespace bench {
#if CONFIG_NBP_BENCH
void start();
#else
inline void start() {}
#endif
}  // namespace bench
