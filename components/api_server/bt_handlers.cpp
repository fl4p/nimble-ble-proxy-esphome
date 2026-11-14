#include "bt_handlers.h"

#include "proxy_config.h"  // CONFIG_NBP_BLE (via sdkconfig.h)

#if CONFIG_NBP_BLE

#include "api_proto.h"
#include "api_server.h"
#include "ble_backend.h"
#include "stats.h"
#if NBP_HEAP_SELFTEST
#include "heap_selftest.h"
#endif

// ble_backend internal modules — bt_handlers is the bridge layer so
// pulling them in is intentional. Keeping the dispatch in api_server
// means handshake & BT live in the same component (they share the
// frame/dispatch loop), while ble_backend stays protocol-free.
#include "../ble_backend/address.h"
#include "../ble_backend/connection.h"
#include "../ble_backend/gatt_discovery.h"
#include "../ble_backend/scanner.h"

#include "NimBLEDevice.h"
#include "NimBLERemoteCharacteristic.h"
#include "NimBLERemoteDescriptor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <atomic>
#include <cstring>

namespace api_server::bt_handlers {

namespace {

constexpr const char *TAG = "api.bt";

// Subscription ref counts across all clients. send_async broadcasts to
// every connected client anyway, so we only need "is anyone subscribed".
// Per-client flags live in Context::subs so a stuck or crashing client
// can have its increments rolled back via on_client_disconnect rather
// than waiting for the LAST client to leave.
//
// Atomic because per-client tasks compete on the same counter.
std::atomic<int> g_sub_adv_count{0};
std::atomic<int> g_sub_free_count{0};

// ---- encode helpers ----

template <typename T>
size_t encode_one(void *vctx, uint8_t *buf, size_t cap, const pb_msgdesc_t *desc) {
  pb_ostream_t stream = pb_ostream_from_buffer(buf, cap);
  if (!pb_encode(&stream, desc, vctx)) {
    ESP_LOGE(TAG, "encode failed: %s", PB_GET_ERROR(&stream));
    return 0;
  }
  return stream.bytes_written;
}

size_t encode_connections_free(void *vctx, uint8_t *buf, size_t cap) {
  return encode_one<proxyapi_BluetoothConnectionsFreeResponse>(
      vctx, buf, cap, proxyapi_BluetoothConnectionsFreeResponse_fields);
}

size_t encode_connection_response(void *vctx, uint8_t *buf, size_t cap) {
  return encode_one<proxyapi_BluetoothDeviceConnectionResponse>(
      vctx, buf, cap, proxyapi_BluetoothDeviceConnectionResponse_fields);
}

size_t encode_read_response(void *vctx, uint8_t *buf, size_t cap) {
  return encode_one<proxyapi_BluetoothGATTReadResponse>(
      vctx, buf, cap, proxyapi_BluetoothGATTReadResponse_fields);
}

size_t encode_write_response(void *vctx, uint8_t *buf, size_t cap) {
  return encode_one<proxyapi_BluetoothGATTWriteResponse>(
      vctx, buf, cap, proxyapi_BluetoothGATTWriteResponse_fields);
}

size_t encode_notify_response(void *vctx, uint8_t *buf, size_t cap) {
  return encode_one<proxyapi_BluetoothGATTNotifyResponse>(
      vctx, buf, cap, proxyapi_BluetoothGATTNotifyResponse_fields);
}

size_t encode_notify_data(void *vctx, uint8_t *buf, size_t cap) {
  return encode_one<proxyapi_BluetoothGATTNotifyDataResponse>(
      vctx, buf, cap, proxyapi_BluetoothGATTNotifyDataResponse_fields);
}

size_t encode_error(void *vctx, uint8_t *buf, size_t cap) {
  return encode_one<proxyapi_BluetoothGATTErrorResponse>(
      vctx, buf, cap, proxyapi_BluetoothGATTErrorResponse_fields);
}

size_t encode_pairing_response(void *vctx, uint8_t *buf, size_t cap) {
  return encode_one<proxyapi_BluetoothDevicePairingResponse>(
      vctx, buf, cap, proxyapi_BluetoothDevicePairingResponse_fields);
}

size_t encode_unpairing_response(void *vctx, uint8_t *buf, size_t cap) {
  return encode_one<proxyapi_BluetoothDeviceUnpairingResponse>(
      vctx, buf, cap, proxyapi_BluetoothDeviceUnpairingResponse_fields);
}

size_t encode_clear_cache_response(void *vctx, uint8_t *buf, size_t cap) {
  return encode_one<proxyapi_BluetoothDeviceClearCacheResponse>(
      vctx, buf, cap, proxyapi_BluetoothDeviceClearCacheResponse_fields);
}

// Negative sentinels for the pairing/unpairing/clear-cache responses.
// Positive values in `error` are NimBLE host codes passed through from
// ble_backend, so keep ours out of that space.
// (maybe_unused: which of these is reachable depends on CONFIG_NBP_SMP.)
[[maybe_unused]] constexpr int32_t ERR_NOT_SUPPORTED = -101;  // no NBP_SMP
[[maybe_unused]] constexpr int32_t ERR_NOT_CONNECTED = -102;  // no live link of the requester's
[[maybe_unused]] constexpr int32_t ERR_NO_BOND = -103;  // nothing to remove

// Pairing, unpairing and clear-cache responses answer one request, so
// like GATT errors they go to the requester only (send_to drops them if
// that client has gone).
void send_pairing(uint32_t client_id, uint64_t address, bool paired,
                  int32_t err) {
  proxyapi_BluetoothDevicePairingResponse msg =
      proxyapi_BluetoothDevicePairingResponse_init_zero;
  msg.address = address;
  msg.paired = paired;
  msg.error = err;
  api_server::send_to(client_id,
                      proxyapi::MSG_BLUETOOTH_DEVICE_PAIRING_RESPONSE,
                      &encode_pairing_response, &msg);
}

void send_unpairing(uint32_t client_id, uint64_t address, bool success,
                    int32_t err) {
  proxyapi_BluetoothDeviceUnpairingResponse msg =
      proxyapi_BluetoothDeviceUnpairingResponse_init_zero;
  msg.address = address;
  msg.success = success;
  msg.error = err;
  api_server::send_to(client_id,
                      proxyapi::MSG_BLUETOOTH_DEVICE_UNPAIRING_RESPONSE,
                      &encode_unpairing_response, &msg);
}

void send_clear_cache(uint32_t client_id, uint64_t address, bool success,
                      int32_t err) {
  proxyapi_BluetoothDeviceClearCacheResponse msg =
      proxyapi_BluetoothDeviceClearCacheResponse_init_zero;
  msg.address = address;
  msg.success = success;
  msg.error = err;
  api_server::send_to(client_id,
                      proxyapi::MSG_BLUETOOTH_DEVICE_CLEAR_CACHE_RESPONSE,
                      &encode_clear_cache_response, &msg);
}

#ifdef CONFIG_NBP_SMP
// Fires from ble_backend's bonding worker task (or synchronously from
// connection::pair() when the link is already encrypted). `owner` is the
// client that sent PAIR.
void on_pair_result(ble_backend::connection::OwnerId owner, uint64_t address,
                    const ble_backend::connection::PairResult &r) {
  ESP_LOGI(TAG, "pair %012llx -> paired=%d err=%ld owner=%lu",
           static_cast<unsigned long long>(address), r.paired,
           static_cast<long>(r.error), static_cast<unsigned long>(owner));
  send_pairing(owner, address, r.paired, r.error);
}
#endif

// Errors answer one request, so they go to the requester only — a
// broadcast would land on whichever other client owns `address` and look
// like a failure of its own pending op.
void send_gatt_error(uint32_t client_id, uint64_t address, uint32_t handle,
                     int32_t err) {
  proxyapi_BluetoothGATTErrorResponse msg =
      proxyapi_BluetoothGATTErrorResponse_init_zero;
  msg.address = address;
  msg.handle = handle;
  msg.error = err;
  api_server::send_to(client_id, proxyapi::MSG_BLUETOOTH_GATT_ERROR_RESPONSE,
                      &encode_error, &msg);
}

void send_connection_state(uint32_t client_id, uint64_t address,
                           bool connected, uint16_t mtu, int32_t error) {
  proxyapi_BluetoothDeviceConnectionResponse msg =
      proxyapi_BluetoothDeviceConnectionResponse_init_zero;
  msg.address = address;
  msg.connected = connected;
  msg.mtu = mtu;
  msg.error = error;
  api_server::send_to(client_id,
                      proxyapi::MSG_BLUETOOTH_DEVICE_CONNECTION_RESPONSE,
                      &encode_connection_response, &msg);
}

void build_free_msg(proxyapi_BluetoothConnectionsFreeResponse *m) {
  *m = proxyapi_BluetoothConnectionsFreeResponse_init_zero;
  m->limit = proxy::MAX_CONNECTIONS;
  m->free = ble_backend::connection::free_slots();
  // in_use_addresses writes up to MAX_CONNECTIONS entries into the nanopb
  // `allocated` array, whose size is fixed by max_count in api_subset.options.
  // Keep them coupled at compile time (sizeof on the member is constant even
  // through the pointer — it doesn't dereference m).
  static_assert(proxy::MAX_CONNECTIONS <=
                    sizeof(m->allocated) / sizeof(m->allocated[0]),
                "proxy::MAX_CONNECTIONS exceeds the nanopb allocated[] capacity — "
                "raise max_count in components/api_proto/api_subset.options");
  m->allocated_count = ble_backend::connection::in_use_addresses(
      m->allocated, proxy::MAX_CONNECTIONS);
}

void on_free_change() {
  if (g_sub_free_count.load(std::memory_order_acquire) <= 0) return;
  proxyapi_BluetoothConnectionsFreeResponse msg;
  build_free_msg(&msg);
  api_server::send_async(proxyapi::MSG_BLUETOOTH_CONNECTIONS_FREE_RESPONSE,
                         &encode_connections_free, &msg);
}

// ---- connection completion callback ----
//
// Fires from the api_server task on initial connect result AND later
// from the NimBLE host task on peer-initiated disconnect. Both paths
// route through here so the owner sees one canonical ConnectionResponse
// on every state transition.
//
// Sent to the owning client only. aioesphomeapi/bleak-esphome treat any
// ConnectionResponse(connected=false) for an address they hold as "my
// link dropped", so a broadcast would make one client tear down its view
// of a healthy link because of another client's traffic. An ownerless
// link (its client left) reports to nobody; send_to also drops the
// message if the owner disconnected after the slot was snapshotted.
void on_connection_state(ble_backend::connection::OwnerId owner,
                         uint64_t address,
                         const ble_backend::connection::ConnectionResult &r) {
  if (owner == ble_backend::connection::kNoOwner) return;
  send_connection_state(owner, address, r.connected, r.mtu, r.error);
}

// ---- notify subscription tracking ----
//
// HA expects NotifyDataResponse(79) messages whenever a subscribed
// characteristic fires. NimBLE delivers these via a per-characteristic
// callback closure; we keep a flat list so we can also tear them down
// when the client disconnects (NimBLE auto-clears on physical
// disconnect, but stop_notify can fail silently then).

void notify_cb(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len,
               bool /*is_notify*/) {
  // NimBLE host task. Build response on the stack — send_async will
  // copy it into the TX buffer under the TX mutex.
  proxyapi_BluetoothGATTNotifyDataResponse msg =
      proxyapi_BluetoothGATTNotifyDataResponse_init_zero;
  // Walk back from the characteristic to its parent client to learn
  // the address. NimBLE provides ->getClient() on remote chars.
  NimBLEClient *client = chr->getClient();
  if (client == nullptr) return;
  // NimBLEAddress::operator uint64_t() already memcpys the LE bytes
  // onto a uint64 on this LE host, which is exactly the layout
  // aioesphomeapi expects (MSB of the int == MSB of the printed MAC).
  msg.address = static_cast<uint64_t>(client->getPeerAddress());
  msg.handle = chr->getHandle();
  size_t copy = len;
  if (copy > sizeof(msg.data.bytes)) copy = sizeof(msg.data.bytes);
  std::memcpy(msg.data.bytes, data, copy);
  msg.data.size = copy;
  // Only the client that owns this link gets its notify data. No owner
  // (its client left; teardown in progress) → drop.
  auto owner = ble_backend::connection::owner_of_client(client);
  if (owner == ble_backend::connection::kNoOwner) {
    api_server::stats::record_notify_unowned();
    return;
  }
  api_server::stats::record_notify();
  api_server::send_to(owner, proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE,
                      &encode_notify_data, &msg);
}

// ---- per-request handlers ----

bool handle_subscribe_adv(const uint8_t *payload, size_t payload_len,
                          const Context &ctx) {
  // Decode but ignore `flags` — we always forward raw payloads in v1.
  proxyapi_SubscribeBluetoothLEAdvertisementsRequest req =
      proxyapi_SubscribeBluetoothLEAdvertisementsRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  pb_decode(&s, proxyapi_SubscribeBluetoothLEAdvertisementsRequest_fields, &req);
  // Idempotent per-client: only the FIRST subscribe from a given client
  // bumps the global ref count. Stops a misbehaving client from running
  // the count up unboundedly.
  if (!ctx.subs->sub_adv) {
    ctx.subs->sub_adv = true;
    if (g_sub_adv_count.fetch_add(1, std::memory_order_acq_rel) == 0) {
      ble_backend::scanner::start_forwarding();
    }
  }
  // TODO(active-scan): make effective active-scan = (web toggle, /scan?active=1
  // via scanner::get_active()) OR (HA is subscribed, g_sub_adv_count > 0).
  // NOTE: there is NO active-scan request in the ESPHome protocol — `flags`
  // above is only BLUETOOTH_PROXY_SUBSCRIPTION_FLAG_RAW_ADVERTISEMENTS, so the
  // OR's second operand is "HA is listening", not a scan-mode field. Drive
  // scanner::set_active() from here + the unsubscribe path accordingly.
  return true;
}

bool handle_unsubscribe_adv(const Context &ctx) {
  if (ctx.subs->sub_adv) {
    ctx.subs->sub_adv = false;
    if (g_sub_adv_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      ble_backend::scanner::stop_forwarding();
    }
  }
  return true;
}

bool handle_subscribe_free(const Context &ctx) {
  if (!ctx.subs->sub_free) {
    ctx.subs->sub_free = true;
    g_sub_free_count.fetch_add(1, std::memory_order_acq_rel);
  }
  proxyapi_BluetoothConnectionsFreeResponse msg;
  build_free_msg(&msg);
  // Broadcast — send_async takes g_tx_mutex itself, safe to call here
  // because bt_handlers::handle no longer runs under the mutex.
  api_server::send_async(proxyapi::MSG_BLUETOOTH_CONNECTIONS_FREE_RESPONSE,
                         &encode_connections_free, &msg);
  return true;
}

bool handle_device_request(const uint8_t *payload, size_t payload_len,
                           const Context &ctx) {
  proxyapi_BluetoothDeviceRequest req = proxyapi_BluetoothDeviceRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  if (!pb_decode(&s, proxyapi_BluetoothDeviceRequest_fields, &req)) {
    ESP_LOGW(TAG, "device_request decode failed");
    return true;
  }
  uint8_t addr_type = req.has_address_type ? req.address_type : 0;
  switch (req.request_type) {
    case proxyapi_BluetoothDeviceRequestType_BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT:
    case proxyapi_BluetoothDeviceRequestType_BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT_V3_WITH_CACHE:
    case proxyapi_BluetoothDeviceRequestType_BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT_V3_WITHOUT_CACHE: {
      // See connection::connect for the ownership rule: the owner's own
      // re-CONNECT is answered with the link's state; an address held by
      // another client is refused, never taken over.
      using ble_backend::connection::ConnectStatus;
      uint16_t mtu = 0;
      ConnectStatus st = ble_backend::connection::connect(
          req.address, addr_type, ctx.client_id, &on_connection_state, &mtu);
      switch (st) {
        case ConnectStatus::Started:
        case ConnectStatus::OwnInProgress:  // result follows via the cb
          break;
        case ConnectStatus::OwnConnected:
          send_connection_state(ctx.client_id, req.address, true, mtu, 0);
          break;
        case ConnectStatus::HeldByOther:
        case ConnectStatus::NoSlot:
          // Emit failure so the requester isn't left waiting — to the
          // requester ONLY: the holder of this address must not see a
          // connected=false for its own, healthy link.
          send_connection_state(ctx.client_id, req.address, false, 0,
                                /*sentinel "no slot / already connected"=*/-100);
          break;
      }
      break;
    }
    case proxyapi_BluetoothDeviceRequestType_BLUETOOTH_DEVICE_REQUEST_TYPE_DISCONNECT: {
      using ble_backend::connection::DisconnectStatus;
      DisconnectStatus st =
          ble_backend::connection::disconnect(req.address, ctx.client_id);
      if (st != DisconnectStatus::Started) {
        // Nothing of the requester's to tear down: no slot, or another
        // client's link (left untouched). From the requester's point of
        // view it is disconnected — say so, to it alone, so its
        // disconnect() completes instead of timing out. Started → the
        // owner gets connected=false from the teardown callback.
        send_connection_state(ctx.client_id, req.address, false, 0, 0);
      }
      break;
    }
    case proxyapi_BluetoothDeviceRequestType_BLUETOOTH_DEVICE_REQUEST_TYPE_PAIR: {
#ifdef CONFIG_NBP_SMP
      // connection::pair() answers through on_pair_result — either
      // synchronously (link already encrypted) or from the bonding
      // worker once SMP resolves. A false return means there was
      // nothing of the requester's to pair on, so we answer here instead.
      if (!ble_backend::connection::pair(req.address, ctx.client_id,
                                         &on_pair_result)) {
        send_pairing(ctx.client_id, req.address, false, ERR_NOT_CONNECTED);
      }
#else
      send_pairing(ctx.client_id, req.address, false, ERR_NOT_SUPPORTED);
#endif
      break;
    }
    case proxyapi_BluetoothDeviceRequestType_BLUETOOTH_DEVICE_REQUEST_TYPE_UNPAIR: {
#ifdef CONFIG_NBP_SMP
      // Deleting the bond does not drop a live link — matching BlueZ's
      // RemoveDevice semantics closely enough for HA's purposes. The bond
      // store is device-wide, not per link, so no ownership check.
      bool ok = ble_backend::connection::unpair(req.address);
      send_unpairing(ctx.client_id, req.address, ok, ok ? 0 : ERR_NO_BOND);
#else
      send_unpairing(ctx.client_id, req.address, false, ERR_NOT_SUPPORTED);
#endif
      break;
    }
    case proxyapi_BluetoothDeviceRequestType_BLUETOOTH_DEVICE_REQUEST_TYPE_CLEAR_CACHE:
      // We hold no GATT cache between connections (every connect clears
      // NimBLE's attribute cache, deleteAttributes=true), so the
      // post-condition HA wants is already true.
      send_clear_cache(ctx.client_id, req.address, true, 0);
      break;
  }
  return true;
}

bool handle_get_services(const uint8_t *payload, size_t payload_len,
                         const Context &ctx) {
  proxyapi_BluetoothGATTGetServicesRequest req =
      proxyapi_BluetoothGATTGetServicesRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  if (!pb_decode(&s, proxyapi_BluetoothGATTGetServicesRequest_fields, &req)) {
    return true;
  }
#if NBP_HEAP_SELFTEST
  // QEMU heap self-test (no BLE): a GetServices-sized burst on this
  // connection task, the producer that waits for ring room.
  if (req.address == heap_selftest::kBulkAddress) {
    heap_selftest::bulk_burst(ctx.client_id);
    return true;
  }
#endif
  // Owner-only (gatt_discovery::run checks), so two clients can never
  // run discovery on one link concurrently. It walks the peer only when the
  // cache is empty; a repeat GetServices answers from the cache.
  ble_backend::gatt_discovery::run(req.address, ctx.client_id);
  return true;
}

// Every connect clears the NimBLEClient attribute cache
// (connection::connect, deleteAttributes=true), and we advertise
// REMOTE_CACHING, so HA (disk cache) and bleak_esphome (per-process cache)
// skip GetServices on a reconnect. Without on-demand discovery every
// read/write and the notify registration then failed against an empty
// cache, and the peer's notifications were dropped as "NimBLEClient:
// unknown handle: N" (issue #2, batmon-ha #391). The helpers live in
// gatt_discovery (shared with GetServices); they block, so they run here
// on the API connection task, never on the host task.
using ble_backend::gatt_discovery::ensure_discovered;
using ble_backend::gatt_discovery::fill_gaps;
using ble_backend::gatt_discovery::is_partial;

NimBLERemoteCharacteristic *find_char(NimBLEClient *client, uint16_t handle) {
  for (auto *svc : client->getServices(/*refresh=*/false)) {
    for (auto *chr : svc->getCharacteristics(/*refresh=*/false)) {
      if (chr->getHandle() == handle) return chr;
    }
  }
  return nullptr;
}

NimBLERemoteDescriptor *find_desc(NimBLEClient *client, uint16_t handle) {
  for (auto *svc : client->getServices(/*refresh=*/false)) {
    for (auto *chr : svc->getCharacteristics(/*refresh=*/false)) {
      for (auto *dsc : chr->getDescriptors(/*refresh=*/false)) {
        if (dsc->getHandle() == handle) return dsc;
      }
    }
  }
  return nullptr;
}

NimBLERemoteCharacteristic *char_at(NimBLEClient *client, uint16_t handle) {
  ensure_discovered(client);
  auto *chr = find_char(client, handle);
  if (chr == nullptr && is_partial(client)) {
    fill_gaps(client);
    chr = find_char(client, handle);
  }
  return chr;
}

NimBLERemoteDescriptor *desc_at(NimBLEClient *client, uint16_t handle) {
  ensure_discovered(client);
  auto *dsc = find_desc(client, handle);
  if (dsc == nullptr && is_partial(client)) {
    fill_gaps(client);
    dsc = find_desc(client, handle);
  }
  return dsc;
}

bool handle_read(uint16_t msg_type, const uint8_t *payload, size_t payload_len,
                 const Context &ctx) {
  proxyapi_BluetoothGATTReadRequest req =
      proxyapi_BluetoothGATTReadRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  if (!pb_decode(&s, proxyapi_BluetoothGATTReadRequest_fields, &req)) return true;

  auto *client = static_cast<NimBLEClient *>(
      ble_backend::connection::client_for(req.address, ctx.client_id));
  if (client == nullptr) {
    send_gatt_error(ctx.client_id, req.address, req.handle, -1);
    return true;
  }
  proxyapi_BluetoothGATTReadResponse rsp =
      proxyapi_BluetoothGATTReadResponse_init_zero;
  rsp.address = req.address;
  rsp.handle = req.handle;
  // Copy straight out of the NimBLEAttValue (calloc/realloc-backed, null
  // on OOM). Converting it to std::string allocated values > 15 B through
  // the throwing operator new, which aborts on OOM (no exceptions).
  auto take = [&rsp](const NimBLEAttValue &v) {
    size_t copy = v.size();
    if (copy > sizeof(rsp.data.bytes)) copy = sizeof(rsp.data.bytes);
    if (v.data() == nullptr) copy = 0;
    if (copy > 0) std::memcpy(rsp.data.bytes, v.data(), copy);
    rsp.data.size = copy;
  };
  bool ok = false;
  if (msg_type == proxyapi::MSG_BLUETOOTH_GATT_READ_REQUEST) {
    auto *chr = char_at(client, req.handle);
    if (chr != nullptr) {
      take(chr->readValue());
      ok = true;
    }
  } else {
    auto *dsc = desc_at(client, req.handle);
    if (dsc != nullptr) {
      take(dsc->readValue());
      ok = true;
    }
  }
  if (!ok) {
    send_gatt_error(ctx.client_id, req.address, req.handle, -2);
    return true;
  }

  api_server::stats::record_read();
  api_server::send_to(ctx.client_id, proxyapi::MSG_BLUETOOTH_GATT_READ_RESPONSE,
                      &encode_read_response, &rsp);
  return true;
}

bool handle_write_char(const uint8_t *payload, size_t payload_len,
                       const Context &ctx) {
  proxyapi_BluetoothGATTWriteRequest req =
      proxyapi_BluetoothGATTWriteRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  if (!pb_decode(&s, proxyapi_BluetoothGATTWriteRequest_fields, &req)) return true;

  auto *client = static_cast<NimBLEClient *>(
      ble_backend::connection::client_for(req.address, ctx.client_id));
  if (client == nullptr) {
    send_gatt_error(ctx.client_id, req.address, req.handle, -1);
    return true;
  }
  auto *chr = char_at(client, req.handle);
  if (chr == nullptr) {
    send_gatt_error(ctx.client_id, req.address, req.handle, -2);
    return true;
  }
  bool ok = chr->writeValue(req.data.bytes, req.data.size, req.response);
  if (!ok) {
    send_gatt_error(ctx.client_id, req.address, req.handle, -3);
    return true;
  }
  proxyapi_BluetoothGATTWriteResponse rsp =
      proxyapi_BluetoothGATTWriteResponse_init_zero;
  rsp.address = req.address;
  rsp.handle = req.handle;
  api_server::stats::record_write();
  api_server::send_to(ctx.client_id,
                      proxyapi::MSG_BLUETOOTH_GATT_WRITE_RESPONSE,
                      &encode_write_response, &rsp);
  return true;
}

bool handle_write_desc(const uint8_t *payload, size_t payload_len,
                       const Context &ctx) {
  proxyapi_BluetoothGATTWriteDescriptorRequest req =
      proxyapi_BluetoothGATTWriteDescriptorRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  if (!pb_decode(&s, proxyapi_BluetoothGATTWriteDescriptorRequest_fields,
                 &req)) {
    return true;
  }
  auto *client = static_cast<NimBLEClient *>(
      ble_backend::connection::client_for(req.address, ctx.client_id));
  if (client == nullptr) {
    send_gatt_error(ctx.client_id, req.address, req.handle, -1);
    return true;
  }
  auto *dsc = desc_at(client, req.handle);
  if (dsc == nullptr) {
    send_gatt_error(ctx.client_id, req.address, req.handle, -2);
    return true;
  }
  bool ok = dsc->writeValue(req.data.bytes, req.data.size,
                            /*response=*/true);
  if (!ok) {
    send_gatt_error(ctx.client_id, req.address, req.handle, -3);
    return true;
  }
  proxyapi_BluetoothGATTWriteResponse rsp =
      proxyapi_BluetoothGATTWriteResponse_init_zero;
  rsp.address = req.address;
  rsp.handle = req.handle;
  api_server::stats::record_write();
  api_server::send_to(ctx.client_id,
                      proxyapi::MSG_BLUETOOTH_GATT_WRITE_RESPONSE,
                      &encode_write_response, &rsp);
  return true;
}

bool handle_notify(const uint8_t *payload, size_t payload_len,
                   const Context &ctx) {
  proxyapi_BluetoothGATTNotifyRequest req =
      proxyapi_BluetoothGATTNotifyRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  if (!pb_decode(&s, proxyapi_BluetoothGATTNotifyRequest_fields, &req)) {
    return true;
  }
  auto *client = static_cast<NimBLEClient *>(
      ble_backend::connection::client_for(req.address, ctx.client_id));
  if (client == nullptr) {
    send_gatt_error(ctx.client_id, req.address, req.handle, -1);
    return true;
  }
  auto *chr = char_at(client, req.handle);
  if (chr == nullptr) {
    send_gatt_error(ctx.client_id, req.address, req.handle, -2);
    return true;
  }
  // Register the notify callback but DON'T write the CCCD here —
  // bleak-esphome's bluetooth_gatt_start_notify is immediately followed
  // by an explicit bluetooth_gatt_write_descriptor that writes the same
  // CCCD value. Writing it twice (200 ms apart) breaks ANT-BLE20PHUB:
  // the BMS goes radio-silent after the second CCCD write and our
  // controller supervision-times-out ~6 s later (reason 0x208).
  chr->setNotifyCallback(req.enable ? &notify_cb : nullptr);
  bool ok = true;
  if (!ok) {
    send_gatt_error(ctx.client_id, req.address, req.handle, -3);
    return true;
  }
  proxyapi_BluetoothGATTNotifyResponse rsp =
      proxyapi_BluetoothGATTNotifyResponse_init_zero;
  rsp.address = req.address;
  rsp.handle = req.handle;
  api_server::send_to(ctx.client_id,
                      proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_RESPONSE,
                      &encode_notify_response, &rsp);
  return true;
}

}  // namespace

bool handle(uint16_t request_type, const uint8_t *request_payload,
            size_t request_len, const Context &ctx) {
  // Wire the free-change callback lazily so we don't depend on
  // construction order between this component and ble_backend.
  static bool s_wired = false;
  if (!s_wired) {
    ble_backend::connection::register_free_change_cb(&on_free_change);
    s_wired = true;
  }

  switch (request_type) {
    case proxyapi::MSG_SUBSCRIBE_BLE_ADVERTISEMENTS_REQUEST:
      return handle_subscribe_adv(request_payload, request_len, ctx);
    case proxyapi::MSG_UNSUBSCRIBE_BLE_ADVERTISEMENTS_REQUEST:
      return handle_unsubscribe_adv(ctx);
    case proxyapi::MSG_SUBSCRIBE_BLUETOOTH_CONNECTIONS_FREE_REQUEST:
      return handle_subscribe_free(ctx);
    case proxyapi::MSG_BLUETOOTH_DEVICE_REQUEST:
      return handle_device_request(request_payload, request_len, ctx);
    case proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_REQUEST:
      return handle_get_services(request_payload, request_len, ctx);
    case proxyapi::MSG_BLUETOOTH_GATT_READ_REQUEST:
    case proxyapi::MSG_BLUETOOTH_GATT_READ_DESCRIPTOR_REQUEST:
      return handle_read(request_type, request_payload, request_len, ctx);
    case proxyapi::MSG_BLUETOOTH_GATT_WRITE_REQUEST:
      return handle_write_char(request_payload, request_len, ctx);
    case proxyapi::MSG_BLUETOOTH_GATT_WRITE_DESCRIPTOR_REQUEST:
      return handle_write_desc(request_payload, request_len, ctx);
    case proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_REQUEST:
      return handle_notify(request_payload, request_len, ctx);
    default:
      return false;
  }
}

void on_client_disconnect(ClientSubs &subs) {
  // Release whatever this client subscribed to. Counters drop to zero
  // → stop the underlying ble_backend work (adv forwarding etc.).
  if (subs.sub_adv) {
    subs.sub_adv = false;
    if (g_sub_adv_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      ble_backend::scanner::stop_forwarding();
    }
  }
  if (subs.sub_free) {
    subs.sub_free = false;
    g_sub_free_count.fetch_sub(1, std::memory_order_acq_rel);
  }
}

void on_client_links_release(uint32_t client_id) {
  ble_backend::connection::release_owner(client_id);
}

void on_last_client_disconnect() {
  // Backstop: on_client_links_release already dropped each client's own
  // links. With everyone gone, also drop anything left (ownerless slots
  // still winding down) so no peer stays linked with nothing reading.
  ble_backend::on_api_client_disconnect();
}

}  // namespace api_server::bt_handlers

#else  // !CONFIG_NBP_BLE

// BLE disabled (NBP_BLE off): the BluetoothProxy protocol and the whole
// ble_backend are absent from this build. Provide trivial stubs so
// api_server's dispatch loop still links. handle() reports "not
// recognized", so any Bluetooth* message HA sends is silently ignored
// (the device advertises 0 bluetooth_proxy_feature_flags anyway, so a
// well-behaved HA never sends them); the teardown hooks are no-ops.
namespace api_server::bt_handlers {

bool handle(uint16_t, const uint8_t *, size_t, const Context &) {
  return false;
}
void on_client_disconnect(ClientSubs &) {}
void on_client_links_release(uint32_t) {}
void on_last_client_disconnect() {}

}  // namespace api_server::bt_handlers

#endif  // CONFIG_NBP_BLE
