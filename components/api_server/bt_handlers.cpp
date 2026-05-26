#include "bt_handlers.h"

#include "api_proto.h"
#include "api_server.h"
#include "ble_backend.h"
#include "proxy_config.h"
#include "stats.h"

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
#include "pb_decode.h"
#include "pb_encode.h"

#include <cstring>
#include <vector>

namespace api_server::bt_handlers {

namespace {

constexpr const char *TAG = "api.bt";

// Subscription counters across all clients. send_async broadcasts to
// every connected client anyway, so we only need to know "is anyone
// subscribed". Counters get reset by on_last_client_disconnect()
// rather than per-client decrement — simpler, and any stale increment
// gets cleared the next time the API goes idle.
int g_sub_adv_count = 0;
int g_sub_free_count = 0;

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

void send_gatt_error(uint64_t address, uint32_t handle, int32_t err) {
  proxyapi_BluetoothGATTErrorResponse msg =
      proxyapi_BluetoothGATTErrorResponse_init_zero;
  msg.address = address;
  msg.handle = handle;
  msg.error = err;
  api_server::send_async(proxyapi::MSG_BLUETOOTH_GATT_ERROR_RESPONSE,
                         &encode_error, &msg);
}

void build_free_msg(proxyapi_BluetoothConnectionsFreeResponse *m) {
  *m = proxyapi_BluetoothConnectionsFreeResponse_init_zero;
  m->limit = proxy::MAX_CONNECTIONS;
  m->free = ble_backend::connection::free_slots();
  m->allocated_count = ble_backend::connection::in_use_addresses(
      m->allocated, proxy::MAX_CONNECTIONS);
}

void on_free_change() {
  if (g_sub_free_count <= 0) return;
  proxyapi_BluetoothConnectionsFreeResponse msg;
  build_free_msg(&msg);
  api_server::send_async(proxyapi::MSG_BLUETOOTH_CONNECTIONS_FREE_RESPONSE,
                         &encode_connections_free, &msg);
}

// ---- connection completion callback ----
//
// Fires from the api_server task on initial connect result AND later
// from the NimBLE host task on peer-initiated disconnect. Both paths
// route through here so HA sees one canonical ConnectionResponse on
// every state transition.
void on_connection_state(uint64_t address,
                         const ble_backend::connection::ConnectionResult &r) {
  proxyapi_BluetoothDeviceConnectionResponse msg =
      proxyapi_BluetoothDeviceConnectionResponse_init_zero;
  msg.address = address;
  msg.connected = r.connected;
  msg.mtu = r.mtu;
  msg.error = r.error;
  api_server::send_async(proxyapi::MSG_BLUETOOTH_DEVICE_CONNECTION_RESPONSE,
                         &encode_connection_response, &msg);
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
  msg.address = ble_backend::address::swap6(
      static_cast<uint64_t>(client->getPeerAddress()));
  msg.handle = chr->getHandle();
  size_t copy = len;
  if (copy > sizeof(msg.data.bytes)) copy = sizeof(msg.data.bytes);
  std::memcpy(msg.data.bytes, data, copy);
  msg.data.size = copy;
  api_server::stats::record_notify();
  api_server::send_async(proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE,
                         &encode_notify_data, &msg);
}

// ---- per-request handlers ----

bool handle_subscribe_adv(const uint8_t *payload, size_t payload_len) {
  // Decode but ignore `flags` — we always forward raw payloads in v1.
  proxyapi_SubscribeBluetoothLEAdvertisementsRequest req =
      proxyapi_SubscribeBluetoothLEAdvertisementsRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  pb_decode(&s, proxyapi_SubscribeBluetoothLEAdvertisementsRequest_fields, &req);
  if (++g_sub_adv_count == 1) ble_backend::scanner::start_forwarding();
  return true;
}

bool handle_unsubscribe_adv() {
  if (g_sub_adv_count > 0 && --g_sub_adv_count == 0) {
    ble_backend::scanner::stop_forwarding();
  }
  return true;
}

bool handle_subscribe_free(const Context &ctx) {
  ++g_sub_free_count;
  proxyapi_BluetoothConnectionsFreeResponse msg;
  build_free_msg(&msg);
  size_t n = encode_one<proxyapi_BluetoothConnectionsFreeResponse>(
      &msg, ctx.response_buf, ctx.response_cap,
      proxyapi_BluetoothConnectionsFreeResponse_fields);
  if (n == 0) return true;
  ctx.send_response(ctx.client_fd,
                    proxyapi::MSG_BLUETOOTH_CONNECTIONS_FREE_RESPONSE, n);
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
      bool started = ble_backend::connection::connect(req.address, addr_type,
                                                      &on_connection_state);
      if (!started) {
        // Emit failure inline so HA isn't left waiting.
        proxyapi_BluetoothDeviceConnectionResponse rsp =
            proxyapi_BluetoothDeviceConnectionResponse_init_zero;
        rsp.address = req.address;
        rsp.connected = false;
        rsp.error = -100;  // sentinel: "no slot / already connected"
        size_t n = encode_one<proxyapi_BluetoothDeviceConnectionResponse>(
            &rsp, ctx.response_buf, ctx.response_cap,
            proxyapi_BluetoothDeviceConnectionResponse_fields);
        if (n > 0) {
          ctx.send_response(
              ctx.client_fd,
              proxyapi::MSG_BLUETOOTH_DEVICE_CONNECTION_RESPONSE, n);
        }
      }
      break;
    }
    case proxyapi_BluetoothDeviceRequestType_BLUETOOTH_DEVICE_REQUEST_TYPE_DISCONNECT:
      ble_backend::connection::disconnect(req.address);
      break;
    case proxyapi_BluetoothDeviceRequestType_BLUETOOTH_DEVICE_REQUEST_TYPE_PAIR:
    case proxyapi_BluetoothDeviceRequestType_BLUETOOTH_DEVICE_REQUEST_TYPE_UNPAIR:
    case proxyapi_BluetoothDeviceRequestType_BLUETOOTH_DEVICE_REQUEST_TYPE_CLEAR_CACHE:
      // Pairing/cache flags aren't advertised in our feature bits, but
      // HA may still send them speculatively — return a graceful error.
      send_gatt_error(req.address, 0, /*err=*/-99);
      break;
  }
  return true;
}

