#include "clone.h"

#ifdef CONFIG_NBP_CLONE

#include "clone_config.h"
#include "clone_mirror.h"
#include "clone_upstream.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ble_clone {

namespace {

constexpr const char *TAG = "clone";

// "AA:BB:CC:DD:EE:FF" → MSB-first packed uint64. Returns true on parse.
bool parse_mac(const char *s, uint64_t *out) {
  if (s == nullptr) return false;
  uint64_t acc = 0;
  int hex_seen = 0;
  for (const char *p = s; *p; ++p) {
    char c = *p;
    if (c == ':' || c == '-') continue;
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    int v = (c >= '0' && c <= '9') ? c - '0'
            : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                     : c - 'A' + 10;
    acc = (acc << 4) | static_cast<uint64_t>(v);
    ++hex_seen;
  }
  if (hex_seen != 12) return false;
  *out = acc;
  return true;
}

void fmt_mac(uint64_t addr, char *out, size_t cap) {
  std::snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
                static_cast<unsigned>((addr >> 40) & 0xff),
                static_cast<unsigned>((addr >> 32) & 0xff),
                static_cast<unsigned>((addr >> 24) & 0xff),
                static_cast<unsigned>((addr >> 16) & 0xff),
                static_cast<unsigned>((addr >> 8) & 0xff),
                static_cast<unsigned>(addr & 0xff));
}

const char *state_name(upstream::State s) {
  switch (s) {
    case upstream::State::Disabled: return "Disabled";
    case upstream::State::Idle: return "Idle";
    case upstream::State::Scanning: return "Scanning";
    case upstream::State::Connecting: return "Connecting";
    case upstream::State::Discovering: return "Discovering";
    case upstream::State::Ready: return "Ready";
    case upstream::State::Reconnecting: return "Reconnecting";
  }
  return "?";
}

esp_err_t clone_get(httpd_req_t *req) {
  config::Target t = config::snapshot();
  upstream::Status us = upstream::status();
  mirror::Stats ms = mirror::stats();

  char mac_s[20];
  fmt_mac(t.address, mac_s, sizeof(mac_s));

  char body[384];
  int n = std::snprintf(
      body, sizeof(body),
      "{"
      "\"enabled\":%s,"
      "\"addr\":\"%s\","
      "\"type\":%u,"
      "\"name_suffix\":\"%s\","
      "\"state\":\"%s\","
      "\"mtu\":%u,"
      "\"reconnects\":%lu,"
      "\"last_disconnect_reason\":%lu,"
      "\"notifies_in\":%lu,"
      "\"notifies_out\":%lu,"
      "\"writes_drained\":%lu,"
      "\"writes_dropped\":%lu,"
      "\"services\":%u,"
      "\"characteristics\":%u,"
      "\"connected_centrals\":%u,"
      "\"advertising\":%s"
      "}",
      t.enabled ? "true" : "false", mac_s, t.address_type, t.name_suffix,
      state_name(us.state), us.mtu,
      static_cast<unsigned long>(us.reconnects),
      static_cast<unsigned long>(us.last_disconnect_reason),
      static_cast<unsigned long>(us.notifies_seen),
      static_cast<unsigned long>(ms.notifies_out),
      static_cast<unsigned long>(us.writes_drained),
      static_cast<unsigned long>(us.writes_dropped),
      ms.services, ms.characteristics, ms.connected_centrals,
      ms.advertising ? "true" : "false");

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, n);
}

esp_err_t clone_post(httpd_req_t *req) {
  char query[160];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }

  config::Target prev = config::snapshot();
  config::Target next = prev;
  bool addr_changed = false;

  char buf[32];
  if (httpd_query_key_value(query, "addr", buf, sizeof(buf)) == ESP_OK) {
    uint64_t parsed = 0;
    if (!parse_mac(buf, &parsed)) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "addr must be AA:BB:CC:DD:EE:FF");
    }
    if (parsed != prev.address) addr_changed = true;
    next.address = parsed;
  }
  if (httpd_query_key_value(query, "type", buf, sizeof(buf)) == ESP_OK) {
    long v = std::strtol(buf, nullptr, 10);
    if (v < 0 || v > 3) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "type must be 0..3");
    }
    next.address_type = static_cast<uint8_t>(v);
  }
  if (httpd_query_key_value(query, "enabled", buf, sizeof(buf)) == ESP_OK) {
    next.enabled = (buf[0] == '1' || buf[0] == 't' || buf[0] == 'T');
  }
  if (httpd_query_key_value(query, "name_suffix", buf, sizeof(buf)) ==
      ESP_OK) {
    std::strncpy(next.name_suffix, buf, sizeof(next.name_suffix) - 1);
    next.name_suffix[sizeof(next.name_suffix) - 1] = 0;
  }

  if (!config::set(next)) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "nvs write failed");
  }

  // NimBLE's GATT DB is one-shot: changing the target MAC after the
  // mirror has been built means the next session's services may differ,
  // but we can't re-register. Surface this to the caller so the UI can
  // prompt for a reboot.
  bool reboot_required = addr_changed && (mirror::stats().services > 0);

  char body[64];
  int n = std::snprintf(body, sizeof(body),
                        "{\"ok\":true,\"reboot_required\":%s}",
                        reboot_required ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, n);
}

}  // namespace

void init() {
  upstream::init();
  upstream::start();
  ESP_LOGI(TAG, "init done; supervisor running");
}

void register_endpoints(void *httpd_handle) {
  if (httpd_handle == nullptr) return;
  auto srv = static_cast<httpd_handle_t>(httpd_handle);
  httpd_uri_t g = {.uri = "/clone",
                   .method = HTTP_GET,
                   .handler = &clone_get,
                   .user_ctx = nullptr};
  httpd_uri_t p = {.uri = "/clone",
                   .method = HTTP_POST,
                   .handler = &clone_post,
                   .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &g);
  httpd_register_uri_handler(srv, &p);
  ESP_LOGI(TAG, "/clone endpoints registered");
}

}  // namespace ble_clone

#endif  // CONFIG_NBP_CLONE
