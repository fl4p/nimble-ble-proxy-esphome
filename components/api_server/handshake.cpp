#include "handshake.h"

#include "api_proto.h"
#include "proxy_config.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <cstdio>
#include <cstring>

namespace api_server::handshake {

namespace {

constexpr const char *TAG = "api.handshake";

// Format the active MAC of the given type into "XX:XX:XX:XX:XX:XX".
// Caller passes the destination + its actual capacity; snprintf truncates
// at cap-1 chars and always null-terminates. Caller's array must be ≥18.
bool format_mac(esp_mac_type_t type, char *buf, size_t cap) {
  uint8_t mac[6];
  if (esp_read_mac(mac, type) != ESP_OK) return false;
  std::snprintf(buf, cap, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
  return true;
}

// Helper to encode a nanopb message into response_buf.
template <typename T>
size_t encode_into(uint8_t *buf, size_t cap, const pb_msgdesc_t *desc, const T *msg) {
  pb_ostream_t stream = pb_ostream_from_buffer(buf, cap);
  if (!pb_encode(&stream, desc, msg)) {
    ESP_LOGE(TAG, "pb_encode failed: %s", PB_GET_ERROR(&stream));
    return 0;
  }
  return stream.bytes_written;
}

Result hello_response(uint8_t *buf, size_t cap) {
  proxyapi_HelloResponse resp = proxyapi_HelloResponse_init_zero;
  resp.api_version_major = proxy::API_VERSION_MAJOR;
  resp.api_version_minor = proxy::API_VERSION_MINOR;
  std::snprintf(resp.server_info, sizeof(resp.server_info), "nimble-ble-proxy %s",
                proxy::VERSION);
  std::snprintf(resp.name, sizeof(resp.name), "%s", proxy::hostname());
  size_t n = encode_into(buf, cap, proxyapi_HelloResponse_fields, &resp);
  return {true, proxyapi::MSG_HELLO_RESPONSE, n, false};
}

Result connect_response(uint8_t *buf, size_t cap) {
  // Password auth is deprecated in ESPHome 2026.1; always accept.
  proxyapi_ConnectResponse resp = proxyapi_ConnectResponse_init_zero;
  resp.invalid_password = false;
  size_t n = encode_into(buf, cap, proxyapi_ConnectResponse_fields, &resp);
  return {true, proxyapi::MSG_CONNECT_RESPONSE, n, false};
}

Result device_info_response(uint8_t *buf, size_t cap) {
  proxyapi_DeviceInfoResponse resp = proxyapi_DeviceInfoResponse_init_zero;
  resp.uses_password = false;
  std::snprintf(resp.name, sizeof(resp.name), "%s", proxy::hostname());
  format_mac(ESP_MAC_WIFI_STA, resp.mac_address, sizeof(resp.mac_address));
  format_mac(ESP_MAC_BT, resp.bluetooth_mac_address,
             sizeof(resp.bluetooth_mac_address));
  std::snprintf(resp.esphome_version, sizeof(resp.esphome_version), "%s",
                proxy::FAKE_ESPHOME_VERSION);
  std::snprintf(resp.compilation_time, sizeof(resp.compilation_time), "%s %s",
                __DATE__, __TIME__);
  std::snprintf(resp.model, sizeof(resp.model), "%s", proxy::MODEL);
  std::snprintf(resp.manufacturer, sizeof(resp.manufacturer), "%s",
                proxy::MANUFACTURER);
  std::snprintf(resp.friendly_name, sizeof(resp.friendly_name), "%s",
                proxy::FRIENDLY_NAME);
#if CONFIG_NBP_BLE
  resp.bluetooth_proxy_feature_flags = proxy::BT_PROXY_FEATURE_FLAGS;
#else
  // BLE compiled out — don't advertise BluetoothProxy capability so HA
  // doesn't try to drive a Bluetooth* protocol this build can't serve.
  resp.bluetooth_proxy_feature_flags = 0;
#endif
  size_t n = encode_into(buf, cap, proxyapi_DeviceInfoResponse_fields, &resp);
  return {true, proxyapi::MSG_DEVICE_INFO_RESPONSE, n, false};
}

}  // namespace

Result handle(uint16_t request_type, const uint8_t * /*request_payload*/,
              size_t /*request_len*/, uint8_t *response_buf, size_t response_cap) {
  switch (request_type) {
    case proxyapi::MSG_HELLO_REQUEST:
      return hello_response(response_buf, response_cap);
    case proxyapi::MSG_CONNECT_REQUEST:
      return connect_response(response_buf, response_cap);
    case proxyapi::MSG_DEVICE_INFO_REQUEST:
      return device_info_response(response_buf, response_cap);
    case proxyapi::MSG_PING_REQUEST:
      // Empty PingResponse — payload_len = 0.
      return {true, proxyapi::MSG_PING_RESPONSE, 0, false};
    case proxyapi::MSG_LIST_ENTITIES_REQUEST:
      return {true, proxyapi::MSG_LIST_ENTITIES_DONE_RESPONSE, 0, false};
    case proxyapi::MSG_DISCONNECT_REQUEST:
      return {true, proxyapi::MSG_DISCONNECT_RESPONSE, 0, true};
    case proxyapi::MSG_SUBSCRIBE_LOGS_REQUEST:
      // Accept and never send SubscribeLogsResponse — we don't proxy logs.
      return {true, 0, 0, false};
    default:
      return {false, 0, 0, false};
  }
}

}  // namespace api_server::handshake
