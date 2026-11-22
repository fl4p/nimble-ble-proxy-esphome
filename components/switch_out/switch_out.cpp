#include "switch_out.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
enum : uint8_t { kBootOff, kBootOn, kBootLast };
constexpr const char *kBoot[] = {"off", "on", "last"};
constexpr size_t kNameMax = 24;

// The pin comes from NVS "gpio" (set from the dashboard), else from
// CONFIG_NBP_SWITCH_GPIO (-1 = not configured, the default in release
// images). A build with a Kconfig pin writes it to NVS on first boot, so a
// later image without that default (a stock release) keeps the pin.
int g_pin = -1;
char g_name[kNameMax + 1] = "";
bool g_ok = false, g_on = false;
uint8_t g_boot = kBootLast;
uint8_t g_nvs_on = 0xff;  // stored "on", 0xff = none
uint32_t g_changes = 0;
int64_t g_since_us = 0;

// Level into the output register first, then release and re-take the hold:
// the pad goes straight from the old to the new level.
void drive(bool on) {
  const auto pin = static_cast<gpio_num_t>(g_pin);
  gpio_set_level(pin, on);
  gpio_hold_dis(pin);
  gpio_hold_en(pin);
}

void g_claimed_release(int pin) { nbp_gpio::g_claimed &= ~nbp_gpio::bit(pin); }

// Take `pin` as the output at level `on` and hold it. Caller checked
// nbp_gpio::allowed(). Same order as at boot: level first, then output.
void setup_pin(int pin, bool on) {
  const auto p = static_cast<gpio_num_t>(pin);
  nbp_gpio::claim(nbp_gpio::bit(pin));
  g_pin = pin;
  // A held pad ignores this until drive() releases it; an unheld one (power-on)
  // gets the level before the output is enabled.
  gpio_set_level(p, on);
  gpio_config_t io = {};
  io.pin_bit_mask = BIT64(pin);
  io.mode = GPIO_MODE_INPUT_OUTPUT;
  gpio_config(&io);
  // Keep the active config in light sleep (startup isolates all pads there).
  gpio_sleep_sel_dis(p);
  drive(on);
  g_on = on;
  g_ok = true;
}

