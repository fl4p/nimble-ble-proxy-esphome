#include "improv_ble.h"

#include "NimBLEAdvertising.h"
#include "NimBLECharacteristic.h"
#include "NimBLEDevice.h"
#include "NimBLEServer.h"
#include "NimBLEService.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "improv_proto.h"
#include "proxy_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace improv_ble {

namespace {

constexpr const char *TAG = "improv";

constexpr const char *SVC_UUID = "00467768-6228-2272-4663-277478268000";
constexpr const char *STATE_UUID = "00467768-6228-2272-4663-277478268001";
constexpr const char *ERROR_UUID = "00467768-6228-2272-4663-277478268002";
constexpr const char *RPC_UUID = "00467768-6228-2272-4663-277478268003";
constexpr const char *RESULT_UUID = "00467768-6228-2272-4663-277478268004";
constexpr const char *CAPS_UUID = "00467768-6228-2272-4663-277478268005";
constexpr uint16_t SVC_DATA_UUID = 0x4677;
// No identify: there is no LED or buzzer this firmware drives.
constexpr uint8_t CAPS = improv::CAP_DEVICE_INFO | improv::CAP_SCAN;

// Guards everything below. Taken by the NimBLE host task (RPC writes,
// disconnects) and the provisioning loop (join/scan outcomes). The mutex
// and the assembler live on the heap: only provisioning needs them.
SemaphoreHandle_t g_mux = nullptr;
improv::Assembler *g_asm = nullptr;
NimBLECharacteristic *g_state_chr = nullptr;
NimBLECharacteristic *g_error_chr = nullptr;
NimBLECharacteristic *g_result_chr = nullptr;
uint8_t g_state = improv::STATE_AUTHORIZED;
// The connection that sent the pending RPC, and its ATT MTU.
uint16_t g_rpc_conn = BLE_HS_CONN_HANDLE_NONE;
uint16_t g_rpc_mtu = BLE_ATT_MTU_DFLT;

struct Lock {
  Lock() { xSemaphoreTake(g_mux, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(g_mux); }
};

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Service UUID + service data 0x4677 fill the 31-byte legacy advert
// exactly (flags 3 + UUID 18 + data 10); the name goes in the scan response.
void update_adv() {
  NimBLEAdvertisementData ad;
  ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  ad.addServiceUUID(NimBLEUUID(SVC_UUID));
  uint8_t sd[6];
  improv::service_data(g_state, CAPS, sd);
  ad.setServiceData(NimBLEUUID(SVC_DATA_UUID), sd, sizeof(sd));
  NimBLEDevice::getAdvertising()->setAdvertisementData(ad);
}

void set_error(uint8_t e) {
  g_error_chr->setValue(&e, 1);
  g_error_chr->notify();
}

void set_state(uint8_t s) {
  const bool changed = s != g_state;
  g_state = s;
  g_state_chr->setValue(&s, 1);
  g_state_chr->notify();
  if (changed) update_adv();
}

// A result never exceeds what one notification carries on the requesting
// link: a client parses the notification as is, so a cut packet would
// fail its checksum.
size_t result_cap() {
  const size_t att = g_rpc_mtu > 3 ? g_rpc_mtu - 3u : 20u;
  return att < improv::MAX_PACKET ? att : improv::MAX_PACKET;
}

void set_result(const uint8_t *pkt, size_t n) {
  g_result_chr->setValue(pkt, n);
  g_result_chr->notify(g_rpc_conn);
}

void send_device_info() {
  uint8_t buf[improv::MAX_PACKET];
  improv::Result r(improv::CMD_DEVICE_INFO, buf, result_cap());
  if (!r.add("nimble-ble-proxy") || !r.add(esp_app_get_description()->version) ||
      !r.add(CONFIG_IDF_TARGET) || !r.add(proxy::hostname())) {
    ESP_LOGW(TAG, "device info does not fit MTU %u", g_rpc_mtu);
    set_error(improv::ERR_UNKNOWN);
    return;
  }
  set_result(buf, r.finish());
}

const char *auth_name(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "NO";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2 EAP";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    default: return "WPA2";
  }
}

void handle_rpc(const uint8_t *data, size_t len, uint16_t conn, uint16_t mtu) {
  Lock l;
  uint8_t pkt[improv::MAX_PACKET];
  const int n = g_asm->feed(data, len, now_ms(), pkt);
  if (n == 0) return;
  if (n < 0) {
    ESP_LOGW(TAG, "malformed RPC packet");
    set_error(improv::ERR_INVALID_RPC);
    return;
  }
  g_rpc_conn = conn;
  g_rpc_mtu = mtu;
  // Cleared on every RPC: clients treat a non-zero error as the end of it.
  set_error(improv::ERR_NONE);
  const uint8_t *d = pkt + 2;
  const size_t dn = pkt[1];
  switch (pkt[0]) {
    case improv::CMD_WIFI_SETTINGS: {
      if (g_state != improv::STATE_AUTHORIZED) {
        set_error(improv::ERR_UNKNOWN);  // attempt running or done
        return;
      }
      const uint8_t *ssid, *psk;
      size_t sl, pl;
      const uint8_t e = improv::parse_wifi_settings(d, dn, &ssid, &sl, &psk, &pl);
      if (e != improv::ERR_NONE) {
        set_error(e);
        return;
      }
      // PROVISIONING before the request: the outcome takes this lock, so it
      // cannot overtake.
      set_state(improv::STATE_PROVISIONING);
      const auto r = wifi_sta::request_join(reinterpret_cast<const char *>(ssid),
                                            sl, reinterpret_cast<const char *>(psk),
                                            pl);
      ESP_LOGI(TAG, "wifi settings for a %u-byte SSID: %s",
               static_cast<unsigned>(sl),
               r == wifi_sta::Request::Queued ? "joining" : "refused");
      if (r != wifi_sta::Request::Queued) {
        set_state(improv::STATE_AUTHORIZED);
        set_error(r == wifi_sta::Request::Invalid ? improv::ERR_UNABLE_TO_CONNECT
                                                  : improv::ERR_UNKNOWN);
      }
      return;
    }
    case improv::CMD_DEVICE_INFO:
      send_device_info();
      return;
    case improv::CMD_SCAN:
      if (g_state != improv::STATE_AUTHORIZED ||
          wifi_sta::request_scan() != wifi_sta::Request::Queued) {
        set_error(improv::ERR_UNKNOWN);
      }
      return;
    default:
      set_error(improv::ERR_UNKNOWN_RPC);
      return;
  }
}

void on_join_done(bool ok, const char *url) {
  Lock l;
  if (g_state != improv::STATE_PROVISIONING) return;
  if (!ok) {
    set_state(improv::STATE_AUTHORIZED);
    set_error(improv::ERR_UNABLE_TO_CONNECT);
    return;
  }
  // Result first, then state: the JS client resolves on the result and only
  // ignores the following disconnect once it has seen either.
  uint8_t buf[improv::MAX_PACKET];
  improv::Result r(improv::CMD_WIFI_SETTINGS, buf, result_cap());
  if (url[0] != '\0') r.add(url);  // dropped if it does not fit the MTU
  set_result(buf, r.finish());
  set_state(improv::STATE_PROVISIONED);
}

void on_scan_done(bool ok, const wifi_ap_record_t *aps, uint16_t n) {
  Lock l;
  if (!ok) {
    set_error(improv::ERR_UNKNOWN);
    return;
  }
  uint8_t buf[improv::MAX_PACKET];
  improv::Result r(improv::CMD_SCAN, buf, result_cap());
  for (uint16_t i = 0; i < n; ++i) {
    const char *ssid = reinterpret_cast<const char *>(aps[i].ssid);
    if (ssid[0] == '\0') continue;  // hidden
    bool dup = false;
    for (uint16_t j = 0; j < i && !dup; ++j) {
      dup = std::strcmp(ssid, reinterpret_cast<const char *>(aps[j].ssid)) == 0;
    }
    if (dup) continue;  // records are strongest first
    char rssi[8];
    std::snprintf(rssi, sizeof(rssi), "%d", aps[i].rssi);
    const size_t m = r.mark();
    if (!r.add(ssid) || !r.add(rssi) || !r.add(auth_name(aps[i].authmode))) {
      r.rewind(m);  // whole triples only
      break;
    }
  }
  set_result(buf, r.finish());
}

class RpcCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &ci) override {
    const NimBLEAttValue v = c->getValue();
    handle_rpc(v.data(), v.size(), ci.getConnHandle(), ci.getMTU());
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &ci) override {
    ESP_LOGI(TAG, "client connected (%s)", ci.getAddress().toString().c_str());
    // Results are single notifications; ask for a large MTU from this side
    // too, for clients that never start the exchange themselves.
    ble_gattc_exchange_mtu(ci.getConnHandle(), nullptr, nullptr);
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    ESP_LOGI(TAG, "client disconnected (reason 0x%x)", reason);
    Lock l;
    *g_asm = improv::Assembler();
  }
};

