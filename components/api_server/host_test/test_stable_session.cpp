// Host test for the liveness client-traffic rule (stable_session.h).
//
//   c++ -std=c++17 -Wall -Wextra -Werror -I components/api_server \
//       components/api_server/host_test/test_stable_session.cpp -o /tmp/t && /tmp/t

#include "stable_session.h"

#include <cstdio>

using api_server::stable_session::newest_rx;
using api_server::stable_session::Snap;

static int g_fail = 0;

constexpr int64_t S = 1000000;
constexpr int64_t MIN_AGE = 60 * S;  // max(60 s, 2 x 30 s interval)

static void expect(const char *what, const Snap *s, size_t n, int64_t now, int64_t want) {
  int64_t got = newest_rx(s, n, now, MIN_AGE);
  bool ok = got == want;
  if (!ok) ++g_fail;
  std::printf("%s %s: got %lld want %lld\n", ok ? "PASS" : "FAIL", what,
              static_cast<long long>(got), static_cast<long long>(want));
}

int main() {
  const int64_t now = 10000 * S;

  // (1) one long-lived healthy session: its RX counts.
  Snap stable[] = {{now - 3600 * S, now - 5 * S}, {0, 0}};
  expect("long-lived session", stable, 2, now, now - 5 * S);

  // (2) a session that reconnects every ~40 s never reaches the minimum age.
  Snap churn[] = {{now - 40 * S, now - 1 * S}, {0, 0}};
  expect("churning session (40 s)", churn, 2, now, 0);
  Snap young[] = {{now - 59 * S, now - 1 * S}};
  expect("session just under min age", young, 1, now, 0);
  Snap edge[] = {{now - 60 * S, now - 1 * S}};
  expect("session exactly at min age", edge, 1, now, now - 1 * S);

  // (3) no clients at all.
  Snap none[] = {{0, 0}, {0, 0}};
  expect("no clients", none, 2, now, 0);

  // (4) one stable and one churning: only the stable one's RX counts, even
  // when the churning one is newer.
  Snap mixed[] = {{now - 30 * S, now - 1 * S}, {now - 900 * S, now - 20 * S}};
  expect("stable + churning", mixed, 2, now, now - 20 * S);

  // Old session that never sent a frame contributes nothing.
  Snap silent[] = {{now - 900 * S, 0}};
  expect("old silent session", silent, 1, now, 0);

  std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
