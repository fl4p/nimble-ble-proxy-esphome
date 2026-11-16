#include "switch_out.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "http_confirm.h"
#include "lwip/sockets.h"
#include "nbp_gpio.h"
#include "nvs.h"

namespace switch_out {
namespace {

constexpr const char *TAG = "switch";
constexpr const char *NVS_NS = "switch";
constexpr gpio_num_t kPin = static_cast<gpio_num_t>(CONFIG_NBP_SWITCH_GPIO);
enum : uint8_t { kBootOff, kBootOn, kBootLast };
constexpr const char *kBoot[] = {"off", "on", "last"};

bool g_ok = false, g_on = false;
uint8_t g_boot = kBootLast;
uint8_t g_nvs_on = 0xff;  // stored "on", 0xff = none
uint32_t g_changes = 0;
int64_t g_since_us = 0;

// Level into the output register first, then release and re-take the hold:
// the pad goes straight from the old to the new level.
void drive(bool on) {
  gpio_set_level(kPin, on);
  gpio_hold_dis(kPin);
  gpio_hold_en(kPin);
}

bool v4(const sockaddr_storage &ss, uint32_t &a) {
  if (ss.ss_family == AF_INET) {
    a = reinterpret_cast<const sockaddr_in &>(ss).sin_addr.s_addr;
    return true;
  }
  const auto &s6 = reinterpret_cast<const sockaddr_in6 &>(ss);
  if (ss.ss_family != AF_INET6 || !IN6_IS_ADDR_V4MAPPED(&s6.sin6_addr)) return false;
  a = s6.sin6_addr.un.u32_addr[3];
  return true;
}

bool in_subnet(uint32_t a, const esp_netif_ip_info_t &i) {
  return i.ip.addr != 0 && ((a ^ i.ip.addr) & i.netmask.addr) == 0;
}

// Same rule as the modbus bridge: request arrived on the STA address from
// the STA subnet, never from a SoftAP client. IPv6 peers are refused.
bool sta_client(httpd_req_t *req) {
  int fd = httpd_req_to_sockfd(req);
  sockaddr_storage l = {}, p = {};
  socklen_t ll = sizeof(l), pl = sizeof(p);
  uint32_t la, pa;
  if (fd < 0 || getsockname(fd, reinterpret_cast<sockaddr *>(&l), &ll) != 0 ||
      getpeername(fd, reinterpret_cast<sockaddr *>(&p), &pl) != 0 || !v4(l, la) ||
      !v4(p, pa))
    return false;
  esp_netif_ip_info_t si = {}, ai = {};
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta == nullptr || esp_netif_get_ip_info(sta, &si) != ESP_OK || si.ip.addr == 0 ||
      la != si.ip.addr)
    return false;
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap != nullptr && esp_netif_get_ip_info(ap, &ai) == ESP_OK && in_subnet(pa, ai))
    return false;
  return in_subnet(pa, si);
}

