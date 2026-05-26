// Continuous BLE scanner. Raw advertisement callbacks are batched and
// emitted as a single BluetoothLERawAdvertisementsResponse (msg 93)
// every proxy::ADV_FLUSH_INTERVAL_MS, or when the batch fills.
//
// Forwarding is enabled/disabled by the api_server when HA sends
// (Un)SubscribeBluetoothLEAdvertisementsRequest. Init spawns the flush
// task; start() begins NimBLE scanning.

#pragma once

namespace ble_backend::scanner {

// One-time NimBLE scan object setup and flush-task spawn. Call before start().
void init();

// Begin scanning (continuous, passive by default). Idempotent.
void start();

// Toggle whether each adv callback is queued for forwarding to HA.
// When off, ads are dropped on the floor.
void start_forwarding();
void stop_forwarding();

}  // namespace ble_backend::scanner
