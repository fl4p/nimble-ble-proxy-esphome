#include "frame_codec.h"

namespace api_server::frame_codec {

size_t encode_varint(uint8_t *out, uint32_t value) {
  size_t i = 0;
  while (value >= 0x80) {
    out[i++] = static_cast<uint8_t>(value | 0x80);
    value >>= 7;
  }
  out[i++] = static_cast<uint8_t>(value);
  return i;
}

VarintParse decode_varint(const uint8_t *in, size_t len, size_t max_bytes) {
  uint32_t v = 0;
  size_t shift = 0;
  size_t cap = (len < max_bytes) ? len : max_bytes;
  for (size_t i = 0; i < cap; ++i) {
    uint8_t b = in[i];
    v |= static_cast<uint32_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) {
      return {v, i + 1};
    }
    shift += 7;
    if (shift >= 32) {
      return {0, 0};
    }
  }
  return {0, 0};
}

namespace {

// Helper: read exactly `n` bytes using read_fn, retrying short reads.
// Returns Error::Ok or one of {Eof, SocketError}.
Error read_exact(ReadFn read_fn, void *ctx, uint8_t *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = read_fn(ctx, buf + got, n - got);
    if (r == 0) return Error::Eof;
    if (r < 0) return Error::SocketError;
    got += static_cast<size_t>(r);
  }
  return Error::Ok;
}

// Read a single varint byte-at-a-time from the socket. Cheap because
// lwip socket buffering absorbs per-syscall cost, and we don't risk
// reading past the frame into the next one.
Error read_varint_streaming(ReadFn read_fn, void *ctx, uint32_t *out,
                            size_t max_bytes) {
  uint32_t v = 0;
  size_t shift = 0;
  for (size_t i = 0; i < max_bytes; ++i) {
    uint8_t b;
    Error e = read_exact(read_fn, ctx, &b, 1);
    if (e != Error::Ok) return e;
    v |= static_cast<uint32_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) {
      *out = v;
      return Error::Ok;
    }
    shift += 7;
    if (shift >= 32) return Error::VarintTooLong;
  }
  return Error::VarintTooLong;
}

}  // namespace

Error read_frame(ReadFn read_fn, void *ctx,
                 uint8_t *payload_buf, size_t payload_cap,
                 uint16_t *type_out, size_t *payload_len_out) {
  uint8_t indicator;
  if (Error e = read_exact(read_fn, ctx, &indicator, 1); e != Error::Ok) {
    return e;
  }
  if (indicator != 0x00) {
    return Error::BadIndicator;
  }

  uint32_t size = 0, type = 0;
  if (Error e = read_varint_streaming(read_fn, ctx, &size, 3); e != Error::Ok) {
    return e;
  }
  if (Error e = read_varint_streaming(read_fn, ctx, &type, 2); e != Error::Ok) {
    return e;
  }
  if (type > 0xffff) {
    return Error::VarintTooLong;
  }
  if (size > payload_cap) {
    return Error::MessageTooLarge;
  }

  if (Error e = read_exact(read_fn, ctx, payload_buf, size); e != Error::Ok) {
    return e;
  }

  *type_out = static_cast<uint16_t>(type);
  *payload_len_out = static_cast<size_t>(size);
  return Error::Ok;
}

size_t prepend_header(uint8_t *buf, uint16_t msg_type, size_t payload_len) {
  size_t size_vl = varint_len(static_cast<uint32_t>(payload_len));
  size_t type_vl = varint_len(msg_type);
  size_t header_len = 1 + size_vl + type_vl;
  size_t start = MAX_HEADER_LEN - header_len;

  buf[start] = 0x00;
  encode_varint(&buf[start + 1], static_cast<uint32_t>(payload_len));
  encode_varint(&buf[start + 1 + size_vl], msg_type);
  return start;
}

}  // namespace api_server::frame_codec
