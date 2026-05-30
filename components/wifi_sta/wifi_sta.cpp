#include "wifi_sta.h"

#include "proxy_config.h"

// wifi_creds.h is required for a fixed-credential STA build. With
// NBP_AP_FALLBACK=y, runtime provisioning via the SoftAP page replaces
// the header; an empty WIFI_SSID just means "go straight to AP" on
// first boot.
#if __has_include("wifi_creds.h")
#include "wifi_creds.h"
#elif CONFIG_NBP_AP_FALLBACK
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#else
#error \
    "Missing wifi_creds.h. Copy include/wifi_creds.h.example to " \
    "include/wifi_creds.h and fill in WIFI_SSID / WIFI_PASSWORD, " \
    "or enable NBP_AP_FALLBACK for runtime provisioning."
#endif

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

#if CONFIG_NBP_AP_FALLBACK
#include "esp_http_server.h"
#endif

#include <atomic>
#include <cstdio>
#include <cstring>

namespace wifi_sta {

namespace {

constexpr const char *TAG = "wifi";
constexpr int BIT_GOT_IP = BIT0;

// Live STA connectivity, tracked off the WiFi/IP events. Distinct from
// BIT_GOT_IP (which latches once at first IP for the boot wait and is
// never cleared on disconnect). Read via is_connected().
std::atomic<bool> g_sta_connected{false};

// Mirrored from api_server/stats.cpp — both ends read/write the same
// key. Kept duplicated here to avoid pulling the whole api_server
// dependency tree into wifi_sta just for one int8.
constexpr const char *NVS_NS = "stats";
constexpr const char *NVS_WIFI_LI_KEY = "wifi_li";
// 0 = WIFI_PS_NONE (power-save OFF). Default off: WIFI_PS_MAX_MODEM +
// esp_pm light sleep made the device intermittently unreachable and
// killed USB-Serial/JTAG console output in steady state. Opt back in
// per-device via POST /wifips?li=N (1..10) if you need the power saving.
constexpr int DEFAULT_WIFI_LI = 0;

// Credentials namespace (separate from "stats" so the provisioning
// flow doesn't intermix with telemetry tunables).
constexpr const char *NVS_WIFI_NS = "wifi";
constexpr const char *NVS_WIFI_SSID = "ssid";
constexpr const char *NVS_WIFI_PSK = "psk";

EventGroupHandle_t g_events = nullptr;

int read_listen_interval_from_nvs() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return DEFAULT_WIFI_LI;
  int8_t v = DEFAULT_WIFI_LI;
  esp_err_t err = nvs_get_i8(h, NVS_WIFI_LI_KEY, &v);
  nvs_close(h);
  if (err != ESP_OK) return DEFAULT_WIFI_LI;
  if (v < 0 || v > 10) return DEFAULT_WIFI_LI;
  return v;
}

// Returns true if NVS held a usable SSID. PSK may be empty (open AP).
// ssid_out / psk_out are NUL-terminated within their buffer caps.
bool read_creds_from_nvs(char *ssid_out, size_t ssid_cap,
                         char *psk_out, size_t psk_cap) {
  ssid_out[0] = '\0';
  psk_out[0] = '\0';
  nvs_handle_t h;
  if (nvs_open(NVS_WIFI_NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t n = ssid_cap;
  esp_err_t err = nvs_get_str(h, NVS_WIFI_SSID, ssid_out, &n);
  if (err != ESP_OK || ssid_out[0] == '\0') {
    nvs_close(h);
    return false;
  }
  n = psk_cap;
  nvs_get_str(h, NVS_WIFI_PSK, psk_out, &n);  // may be absent → empty
  nvs_close(h);
  return true;
}

void on_wifi_event(void * /*arg*/, esp_event_base_t base, int32_t id,
                   void * /*data*/) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    g_sta_connected.store(false, std::memory_order_relaxed);
    ESP_LOGW(TAG, "disconnected; retrying");
    esp_wifi_connect();
  }
}

void on_ip_event(void * /*arg*/, esp_event_base_t base, int32_t id,
                 void *data) {
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto *evt = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&evt->ip_info.ip));
    g_sta_connected.store(true, std::memory_order_relaxed);
    xEventGroupSetBits(g_events, BIT_GOT_IP);
  }
}