bool handle_get_services(const uint8_t *payload, size_t payload_len) {
  proxyapi_BluetoothGATTGetServicesRequest req =
      proxyapi_BluetoothGATTGetServicesRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  if (!pb_decode(&s, proxyapi_BluetoothGATTGetServicesRequest_fields, &req)) {
    return true;
  }
  ble_backend::gatt_discovery::run(req.address);
  return true;
}

NimBLERemoteCharacteristic *char_at(NimBLEClient *client, uint16_t handle) {
  const auto &services = client->getServices(/*refresh=*/false);
  for (auto *svc : services) {
    const auto &chars = svc->getCharacteristics(/*refresh=*/false);
    for (auto *chr : chars) {
      if (chr->getHandle() == handle) return chr;
    }
  }
  return nullptr;
}

NimBLERemoteDescriptor *desc_at(NimBLEClient *client, uint16_t handle) {
  const auto &services = client->getServices(/*refresh=*/false);
  for (auto *svc : services) {
    const auto &chars = svc->getCharacteristics(/*refresh=*/false);
    for (auto *chr : chars) {
      const auto &descs = chr->getDescriptors(/*refresh=*/false);
      for (auto *dsc : descs) {
        if (dsc->getHandle() == handle) return dsc;
      }
    }
  }
  return nullptr;
}

bool handle_read(uint16_t msg_type, const uint8_t *payload, size_t payload_len) {
  proxyapi_BluetoothGATTReadRequest req =
      proxyapi_BluetoothGATTReadRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  if (!pb_decode(&s, proxyapi_BluetoothGATTReadRequest_fields, &req)) return true;

  auto *client = static_cast<NimBLEClient *>(
      ble_backend::connection::client_for(req.address));
  if (client == nullptr) {
    send_gatt_error(req.address, req.handle, -1);
    return true;
  }
  std::string value;
  bool ok = false;
  if (msg_type == proxyapi::MSG_BLUETOOTH_GATT_READ_REQUEST) {
    auto *chr = char_at(client, req.handle);
    if (chr != nullptr) {
      value = chr->readValue();
      ok = true;
    }
  } else {
    auto *dsc = desc_at(client, req.handle);
    if (dsc != nullptr) {
      value = dsc->readValue();
      ok = true;
    }
  }
  if (!ok) {
    send_gatt_error(req.address, req.handle, -2);
    return true;
  }

  proxyapi_BluetoothGATTReadResponse rsp =
      proxyapi_BluetoothGATTReadResponse_init_zero;
  rsp.address = req.address;
  rsp.handle = req.handle;
  size_t copy = value.size();
  if (copy > sizeof(rsp.data.bytes)) copy = sizeof(rsp.data.bytes);
  std::memcpy(rsp.data.bytes, value.data(), copy);
  rsp.data.size = copy;
  api_server::stats::record_read();
  api_server::send_async(proxyapi::MSG_BLUETOOTH_GATT_READ_RESPONSE,
                         &encode_read_response, &rsp);
  return true;
}

