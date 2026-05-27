// BTHome v2 (https://bthome.io) service-data decoder. Receives the raw
// 0xFCD2 service-data payload from the scan callback, walks the TLV
// stream, and caches the most-common decoded fields per MAC for the
// dashboard's /devices endpoint to join against.
//
// Whole-file gated on CONFIG_NBP_BTHOME — when off this TU compiles
// empty and the public API simply doesn't exist (callers also
// gate their use site).

#pragma once

#include "sdkconfig.h"

#if CONFIG_NBP_BTHOME

#include <cstddef>
#include <cstdint>

namespace ble_backend::bthome {

// Per-MAC cache entry. Each `have_*` flag is true iff the most recent
// frame from this MAC carried the corresponding object id. Last-frame-
// wins semantics — older fields are not retained across frames (BTHome
// emitters typically resend their full set every advert).
struct Reading {
  uint64_t addr;       // MSB-first MAC in low 48 bits (matches DeviceRow.addr)
  uint32_t last_ms;    // FreeRTOS millis at most recent successful decode
  bool     encrypted;  // info-byte bit 0 set; other have_* will be false
  bool     have_packet_id;     uint8_t  packet_id;          // 0x00
  bool     have_battery;       uint8_t  battery_pct;        // 0x01 (%)
  bool     have_temperature;   int16_t  temperature_c100;   // 0x02 / 0x45 (°C × 100)
  bool     have_humidity;      uint16_t humidity_pct100;    // 0x03 / 0x2E (% × 100)
  bool     have_voltage;       uint16_t voltage_mv;         // 0x0C (V × 1000 = mV)
  bool     have_illuminance;   uint32_t illuminance_lx100;  // 0x05 (lux × 100)
  bool     have_motion;        uint8_t  motion;             // 0x21
  bool     have_button;        uint8_t  button_event;       // 0x3A
  bool     have_power;         uint32_t power_w100;         // 0x0B (W × 100)
  bool     have_co2;           uint16_t co2_ppm;            // 0x12 (ppm)
};

// Decode a BTHome v2 service-data payload. `data` points at the info
// byte (i.e. what NimBLE's getServiceData(UUID 0xFCD2) hands you, with
// the 16-bit UUID already stripped). Cheap; runs inline from the NimBLE
// host task in the scan callback.
void ingest(uint64_t addr, const uint8_t *data, size_t len);

// Mutex-guarded copy of up to `cap` cache entries into `out`. Cache
// holds 32 unique MACs with LRU-by-last_ms eviction. Returns count.
size_t snapshot(Reading *out, size_t cap);

}  // namespace ble_backend::bthome

#endif  // CONFIG_NBP_BTHOME
