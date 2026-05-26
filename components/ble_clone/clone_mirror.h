// Local GATT image of the cloned peripheral. One-shot build: NimBLE's
// ble_gatts_start() locks the attribute table for the life of the host,
// so re-targeting needs a reboot.
//
// Each mirrored characteristic carries an upstream value handle and is
// wired to a NimBLECharacteristicCallbacks that proxies onRead/onWrite/
// onSubscribe to upstream::*. Read responses come from a small in-RAM
// cache that gets primed at build time and refreshed on a notify or
// post-write. The cache is per-char, sized by NBP_CLONE_VALUE_CACHE_BYTES.

#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_NBP_CLONE

#include <cstddef>
#include <cstdint>

namespace ble_clone::mirror {

// Walk the upstream client's discovered attribute table and create
// matching local NimBLEService / NimBLECharacteristic / NimBLEDescriptor
// objects. Does NOT yet call ble_gatts_start — that has to happen with
// the upstream link DOWN (otherwise add_svcs returns EBUSY and the GAP
// init re-registration asserts).
//
// Idempotent: subsequent calls route to rebind_upstream().
// Returns false on attribute-cap overflow or other failure.
bool build_from(void *upstream_client);

// Register the local GATT DB with the host (NimBLEServer::start →
// ble_gatts_start). MUST be called with no active BLE connections.
// One-shot: after this, the attribute table is frozen for this boot.
bool finalize_server();

// On reconnect, re-attach each MirrorChar to its upstream
// NimBLERemoteCharacteristic* (handles can shift across rediscovery).
// Returns true if at least one char was matched. Skipped if build_from
// hasn't run yet.
bool rebind_upstream(void *upstream_client);

// Null out upstream pointers when the upstream link drops so subsequent
// writes from local centrals don't dereference a stale RemoteChar.
void disconnect_upstream();

// Push an initial-read result from the supervisor into the cache and
// the local NimBLE characteristic value (so local reads return real
// bytes immediately, before any notify arrives).
void prime_cache(uint16_t upstream_value_handle, const uint8_t *data,
                 size_t len);

// Iterate over MirrorChars whose upstream chr is currently bound and
// has NOTIFY (or only INDICATE — passed as `indicate=true`). Supervisor
// uses this to subscribe everything at session start.
void for_each_subscribable(
    void (*cb)(void *ctx, void *upstream_chr, uint16_t handle, bool indicate),
    void *ctx);

// Iterate over MirrorChars whose upstream chr is currently bound and
// has READ. Supervisor uses this to prime the cache at session start
// so local centrals don't see empty reads.
void for_each_readable(
    void (*cb)(void *ctx, void *upstream_chr, uint16_t handle), void *ctx);

// Begin local advertising. base_name is the upstream's name (or
// "unknown") with config::Target::name_suffix appended. Idempotent.
// Called once after build_from() succeeds; stopped+restarted by the
// supervisor on disable/re-enable.
void start_advertising(const char *base_name);
void stop_advertising();

// Push an upstream notification into the corresponding local char.
// NimBLE handles fan-out to every subscribed local central.
// Invoked from the upstream notify callback (NimBLE host task).
void on_upstream_notify(uint16_t upstream_value_handle,
                        const uint8_t *data, size_t len);

struct Stats {
  uint16_t services;
  uint16_t characteristics;
  uint16_t descriptors;
  uint32_t reads_served_from_cache;
  uint32_t reads_proxied;
  uint32_t writes_proxied;
  uint32_t notifies_out;          // local notify() calls to centrals
  uint8_t connected_centrals;
  bool advertising;
};
Stats stats();

}  // namespace ble_clone::mirror

#endif  // CONFIG_NBP_CLONE
