#include "improv_proto.h"

#include <cstring>

namespace improv {

uint8_t checksum(const uint8_t *p, size_t n) {
  uint8_t s = 0;
  for (size_t i = 0; i < n; ++i) s = static_cast<uint8_t>(s + p[i]);
  return s;
}

namespace {

// True if a followed by b is one whole packet with a valid checksum.
bool whole(const uint8_t *a, size_t na, const uint8_t *b, size_t nb) {
  const size_t n = na + nb;
  if (n < 3) return false;
  auto at = [&](size_t i) { return i < na ? a[i] : b[i - na]; };
  if (static_cast<size_t>(at(1)) + 3 != n) return false;
  uint8_t s = 0;
  for (size_t i = 0; i + 1 < n; ++i) s = static_cast<uint8_t>(s + at(i));
  return s == at(n - 1);
}

}  // namespace

int Assembler::feed(const uint8_t *chunk, size_t n, uint32_t now_ms,
                    uint8_t *out) {
  if (len_ && now_ms - last_ms_ > ASSEMBLY_TIMEOUT_MS) len_ = 0;
  if (len_ && whole(chunk, n, nullptr, 0) && !whole(buf_, len_, chunk, n)) {
    len_ = 0;
  }
  last_ms_ = now_ms;
  if (len_ + n > MAX_PACKET) {
    len_ = 0;
    return -1;
  }
  std::memcpy(buf_ + len_, chunk, n);
  len_ += n;
  if (len_ < 2) return 0;
  const size_t need = static_cast<size_t>(buf_[1]) + 3;
  if (len_ < need) return 0;
  const bool ok = len_ == need && checksum(buf_, need - 1) == buf_[need - 1];
  len_ = 0;
  if (!ok) return -1;
  std::memcpy(out, buf_, need);
  return static_cast<int>(need);
}

uint8_t parse_wifi_settings(const uint8_t *data, size_t n, const uint8_t **ssid,
                            size_t *ssid_len, const uint8_t **psk,
                            size_t *psk_len) {
  if (n < 2) return ERR_INVALID_RPC;
  const size_t sl = data[0];
  if (sl == 0 || 1 + sl >= n) return ERR_INVALID_RPC;
  const size_t pl = data[1 + sl];
  if (2 + sl + pl != n) return ERR_INVALID_RPC;
  *ssid = data + 1;
  *ssid_len = sl;
  *psk = data + 2 + sl;
  *psk_len = pl;
  return ERR_NONE;
}

Result::Result(uint8_t cmd, uint8_t *buf, size_t cap)
    : buf_(buf), cap_(cap < MAX_PACKET ? cap : MAX_PACKET), len_(2) {
  buf_[0] = cmd;
  buf_[1] = 0;
}

bool Result::add(const char *s, size_t n) {
  // Room for the length byte, the string and the checksum; data <= 255.
  if (n > 255 || len_ + 1 + n + 1 > cap_ || len_ - 2 + 1 + n > 255) {
    return false;
  }
  buf_[len_++] = static_cast<uint8_t>(n);
  std::memcpy(buf_ + len_, s, n);
  len_ += n;
  return true;
}

bool Result::add(const char *s) { return add(s, std::strlen(s)); }

size_t Result::finish() {
  buf_[1] = static_cast<uint8_t>(len_ - 2);
  buf_[len_] = checksum(buf_, len_);
  return len_ + 1;
}

void service_data(uint8_t state, uint8_t caps, uint8_t out[6]) {
  out[0] = state;
  out[1] = caps;
  out[2] = out[3] = out[4] = out[5] = 0;
}

}  // namespace improv
