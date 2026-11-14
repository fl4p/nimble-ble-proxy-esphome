#include "wifi_sta.h"

#include "proxy_config.h"

// wifi_creds.h is required for a fixed-credential STA build. With
// NBP_AP_FALLBACK=y, runtime provisioning via the SoftAP page replaces
// the header when no compile-time SSID is available.
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

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

#include <atomic>
#include <cctype>
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

// True while the SoftAP provisioning page is up. The STA event handler
// then stops reconnecting on its own; the provisioning loop retries the
// saved network on a slow, gated cadence instead (see
// enter_ap_provisioning).
std::atomic<bool> g_provisioning{false};

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

// ──────────────────── disconnect bookkeeping ─────────────────────
// Why the STA dropped off the upstream AP, and why SoftAP clients left.
// Logged under its own tag (the "wifi" tag is held at WARN for the driver's
// sake) and kept in a small ring for /wifidisc, because the 4 KB /log ring
// rolls over within minutes on a busy proxy. Updated from the default event
// loop task, read from httpd: guarded by g_disc_mux.
constexpr const char *EV_TAG = "wifi_ev";
constexpr size_t DISC_RING = 8;
constexpr uint8_t KIND_STA = 0;
constexpr uint8_t KIND_AP = 1;

struct DiscEvent {
  uint32_t t_s;      // uptime at the event
  uint32_t down_ms;  // STA: time until the next IP (0 while still down)
  uint8_t mac[6];    // STA: upstream BSSID; AP: the client that left
  uint8_t kind;
  uint8_t reason;    // wifi_err_reason_t (all values fit in a byte)
  int8_t rssi;       // STA: RSSI the driver reported with the event; AP: 0
};

portMUX_TYPE g_disc_mux = portMUX_INITIALIZER_UNLOCKED;
DiscEvent g_disc_ring[DISC_RING] = {};
uint32_t g_disc_total = 0;  // events ever recorded; ring slot = n % DISC_RING
uint32_t g_sta_disc = 0;    // link losses (associated -> not), not retries
uint32_t g_ap_disc = 0;     // SoftAP client departures
uint8_t g_sta_last_reason = 0;
int8_t g_sta_last_rssi = 0;
uint32_t g_sta_last_down_ms = 0;
uint8_t g_ap_last_reason = 0;
uint8_t g_ap_last_mac[6] = {};
// STA outage in progress: start time (0 = link up) and its ring entry.
int64_t g_sta_down_since_us = 0;
uint32_t g_sta_down_event = 0;  // g_disc_total value of that entry
uint32_t g_sta_retries = 0;
uint8_t g_sta_retry_reason = 0;
bool g_sta_ever_up = false;

const char *reason_name(unsigned r) {
  switch (r) {
    case WIFI_REASON_UNSPECIFIED: return "UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY: return "DISASSOC_INACTIVITY";
    case WIFI_REASON_ASSOC_TOOMANY: return "ASSOC_TOOMANY";
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA: return "CLASS2_FROM_NONAUTH";
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA: return "CLASS3_FROM_NONASSOC";
    case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED: return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD: return "PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD: return "SUPCHAN_BAD";
    case WIFI_REASON_BSS_TRANSITION_DISASSOC: return "BSS_TRANSITION";
    case WIFI_REASON_IE_INVALID: return "IE_INVALID";
    case WIFI_REASON_MIC_FAILURE: return "MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_GROUP_CIPHER_INVALID: return "GROUP_CIPHER_INVALID";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID: return "PAIRWISE_CIPHER_INVALID";
    case WIFI_REASON_AKMP_INVALID: return "AKMP_INVALID";
    case WIFI_REASON_802_1X_AUTH_FAILED: return "802_1X_AUTH_FAILED";
    case WIFI_REASON_CIPHER_SUITE_REJECTED: return "CIPHER_SUITE_REJECTED";
    case WIFI_REASON_MISSING_ACKS: return "MISSING_ACKS";
    case WIFI_REASON_STA_LEAVING: return "STA_LEAVING";
    case WIFI_REASON_TIMEOUT: return "TIMEOUT";
    case WIFI_REASON_PEER_INITIATED: return "PEER_INITIATED";
    case WIFI_REASON_AP_INITIATED: return "AP_INITIATED";
    case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET: return "AP_TSF_RESET";
    case WIFI_REASON_ROAMING: return "ROAMING";
    case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG: return "ASSOC_COMEBACK_TOO_LONG";
    case WIFI_REASON_SA_QUERY_TIMEOUT: return "SA_QUERY_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "NO_AP_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "NO_AP_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "NO_AP_IN_RSSI_THRESHOLD";
    default: return "?";
  }
}

