// Upstream connection — owns the single NimBLEClient that talks to the
// cloned peripheral and the supervisor task that drives its lifecycle
// (scan→connect→discover→build mirror→subscribe→advertise→supervise).
//
// The local GATT-write path enqueues each write via enqueue_write(); the
// supervisor task drains the queue and forwards to upstream. This avoids
// blocking the NimBLE host task on a multi-step long-write — NimBLE-CPP
// writeValue() blocks on its own task-wait semaphore which would
// deadlock if called from the host task.

#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_NBP_CLONE

#include <cstddef>
#include <cstdint>

namespace ble_clone::upstream {

enum class State : uint8_t {
  Disabled = 0,    // CONFIG_NBP_CLONE on but config.enabled=false
  Idle,            // before supervisor starts
  Scanning,        // probing connect to target
  Connecting,      // GAP connect in flight
  Discovering,     // walking GATT
  Ready,           // mirror built / rebound, notifies forwarding
  Reconnecting,    // upstream dropped, retrying
};

struct Status {
  State state;
  uint64_t address;
  uint16_t mtu;
  uint32_t reconnects;
  uint32_t last_disconnect_reason;
  uint32_t notifies_seen;
  uint32_t writes_drained;
  uint32_t writes_dropped;
};

// One-time setup. Idempotent; safe to call before NimBLE is fully up
// (just creates the queue + mutex).
void init();

// Spawn the supervisor task. Must follow ble_backend::start() so
// NimBLEDevice::init() has run.
void start();

// Lock-free snapshot.
Status status();

// Queue an upstream write. data is copied; safe to free after return.
// Returns false if the queue is full (write is dropped, caller logs).
// Called from the NimBLE host task via the mirror's onWrite callback.
//
// `upstream_chr` is a NimBLERemoteCharacteristic*; type-erased to keep
// the header free of NimBLE includes (matches the connection::client_for
// pattern in ble_backend).
bool enqueue_write(void *upstream_chr, const uint8_t *data, size_t len,
                   bool with_response);

}  // namespace ble_clone::upstream

#endif  // CONFIG_NBP_CLONE
