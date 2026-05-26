// Plaintext aioesphomeapi server. Single concurrent client. Spins a
// FreeRTOS task that owns the listening socket and per-connection
// handlers; main just calls start() once after wifi is up.

#pragma once

#include <cstddef>
#include <cstdint>

namespace api_server {

// Spawn the listener task. Idempotent — second call is a no-op.
void start();

// Async send entry point for ble_backend (called from NimBLE host task,
// timer tasks, etc.). Acquires the TX mutex internally, asks `encode`
// to write the payload starting at the caller-supplied buffer, then
// frames and ships it over the active client socket.
//
// `encode` returns the number of bytes written (0 → drop the message).
// Returns false if there is no active client OR send failed — caller
// should not retry from a callback hot path.
using EncodeFn = size_t (*)(void *ctx, uint8_t *buf, size_t cap);
bool send_async(uint16_t msg_type, EncodeFn encode, void *ctx);

// True while a client is connected. Cheap, lock-free check for
// callbacks to bail out early before doing real work.
bool has_active_client();

}  // namespace api_server