uint32_t uptime_s() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000000);
}

// Append under g_disc_mux; returns the event's sequence number.
uint32_t record_event_locked(uint8_t kind, uint8_t reason, int8_t rssi,
                             const uint8_t mac[6]) {
  DiscEvent &e = g_disc_ring[g_disc_total % DISC_RING];
  e = DiscEvent{};
  e.t_s = uptime_s();
  e.kind = kind;
  e.reason = reason;
  e.rssi = rssi;
  std::memcpy(e.mac, mac, 6);
  return g_disc_total++;
}

void on_sta_disconnected(const wifi_event_sta_disconnected_t *d) {
  const uint8_t reason = d != nullptr ? d->reason : 0;
  const int8_t rssi = d != nullptr ? d->rssi : 0;
  static const uint8_t kZero[6] = {};
  const uint8_t *bssid = d != nullptr ? d->bssid : kZero;
  bool lost = false;
  uint32_t retries = 0;
  bool log_retry = false;
  taskENTER_CRITICAL(&g_disc_mux);
  if (g_sta_down_since_us == 0) {
    // First event of an outage (or of the boot-time connect).
    g_sta_down_since_us = esp_timer_get_time();
    g_sta_retries = 0;
    g_sta_retry_reason = reason;
    if (g_sta_ever_up) {
      lost = true;
      ++g_sta_disc;
      g_sta_last_reason = reason;
      g_sta_last_rssi = rssi;
      g_sta_down_event = record_event_locked(KIND_STA, reason, rssi, bssid);
    }
  } else {
    // A reconnect attempt failed. Log only when the reason changes and on
    // every 10th attempt, so a long outage cannot flush the /log ring.
    retries = ++g_sta_retries;
    log_retry = reason != g_sta_retry_reason || retries % 10 == 0;
    g_sta_retry_reason = reason;
  }
  taskEXIT_CRITICAL(&g_disc_mux);
  if (lost) {
    ESP_LOGW(EV_TAG,
             "STA disconnected: reason %u (%s), rssi %d dBm, bssid "
             "%02x:%02x:%02x:%02x:%02x:%02x; reconnecting",
             reason, reason_name(reason), rssi, bssid[0], bssid[1], bssid[2],
             bssid[3], bssid[4], bssid[5]);
  } else if (log_retry) {
    ESP_LOGW(EV_TAG, "STA reconnect attempt %u failed: reason %u (%s), rssi %d",
             static_cast<unsigned>(retries), reason, reason_name(reason), rssi);
  }
}

void on_sta_got_ip_event() {
  uint32_t down_ms = 0;
  uint32_t retries = 0;
  bool was_down = false;
  taskENTER_CRITICAL(&g_disc_mux);
  if (g_sta_down_since_us != 0 && g_sta_ever_up) {
    was_down = true;
    down_ms = static_cast<uint32_t>(
        (esp_timer_get_time() - g_sta_down_since_us) / 1000);
    retries = g_sta_retries;
    g_sta_last_down_ms = down_ms;
    // Fill the outage's ring entry unless 8 newer events overwrote it.
    if (g_disc_total - g_sta_down_event <= DISC_RING) {
      g_disc_ring[g_sta_down_event % DISC_RING].down_ms = down_ms;
    }
  }
  g_sta_down_since_us = 0;
  g_sta_ever_up = true;
  taskEXIT_CRITICAL(&g_disc_mux);
  if (was_down) {
    ESP_LOGW(EV_TAG, "STA back after %u ms (%u failed reconnect attempts)",
             static_cast<unsigned>(down_ms), static_cast<unsigned>(retries));
  }
}

