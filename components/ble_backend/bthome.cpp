#include "bthome.h"

#if CONFIG_NBP_BTHOME

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace ble_backend::bthome {

namespace {

constexpr size_t CACHE_SIZE = 32;

Reading g_cache[CACHE_SIZE];
size_t g_count = 0;
SemaphoreHandle_t g_mutex = nullptr;
StaticSemaphore_t g_mutex_buf;

void ensure_mutex() {
  if (!g_mutex) g_mutex = xSemaphoreCreateMutexStatic(&g_mutex_buf);
}

// Object-ID → payload byte count, per BTHome v2 (https://bthome.io/format/).
// Sentinels:
//   -1 : unknown / reserved object id — abort the rest of the frame
//        (BTHome has no separators, so once we lose sync we cannot
//        resume mid-stream).
//   -2 : variable-length — first payload byte is the length.
//
// Covers 0x00..0x5F; anything past that is treated as unknown.
constexpr int8_t kObjSize[] = {
    // 0x00..0x0F: packet_id, battery, temp, humidity, pressure, illuminance,
    //             mass-kg, mass-lb, dewpoint, count, energy24, power24,
    //             voltage, pm2_5, pm10, generic-bool
    1, 1, 2, 2, 3, 3, 2, 2, 2, 1, 3, 3, 2, 2, 2, 1,
    // 0x10..0x1F: power on/off, opening, CO2, TVOC, moisture16,
    //             then binary sensors (battery low, charging, cold, …)
    1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    // 0x20..0x2F: more binary sensors; 0x21 is motion
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    // 0x30..0x3F: binary sensors + button (0x3A), dimmer (0x3C),
    //             count16/32 (0x3D/0x3E), rotation (0x3F).
    //             0x3B is reserved — treat as unknown.
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, 2, 2, 4, 2,
    // 0x40..0x4F: distance mm/m, duration, current, speed, temp 0.1,
    //             UV, volume L / mL, volume flow, voltage 0.1,
    //             gas24, gas32, energy32, volume32, water32.
    2, 2, 3, 2, 2, 2, 1, 2, 2, 2, 2, 3, 4, 4, 4, 4,
    // 0x50..0x5F: timestamp, accel, gyro, text (var), raw (var),
    //             volume storage, conductivity, temp variants,
    //             low-battery / charging binaries; tail reserved.
    4, 2, 2, -2, -2, 4, 2, 1, 1, 1, 2, 1, 1, -1, -1, -1,
};
constexpr size_t kObjMax = sizeof(kObjSize);

inline uint16_t rd_u16(const uint8_t *p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline int16_t rd_s16(const uint8_t *p) { return int16_t(rd_u16(p)); }
inline uint32_t rd_u24(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
}

// Look up or allocate a slot for `addr`. Mutex must be held.
Reading *get_slot(uint64_t addr) {
  for (size_t i = 0; i < g_count; ++i) {
    if (g_cache[i].addr == addr) return &g_cache[i];
  }
  if (g_count < CACHE_SIZE) return &g_cache[g_count++];
  // Evict the LRU by last_ms.
  size_t lru = 0;
  for (size_t i = 1; i < g_count; ++i) {
    if (g_cache[i].last_ms < g_cache[lru].last_ms) lru = i;
  }
  return &g_cache[lru];
}

}  // namespace

void ingest(uint64_t addr, const uint8_t *data, size_t len) {
  if (len < 1) return;
  // Info byte layout: bits 5..7 = version (v2 = 0b010), bit 0 = encrypted.
  const uint8_t info = data[0];
  if (((info >> 5) & 0x07) != 0x02) return;  // not BTHome v2 — ignore.
  const bool encrypted = (info & 0x01) != 0;
  const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

  ensure_mutex();
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  Reading *r = get_slot(addr);
  // Last-frame-wins: wipe decoded fields before replaying this frame.
  std::memset(r, 0, sizeof(*r));
  r->addr = addr;
  r->last_ms = now_ms;
  r->encrypted = encrypted;
  if (encrypted) {
    // Decryption requires a per-device key we don't have yet.
    xSemaphoreGive(g_mutex);
    return;
  }

  const uint8_t *p = data + 1;
  const uint8_t *const end = data + len;
  while (p < end) {
    const uint8_t id = *p++;
    if (id >= kObjMax) break;
    const int8_t sz = kObjSize[id];
    if (sz == -1) break;
    size_t plen;
    if (sz == -2) {
      if (p >= end) break;
      plen = *p++;
    } else {
      plen = static_cast<size_t>(sz);
    }
    if (p + plen > end) break;
    switch (id) {
      case 0x00:
        r->packet_id = p[0];
        r->have_packet_id = true;
        break;
      case 0x01:
        r->battery_pct = p[0];
        r->have_battery = true;
        break;
      case 0x02:
        r->temperature_c100 = rd_s16(p);
        r->have_temperature = true;
        break;
      case 0x03:
        r->humidity_pct100 = rd_u16(p);
        r->have_humidity = true;
        break;
      case 0x05:
        r->illuminance_lx100 = rd_u24(p);
        r->have_illuminance = true;
        break;
      case 0x0B:
        r->power_w100 = rd_u24(p);
        r->have_power = true;
        break;
      case 0x0C:
        // Spec: factor 0.001 V. Raw uint16 is already millivolts.
        r->voltage_mv = rd_u16(p);
        r->have_voltage = true;
        break;
      case 0x12:
        r->co2_ppm = rd_u16(p);
        r->have_co2 = true;
        break;
      case 0x21:
        r->motion = p[0];
        r->have_motion = true;
        break;
      case 0x2E:
        // Integer-% humidity. Promote to ×100 so the JSON key is consistent.
        r->humidity_pct100 = static_cast<uint16_t>(p[0]) * 100;
        r->have_humidity = true;
        break;
      case 0x3A:
        r->button_event = p[0];
        r->have_button = true;
        break;
      case 0x45:
        // Spec: factor 0.1 °C. Multiply by 10 to share the ×100 slot.
        r->temperature_c100 = static_cast<int16_t>(rd_s16(p) * 10);
        r->have_temperature = true;
        break;
      default:
        // Recognized for cursor advance, but not stored.
        break;
    }
    p += plen;
  }
  xSemaphoreGive(g_mutex);
}

size_t snapshot(Reading *out, size_t cap) {
  ensure_mutex();
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  size_t n = g_count < cap ? g_count : cap;
  std::memcpy(out, g_cache, n * sizeof(Reading));
  xSemaphoreGive(g_mutex);
  return n;
}

}  // namespace ble_backend::bthome

#endif  // CONFIG_NBP_BTHOME
