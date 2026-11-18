// Host test for the liveness watchdog policy (liveness_policy.h).
//
//   c++ -std=c++17 -Wall -Wextra -Werror -I components/liveness_wdt \
//       components/liveness_wdt/host_test/test_liveness_policy.cpp -o /tmp/t && /tmp/t

#include "liveness_policy.h"

#include <cstdio>
#include <cstdlib>

using namespace liveness_wdt::policy;

static int g_fail = 0;

static void check(const char *what, bool ok) {
  if (!ok) ++g_fail;
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

// One probe cycle: probe result, RTT, client vouch.
struct Cyc {
  bool ok;
  uint16_t rtt;
  bool client;
};

// Feed cycles; return the index of the first non-NONE action (or -1) and the action.
static int run(Decider &d, const Cyc *c, int n, uint32_t thr, Action *first, bool armed = true) {
  for (int i = 0; i < n; ++i) {
    Action a = d.on_cycle(cycle_bad(c[i].ok, c[i].rtt, c[i].client), thr, armed);
    if (a != Action::NONE) {
      *first = a;
      return i;
    }
  }
  *first = Action::NONE;
  return -1;
}

// The old rule: `thr` consecutive failed probes (slow but answered = pass).
static int run_old(const Cyc *c, int n, uint32_t thr) {
  uint32_t f = 0;
  for (int i = 0; i < n; ++i) {
    if (c[i].ok || c[i].client) f = 0;
    else if (++f >= thr) return i;
  }
  return -1;
}

int main() {
  const uint32_t THR = 6;

  // cycle_bad
  check("fast probe is good", !cycle_bad(true, 20, false));
  check("probe at the slow limit is good", !cycle_bad(true, SLOW_RTT_MS, false));
  check("slow probe is bad", cycle_bad(true, SLOW_RTT_MS + 1, false));
  check("failed probe is bad", cycle_bad(false, 0xFFFF, false));
  check("failed probe + vouching client is good", !cycle_bad(false, 0xFFFF, true));
  check("slow probe + vouching client is good", !cycle_bad(true, 1500, true));

  // Window arithmetic, including wrap and size change.
  {
    Window w;
    for (int i = 0; i < 8; ++i) w.push(i % 2 == 0, 8);  // B G B G B G B G
    check("window counts 4 bad of 8", w.bad() == 4 && w.filled() == 8);
    w.push(false, 8);  // evicts the oldest (bad)
    check("eviction drops the oldest bad", w.bad() == 3);
    w.push(true, 10);  // size change -> reset, then push
    check("size change resets", w.bad() == 1 && w.filled() == 1 && w.size() == 10);
    Window big;
    for (int i = 0; i < 5000; ++i) big.push(true, 99999);
    check("window clamps to MAX_WINDOW", big.size() == MAX_WINDOW && big.bad() == MAX_WINDOW);
  }

  // (1) Healthy link: never acts, even with an isolated failure or slow probe.
  {
    Decider d;
    Action a;
    Cyc c[400];
    for (int i = 0; i < 400; ++i) c[i] = {i % 37 != 0, static_cast<uint16_t>(i % 53 == 0 ? 1200 : 20), false};
    check("healthy link with sporadic misses: no action", run(d, c, 400, THR, &a) == -1);
  }

  // (2) A 30-50 % loss link that occasionally lets one probe through: the old
  // rule is reset by every lucky probe; the window fires.
  {
    // pattern per 4 cycles: fail, fail, slow-ok(1400 ms), fail -> 1 lucky in 4
    Cyc c[80];
    for (int i = 0; i < 80; ++i) c[i] = (i % 4 == 2) ? Cyc{true, 1400, false} : Cyc{false, 0xFFFF, false};
    Decider d;
    Action a;
    int at = run(d, c, 80, THR, &a);
    check("lossy+slow link: window fires within 8 cycles", at >= 0 && at < 8 && a == Action::RECONNECT);
    check("lossy+slow link: old rule never fires", run_old(c, 80, THR) == -1);
  }

  // (3) Same, but the lucky probe is FAST (link passes a probe now and then):
  // 3 bad of every 4 still trips 6-of-8.
  {
    Cyc c[80];
    for (int i = 0; i < 80; ++i) c[i] = (i % 4 == 2) ? Cyc{true, 25, false} : Cyc{false, 0xFFFF, false};
    Decider d;
    Action a;
    int at = run(d, c, 80, THR, &a);
    check("75 % loss with fast lucky probes: fires", at >= 0 && at < 10);
    check("75 % loss: old rule never fires", run_old(c, 80, THR) == -1);
  }

  // (4) 50 % loss with fast passes: below 6-of-8, must NOT fire (degraded but usable).
  {
    Cyc c[200];
    for (int i = 0; i < 200; ++i) c[i] = (i % 2) ? Cyc{true, 30, false} : Cyc{false, 0xFFFF, false};
    Decider d;
    Action a;
    check("50 % loss with fast passes: no action", run(d, c, 200, THR, &a) == -1);
  }

  // (5) Escalation: trigger -> RECONNECT; still dead -> REBOOT on the next trigger.
  {
    Decider d;
    Action a;
    Cyc dead[40];
    for (auto &x : dead) x = {false, 0xFFFF, false};
    int t1 = run(d, dead, 40, THR, &a);
    check("dead link: first trigger reconnects at cycle 6", t1 == THR - 1 && a == Action::RECONNECT);
    int t2 = run(d, dead, 40, THR, &a);
    check("still dead after reconnect: reboot 6 cycles later", t2 == THR - 1 && a == Action::REBOOT);
    check("reconnect counted once", d.reconnects() == 1);
  }

  // (6) Reconnect fixed it; a much later trigger reconnects again instead of rebooting.
  {
    Decider d;
    Action a;
    Cyc dead[10], good[40];
    for (auto &x : dead) x = {false, 0xFFFF, false};
    for (auto &x : good) x = {true, 20, false};
    run(d, dead, 10, THR, &a);
    run(d, good, 40, THR, &a);
    int t = run(d, dead, 10, THR, &a);
    check("late recurrence after a good spell: reconnect, not reboot", t >= 0 && a == Action::RECONNECT);
    check("two reconnects counted", d.reconnects() == 2);
  }

  // (7) Unarmed (no probe target ever answered): never acts.
  {
    Decider d;
    Action a;
    Cyc dead[100];
    for (auto &x : dead) x = {false, 0xFFFF, false};
    check("unarmed: no action", run(d, dead, 100, THR, &a, /*armed=*/false) == -1);
  }

  // (8) Monotonicity: as loss rises from 0 to 100 %, once the policy acts it
  // keeps acting (never flips back to "no action" at a worse loss rate).
  {
    bool acted_before = false, monotone = true;
    for (int lossq = 0; lossq <= 8; ++lossq) {  // loss = lossq / 8
      Cyc c[160];
      for (int i = 0; i < 160; ++i) c[i] = ((i % 8) < lossq) ? Cyc{false, 0xFFFF, false} : Cyc{true, 20, false};
      Decider d;
      Action a;
      bool acted = run(d, c, 160, THR, &a) >= 0;
      if (acted_before && !acted) monotone = false;
      acted_before = acted_before || acted;
      std::printf("  loss %d/8: %s\n", lossq, acted ? "acts" : "no action");
    }
    check("monotone in loss rate", monotone && acted_before);
  }

  // (9) Backoff: threshold 6 << 5 = 192 bad cycles needed, window 194.
  {
    Decider d;
    Action a;
    static Cyc dead[400];
    for (auto &x : dead) x = {false, 0xFFFF, false};
    int t = run(d, dead, 400, THR << 5, &a);
    check("backed-off threshold fires at cycle 192", t == static_cast<int>((THR << 5) - 1));
  }

  std::printf(g_fail ? "%d FAILED\n" : "ALL PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
