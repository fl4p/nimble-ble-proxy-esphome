// NVS-backed target configuration for the clone feature. Namespaced
// "nbp_clone" — keys: addr (u64), type (u8), name_suffix (str),
// enabled (u8).
//
// load() is called once at boot before init(). set() persists + atomically
// swaps the in-memory snapshot; the supervisor task picks it up on the
// next reconnect cycle. NimBLE's GATT DB is one-shot, so a target change
// only takes full effect after reboot — but enabled=false stops adv +
// drops the upstream link without reboot.

#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_NBP_CLONE

#include <cstdint>

namespace ble_clone::config {

struct Target {
  uint64_t address;         // MSB-first packed MAC (same layout as scanner uses)
  uint8_t address_type;     // BLE_ADDR_PUBLIC / RANDOM / ...
  bool enabled;
  char name_suffix[16];     // appended to upstream name for local adv, e.g. "_cloned"
};

// One-time load from NVS into the in-memory snapshot. Defaults to
// {address=0, enabled=false, name_suffix="_cloned"} on first boot.
void load();

// Lock-free snapshot copy.
Target snapshot();

// Persist + swap. Returns false on NVS error. The supervisor reads the
// new snapshot on its next loop iteration.
bool set(const Target &t);

}  // namespace ble_clone::config

#endif  // CONFIG_NBP_CLONE
