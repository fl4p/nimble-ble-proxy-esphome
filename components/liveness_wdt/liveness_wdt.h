// Liveness watchdog: a supervisor task that probes upstream LAN reachability
// and esp_restart()s the device after sustained failure. Recovers a total
// network wedge (WiFi/coex livelock under heavy forwarded throughput) where
// tasks stay alive — so the IDF Task-WDT never fires — but the IP path is dead.
// The probe is multi-target (STA gateway + a known-open LAN host) and only a
// cycle where *all* targets are unreachable counts as a failure, so a single
// filtered/down target can't trigger a spurious reboot.

#pragma once

#include <cstdint>

namespace liveness_wdt {

// Spawn the probe task. Loads enabled/interval/threshold from NVS first.
// Idempotent — a second call is a no-op. No-op when CONFIG_NBP_LIVENESS_WDT=n.
void start();

struct State {
  bool enabled;
  uint32_t interval_s;     // seconds between probe cycles
  uint32_t threshold;      // consecutive failed cycles before reboot
  uint32_t failures;       // current consecutive-failure count
  uint32_t last_ok_age_s;  // seconds since the last reachable cycle (UINT32_MAX if never)
};

State get_state();

// Update runtime params and persist to NVS. Pass -1 for any field to leave it
// unchanged. Returns nullptr on success or a static error string.
const char *set_params(int enabled, int interval_s, int threshold);

// Force the failure counter to the threshold so the next tick reboots — proves
// the recovery path end-to-end without having to wedge the network.
void force_fail_test();

}  // namespace liveness_wdt
