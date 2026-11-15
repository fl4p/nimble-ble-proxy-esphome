// SHT3x / DS18B20 frame checks, conversions and the /sensors JSON body.
// No IDF dependencies (host-tested: host_test/test_env_core.cpp).

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace env {

enum Err : uint8_t { kOk, kCrc, kI2c, kAbsent, kBus, kPor, kStale };

inline const char *err_name(uint8_t e) {
  static const char *const n[] = {"ok", "crc", "i2c", "absent", "bus", "por", "stale"};
  return e <= kStale ? n[e] : "?";
}

// Sensirion: poly 0x31, init 0xFF, MSB first.
inline uint8_t crc8_sht(const uint8_t *p, size_t n) {
  uint8_t c = 0xFF;
  while (n--) {
    c ^= *p++;
    for (int i = 0; i < 8; i++) c = (c & 0x80) ? (c << 1) ^ 0x31 : c << 1;
  }
  return c;
}

// Maxim 1-Wire: poly 0x31 reflected (0x8C), init 0, LSB first.
inline uint8_t crc8_ow(const uint8_t *p, size_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= *p++;
    for (int i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0x8C : c >> 1;
  }
  return c;
}

// SHT3x measurement reply: T msb, lsb, crc, RH msb, lsb, crc.
inline Err sht_parse(const uint8_t b[6], float *t, float *rh) {
  if (crc8_sht(b, 2) != b[2] || crc8_sht(b + 3, 2) != b[5]) return kCrc;
  *t = -45.0f + 175.0f * static_cast<float>((b[0] << 8) | b[1]) / 65535.0f;
  float h = 100.0f * static_cast<float>((b[3] << 8) | b[4]) / 65535.0f;
  *rh = h < 0 ? 0 : h > 100 ? 100 : h;
  return kOk;
}

// DS18B20 scratchpad (9 B). A bus held low reads all zeros with a valid
// CRC; the config byte (0RR11111) rejects that. 85.0 C is the power-on
// value: the conversion did not run.
inline Err ds_parse(const uint8_t sp[9], float *t) {
  if (crc8_ow(sp, 8) != sp[8] || (sp[4] & 0x9F) != 0x1F) return kCrc;
  int16_t raw = static_cast<int16_t>((sp[1] << 8) | sp[0]);
  if (raw == 0x0550) return kPor;
  *t = raw / 16.0f;
  return kOk;
}

struct Reading {
  uint64_t id;     // SHT: I2C address; DS: ROM, family byte lowest; 0 = free
  float t, rh;     // valid only with err == kOk
  int64_t at_us;   // time of the last good sample
  uint32_t fails;  // consecutive failed attempts
  uint8_t err;     // of the last attempt
};

struct Counters {
  uint32_t cycles, sht_crc, sht_i2c, ds_crc, ds_miss, ds_bus, ds_por, ds_full;
};

struct Cfg {
  int sda, scl, ow, period_s;
  bool running;
  unsigned stack_free;
};

// Bound for render(): frame + cfg + counters, then per reading.
constexpr size_t kJsonBase = 300, kJsonSht = 72, kJsonDs = 74;

struct Out {
  char *buf;
  size_t cap, n;
  bool over;
  __attribute__((format(printf, 2, 3))) void put(const char *fmt, ...) {
    if (over) return;
    va_list ap;
    va_start(ap, fmt);
    int m = std::vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    if (m < 0 || static_cast<size_t>(m) >= cap - n) over = true;
    else n += m;
  }
};

inline void render_one(Out &o, const Reading &r, bool is_sht, int period_s,
                       int64_t now_us) {
  if (is_sht) {
    o.put("{\"addr\":%u", static_cast<unsigned>(r.id));
  } else {
    o.put("{\"id\":\"");
    for (int i = 0; i < 8; i++) o.put("%02x", static_cast<unsigned>(r.id >> (8 * i)) & 0xFF);
    o.put("\"");
  }
  uint8_t e = r.err;
  long age = r.at_us ? static_cast<long>((now_us - r.at_us) / 1000000) : -1;
  if (e == kOk && (age < 0 || age > 3L * period_s)) e = kStale;
  if (e == kOk) {
    o.put(",\"t\":%.2f", static_cast<double>(r.t));
    if (is_sht) o.put(",\"rh\":%.2f", static_cast<double>(r.rh));
    o.put(",\"age_s\":%ld}", age);
  } else {
    o.put(is_sht ? ",\"t\":null,\"rh\":null" : ",\"t\":null");
    o.put(",\"err\":\"%s\",\"fails\":%lu}", err_name(e), static_cast<unsigned long>(r.fails));
  }
}

// A value is reported only if the last attempt succeeded and it is younger
// than 3 periods; otherwise null with the reason. Returns the length, or
// -1 if cap was too small.
inline int render(char *buf, size_t cap, const Reading *sht, int nsht,
                  const Reading *ds, int nds, const Counters &c, const Cfg &cfg,
                  int64_t now_us) {
  Out o = {buf, cap, 0, false};
  o.put("{\"sht\":[");
  for (int k = 0; k < 2; k++) {
    const Reading *r = k ? ds : sht;
    bool first = true;
    if (k) o.put("],\"ds\":[");
    for (int i = 0; i < (k ? nds : nsht); i++) {
      if (!r[i].id) continue;
      if (!first) o.put(",");
      first = false;
      render_one(o, r[i], !k, cfg.period_s, now_us);
    }
  }
  o.put("],\"err\":{\"sht_crc\":%lu,\"sht_i2c\":%lu,\"ds_crc\":%lu,\"ds_miss\":%lu,"
        "\"ds_bus\":%lu,\"ds_por\":%lu,\"ds_full\":%lu}",
        static_cast<unsigned long>(c.sht_crc), static_cast<unsigned long>(c.sht_i2c),
        static_cast<unsigned long>(c.ds_crc), static_cast<unsigned long>(c.ds_miss),
        static_cast<unsigned long>(c.ds_bus), static_cast<unsigned long>(c.ds_por),
        static_cast<unsigned long>(c.ds_full));
  o.put(",\"cfg\":{\"sda\":%d,\"scl\":%d,\"ow\":%d,\"period_s\":%d,\"running\":%s,"
        "\"stack_free\":%u,\"cycles\":%lu}}",
        cfg.sda, cfg.scl, cfg.ow, cfg.period_s, cfg.running ? "true" : "false",
        cfg.stack_free, static_cast<unsigned long>(c.cycles));
  return o.over ? -1 : static_cast<int>(o.n);
}

}  // namespace env
