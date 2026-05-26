// Dispatch for all Bluetooth* messages from HA. Each handler decodes
// the request, calls into ble_backend, and emits one or more responses
// via Context::send_response (sync) or api_server::send_async (async).

#pragma once

#include <cstddef>
#include <cstdint>

namespace api_server::bt_handlers {

struct Context {
  int client_fd;
  uint8_t *response_buf;
  size_t response_cap;
  bool (*send_response)(int fd, uint16_t msg_type, size_t payload_len);
};

// Returns true if the message was recognized.
bool handle(uint16_t request_type, const uint8_t *request_payload,
            size_t request_len, const Context &ctx);

// Called when the LAST API client disconnects (active_count → 0). Tears
// down BLE state that only makes sense while someone's listening.
void on_last_client_disconnect();

}  // namespace api_server::bt_handlers
