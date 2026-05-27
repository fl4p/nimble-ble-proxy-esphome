// Continuous BLE scanner. Raw advertisement callbacks are batched and
// emitted as a single BluetoothLERawAdvertisementsResponse (msg 93)
// every proxy::ADV_FLUSH_INTERVAL_MS, or when the batch fills.
//
// Forwarding is enabled/disabled by the api_server when HA sends
// (Un)SubscribeBluetoothLEAdvertisementsRequest. Init spawns the flush
// task; start() begins NimBLE scanning.

#pragma once

#include <cstddef>
#include <cstdint>

#include "proxy_config.h"

namespace ble_backend::scanner {

// One-time NimBLE scan object setup and flush-task spawn. Call before start().
void init();

// Begin scanning (continuous, passive by default). Idempotent.
void start();

// Ensure the scan is running. NimBLE auto-suspends the scan whenever
// it starts a GAP connect procedure; this restarts it once the
// procedure ends so the scanner doesn't stay dead between connects.
void resume();

// Stop scanning entirely (for diagnostic traces). Adv callbacks stop
// firing and `onConnect`/`onDisconnect` will not auto-resume. Pair
// with resume() to come back online.
void pause();

// Toggle whether each adv callback is queued for forwarding to HA.
// When off, ads are dropped on the floor.
void start_forwarding();
void stop_forwarding();

// Set the BLE scan duty cycle. Both args are in milliseconds; caller
// must ensure window_ms <= interval_ms. Applies live (stops + restarts
// the scan so the new params take effect immediately). NimBLE's
// underlying setInterval/setWindow take 1ms-resolution uint16s; the
// BLE spec allows 2.5..10240 ms but practical lower bound is ~20 ms.
void set_duty(uint16_t window_ms, uint16_t interval_ms);
void get_duty(uint16_t *window_ms, uint16_t *interval_ms);

// Active scan: when on, the radio sends SCAN_REQ for each advert and
// captures the SCAN_RSP — so peripherals advertising their full name /
// extra service data in the scan response become visible. Costs a bit
// of radio airtime per advert and slightly more power. Default on.
void set_active(bool on);
bool get_active();

// Cumulative count of advertisements seen by the radio since boot.
// Bumped from the NimBLE host task in every onResult, before any
// forwarding/subscription gating.
uint32_t adv_count();

#if CONFIG_NBP_DEVICES_PANEL
// Per-device entry in the live device table. Populated regardless of
// forwarding state — used by the web UI to show what's nearby.
struct DeviceRow {
  uint64_t addr;       // MSB-first MAC packed into 48 low bits
  uint8_t addr_type;   // BLE_ADDR_PUBLIC / RANDOM / ...
  int8_t rssi;         // dBm from the most recent advert
  char name[24];       // last non-empty Complete/Shortened Local Name, NUL-term
  uint32_t adv_count;  // total adverts seen from this MAC
  uint32_t last_ms;    // FreeRTOS millis at last sighting
#if CONFIG_NBP_DEV_DETAILS
  uint16_t appearance; // AD 0x19 (GAP Appearance); 0 when absent
  bool connectable;    // PDU type is connectable (ADV_IND / ADV_DIRECT_IND)
  const char *tag;     // vendor / class label (static string), nullptr if none
#endif
};

// Snapshot up to `cap` rows of the device table into `out`. Internal
// table holds up to ~64 unique MACs with LRU-by-last_ms eviction.
// Returns the number of rows written.
size_t snapshot_devices(DeviceRow *out, size_t cap);
#endif  // CONFIG_NBP_DEVICES_PANEL

}  // namespace ble_backend::scanner