#if CONFIG_NBP_AP_FALLBACK
// ──────────────────────────────────────────────────────────────────
// SoftAP provisioning fallback
// ──────────────────────────────────────────────────────────────────
// When the STA can't associate within CONFIG_NBP_AP_FALLBACK_SECS we
// switch the radio into SoftAP mode, spin up a tiny httpd on
// 192.168.4.1, and let the user POST new credentials to /wifi. Once
// the creds land in NVS we reboot — next start_and_wait_for_ip() will
// pick them up.
//
// No other subsystem starts while we're in this mode; main.cpp blocks
// inside ap_fallback_loop() until reboot. Captive-portal style is not
// implemented (no DNS redirection) — the user has to know the IP, but
// most phones will offer to open the AP's "config" page automatically.

constexpr const char *AP_TAG = "wifi-ap";
constexpr const char *AP_PSK = "nimble-proxy";  // WPA2-PSK; 8+ chars req
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONN = 2;

// Single page with a form + plain instructions. Kept small enough to
// fit in one TCP segment so old phones render it before timing out.
const char *AP_PAGE_HTML =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>nimble-proxy setup</title>"
    "<style>body{font:14px system-ui;max-width:480px;margin:2em auto;"
    "padding:0 1em;color:#222}input,button{font:inherit;padding:.5em;"
    "width:100%;box-sizing:border-box;margin:.3em 0}button{background:"
    "#2563eb;color:#fff;border:0;border-radius:4px}</style>"
    "<h2>nimble-proxy setup</h2>"
    "<p>Submit your WiFi credentials. The device will save them and "
    "reboot to join your network.</p>"
    "<form method=POST action=/wifi>"
    "<label>SSID<input name=ssid autocomplete=off required></label>"
    "<label>Password<input name=psk type=password autocomplete=off></label>"
    "<button type=submit>Save and reboot</button>"
    "</form>";

// Naive form-urlencoded value extractor: finds `key=` in `src`, copies
// the value (up to '&' or end) into `out` with simple percent-decoding
// for '+' → space and "%hh". Returns true if key was found.
bool form_get(const char *src, const char *key, char *out, size_t cap) {
  out[0] = '\0';
  size_t klen = std::strlen(key);
  const char *p = src;
  while (*p) {
    const char *eq = std::strchr(p, '=');
    if (!eq) break;
    bool match = (static_cast<size_t>(eq - p) == klen) &&
                 std::strncmp(p, key, klen) == 0;
    const char *amp = std::strchr(eq + 1, '&');
    const char *end = amp ? amp : (eq + 1 + std::strlen(eq + 1));
    if (match) {
      size_t i = 0;
      for (const char *q = eq + 1; q < end && i + 1 < cap; ++q) {
        if (*q == '+') {
          out[i++] = ' ';
        } else if (*q == '%' && q + 2 < end) {
          auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + c - 'a';
            if (c >= 'A' && c <= 'F') return 10 + c - 'A';
            return -1;
          };
          int hi = hex(q[1]), lo = hex(q[2]);
          if (hi >= 0 && lo >= 0) {
            out[i++] = static_cast<char>((hi << 4) | lo);
            q += 2;
          } else {
            out[i++] = *q;
          }
        } else {
          out[i++] = *q;
        }
      }
      out[i] = '\0';
      return true;
    }
    if (!amp) break;
    p = amp + 1;
  }
  return false;
}

