// Small DI seam to let ble_backend publish messages back to whatever
// transport the app uses, without ble_backend taking a build-time
// dependency on api_server. main wires this up at startup with
// api_server::send_async / api_server::has_active_client.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ble_backend::publish {

using EncodeFn = size_t (*)(void *ctx, uint8_t *buf, size_t cap);
using SendFn = bool (*)(uint16_t msg_type, EncodeFn encode, void *ctx);
using SendToFn = bool (*)(uint32_t client_id, uint16_t msg_type,
                          EncodeFn encode, void *ctx);
using HasClientFn = bool (*)();

// Install the transport hooks. Safe to call once at boot.
void install(SendFn send, SendToFn send_to, HasClientFn has_client);

// Convenience proxies. Both return false if no transport is installed
// or no client is connected.
bool send_async(uint16_t msg_type, EncodeFn encode, void *ctx);
// Targeted: only to API client `client_id` (a connection::OwnerId).
// Use for anything that answers one client's request or carries data
// from a link it owns; broadcasting that would leak it to other clients
// and could resolve their pending request for the same address.
bool send_to(uint32_t client_id, uint16_t msg_type, EncodeFn encode,
             void *ctx);
bool has_client();

}  // namespace ble_backend::publish