bool handle_write_char(const uint8_t *payload, size_t payload_len) {
  proxyapi_BluetoothGATTWriteRequest req =
      proxyapi_BluetoothGATTWriteRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  if (!pb_decode(&s, proxyapi_BluetoothGATTWriteRequest_fields, &req)) return true;

  auto *client = static_cast<NimBLEClient *>(
      ble_backend::connection::client_for(req.address));
  if (client == nullptr) {
    send_gatt_error(req.address, req.handle, -1);
    return true;
  }
  auto *chr = char_at(client, req.handle);
  if (chr == nullptr) {
    send_gatt_error(req.address, req.handle, -2);
    return true;
  }
  bool ok = chr->writeValue(req.data.bytes, req.data.size, req.response);
  if (!ok) {
    send_gatt_error(req.address, req.handle, -3);
    return true;
  }
  proxyapi_BluetoothGATTWriteResponse rsp =
      proxyapi_BluetoothGATTWriteResponse_init_zero;
  rsp.address = req.address;
  rsp.handle = req.handle;
  api_server::stats::record_write();
  api_server::send_async(proxyapi::MSG_BLUETOOTH_GATT_WRITE_RESPONSE,
                         &encode_write_response, &rsp);
  return true;
}

bool handle_write_desc(const uint8_t *payload, size_t payload_len) {
  proxyapi_BluetoothGATTWriteDescriptorRequest req =
      proxyapi_BluetoothGATTWriteDescriptorRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  if (!pb_decode(&s, proxyapi_BluetoothGATTWriteDescriptorRequest_fields,
                 &req)) {
    return true;
  }
  auto *client = static_cast<NimBLEClient *>(
      ble_backend::connection::client_for(req.address));
  if (client == nullptr) {
    send_gatt_error(req.address, req.handle, -1);
    return true;
  }
  auto *dsc = desc_at(client, req.handle);
  if (dsc == nullptr) {
    send_gatt_error(req.address, req.handle, -2);
    return true;
  }
  bool ok = dsc->writeValue(req.data.bytes, req.data.size,
                            /*response=*/true);
  if (!ok) {
    send_gatt_error(req.address, req.handle, -3);
    return true;
  }
  proxyapi_BluetoothGATTWriteResponse rsp =
      proxyapi_BluetoothGATTWriteResponse_init_zero;
  rsp.address = req.address;
  rsp.handle = req.handle;
  api_server::stats::record_write();
  api_server::send_async(proxyapi::MSG_BLUETOOTH_GATT_WRITE_RESPONSE,
                         &encode_write_response, &rsp);
  return true;
}

bool handle_notify(const uint8_t *payload, size_t payload_len) {
  proxyapi_BluetoothGATTNotifyRequest req =
      proxyapi_BluetoothGATTNotifyRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(payload, payload_len);
  if (!pb_decode(&s, proxyapi_BluetoothGATTNotifyRequest_fields, &req)) {
    return true;
  }
  auto *client = static_cast<NimBLEClient *>(
      ble_backend::connection::client_for(req.address));
  if (client == nullptr) {
    send_gatt_error(req.address, req.handle, -1);
    return true;
  }
  auto *chr = char_at(client, req.handle);
  if (chr == nullptr) {
    send_gatt_error(req.address, req.handle, -2);
    return true;
  }
  bool ok = req.enable ? chr->subscribe(/*notifications=*/true, &notify_cb,
                                        /*response=*/false)
                       : chr->unsubscribe(/*response=*/false);
  if (!ok) {
    send_gatt_error(req.address, req.handle, -3);
    return true;
  }
  proxyapi_BluetoothGATTNotifyResponse rsp =
      proxyapi_BluetoothGATTNotifyResponse_init_zero;
  rsp.address = req.address;
  rsp.handle = req.handle;
  api_server::send_async(proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_RESPONSE,
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
      return handle_subscribe_adv(request_payload, request_len);
    case proxyapi::MSG_UNSUBSCRIBE_BLE_ADVERTISEMENTS_REQUEST:
      return handle_unsubscribe_adv();
    case proxyapi::MSG_SUBSCRIBE_BLUETOOTH_CONNECTIONS_FREE_REQUEST:
      return handle_subscribe_free(ctx);
    case proxyapi::MSG_BLUETOOTH_DEVICE_REQUEST:
      return handle_device_request(request_payload, request_len, ctx);
    case proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_REQUEST:
      return handle_get_services(request_payload, request_len);
    case proxyapi::MSG_BLUETOOTH_GATT_READ_REQUEST:
    case proxyapi::MSG_BLUETOOTH_GATT_READ_DESCRIPTOR_REQUEST:
      return handle_read(request_type, request_payload, request_len);
    case proxyapi::MSG_BLUETOOTH_GATT_WRITE_REQUEST:
      return handle_write_char(request_payload, request_len);
    case proxyapi::MSG_BLUETOOTH_GATT_WRITE_DESCRIPTOR_REQUEST:
      return handle_write_desc(request_payload, request_len);
    case proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_REQUEST:
      return handle_notify(request_payload, request_len);
    default:
      return false;
  }
}

void on_last_client_disconnect() {
  g_sub_adv_count = 0;
  g_sub_free_count = 0;
  ble_backend::on_api_client_disconnect();
}

}  // namespace api_server::bt_handlers