esp_err_t ap_root_get(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, AP_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t ap_wifi_post(httpd_req_t *req) {
  char buf[256];
  int total = 0;
  while (total < static_cast<int>(sizeof(buf)) - 1) {
    int n = httpd_req_recv(req, buf + total, sizeof(buf) - 1 - total);
    if (n <= 0) {
      if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
      break;
    }
    total += n;
  }
  buf[total] = '\0';

  char ssid[33] = {0};
  char psk[65] = {0};
  if (!form_get(buf, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
    return ESP_FAIL;
  }
  form_get(buf, "psk", psk, sizeof(psk));  // may be empty (open AP)

  nvs_handle_t h;
  if (nvs_open(NVS_WIFI_NS, NVS_READWRITE, &h) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs open");
    return ESP_FAIL;
  }
  nvs_set_str(h, NVS_WIFI_SSID, ssid);
  nvs_set_str(h, NVS_WIFI_PSK, psk);
  nvs_commit(h);
  nvs_close(h);

  ESP_LOGI(AP_TAG, "creds saved (ssid='%s', psk len=%u); rebooting",
           ssid, static_cast<unsigned>(std::strlen(psk)));

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr(req,
                     "<!doctype html><meta charset=utf-8>"
                     "<title>saved</title><body style='font:14px system-ui'>"
                     "<h2>Saved — rebooting</h2>"
                     "<p>Reconnect to your normal WiFi and look for "
                     "<code>nimble-proxy.local</code> in a few seconds.</p>");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

// Compose a per-device SSID suffix from the WiFi MAC so two devices on
// the same bench don't collide. Result like "nimble-proxy-setup-AB12".
void compose_ap_ssid(char *out, size_t cap) {
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_AP, mac);
  std::snprintf(out, cap, "%s-setup-%02X%02X", proxy::hostname(),
                mac[4], mac[5]);
}

// Switches the radio from STA to AP, spins up the captive httpd, and
// blocks forever. Callers don't return — esp_restart() in ap_wifi_post
// is the only exit.
[[noreturn]] void enter_ap_provisioning() {
  ESP_LOGW(AP_TAG, "entering SoftAP provisioning mode");

  // Tear down the half-running STA before re-binding netif as AP.
  // create_default_wifi_ap registers the AP netif (192.168.4.1/24 +
  // built-in DHCP server) and its event handlers; do it before
  // set_mode so the wifi driver sees both netifs at mode-switch time.
  esp_wifi_stop();
  esp_netif_create_default_wifi_ap();
  esp_wifi_set_mode(WIFI_MODE_AP);

  wifi_config_t ap = {};
  char ssid[33];
  compose_ap_ssid(ssid, sizeof(ssid));
  std::strncpy(reinterpret_cast<char *>(ap.ap.ssid), ssid,
               sizeof(ap.ap.ssid) - 1);
  ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
  std::strncpy(reinterpret_cast<char *>(ap.ap.password), AP_PSK,
               sizeof(ap.ap.password) - 1);
  ap.ap.channel = AP_CHANNEL;
  ap.ap.max_connection = AP_MAX_CONN;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  ESP_ERROR_CHECK(esp_wifi_start());

  httpd_handle_t srv = nullptr;
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 4096;
  cfg.max_uri_handlers = 4;
  cfg.max_open_sockets = 3;
  if (httpd_start(&srv, &cfg) != ESP_OK) {
    ESP_LOGE(AP_TAG, "httpd_start failed in AP mode");
    // No recovery path — sleep so the device is still discoverable on
    // the AP for diagnostics.
    while (true) vTaskDelay(portMAX_DELAY);
  }

  httpd_uri_t root = {.uri = "/", .method = HTTP_GET,
                      .handler = &ap_root_get, .user_ctx = nullptr};
  httpd_uri_t post = {.uri = "/wifi", .method = HTTP_POST,
                      .handler = &ap_wifi_post, .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &root);
  httpd_register_uri_handler(srv, &post);

  ESP_LOGW(AP_TAG, "SoftAP up: SSID='%s' PSK='%s' — http://192.168.4.1/",
           ssid, AP_PSK);

  // Block forever; ap_wifi_post reboots the device on success.
  while (true) vTaskDelay(portMAX_DELAY);
}
#endif  // CONFIG_NBP_AP_FALLBACK

}  // namespace

void start_and_wait_for_ip() {
  g_events = xEventGroupCreate();

  // Silence the WiFi driver's per-association state-machine churn and the
  // SoftAP PMF SA-Query/disassoc flood — all I-level under tag "wifi" — plus
  // the DHCP-server "assigned IP" info lines (tag "esp_netif_lwip"). Under
  // coex-induced AP-client flapping these dominate both the serial console
  // and the /log web console. WARN keeps genuine WiFi warnings/errors (and
  // our own "disconnected; retrying") visible. esp_log_level_set is global
  // per-tag, so this one call also covers the AP side started by nat_router.
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);

  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_t *netif = esp_netif_create_default_wifi_sta();
  esp_netif_set_hostname(netif, proxy::hostname());

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, nullptr, nullptr));

  // Credentials: NVS overrides compile-time defaults from wifi_creds.h.
  // With NBP_AP_FALLBACK=y the defaults may be empty strings — that's
  // the "no creds set yet, go straight to AP" path below.
  char ssid[33] = {0};
  char psk[65] = {0};
  if (!read_creds_from_nvs(ssid, sizeof(ssid), psk, sizeof(psk))) {
    std::strncpy(ssid, WIFI_SSID, sizeof(ssid) - 1);
    std::strncpy(psk, WIFI_PASSWORD, sizeof(psk) - 1);
  }

