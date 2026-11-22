// Improv Wi-Fi over BLE, protocol layer (https://www.improv-wifi.com/ble/).
// No IDF or NimBLE dependency, so it compiles on the host for tests.

#pragma once

#include <cstddef>
#include <cstdint>

namespace improv {

constexpr uint8_t STATE_AUTHORIZED = 0x02;
constexpr uint8_t STATE_PROVISIONING = 0x03;
constexpr uint8_t STATE_PROVISIONED = 0x04;

constexpr uint8_t ERR_NONE = 0x00;
constexpr uint8_t ERR_INVALID_RPC = 0x01;
constexpr uint8_t ERR_UNKNOWN_RPC = 0x02;
constexpr uint8_t ERR_UNABLE_TO_CONNECT = 0x03;
constexpr uint8_t ERR_UNKNOWN = 0xFF;

constexpr uint8_t CMD_WIFI_SETTINGS = 0x01;
constexpr uint8_t CMD_DEVICE_INFO = 0x03;
constexpr uint8_t CMD_SCAN = 0x04;

constexpr uint8_t CAP_DEVICE_INFO = 0x02;
constexpr uint8_t CAP_SCAN = 0x04;

constexpr size_t MAX_PACKET = 2 + 255 + 1;
// A partial RPC older than this is dropped, so an abandoned client cannot
// turn the next client's first write into a continuation.
constexpr uint32_t ASSEMBLY_TIMEOUT_MS = 5000;

uint8_t checksum(const uint8_t *p, size_t n);

// Reassembles an RPC packet written in several pieces (writes at offset 0;
// NimBLE joins prepared writes itself). A write that completes the held
// packet with a valid checksum is a continuation; otherwise a write that is
// a whole valid packet on its own replaces the partial one.
class Assembler {
 public:
  // Returns the packet length once complete (copied to `out`, MAX_PACKET
  // bytes), 0 while more bytes are needed, -1 for a malformed packet.
  int feed(const uint8_t *chunk, size_t n, uint32_t now_ms, uint8_t *out);

 private:
  uint8_t buf_[MAX_PACKET];
  size_t len_ = 0;
  uint32_t last_ms_ = 0;
};

// Splits a Send-Wi-Fi-settings payload. Pointers point into `data`.
// Returns ERR_NONE or ERR_INVALID_RPC.
uint8_t parse_wifi_settings(const uint8_t *data, size_t n, const uint8_t **ssid,
                            size_t *ssid_len, const uint8_t **psk,
                            size_t *psk_len);

// Builds an RPC result packet in a caller buffer.
class Result {
 public:
  Result(uint8_t cmd, uint8_t *buf, size_t cap);
  // Appends one string; false (and nothing appended) if it does not fit.
  bool add(const char *s, size_t n);
  bool add(const char *s);
  // Writes the checksum, returns the packet length.
  size_t finish();
  size_t mark() const { return len_; }
  void rewind(size_t mark) { len_ = mark; }

 private:
  uint8_t *buf_;
  size_t cap_;
  size_t len_;
};

// The 6-byte service data advertised under UUID 0x4677.
void service_data(uint8_t state, uint8_t caps, uint8_t out[6]);

}  // namespace improv