void on_ap_sta_left(const wifi_event_ap_stadisconnected_t *d) {
  if (d == nullptr) return;
  const uint8_t reason = static_cast<uint8_t>(d->reason);
  taskENTER_CRITICAL(&g_disc_mux);
  ++g_ap_disc;
  g_ap_last_reason = reason;
  std::memcpy(g_ap_last_mac, d->mac, 6);
  record_event_locked(KIND_AP, reason, 0, d->mac);
  taskEXIT_CRITICAL(&g_disc_mux);
  ESP_LOGW(EV_TAG, "AP client %02x:%02x:%02x:%02x:%02x:%02x left: reason %u (%s)",
           d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
           static_cast<unsigned>(d->reason), reason_name(d->reason));
}

void on_ap_sta_joined(const wifi_event_ap_staconnected_t *d) {
  if (d == nullptr) return;
  ESP_LOGI(EV_TAG, "AP client %02x:%02x:%02x:%02x:%02x:%02x joined (aid %u)",
           d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
           static_cast<unsigned>(d->aid));
}

esp_err_t wifidisc_get(httpd_req_t *req) {
  DiscEvent ev[DISC_RING];
  uint32_t total;
  int64_t down_since;
  taskENTER_CRITICAL(&g_disc_mux);
  std::memcpy(ev, g_disc_ring, sizeof(ev));
  total = g_disc_total;
  down_since = g_sta_down_since_us;
  taskEXIT_CRITICAL(&g_disc_mux);
  char buf[1280];
  const uint32_t now = uptime_s();
  int off = std::snprintf(
      buf, sizeof(buf), "{\"uptime_s\":%lu,\"sta_down_ms\":%lu,\"total\":%lu,\"events\":[",
      static_cast<unsigned long>(now),
      static_cast<unsigned long>(
          down_since != 0 && g_sta_ever_up
              ? (esp_timer_get_time() - down_since) / 1000
              : 0),
      static_cast<unsigned long>(total));
  // Newest first.
  const uint32_t n = total < DISC_RING ? total : DISC_RING;
  for (uint32_t k = 0; k < n && off < static_cast<int>(sizeof(buf)) - 160; ++k) {
    const DiscEvent &e = ev[(total - 1 - k) % DISC_RING];
    off += std::snprintf(
        buf + off, sizeof(buf) - off,
        "%s{\"if\":\"%s\",\"t_s\":%lu,\"age_s\":%lu,\"reason\":%u,"
        "\"name\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"",
        k ? "," : "", e.kind == KIND_STA ? "sta" : "ap",
        static_cast<unsigned long>(e.t_s),
        static_cast<unsigned long>(now - e.t_s), e.reason,
        reason_name(e.reason), e.mac[0], e.mac[1], e.mac[2], e.mac[3],
        e.mac[4], e.mac[5]);
    if (e.kind == KIND_STA) {
      off += std::snprintf(buf + off, sizeof(buf) - off,
                           ",\"rssi\":%d,\"down_ms\":%lu}", e.rssi,
                           static_cast<unsigned long>(e.down_ms));
    } else {
      off += std::snprintf(buf + off, sizeof(buf) - off, "}");
    }
  }
  off += std::snprintf(buf + off, sizeof(buf) - off, "]}");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, off);
}

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
                   void *data) {
  const bool prov = g_provisioning.load(std::memory_order_relaxed);
  if (base != WIFI_EVENT) return;
  if (id == WIFI_EVENT_STA_START) {
    if (!prov) esp_wifi_connect();
  } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
    g_sta_connected.store(false, std::memory_order_relaxed);
    if (prov) return;  // provisioning loop owns the retry cadence
    on_sta_disconnected(static_cast<wifi_event_sta_disconnected_t *>(data));
    esp_wifi_connect();
  } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
    on_ap_sta_left(static_cast<wifi_event_ap_stadisconnected_t *>(data));
  } else if (id == WIFI_EVENT_AP_STACONNECTED) {
    on_ap_sta_joined(static_cast<wifi_event_ap_staconnected_t *>(data));
  }
}

void on_ip_event(void * /*arg*/, esp_event_base_t base, int32_t id,
                 void *data) {
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto *evt = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&evt->ip_info.ip));
    g_sta_connected.store(true, std::memory_order_relaxed);
    on_sta_got_ip_event();
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
// inside enter_ap_provisioning() until reboot. Captive-portal style is not
// implemented (no DNS redirection) — the user has to know the IP, but
// most phones will offer to open the AP's "config" page automatically.