// Name: printable ASCII without '"' and '\\' (it goes into JSON as is),
// 1..kNameMax chars after %XX / '+' decoding. False if invalid.
bool decode_name(const char *in, char *out) {
  size_t n = 0;
  for (const char *c = in; *c; c++) {
    int ch = static_cast<unsigned char>(*c);
    if (ch == '+') {
      ch = ' ';
    } else if (ch == '%') {
      auto hex = [](char h) {
        return h >= '0' && h <= '9' ? h - '0'
             : h >= 'a' && h <= 'f' ? h - 'a' + 10
             : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
      };
      if (!c[1] || !c[2] || hex(c[1]) < 0 || hex(c[2]) < 0) return false;
      ch = hex(c[1]) * 16 + hex(c[2]);
      c += 2;
    }
    if (ch < 0x20 || ch > 0x7e || ch == '"' || ch == '\\' || n >= kNameMax) return false;
    out[n++] = static_cast<char>(ch);
  }
  out[n] = 0;
  return n > 0;
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
  char buf[176];
  int n = snprintf(
      buf, sizeof(buf),
      "{\"gpio\":%d,\"name\":\"%s\",\"on\":%s,\"boot\":\"%s\",\"changes\":%lu,"
      "\"since_s\":%lu,\"running\":%s}",
      g_pin, g_name, g_on ? "true" : "false", kBoot[g_boot],
      static_cast<unsigned long>(g_changes),
      static_cast<unsigned long>((esp_timer_get_time() - g_since_us) / 1000000),
      g_ok ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

// POST /switch?on=0|1&boot=off|on|last&gpio=N|none&name=TEXT. NVS first,
// then the pin; unchanged values write nothing. A new gpio applies at once:
// the old pin is driven off and stays held off (the safe level), the new one
// starts off. on/boot need a working pin; gpio/name do not.
esp_err_t switch_post(httpd_req_t *req) {
  if (!nbp_http::confirmed(req)) return nbp_http::send_unconfirmed(req);
  if (!sta_client(req))
    return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "STA side only");
  char q[128], v[80], name[kNameMax + 1];
  int on = -1, boot = -1, pin = -2;  // pin -2 = not given
  bool has_name = false;
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
  if (httpd_query_key_value(q, "gpio", v, sizeof(v)) == ESP_OK) {
    char *e = nullptr;
    long p = std::strtol(v, &e, 10);
    if (std::strcmp(v, "none") == 0 || std::strcmp(v, "-1") == 0) {
      pin = -1;
    } else if (e == v || *e != 0 || p < 0 || p > 63 ||
               !nbp_gpio::allowed(static_cast<int>(p), nbp_gpio::bit(g_pin))) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "gpio not allowed (reserved, strap, in use or invalid)");
    } else {
      pin = static_cast<int>(p);
    }
  }
  if (httpd_query_key_value(q, "name", v, sizeof(v)) == ESP_OK) {
    if (!decode_name(v, name))
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "name: 1-24 printable chars, no \" or \\");
    has_name = true;
  }
  if (on < 0 && boot < 0 && pin == -2 && !has_name)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "on=0|1, boot=off|on|last, gpio=N|none or name=TEXT");
  const bool new_pin = pin != -2 && pin != g_pin;
  if ((on >= 0 || boot >= 0) && !new_pin && !g_ok)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               g_pin < 0 ? "switch not configured (set gpio)" : "pin not allowed");
  if (new_pin && on == 1)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "a new gpio starts off; send on=1 separately");
  // A pin change stores on=0: the new pin starts off and "last" must agree.
  if (new_pin) on = 0;
  bool w_on = on >= 0 && on != g_nvs_on, w_boot = boot >= 0 && boot != g_boot;
  bool w_name = has_name && std::strcmp(name, g_name) != 0;
  if (w_on || w_boot || new_pin || w_name) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
      if (w_on) e = nvs_set_u8(h, "on", static_cast<uint8_t>(on));
      if (e == ESP_OK && w_boot) e = nvs_set_u8(h, "boot", static_cast<uint8_t>(boot));
      if (e == ESP_OK && new_pin) e = nvs_set_i8(h, "gpio", static_cast<int8_t>(pin));
      if (e == ESP_OK && w_name) e = nvs_set_str(h, "name", name);
      if (e == ESP_OK) e = nvs_commit(h);
      nvs_close(h);
    }
    if (e != ESP_OK)
      return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write");
    if (w_on) g_nvs_on = static_cast<uint8_t>(on);
    if (w_boot) g_boot = static_cast<uint8_t>(boot);
    if (w_name) std::strcpy(g_name, name);
  }
  if (new_pin) {
    const int old = g_pin;
    if (g_ok) {
      drive(false);  // old pin off, stays held off
      g_claimed_release(old);
      ESP_LOGW(TAG, "GPIO%d switched off, stays held off", old);
    }
    g_ok = false;
    g_on = false;
    g_pin = pin;
    if (pin >= 0) {
      setup_pin(pin, false);
      ESP_LOGW(TAG, "output moved to GPIO%d (off)", pin);
    } else {
      ESP_LOGW(TAG, "output disabled");
    }
    g_changes++;
    g_since_us = esp_timer_get_time();
  } else if (on >= 0 && on != g_on) {
    drive(on);
    g_on = on;
    g_changes++;
    g_since_us = esp_timer_get_time();
    ESP_LOGW(TAG, "GPIO%d %s", g_pin, on ? "on" : "off");
  }
  return switch_get(req);
}

}  // namespace

void early_init() {
  static bool done = false;
  if (done) return;
  done = true;
  int pin = CONFIG_NBP_SWITCH_GPIO;
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    uint8_t u8;
    int8_t i8;
    if (nvs_get_u8(h, "on", &u8) == ESP_OK) g_nvs_on = u8 ? 1 : 0;
    if (nvs_get_u8(h, "boot", &u8) == ESP_OK && u8 <= kBootLast) g_boot = u8;
    size_t len = sizeof(g_name);
    if (nvs_get_str(h, "name", g_name, &len) != ESP_OK) g_name[0] = 0;
    if (nvs_get_i8(h, "gpio", &i8) == ESP_OK) {
      pin = i8;
    } else if (pin >= 0) {
      // Migration: pin from this build's Kconfig, keep it for later images.
      if (nvs_set_i8(h, "gpio", static_cast<int8_t>(pin)) == ESP_OK) nvs_commit(h);
    }
    nvs_close(h);
  }
  g_pin = pin;
  g_since_us = esp_timer_get_time();
  if (pin < 0) {
    ESP_LOGI(TAG, "not configured (POST /switch?gpio=N)");
    return;
  }
  if (!nbp_gpio::allowed(pin)) {
    ESP_LOGE(TAG, "GPIO%d not allowed, switch off", pin);
    return;
  }
  setup_pin(pin, g_boot == kBootLast ? g_nvs_on == 1 : g_boot == kBootOn);
  ESP_LOGI(TAG, "GPIO%d %s (boot %s)", pin, g_on ? "on" : "off", kBoot[g_boot]);
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
  return std::snprintf(buf, cap, ",\"sw_on\":%d,\"sw_changes\":%lu,\"sw_cfg\":%d",
                       g_on ? 1 : 0, static_cast<unsigned long>(g_changes),
                       g_ok ? 1 : 0);
}

}  // namespace switch_out