#if CONFIG_NBP_AP_FALLBACK
  if (ssid[0] == '\0') {
    ESP_LOGW(TAG, "no SSID configured; entering AP provisioning");
    enter_ap_provisioning();  // [[noreturn]]
  }
#endif

  wifi_config_t wc = {};
  std::strncpy(reinterpret_cast<char *>(wc.sta.ssid), ssid,
               sizeof(wc.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char *>(wc.sta.password), psk,
               sizeof(wc.sta.password) - 1);
  wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  // Power-save: persisted listen_interval (DTIM beacons between RX
  // wakeups). 0 means PS_NONE (radio always on, lowest RX latency,
  // highest energy). 1..N means PS_MAX_MODEM with N×DTIM sleeps —
  // bigger N = more energy savings but more downstream RX latency.
  // Listen-interval must be set BEFORE esp_wifi_start so it's part of
  // the initial association; set_ps mode flips after start.
  const int li = read_listen_interval_from_nvs();
  wc.sta.listen_interval = static_cast<uint16_t>(li > 0 ? li : 0);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_ps(li > 0 ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE));
  ESP_LOGI(TAG, "wifi PS: %s (listen_interval=%d)",
           li > 0 ? "MAX_MODEM" : "NONE", li);

  ESP_LOGI(TAG, "connecting to SSID '%s'…", ssid);

#if CONFIG_NBP_AP_FALLBACK
  // Bounded wait: if the AP is wrong/down, fall into provisioning
  // instead of blocking the rest of boot forever.
  TickType_t to = pdMS_TO_TICKS(CONFIG_NBP_AP_FALLBACK_SECS * 1000);
  EventBits_t bits = xEventGroupWaitBits(g_events, BIT_GOT_IP,
                                         pdFALSE, pdTRUE, to);
  if ((bits & BIT_GOT_IP) == 0) {
    ESP_LOGW(TAG, "STA timeout after %d s; falling back to SoftAP",
             CONFIG_NBP_AP_FALLBACK_SECS);
    enter_ap_provisioning();  // [[noreturn]]
  }
#else
  xEventGroupWaitBits(g_events, BIT_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
#endif
}

bool is_connected() {
  return g_sta_connected.load(std::memory_order_relaxed);
}

}  // namespace wifi_sta
