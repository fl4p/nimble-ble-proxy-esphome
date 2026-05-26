// Plaintext aioesphomeapi frame codec.
//
// Wire format (matches ESPHome api_frame_helper_plaintext.cpp:72-180):
//   [0x00] [size_varint up to 3 B] [type_varint up to 2 B] [payload]
//
// `size_varint` carries the payload length (not including header), as a
// base-128 little-endian varint. Same for `type_varint` which carries the
// message-type ID (see proxyapi::MessageId in api_proto.h).
//
// Note: these IDs live in the framing — they are NOT proto field numbers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>  // ssize_t

namespace api_server::frame_codec {

enum class Error : int {
  Ok = 0,
  Eof = -1,            // peer closed connection cleanly
  SocketError = -2,    // read returned negative
  BadIndicator = -3,   // first byte was not 0x00
  VarintTooLong = -4,  // varint exceeded its byte budget
  MessageTooLarge = -5,
};

// Header is at most 6 bytes: 1 indicator + 3 size + 2 type.
inline constexpr size_t MAX_HEADER_LEN = 6;

// Caller-supplied blocking-ish read. Returns # bytes read into `buf`,
// 0 on clean EOF, negative on error. Implementation may short-read.
using ReadFn = ssize_t (*)(void *ctx, uint8_t *buf, size_t n);

// Read exactly one full frame using `read_fn`. On Ok, *type_out holds
// the message-type ID and *payload_len_out the number of payload bytes
// written into `payload_buf`. payload_cap must be large enough for the
// incoming payload; if not, MessageTooLarge is returned (and the
// connection should be dropped).
Error read_frame(ReadFn read_fn, void *ctx,
                 uint8_t *payload_buf, size_t payload_cap,
                 uint16_t *type_out, size_t *payload_len_out);

// Prepend a header for a payload already sitting at `buf + MAX_HEADER_LEN`.
// Returns the offset within `buf` where the frame starts; total frame
// length is (MAX_HEADER_LEN - offset) + payload_len. Caller sends from
// `buf + offset` for that many bytes — a single contiguous write.
size_t prepend_header(uint8_t *buf, uint16_t msg_type, size_t payload_len);

// Varint helpers — exposed for host-side unit tests.
size_t encode_varint(uint8_t *out, uint32_t value);
constexpr size_t varint_len(uint32_t v) {
  size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++n;
  }
  return n;
}

struct VarintParse {
  uint32_t value;
  size_t consumed;  // 0 = not enough bytes or overflow
};
VarintParse decode_varint(const uint8_t *in, size_t len, size_t max_bytes);

}  // namespace api_server::frame_codec
