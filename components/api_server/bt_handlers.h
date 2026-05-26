// Dispatch for all Bluetooth* messages from HA. Each handler decodes
// the request, calls into ble_backend, and emits responses via
// api_server::send_async.

#pragma once

#include <cstddef>
#include <cstdint>

namespace api_server::bt_handlers {

// Per-client subscription state. The connection task stack-allocates one
// of these and passes a pointer in Context, so handlers can flip flags
// idempotently and on_client_disconnect knows what to release. Avoids
// the global "reset on last disconnect" hack — which would leak forever
// if any client task got stuck mid-handler.
struct ClientSubs {
  bool sub_adv = false;
  bool sub_free = false;
};

struct Context {
  int client_fd;
  ClientSubs *subs;  // owned by the connection task; never nullptr
};

// Returns true if the message was recognized.
bool handle(uint16_t request_type, const uint8_t *request_payload,
            size_t request_len, const Context &ctx);

// Per-client teardown — release any subscription refs this client held.
void on_client_disconnect(ClientSubs &subs);

// Last-client teardown — disconnect any active GATT links so we don't
// keep them open with no one listening.
void on_last_client_disconnect();

}  // namespace api_server::bt_handlers
