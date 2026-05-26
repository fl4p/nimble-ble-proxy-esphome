// Connection-level message handlers: Hello, Connect, Ping, DeviceInfo,
// ListEntities, Disconnect, SubscribeLogs. Pure protocol — no BLE.

#pragma once

#include <cstddef>
#include <cstdint>

namespace api_server::handshake {

struct Result {
  bool handled;            // true if the message was a handshake message
  uint16_t response_type;  // 0 = no response to send
  size_t payload_len;      // bytes written to response_buf
  bool close_after;        // true after DisconnectResponse
};

// If `request_type` is one of the handshake message IDs, decode the
// request, build the response into `response_buf`, and return handled=true.
// For unknown / non-handshake types, returns handled=false untouched and
// the caller routes to the Bluetooth handlers instead.
Result handle(uint16_t request_type, const uint8_t *request_payload,
              size_t request_len, uint8_t *response_buf, size_t response_cap);

}  // namespace api_server::handshake
