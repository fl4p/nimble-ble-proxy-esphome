#include "wifi_sta.h"

#include "proxy_config.h"

#if __has_include("wifi_creds.h")
#include "wifi_creds.h"
#else
#error \
    "Missing wifi_creds.h. Copy include/wifi_creds.h.example to " \
    "include/wifi_creds.h and fill in WIFI_SSID / WIFI_PASSWORD."
#endif

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <cstring>

namespace wifi_sta {

namespace {

constexpr const char *TAG = "wifi";
constexpr int BIT_GOT_IP = BIT0;

EventGroupHandle_t g_events = nullptr;

void on_wifi_event(void * /*arg*/, esp_event_base_t base, int32_t id,
                   void * /*data*/) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGW(TAG, "disconnected; retrying");
    esp_wifi_connect();
  }
}

void on_ip_event(void * /*arg*/, esp_event_base_t base, int32_t id,
                 void *data) {
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto *evt = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&evt->ip_info.ip));
    xEventGroupSetBits(g_events, BIT_GOT_IP);
  }
}

}  // namespace

void start_and_wait_for_ip() {
  g_events = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_t *netif = esp_netif_create_default_wifi_sta();
  esp_netif_set_hostname(netif, proxy::HOSTNAME);

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, nullptr, nullptr));

  wifi_config_t wc = {};
  std::strncpy(reinterpret_cast<char *>(wc.sta.ssid), WIFI_SSID,
               sizeof(wc.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char *>(wc.sta.password), WIFI_PASSWORD,
               sizeof(wc.sta.password) - 1);
  wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "connecting to SSID '%s'…", WIFI_SSID);
  xEventGroupWaitBits(g_events, BIT_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
}

}  // namespace wifi_sta
