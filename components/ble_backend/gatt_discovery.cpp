#include "gatt_discovery.h"

#include "api_proto.h"
#include "publish.h"
#include "connection.h"

#include "NimBLEDevice.h"
#include "NimBLERemoteCharacteristic.h"
#include "NimBLERemoteDescriptor.h"
#include "NimBLERemoteService.h"
#include "NimBLEUUID.h"
#include "esp_log.h"
#include "pb_encode.h"

#include <cstring>
#include <memory>

namespace ble_backend::gatt_discovery {

namespace {

constexpr const char *TAG = "ble.gatt.disc";

// Pack a NimBLEUUID into the proto's (short_uuid OR uuid[hi,lo]) layout.
// aioesphomeapi's uuid_from_pair(hi, lo) expects [0]=high 64 bits,
// [1]=low 64 bits — see aioesphomeapi/_frame_helper/plain_text.py.
void pack_uuid(const NimBLEUUID &uuid, uint32_t *short_out,
               uint64_t uuid_arr[2], pb_size_t *uuid_count) {
  *short_out = 0;
  *uuid_count = 0;
  uuid_arr[0] = 0;
  uuid_arr[1] = 0;

  uint8_t bits = uuid.bitSize();
  if (bits == 16 || bits == 32) {
    // The NimBLEUUID internal value is stored as a 128-bit array even
    // for 16/32-bit UUIDs (with the spec base UUID filled in). Pull the
    // bits out by going to 128 and reading the lowest 4 bytes.
    NimBLEUUID u128 = uuid;
    u128.to128();
    const uint8_t *v = u128.getValue();  // LE bytes
    uint32_t s = 0;
    s |= static_cast<uint32_t>(v[12]) << 0;
    s |= static_cast<uint32_t>(v[13]) << 8;
    s |= static_cast<uint32_t>(v[14]) << 16;
    s |= static_cast<uint32_t>(v[15]) << 24;
    *short_out = s;
    return;
  }

  // 128-bit: pack into (hi, lo). NimBLEUUID::getValue() returns 16 LE
  // bytes (byte[0] = LSB of the 128-bit value).
  NimBLEUUID u128 = uuid;
  u128.to128();
  const uint8_t *v = u128.getValue();
  uint64_t lo = 0, hi = 0;
  for (int i = 7; i >= 0; --i) {
    lo = (lo << 8) | v[i];
    hi = (hi << 8) | v[i + 8];
  }
  uuid_arr[0] = hi;
  uuid_arr[1] = lo;
  *uuid_count = 2;
}

// Context owned for the duration of one discovery — passed to send_async
// for the services + done encodes.
//
// HEAP-allocated: proxyapi_BluetoothGATTGetServicesResponse is ~25 KiB
// (worst-case static nanopb arrays for 8 services × 12 chars × 6 descs).
// api_client tasks have only 8 KiB stack, so this MUST NOT live on the
// stack of run(). send_async invokes the encode callback synchronously
// before returning, so unique_ptr scoped to run() is sufficient.
struct ServicesEncodeCtx {
  proxyapi_BluetoothGATTGetServicesResponse msg;
};

size_t encode_services(void *vctx, uint8_t *buf, size_t cap) {
  auto *ctx = static_cast<ServicesEncodeCtx *>(vctx);
  pb_ostream_t stream = pb_ostream_from_buffer(buf, cap);
  if (!pb_encode(&stream, proxyapi_BluetoothGATTGetServicesResponse_fields,
                 &ctx->msg)) {
    ESP_LOGE(TAG, "encode services failed: %s", PB_GET_ERROR(&stream));
    return 0;
  }
  return stream.bytes_written;
}

struct DoneEncodeCtx {
  proxyapi_BluetoothGATTGetServicesDoneResponse msg;
};

size_t encode_done(void *vctx, uint8_t *buf, size_t cap) {
  auto *ctx = static_cast<DoneEncodeCtx *>(vctx);
  pb_ostream_t stream = pb_ostream_from_buffer(buf, cap);
  if (!pb_encode(&stream, proxyapi_BluetoothGATTGetServicesDoneResponse_fields,
                 &ctx->msg)) {
    ESP_LOGE(TAG, "encode done failed: %s", PB_GET_ERROR(&stream));
    return 0;
  }
  return stream.bytes_written;
}

struct ErrorEncodeCtx {
  proxyapi_BluetoothGATTErrorResponse msg;
};

size_t encode_error(void *vctx, uint8_t *buf, size_t cap) {
  auto *ctx = static_cast<ErrorEncodeCtx *>(vctx);
  pb_ostream_t stream = pb_ostream_from_buffer(buf, cap);
  if (!pb_encode(&stream, proxyapi_BluetoothGATTErrorResponse_fields,
                 &ctx->msg)) {
    return 0;
  }
  return stream.bytes_written;
}

// GATT char properties bitmask (per BLE spec); NimBLE 2.x doesn't
// expose getProperties() so we reconstruct from the can*() helpers.
uint8_t pack_properties(const NimBLERemoteCharacteristic *chr) {
  uint8_t p = 0;
  if (chr->canBroadcast()) p |= 0x01;
  if (chr->canRead()) p |= 0x02;
  if (chr->canWriteNoResponse()) p |= 0x04;
  if (chr->canWrite()) p |= 0x08;
  if (chr->canNotify()) p |= 0x10;
  if (chr->canIndicate()) p |= 0x20;
  if (chr->canWriteSigned()) p |= 0x40;
  return p;
}

void emit_error(uint64_t address, int32_t err) {
  ErrorEncodeCtx ctx;
  ctx.msg = proxyapi_BluetoothGATTErrorResponse_init_zero;
  ctx.msg.address = address;
  ctx.msg.handle = 0;
  ctx.msg.error = err;
  publish::send_async(proxyapi::MSG_BLUETOOTH_GATT_ERROR_RESPONSE,
                      &encode_error, &ctx);
}

}  // namespace

Status run(uint64_t address) {
  auto *client = static_cast<NimBLEClient *>(connection::client_for(address));
  if (client == nullptr) {
    ESP_LOGW(TAG, "%012llx not connected",
             static_cast<unsigned long long>(address));
    emit_error(address, /*err=*/-1);
    return Status::NotConnected;
  }

  ESP_LOGI(TAG, "discover %012llx",
           static_cast<unsigned long long>(address));

  // Force a fresh walk so cached state from a prior session is rebuilt.
  // discoverAttributes() walks services → characteristics → descriptors.
  if (!client->discoverAttributes()) {
    ESP_LOGW(TAG, "discoverAttributes failed for %012llx",
             static_cast<unsigned long long>(address));
    emit_error(address, /*err=*/-2);
    return Status::EncodeFailed;
  }

  auto ctx = std::make_unique<ServicesEncodeCtx>();
  if (ctx == nullptr) {
    ESP_LOGW(TAG, "OOM allocating services encode ctx");
    emit_error(address, /*err=*/-3);
    return Status::EncodeFailed;
  }
  ctx->msg = proxyapi_BluetoothGATTGetServicesResponse_init_zero;
  ctx->msg.address = address;
  ctx->msg.services_count = 0;

  const auto &services = client->getServices(/*refresh=*/false);

  // Hard cap by the static array bound; over-budget peripherals get
  // silently truncated for v1 (logged so we notice).
  constexpr size_t kMaxServices =
      sizeof(ctx->msg.services) / sizeof(ctx->msg.services[0]);
  constexpr size_t kMaxChars =
      sizeof(ctx->msg.services[0].characteristics) /
      sizeof(ctx->msg.services[0].characteristics[0]);
  constexpr size_t kMaxDescs =
      sizeof(ctx->msg.services[0].characteristics[0].descriptors) /
      sizeof(ctx->msg.services[0].characteristics[0].descriptors[0]);

  size_t svc_idx = 0;
  for (const auto *svc : services) {
    if (svc_idx >= kMaxServices) {
      ESP_LOGW(TAG, "truncating: more than %u services on %012llx",
               static_cast<unsigned>(kMaxServices),
               static_cast<unsigned long long>(address));
      break;
    }
    auto &out_svc = ctx->msg.services[svc_idx];
    out_svc = proxyapi_BluetoothGATTService_init_zero;
    out_svc.handle = svc->getHandle();
    pack_uuid(svc->getUUID(), &out_svc.short_uuid, out_svc.uuid,
              &out_svc.uuid_count);

    const auto &chars = svc->getCharacteristics(/*refresh=*/false);
    out_svc.characteristics_count = 0;
    size_t ch_idx = 0;
    for (auto *chr : chars) {
      if (ch_idx >= kMaxChars) break;
      auto &out_ch = out_svc.characteristics[ch_idx];
      out_ch = proxyapi_BluetoothGATTCharacteristic_init_zero;
      out_ch.handle = chr->getHandle();
      out_ch.properties = pack_properties(chr);
      pack_uuid(chr->getUUID(), &out_ch.short_uuid, out_ch.uuid,
                &out_ch.uuid_count);

      const auto &descs = chr->getDescriptors(/*refresh=*/false);
      out_ch.descriptors_count = 0;
      size_t d_idx = 0;
      for (auto *dsc : descs) {
        if (d_idx >= kMaxDescs) break;
        auto &out_d = out_ch.descriptors[d_idx];
        out_d = proxyapi_BluetoothGATTDescriptor_init_zero;
        out_d.handle = dsc->getHandle();
        pack_uuid(dsc->getUUID(), &out_d.short_uuid, out_d.uuid,
                  &out_d.uuid_count);
        ++d_idx;
      }
      out_ch.descriptors_count = d_idx;
      ++ch_idx;
    }
    out_svc.characteristics_count = ch_idx;
    ++svc_idx;
  }
  ctx->msg.services_count = svc_idx;

  if (!publish::send_async(proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_RESPONSE,
                           &encode_services, ctx.get())) {
    ESP_LOGW(TAG, "send services failed");
    return Status::SendFailed;
  }

  DoneEncodeCtx d;
  d.msg = proxyapi_BluetoothGATTGetServicesDoneResponse_init_zero;
  d.msg.address = address;
  publish::send_async(proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_DONE_RESPONSE,
                      &encode_done, &d);
  return Status::Ok;
}

}  // namespace ble_backend::gatt_discovery