esp_err_t switch_get(httpd_req_t *req) {
  char buf[112];
  int n = snprintf(
      buf, sizeof(buf),
      "{\"gpio\":%d,\"on\":%s,\"boot\":\"%s\",\"changes\":%lu,\"since_s\":%lu,"
      "\"running\":%s}",
      CONFIG_NBP_SWITCH_GPIO, g_on ? "true" : "false", kBoot[g_boot],
      static_cast<unsigned long>(g_changes),
      static_cast<unsigned long>((esp_timer_get_time() - g_since_us) / 1000000),
      g_ok ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

// POST /switch?on=0|1&boot=off|on|last. NVS first, then the pin; unchanged
// values write nothing.
esp_err_t switch_post(httpd_req_t *req) {
  if (!nbp_http::confirmed(req)) return nbp_http::send_unconfirmed(req);
  if (!sta_client(req))
    return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "STA side only");
  if (!g_ok)
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "pin not allowed");
  char q[32], v[8];
  int on = -1, boot = -1;
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) q[0] = 0;
  if (httpd_query_key_value(q, "on", v, sizeof(v)) == ESP_OK) {
    if (std::strcmp(v, "0") != 0 && std::strcmp(v, "1") != 0)
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "on=0|1");
    on = v[0] - '0';
  }
  if (httpd_query_key_value(q, "boot", v, sizeof(v)) == ESP_OK) {
    for (int i = 0; i < 3; i++)
      if (std::strcmp(v, kBoot[i]) == 0) boot = i;
    if (boot < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "boot=off|on|last");
  }
  if (on < 0 && boot < 0)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "on=0|1 or boot=off|on|last");
  bool w_on = on >= 0 && on != g_nvs_on, w_boot = boot >= 0 && boot != g_boot;
  if (w_on || w_boot) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
      if (w_on) e = nvs_set_u8(h, "on", static_cast<uint8_t>(on));
      if (e == ESP_OK && w_boot) e = nvs_set_u8(h, "boot", static_cast<uint8_t>(boot));
      if (e == ESP_OK) e = nvs_commit(h);
      nvs_close(h);
    }
    if (e != ESP_OK)
      return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write");
    if (w_on) g_nvs_on = static_cast<uint8_t>(on);
    if (w_boot) g_boot = static_cast<uint8_t>(boot);
  }
  if (on >= 0 && on != g_on) {
    drive(on);
    g_on = on;
    g_changes++;
    g_since_us = esp_timer_get_time();
    ESP_LOGW(TAG, "GPIO%d %s", CONFIG_NBP_SWITCH_GPIO, on ? "on" : "off");
  }
  return switch_get(req);
}

}  // namespace

void early_init() {
  static bool done = false;
  if (done) return;
  done = true;
  if (!nbp_gpio::allowed(kPin)) {
    ESP_LOGE(TAG, "GPIO%d not allowed, switch off", CONFIG_NBP_SWITCH_GPIO);
    return;
  }
  nbp_gpio::claim(nbp_gpio::bit(kPin));
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    uint8_t u8;
    if (nvs_get_u8(h, "on", &u8) == ESP_OK) g_nvs_on = u8 ? 1 : 0;
    if (nvs_get_u8(h, "boot", &u8) == ESP_OK && u8 <= kBootLast) g_boot = u8;
    nvs_close(h);
  }
  g_on = g_boot == kBootLast ? g_nvs_on == 1 : g_boot == kBootOn;
  // A held pad ignores this until drive() releases it; an unheld one (power-on)
  // gets the level before the output is enabled.
  gpio_set_level(kPin, g_on);
  gpio_config_t io = {};
  io.pin_bit_mask = BIT64(kPin);
  io.mode = GPIO_MODE_INPUT_OUTPUT;
  gpio_config(&io);
  // Keep the active config in light sleep (startup isolates all pads there).
  gpio_sleep_sel_dis(kPin);
  drive(g_on);
  g_ok = true;
  g_since_us = esp_timer_get_time();
  ESP_LOGI(TAG, "GPIO%d %s (boot %s)", CONFIG_NBP_SWITCH_GPIO, g_on ? "on" : "off",
           kBoot[g_boot]);
}

void register_endpoints(void *httpd_handle) {
  if (httpd_handle == nullptr) return;
  auto srv = static_cast<httpd_handle_t>(httpd_handle);
  httpd_uri_t u = {};
  u.uri = "/switch";
  u.method = HTTP_GET;
  u.handler = &switch_get;
  httpd_register_uri_handler(srv, &u);
  u.method = HTTP_POST;
  u.handler = &switch_post;
  httpd_register_uri_handler(srv, &u);
}

int append_stats_json(char *buf, size_t cap) {
  return std::snprintf(buf, cap, ",\"sw_on\":%d,\"sw_changes\":%lu", g_on ? 1 : 0,
                       static_cast<unsigned long>(g_changes));
}

}  // namespace switch_out
