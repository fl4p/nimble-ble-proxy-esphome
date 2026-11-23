// Host test for the OTA verification policy (ota_verify_policy.h).
//
//   c++ -std=c++17 -Wall -Wextra -Werror -I components/ota
//       components/ota/host_test/test_ota_verify_policy.cpp -o /tmp/t && /tmp/t

#include "ota_verify_policy.h"

#include <cstdio>

using namespace ota_verify::policy;

static int g_fail = 0;

static void check(const char *what, bool ok) {
  if (!ok) ++g_fail;
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

int main() {
  const Params p{60, 900};
  const Inputs all{0, true, true, true, true};

  // Evidence before the minimum uptime is not enough.
  Inputs in = all;
  for (uint32_t t = 0; t < 60; ++t) {
    in.uptime_s = t;
    if (decide(in, p) != Verdict::Wait) {
      check("all evidence before min_s waits", false);
      break;
    }
  }
  in.uptime_s = 60;
  check("all evidence at min_s is valid", decide(in, p) == Verdict::Valid);

  // Each missing piece alone blocks Valid (unevaluable = missing).
  const char *names[] = {"hooks_ready", "sta_up", "uplink", "service"};
  for (int k = 0; k < 4; ++k) {
    Inputs m = all;
    m.uptime_s = 120;
    bool *f[] = {&m.hooks_ready, &m.sta_up, &m.uplink, &m.service};
    *f[k] = false;
    char what[64];
    std::snprintf(what, sizeof(what), "missing %s before deadline waits", names[k]);
    check(what, decide(m, p) == Verdict::Wait);
    m.uptime_s = 900;
    std::snprintf(what, sizeof(what), "missing %s at deadline rolls back", names[k]);
    check(what, decide(m, p) == Verdict::Rollback);
  }

  // Monotonicity: without evidence, once Rollback, always Rollback; never
  // Valid at any uptime, including the far tail.
  Inputs none{0, true, false, false, false};
  bool seen_rb = false, flip = false, valid = false;
  for (uint64_t t = 0; t <= 0xFFFFFFFFull; t += (t < 2000 ? 1 : 0x1000000)) {
    none.uptime_s = static_cast<uint32_t>(t);
    const Verdict v = decide(none, p);
    if (v == Verdict::Valid) valid = true;
    if (v == Verdict::Rollback) seen_rb = true;
    else if (seen_rb) flip = true;
  }
  check("no evidence is never valid (0 .. 2^32-1 s)", !valid);
  check("no evidence: rollback is sticky", seen_rb && !flip);
  none.uptime_s = 899;
  check("no evidence at deadline-1 waits", decide(none, p) == Verdict::Wait);

  // Evidence arriving late (at/after the deadline) wins over the deadline.
  in = all;
  in.uptime_s = 900;
  check("evidence at the deadline is valid", decide(in, p) == Verdict::Valid);

  // Hooks not wired is never proof, even with every other flag set.
  in = all;
  in.hooks_ready = false;
  in.uptime_s = 600;
  check("hooks not wired waits", decide(in, p) == Verdict::Wait);

  // Old-bootloader start counter.
  check("start 1 of 3 kept", !starts_exceeded(1, 3));
  check("start 2 of 3 kept", !starts_exceeded(2, 3));
  check("start 3 of 3 rolls back", starts_exceeded(3, 3));
  check("start 0xFFFFFFFF rolls back", starts_exceeded(0xFFFFFFFFu, 3));

  std::printf("%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
