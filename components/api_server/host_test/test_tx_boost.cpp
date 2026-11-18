// Host test for the TX-boost helpers (tx_boost.h).
//
//   c++ -std=c++17 -Wall -Wextra -Werror -I components/api_server \
//       components/api_server/host_test/test_tx_boost.cpp -o /tmp/t && /tmp/t

#include "tx_boost.h"

#include <cstdio>
#include <cstring>

using namespace api_server::tx_boost;

static int g_fail = 0;

static void check(const char *what, bool ok) {
  if (!ok) ++g_fail;
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

static bool parses(const char *s, uint32_t want) {
  uint32_t v = 0;
  return parse_for(s, &v) == nullptr && v == want;
}

static bool rejects(const char *s) {
  uint32_t v = 12345;
  return parse_for(s, &v) != nullptr && v == 12345;  // out untouched on error
}

int main() {
  check("for=1", parses("1", 1));
  check("for=300", parses("300", 300));
  check("for=510 (ota_mqtt.py)", parses("510", 510));
  check("for=900 (cap)", parses("900", 900));
  check("rejects 0", rejects("0"));
  check("rejects 901", rejects("901"));
  check("rejects negative", rejects("-5"));
  check("rejects empty", rejects(""));
  check("rejects null", rejects(nullptr));
  check("rejects trailing junk", rejects("300s"));
  check("rejects float", rejects("1.5"));
  check("rejects huge", rejects("99999999999999999999"));

  constexpr int64_t S = 1000000;
  check("no boost reads 0", remaining_s(0, 100 * S) == 0);
  check("expired reads 0", remaining_s(100 * S, 100 * S) == 0);
  check("past reads 0", remaining_s(100 * S, 101 * S) == 0);
  check("full 300 s", remaining_s(400 * S, 100 * S) == 300);
  check("1 us left rounds up to 1", remaining_s(100 * S + 1, 100 * S) == 1);
  check("299.5 s rounds up to 300", remaining_s(400 * S, 100 * S + S / 2) == 300);

  std::printf(g_fail ? "%d FAILED\n" : "ALL PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