constexpr const char *AP_TAG = "wifi-ap";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONN = 2;
// With saved credentials, provisioning mode keeps retrying the saved
// network this often, so a router that was down at boot (power cut,
// liveness-WDT reboot during an outage) no longer strands the device on
// the setup AP. Each attempt scans all channels, which briefly takes the
// SoftAP off-channel, so it is skipped while a client is on the setup AP.
constexpr uint32_t AP_STA_RETRY_SECS = 60;
// The client gate is bounded: a phone that auto-joined the open setup AP
// during the outage and just sits there must not block recovery forever.
// After this many skipped ticks an attempt runs anyway.
constexpr uint32_t AP_STA_MAX_SKIPS = 10;
// After provisioning reboots on a recovered network, the next boot waits
// this long instead of NBP_AP_FALLBACK_SECS, so a slow/marginal link that
// needs >FALLBACK_SECS to get an IP doesn't cycle boot→AP→reboot forever.
constexpr uint32_t RECOVERED_BOOT_WAIT_SECS = 300;
constexpr uint32_t RECOVERED_MAGIC = 0x4E425052;  // "NBPR"
// Survives esp_restart() (SW reset), garbage after power-on; the magic
// check makes power-on values harmless.
RTC_NOINIT_ATTR uint32_t g_recovered_magic;

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
bool is_hex_string(const char *s) {
  for (; *s; ++s) {
    if (!std::isxdigit(static_cast<unsigned char>(*s))) return false;
  }
  return true;
}

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

  char ssid[34] = {0};
  char psk[66] = {0};
  if (!form_get(buf, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
    return ESP_FAIL;
  }
  if (std::strlen(ssid) > 32) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid too long");
    return ESP_FAIL;
  }
  form_get(buf, "psk", psk, sizeof(psk));  // may be empty (open AP)
  size_t psk_len = std::strlen(psk);
  if ((psk_len > 0 && psk_len < 8) || psk_len > 64 ||
      (psk_len == 64 && !is_hex_string(psk))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid password");
    return ESP_FAIL;
  }

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

