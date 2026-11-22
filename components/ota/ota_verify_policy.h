// Pure decision logic of the OTA image verification (ota_verify.cpp), kept
// free of IDF headers so the host test can build it:
//
//   c++ -std=c++17 -Wall -Wextra -Werror -I components/ota
//       components/ota/host_test/test_ota_verify_policy.cpp -o /tmp/t && /tmp/t

#pragma once

#include <cstdint>

namespace ota_verify::policy {

enum class Verdict : uint8_t { Wait, Valid, Rollback };

// One evaluation of the running, unverified image. `hooks_ready` is false
// until main has wired the checks (before that nothing is known, which must
// never count as proof). A check that is not applicable to the build (no
// WiFi) is passed as true by the caller; a check that failed or could not be
// run is false.
struct Inputs {
  uint32_t uptime_s;
  bool hooks_ready;
  bool sta_up;   // STA associated with an IP
  bool uplink;   // something beyond the AP answered (see ota_verify.h)
  bool service;  // own listeners (httpd :80, api_server :6053) accept
};

struct Params {
  uint32_t min_s;       // CONFIG_NBP_OTA_VERIFY_MIN_SECS
  uint32_t deadline_s;  // CONFIG_NBP_OTA_VERIFY_DEADLINE_SECS
};

inline bool criterion(const Inputs &in, const Params &p) {
  return in.hooks_ready && in.uptime_s >= p.min_s && in.sta_up && in.uplink && in.service;
}

// Evidence first: a criterion met at (or after) the deadline still counts,
// the deadline only ends the wait. Past the deadline without the evidence the
// verdict is Rollback and stays Rollback (uptime only grows).
inline Verdict decide(const Inputs &in, const Params &p) {
  if (criterion(in, p)) return Verdict::Valid;
  if (in.uptime_s >= p.deadline_s) return Verdict::Rollback;
  return Verdict::Wait;
}

// Old bootloader (no rollback): `starts` counts this image's unverified
// starts including the current one; the app rolls back at `max_starts`.
inline bool starts_exceeded(uint32_t starts, uint32_t max_starts) {
  return starts >= max_starts;
}

}  // namespace ota_verify::policy