RpcCallbacks g_rpc_cb;
ServerCallbacks g_srv_cb;

void on_entered() {
  void *mem = std::malloc(sizeof(improv::Assembler));
  g_mux = xSemaphoreCreateMutex();
  if (mem == nullptr || g_mux == nullptr) {
    ESP_LOGE(TAG, "no memory; Improv BLE off, SoftAP only");
    return;
  }
  g_asm = new (mem) improv::Assembler();
  if (!NimBLEDevice::init(proxy::hostname())) {
    ESP_LOGE(TAG, "NimBLE init failed; Improv BLE off, SoftAP only");
    return;
  }
  NimBLEDevice::setMTU(247);
  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(&g_srv_cb, false);
  srv->advertiseOnDisconnect(true);

  NimBLEService *svc = srv->createService(SVC_UUID);
  const uint8_t state = g_state, err = improv::ERR_NONE, caps = CAPS;
  g_state_chr = svc->createCharacteristic(
      STATE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 1);
  g_state_chr->setValue(&state, 1);
  g_error_chr = svc->createCharacteristic(
      ERROR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 1);
  g_error_chr->setValue(&err, 1);
  NimBLECharacteristic *rpc = svc->createCharacteristic(
      RPC_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR,
      improv::MAX_PACKET);
  rpc->setCallbacks(&g_rpc_cb);
  g_result_chr = svc->createCharacteristic(
      RESULT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
      improv::MAX_PACKET);
  NimBLECharacteristic *cap_chr =
      svc->createCharacteristic(CAPS_UUID, NIMBLE_PROPERTY::READ, 1);
  cap_chr->setValue(&caps, 1);
  srv->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  {
    Lock l;
    update_adv();
  }
  NimBLEAdvertisementData sr;
  std::string name = proxy::hostname();
  if (name.size() > 29) {
    name.resize(29);
    sr.setShortName(name);
  } else {
    sr.setName(name);
  }
  adv->setScanResponseData(sr);
  adv->enableScanResponse(true);
  // 100-150 ms: found within a second, leaves airtime to the setup AP.
  adv->setMinInterval(160);
  adv->setMaxInterval(240);
  const bool on = adv->start();
  ESP_LOGW(TAG, "Improv BLE provisioning %s as '%s'",
           on ? "advertising" : "FAILED to advertise", proxy::hostname());
}

const wifi_sta::ProvisioningHooks g_hooks = {
    .entered = &on_entered,
    .join_done = &on_join_done,
    .scan_done = &on_scan_done,
};

}  // namespace

const wifi_sta::ProvisioningHooks *hooks() { return &g_hooks; }

}  // namespace improv_ble