// Switches the radio to AP (APSTA with retry_sta), spins up the captive httpd, and
// blocks until reboot. With `retry_sta` (credentials exist) the radio
// runs APSTA and periodically retries the saved network; getting an IP
// reboots into normal mode. Exits: esp_restart() here or in ap_wifi_post.
[[noreturn]] void enter_ap_provisioning(bool retry_sta) {
  ESP_LOGW(AP_TAG, "entering SoftAP provisioning mode");
  g_provisioning.store(true, std::memory_order_relaxed);
  // Clear before the radio restarts, so nothing the new session produces
  // can be erased; a stale pre-fallback GOT_IP at worst costs one reboot.
  xEventGroupClearBits(g_events, BIT_GOT_IP);

  // Tear down the half-running STA before re-binding netif as AP.
  // create_default_wifi_ap registers the AP netif (192.168.4.1/24 +
  // built-in DHCP server) and its event handlers; do it before
  // set_mode so the wifi driver sees both netifs at mode-switch time.
  esp_wifi_stop();
  esp_netif_create_default_wifi_ap();
  esp_wifi_set_mode(retry_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP);

  wifi_config_t ap = {};
  char ssid[33];
  compose_ap_ssid(ssid, sizeof(ssid));
  std::strncpy(reinterpret_cast<char *>(ap.ap.ssid), ssid,
               sizeof(ap.ap.ssid) - 1);
  ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
  ap.ap.channel = AP_CHANNEL;
  ap.ap.max_connection = AP_MAX_CONN;
  ap.ap.authmode = WIFI_AUTH_OPEN;

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

  ESP_LOGW(AP_TAG, "SoftAP up: SSID='%s' (open) — http://192.168.4.1/",
           ssid);

  if (!retry_sta) {
    // Block forever; ap_wifi_post reboots the device on success.
    while (true) vTaskDelay(portMAX_DELAY);
  }

  uint32_t skips = 0;
  while (true) {
    EventBits_t bits = xEventGroupWaitBits(
        g_events, BIT_GOT_IP, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(AP_STA_RETRY_SECS * 1000));
    if (bits & BIT_GOT_IP) {
      ESP_LOGW(AP_TAG, "saved network is back; rebooting into STA mode");
      g_recovered_magic = RECOVERED_MAGIC;
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
    }
    wifi_sta_list_t stas = {};
    if (esp_wifi_ap_get_sta_list(&stas) == ESP_OK && stas.num > 0 &&
        ++skips < AP_STA_MAX_SKIPS) {
      continue;  // someone is on the setup AP; don't hop channels
    }
    skips = 0;
    ESP_LOGI(AP_TAG, "retrying saved network");
    esp_wifi_connect();
  }
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

  char ssid[33] = {0};
  char psk[65] = {0};

  if (!read_creds_from_nvs(ssid, sizeof(ssid), psk, sizeof(psk))) {
    std::strncpy(ssid, WIFI_SSID, sizeof(ssid) - 1);
    std::strncpy(psk, WIFI_PASSWORD, sizeof(psk) - 1);
  }

#if CONFIG_NBP_AP_FALLBACK
  if (ssid[0] == '\0') {
    ESP_LOGW(TAG, "no WiFi credentials configured; entering AP provisioning");
    enter_ap_provisioning(false);  // [[noreturn]]
  }
#endif

  wifi_config_t wc = {};
  std::memcpy(wc.sta.ssid, ssid, std::strlen(ssid));
  std::memcpy(wc.sta.password, psk, std::strlen(psk));
  wc.sta.threshold.authmode = psk[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

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
  uint32_t wait_s = CONFIG_NBP_AP_FALLBACK_SECS;
  if (g_recovered_magic == RECOVERED_MAGIC) {
    // Provisioning just saw this network come back; give it longer.
    wait_s = RECOVERED_BOOT_WAIT_SECS;
    ESP_LOGI(TAG, "network recovered from provisioning; waiting up to %u s",
             static_cast<unsigned>(wait_s));
  }
  g_recovered_magic = 0;
  TickType_t to = pdMS_TO_TICKS(wait_s * 1000);
  EventBits_t bits = xEventGroupWaitBits(g_events, BIT_GOT_IP,
                                         pdFALSE, pdTRUE, to);
  if ((bits & BIT_GOT_IP) == 0) {
    ESP_LOGW(TAG, "STA timeout after %d s; falling back to SoftAP",
             CONFIG_NBP_AP_FALLBACK_SECS);
    enter_ap_provisioning(true);  // [[noreturn]]
  }
#else
  xEventGroupWaitBits(g_events, BIT_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
#endif
}

bool is_connected() {
  return g_sta_connected.load(std::memory_order_relaxed);
}

int append_disc_stats_json(char *buf, size_t cap) {
  uint32_t sta, ap, sta_down;
  uint8_t sta_reason, ap_reason, ap_mac[6];
  int8_t sta_rssi;
  taskENTER_CRITICAL(&g_disc_mux);
  sta = g_sta_disc;
  ap = g_ap_disc;
  sta_reason = g_sta_last_reason;
  sta_rssi = g_sta_last_rssi;
  sta_down = g_sta_last_down_ms;
  ap_reason = g_ap_last_reason;
  std::memcpy(ap_mac, g_ap_last_mac, 6);
  taskEXIT_CRITICAL(&g_disc_mux);
  return std::snprintf(
      buf, cap,
      ",\"sta_disc\":%lu,\"sta_disc_reason\":%u,\"sta_disc_rssi\":%d,"
      "\"sta_disc_down_ms\":%lu,\"ap_disc\":%lu,\"ap_disc_reason\":%u,"
      "\"ap_disc_mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"",
      static_cast<unsigned long>(sta), sta_reason, sta_rssi,
      static_cast<unsigned long>(sta_down), static_cast<unsigned long>(ap),
      ap_reason, ap_mac[0], ap_mac[1], ap_mac[2], ap_mac[3], ap_mac[4],
      ap_mac[5]);
}

void register_endpoints(void *httpd_handle) {
  if (httpd_handle == nullptr) return;
  httpd_uri_t g = {.uri = "/wifidisc",
                   .method = HTTP_GET,
                   .handler = &wifidisc_get,
                   .user_ctx = nullptr};
  httpd_register_uri_handler(static_cast<httpd_handle_t>(httpd_handle), &g);
}

}  // namespace wifi_sta
