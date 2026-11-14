#include "gatt_discovery.h"

#include "api_proto.h"
#include "publish.h"
#include "connection.h"

#include "NimBLEDevice.h"
#include "NimBLERemoteCharacteristic.h"
#include "NimBLERemoteDescriptor.h"
#include "NimBLERemoteService.h"
#include "NimBLEUUID.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "oom_guard.h"
#include "proxy_config.h"
#include "pb_encode.h"

#include <cstring>
#include <vector>

namespace ble_backend::gatt_discovery {

namespace {

constexpr const char *TAG = "ble.gatt.disc";

// GATT error code sent for an allocation failure (HA logs it with the
// address; bleak-esphome fails the get_services call).
constexpr int32_t kErrNoMemory = -3;
constexpr size_t kDiscoveryMinFreeHeap = 6 * 1024;

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

// Context owned for the duration of one discovery — passed to send_to
// for each services response.
//
// One service per BluetoothGATTGetServicesResponse (api_subset.options:
// services max_count 1), as ESPHome itself sends them; aioesphomeapi
// collects responses until the Done message. The struct is ~2.9 KiB (12
// chars x 6 descs of static nanopb arrays). It used to hold 8 services,
// ~23 KiB in ONE contiguous block: the field OOM of 2026-11-12 was exactly
// that allocation (operator new(23376) with heap_min 244 B), and a 23 KiB
// block is rarely free on the NAT build even when 40 KiB are. 8 services of
// a real peer did not fit MAX_MESSAGE_SIZE (2048) either.
//
// HEAP-allocated, not on the api_client stack (8 KiB). send_to invokes the
// encode callback synchronously before returning, so unique_ptr scoped to
// run() is sufficient. Allocated with oom_guard::make_unique, NOT
// std::make_unique: without C++ exceptions a failed throwing new aborts.
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

void emit_error(uint32_t owner, uint64_t address, int32_t err) {
  ErrorEncodeCtx ctx;
  ctx.msg = proxyapi_BluetoothGATTErrorResponse_init_zero;
  ctx.msg.address = address;
  ctx.msg.handle = 0;
  ctx.msg.error = err;
  publish::send_to(owner, proxyapi::MSG_BLUETOOTH_GATT_ERROR_RESPONSE,
                   &encode_error, &ctx);
}

// Links whose last walk failed part-way. discoverAttributes() fills the
// service list before it walks characteristics, so after such a failure the
// list is non-empty but incomplete; without this mark the "list empty" test
// would never look again and later handles would stay unknown until the
// peer reconnects. Touched from several connection tasks. A stale entry
// (the slot's NimBLEClient reconnected) only causes one extra fill_gaps.
NimBLEClient *g_partial[proxy::MAX_CONNECTIONS] = {};
portMUX_TYPE g_partial_mux = portMUX_INITIALIZER_UNLOCKED;

void set_partial(NimBLEClient *c, bool partial) {
  taskENTER_CRITICAL(&g_partial_mux);
  bool done = false;
  for (auto &p : g_partial) {
    if (p == c) {
      if (!partial) p = nullptr;
      done = true;
    }
  }
  if (partial && !done) {
    for (auto &p : g_partial) {
      if (p == nullptr) {
        p = c;
        break;
      }
    }
  }
  taskEXIT_CRITICAL(&g_partial_mux);
}

// Walk an empty cache. discoverAttributes() starts with deleteServices(),
// which is safe only here: an empty list has no characteristics, so no
// notify callbacks to lose and nothing the host task can be walking.
bool walk_empty(NimBLEClient *client) {
  const bool ok = client->discoverAttributes();
  set_partial(client, !ok);
  return ok;
}

// Look up the owner's link, walk its attribute table. On failure the error
// reply is already sent; returns nullptr.
NimBLEClient *discover(uint64_t address, uint32_t owner, Status *st) {
  auto *client =
      static_cast<NimBLEClient *>(connection::client_for(address, owner));
  if (client == nullptr) {
    ESP_LOGW(TAG, "%012llx not connected (or not owned by client %lu)",
             static_cast<unsigned long long>(address),
             static_cast<unsigned long>(owner));
    emit_error(owner, address, /*err=*/-1);
    *st = Status::NotConnected;
    return nullptr;
  }

  ESP_LOGI(TAG, "discover %012llx",
           static_cast<unsigned long long>(address));

  // discoverAttributes() builds NimBLE-cpp's attribute objects with the
  // throwing operator new, which we cannot make nothrow; on OOM it waits
  // CONFIG_NBP_OOM_NEW_WAIT_MS and then aborts (oom_guard.h). Refuse to
  // start it with less than kDiscoveryMinFreeHeap left. A heuristic, not a
  // guarantee: ~100 attribute objects of ~60-100 B for the largest peers we
  // serve, plus vector growth.
  const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
  if (free_now < kDiscoveryMinFreeHeap) {
    ESP_LOGW(TAG, "OOM: %u B free < %u B, not starting discovery of %012llx",
             static_cast<unsigned>(free_now),
             static_cast<unsigned>(kDiscoveryMinFreeHeap),
             static_cast<unsigned long long>(address));
    emit_error(owner, address, kErrNoMemory);
    *st = Status::NoMemory;
    return nullptr;
  }

  // Every connect clears the cache, so a non-empty one belongs to this
  // connection: answer from it (filling gaps after a partial walk). Never
  // rediscover over it; see run() in the header.
  if (!client->getServices(/*refresh=*/false).empty()) {
    if (is_partial(client)) {
      ESP_LOGI(TAG, "%012llx: filling gaps of a partial discovery",
               static_cast<unsigned long long>(address));
      fill_gaps(client);
    } else {
      ESP_LOGI(TAG, "%012llx: services from cache",
               static_cast<unsigned long long>(address));
    }
    return client;
  }
  if (!walk_empty(client)) {
    ESP_LOGW(TAG, "discoverAttributes failed for %012llx",
             static_cast<unsigned long long>(address));
    emit_error(owner, address, /*err=*/-2);
    *st = Status::EncodeFailed;
    return nullptr;
  }
  return client;
}

}  // namespace

Status run(uint64_t address, uint32_t owner) {
  NimBLEClient *client = nullptr;
#if NBP_OOM_SELFTEST
  // QEMU OOM self-test (no BLE): skip the link, exercise the allocation
  // and the reply with an empty service list.
  if (address != kOomSelftestAddress) {
#endif
    Status st = Status::Ok;
    client = discover(address, owner, &st);
    if (client == nullptr) return st;
#if NBP_OOM_SELFTEST
  }
#endif

  // After the walk, not before: holding it through discovery would
  // squeeze the NimBLE allocations above, whose failure aborts.
  auto ctx = oom_guard::make_unique<ServicesEncodeCtx>();
  if (ctx == nullptr) {
    ESP_LOGW(TAG, "OOM: no %u B for the services encode ctx (%012llx); "
             "free %u largest %u",
             static_cast<unsigned>(sizeof(ServicesEncodeCtx)),
             static_cast<unsigned long long>(address),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
    emit_error(owner, address, kErrNoMemory);
    return Status::NoMemory;
  }

  static const std::vector<NimBLERemoteService *> kNoServices;
  const auto &services = client != nullptr
                             ? client->getServices(/*refresh=*/false)
                             : kNoServices;

  static_assert(sizeof(ctx->msg.services) / sizeof(ctx->msg.services[0]) == 1,
                "one service per response: services max_count must be 1");
  constexpr size_t kMaxChars =
      sizeof(ctx->msg.services[0].characteristics) /
      sizeof(ctx->msg.services[0].characteristics[0]);
  constexpr size_t kMaxDescs =
      sizeof(ctx->msg.services[0].characteristics[0].descriptors) /
      sizeof(ctx->msg.services[0].characteristics[0].descriptors[0]);

  for (const auto *svc : services) {
    ctx->msg = proxyapi_BluetoothGATTGetServicesResponse_init_zero;
    ctx->msg.address = address;
    ctx->msg.services_count = 1;
    auto &out_svc = ctx->msg.services[0];
    out_svc = proxyapi_BluetoothGATTService_init_zero;
    out_svc.handle = svc->getHandle();
    pack_uuid(svc->getUUID(), &out_svc.short_uuid, out_svc.uuid,
              &out_svc.uuid_count);

    const auto &chars = svc->getCharacteristics(/*refresh=*/false);
    out_svc.characteristics_count = 0;
    size_t ch_idx = 0;
    for (auto *chr : chars) {
      if (ch_idx >= kMaxChars) {
        ESP_LOGW(TAG, "truncating: more than %u characteristics in service "
                 "0x%04x on %012llx", static_cast<unsigned>(kMaxChars),
                 svc->getHandle(), static_cast<unsigned long long>(address));
        break;
      }
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

    // Services, done and errors answer the owner's request only.
    if (!publish::send_to(owner,
                          proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_RESPONSE,
                          &encode_services, ctx.get())) {
      ESP_LOGW(TAG, "send services failed");
      return Status::SendFailed;
    }
  }
  ctx.reset();

  DoneEncodeCtx d;
  d.msg = proxyapi_BluetoothGATTGetServicesDoneResponse_init_zero;
  d.msg.address = address;
  publish::send_to(owner,
                   proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_DONE_RESPONSE,
                   &encode_done, &d);
  return Status::Ok;
}

bool ensure_discovered(NimBLEClient *client) {
  if (!client->getServices(/*refresh=*/false).empty()) return true;
  ESP_LOGI(TAG, "%s: no GetServices since connect; discovering on demand",
           client->getPeerAddress().toString().c_str());
  const bool ok = walk_empty(client);
  if (!ok) {
    ESP_LOGW(TAG, "on-demand discovery failed part-way; will fill gaps on "
             "lookup misses");
  }
  return ok;
}

bool is_partial(NimBLEClient *client) {
  bool found = false;
  taskENTER_CRITICAL(&g_partial_mux);
  for (auto *p : g_partial) found = found || p == client;
  taskEXIT_CRITICAL(&g_partial_mux);
  return found;
}

// Refreshing an empty vector deletes nothing; new entries are appended on
// the host task, the same task that walks them for notifications.
void fill_gaps(NimBLEClient *client) {
  for (auto *svc : client->getServices(/*refresh=*/false)) {
    if (svc->getCharacteristics(/*refresh=*/false).empty()) {
      svc->getCharacteristics(/*refresh=*/true);
    }
    for (auto *chr : svc->getCharacteristics(/*refresh=*/false)) {
      if (chr->getDescriptors(/*refresh=*/false).empty()) {
        chr->getDescriptors(/*refresh=*/true);
      }
    }
  }
}

}  // namespace ble_backend::gatt_discovery
