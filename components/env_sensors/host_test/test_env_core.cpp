// Host test for env_core.h (CRCs, conversions, /sensors JSON bound).
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I components/env_sensors \
//       components/env_sensors/host_test/test_env_core.cpp -o /tmp/t && /tmp/t

#include "env_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static int g_fail = 0;

static void check(bool ok, const char *what) {
  if (!ok) ++g_fail;
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

static bool near(float a, float b) { return std::fabs(a - b) < 0.005f; }

int main() {
  using namespace env;
  // Sensirion datasheet example: 0xBEEF -> 0x92.
  const uint8_t beef[] = {0xBE, 0xEF};
  check(crc8_sht(beef, 2) == 0x92, "crc8_sht(BEEF) == 92");
  // Maxim AN27 ROM example: 02 1C B8 01 00 00 00 -> A2.
  const uint8_t rom[] = {0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00};
  check(crc8_ow(rom, 7) == 0xA2, "crc8_ow(AN27 ROM) == A2");

  float t = 0, rh = 0;
  uint8_t b[6] = {0x00, 0x00, 0, 0xFF, 0xFF, 0};
  b[2] = crc8_sht(b, 2);
  b[5] = crc8_sht(b + 3, 2);
  check(sht_parse(b, &t, &rh) == kOk && near(t, -45) && near(rh, 100), "sht raw 0 / FFFF");
  b[0] = 0xFF, b[1] = 0xFF, b[2] = crc8_sht(b, 2);
  b[3] = 0x80, b[4] = 0x00, b[5] = crc8_sht(b + 3, 2);
  check(sht_parse(b, &t, &rh) == kOk && near(t, 130) && near(rh, 50.0008f), "sht raw FFFF / 8000");
  b[0] = 0x66, b[1] = 0x66, b[2] = crc8_sht(b, 2);  // 0x6666 -> 25.0 C
  check(sht_parse(b, &t, &rh) == kOk && near(t, 25.0f), "sht raw 6666 = 25.00 C");
  b[5] ^= 1;
  float t0 = 1, rh0 = 2;
  check(sht_parse(b, &t0, &rh0) == kCrc && t0 == 1 && rh0 == 2, "sht RH crc error, outputs untouched");
  b[5] ^= 1;
  b[2] ^= 0x80;
  check(sht_parse(b, &t0, &rh0) == kCrc, "sht T crc error");

  // DS18B20 power-on scratchpad (85 C) has CRC 0x1C.
  uint8_t sp[9] = {0x50, 0x05, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x1C};
  check(ds_parse(sp, &t) == kPor, "ds 85 C power-on value -> por");
  struct { uint16_t raw; float c; } v[] = {
      {0x07D0, 125}, {0x0191, 25.0625f}, {0x0000, 0}, {0xFFF8, -0.5f},
      {0xFF5E, -10.125f}, {0xFC90, -55}};
  for (auto &x : v) {
    sp[0] = x.raw & 0xFF, sp[1] = x.raw >> 8, sp[8] = crc8_ow(sp, 8);
    char what[48];
    std::snprintf(what, sizeof(what), "ds raw %04X = %.4f C", x.raw, static_cast<double>(x.c));
    check(ds_parse(sp, &t) == kOk && t == x.c, what);
  }
  sp[8] ^= 1;
  check(ds_parse(sp, &t) == kCrc, "ds crc error");
  uint8_t zeros[9] = {};
  check(ds_parse(zeros, &t) == kCrc, "ds all-zero scratchpad (bus low) rejected");
  uint8_t ff[9];
  std::memset(ff, 0xFF, 9);
  check(ds_parse(ff, &t) == kCrc, "ds all-FF scratchpad (no device) rejected");

  // render(): worst case 2 SHT + 16 DS (Kconfig max), longest values.
  const int64_t now = 4000000000LL * 1000000;  // ~127 years of uptime
  Reading s[2], d[16];
  for (auto &r : s) r = {0x45, -45.0f, 100.0f, 1, 4294967295u, kOk};
  for (auto &r : d) r = {0xFFFFFFFFFFFFFFFFull, -55.0f, 0, 1, 4294967295u, kOk};
  Counters c = {4294967295u, 4294967295u, 4294967295u, 4294967295u,
                4294967295u, 4294967295u, 4294967295u, 4294967295u};
  Cfg cfg = {-1, -1, -1, 3600, false, 65535};
  char buf[4096];
  int worst = 0;
  for (int pass = 0; pass < 3; pass++) {
    // 0: all values fresh (age large but < 3 periods), 1: all errors, 2: stale.
    int64_t at = pass == 0 ? now - 10799LL * 1000000 : 1;
    for (auto &r : s) r.at_us = at, r.err = pass == 1 ? kAbsent : kOk;
    for (auto &r : d) r.at_us = at, r.err = pass == 1 ? kAbsent : kOk;
    for (int nds : {4, 16}) {
      int n = render(buf, sizeof(buf), s, 2, d, nds, c, cfg, now);
      size_t bound = kJsonBase + 2 * kJsonSht + nds * kJsonDs;
      char what[96];
      std::snprintf(what, sizeof(what), "render pass %d, 2 SHT + %d DS: %d B <= bound %zu", pass,
                    nds, n, bound);
      check(n > 0 && static_cast<size_t>(n) <= bound, what);
      if (n > worst) worst = n;
      if (pass == 0 && nds == 4) std::printf("    %s\n", buf);
      if (pass == 1 && nds == 4) std::printf("    %s\n", buf);
    }
  }
  std::printf("worst case %d B\n", worst);
  check(render(buf, 100, s, 2, d, 4, c, cfg, now) == -1, "render reports a short buffer");

  // Freshness: fresh value shown, old one (> 3 periods) is null/stale.
  Reading one = {0x44, 21.4f, 55.2f, 0, 0, kOk};
  Cfg c30 = {8, 9, 7, 30, true, 1000};
  Counters z = {};
  one.at_us = now - 12LL * 1000000;
  render(buf, sizeof(buf), &one, 1, nullptr, 0, z, c30, now);
  check(std::strstr(buf, "\"t\":21.40,\"rh\":55.20,\"age_s\":12}") != nullptr, "fresh value with age");
  one.at_us = now - 91LL * 1000000;
  render(buf, sizeof(buf), &one, 1, nullptr, 0, z, c30, now);
  check(std::strstr(buf, "\"t\":null,\"rh\":null,\"err\":\"stale\"") != nullptr, "91 s old at 30 s period -> stale");
  one.at_us = now - 1;
  one.err = kCrc;
  one.fails = 1;
  render(buf, sizeof(buf), &one, 1, nullptr, 0, z, c30, now);
  check(std::strstr(buf, "\"t\":null,\"rh\":null,\"err\":\"crc\",\"fails\":1") != nullptr,
        "failed last attempt -> null, never the last good value");
  std::printf("%s\n", g_fail ? "FAILED" : "ALL PASS");
  return g_fail ? 1 : 0;
}
