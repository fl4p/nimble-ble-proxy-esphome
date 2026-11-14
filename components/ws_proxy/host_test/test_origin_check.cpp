// Host test for the /api WebSocket Origin policy (origin_check.h).
//
//   c++ -std=c++17 -Wall -Wextra -Werror -I components/ws_proxy \
//       components/ws_proxy/host_test/test_origin_check.cpp -o /tmp/t && /tmp/t

#include "origin_check.h"

#include <cstdio>

using ws_proxy::origin::check;
using ws_proxy::origin::name;
using ws_proxy::origin::Verdict;

static int g_fail = 0;

static void expect(const char *origin, const char *host, const char *allow,
                   Verdict want) {
  Verdict got = check(origin, host, allow);
  bool ok = got == want;
  if (!ok) ++g_fail;
  std::printf("%s  Origin=%-34s Host=%-24s allow=%-28s -> %s%s%s\n",
              ok ? "PASS" : "FAIL", origin ? origin : "(absent)",
              host ? host : "(absent)", allow, name(got),
              ok ? "" : ", want ", ok ? "" : name(want));
}

int main() {
  const char *none = "";
  // Non-browser clients.
  expect(nullptr, "192.168.1.231", none, Verdict::kAcceptNoOrigin);
  expect("", "192.168.1.231", none, Verdict::kAcceptNoOrigin);
  expect(nullptr, nullptr, none, Verdict::kAcceptNoOrigin);

  // The on-device dashboard.
  expect("http://192.168.1.231", "192.168.1.231", none,
         Verdict::kAcceptSameOrigin);
  expect("http://nimble-proxy.local", "nimble-proxy.local", none,
         Verdict::kAcceptSameOrigin);
  expect("http://Nimble-Proxy.LOCAL", "nimble-proxy.local", none,
         Verdict::kAcceptSameOrigin);
  expect("http://192.168.1.231", "192.168.1.231:80", none,
         Verdict::kAcceptSameOrigin);
  expect("http://192.168.1.231:80", "192.168.1.231", none,
         Verdict::kAcceptSameOrigin);
  expect("http://192.168.4.1:8080", "192.168.4.1:8080", none,
         Verdict::kAcceptSameOrigin);
  expect("http://[fe80::1]", "[fe80::1]", none, Verdict::kAcceptSameOrigin);
  expect("https://dev.lan", "dev.lan:443", none, Verdict::kAcceptSameOrigin);

  // Forged / foreign / null origins.
  expect("http://evil.example", "192.168.1.231", none, Verdict::kRejectForeign);
  expect("http://192.168.1.231.evil.example", "192.168.1.231", none,
         Verdict::kRejectForeign);
  expect("http://192.168.1.23", "192.168.1.231", none, Verdict::kRejectForeign);
  expect("http://192.168.1.231:8080", "192.168.1.231", none,
         Verdict::kRejectForeign);
  expect("http://192.168.1.231", "192.168.1.231:8080", none,
         Verdict::kRejectForeign);
  expect("https://192.168.1.231", "192.168.1.231:80", none,
         Verdict::kRejectForeign);
  expect("http://[fe80::1]", "[fe80::1]:81", none, Verdict::kRejectForeign);
  expect("null", "192.168.1.231", none, Verdict::kRejectNull);
  expect("NULL", "192.168.1.231", none, Verdict::kRejectNull);
  expect("null", "192.168.1.231", "null", Verdict::kRejectNull);
  expect("file://", "192.168.1.231", none, Verdict::kRejectMalformed);
  expect("chrome-extension://abc", "192.168.1.231", none,
         Verdict::kRejectMalformed);
  expect("http://", "192.168.1.231", none, Verdict::kRejectMalformed);
  expect("http://192.168.1.231/", "192.168.1.231", none,
         Verdict::kRejectMalformed);
  expect("http://x@192.168.1.231", "192.168.1.231", none,
         Verdict::kRejectMalformed);
  expect("http://192.168.1.231", nullptr, none, Verdict::kRejectNoHost);
  expect("http://192.168.1.231", "", none, Verdict::kRejectNoHost);

  // Allowlist.
  const char *allow = "https://a.github.io, http://10.0.0.5:8000";
  expect("https://a.github.io", "192.168.1.231", allow,
         Verdict::kAcceptAllowlist);
  expect("HTTPS://A.github.io", "192.168.1.231", allow,
         Verdict::kAcceptAllowlist);
  expect("http://10.0.0.5:8000", "192.168.1.231", allow,
         Verdict::kAcceptAllowlist);
  expect("https://a.github.io.evil", "192.168.1.231", allow,
         Verdict::kRejectForeign);
  expect("https://a.github", "192.168.1.231", allow, Verdict::kRejectForeign);
  expect("http://10.0.0.5", "192.168.1.231", allow, Verdict::kRejectForeign);
  expect("https://a.github.io", nullptr, allow, Verdict::kAcceptAllowlist);

  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
