// Continuous BLE scanner. Raw advertisement callbacks are batched and
// emitted as a single BluetoothLERawAdvertisementsResponse (msg 93)
// every proxy::ADV_FLUSH_INTERVAL_MS, or when the batch fills.
//
// Forwarding is enabled/disabled by the api_server when HA sends
// (Un)SubscribeBluetoothLEAdvertisementsRequest. Init spawns the flush
// task; start() begins NimBLE scanning.

#pragma once

#include <cstdint>

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

// Cumulative count of advertisements seen by the radio since boot.
// Bumped from the NimBLE host task in every onResult, before any
// forwarding/subscription gating.
uint32_t adv_count();

}  // namespace ble_backend::scanner
