#include "ota.h"

#include "proxy_config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#if CONFIG_NBP_OTA
#include "esp_ota_ops.h"
#endif
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <cstdio>
#include <strings.h>  // strncasecmp

namespace ota {

namespace {

constexpr const char *TAG = "ota";

httpd_handle_t g_srv = nullptr;

// Radio-quiesce hooks (see ota.h). Set by main; null ⇒ no quiescing.
QuiesceFn g_quiesce_begin = nullptr;
QuiesceFn g_quiesce_end = nullptr;

#if CONFIG_NBP_OTA
// Chunk size for streaming POST body → OTA flash writes. Small enough
// to keep stack/heap pressure low; large enough to amortize HTTP recv
// overhead. 4 KiB matches the SPI flash sector size, which is what
// esp_ota_write internally aligns to.
constexpr size_t CHUNK = 4096;

esp_err_t update_post(httpd_req_t *req) {
  // OTA is a remote firmware-write primitive — surface it prominently in
  // the web console (WARN) and record who kicked it off.
  char peer[48] = "?";
  struct sockaddr_storage ss;
  socklen_t sl = sizeof(ss);
  if (getpeername(httpd_req_to_sockfd(req),
                  reinterpret_cast<struct sockaddr *>(&ss), &sl) == 0) {
    if (ss.ss_family == AF_INET6) {
      auto *a = reinterpret_cast<struct sockaddr_in6 *>(&ss);
      inet_ntop(AF_INET6, &a->sin6_addr, peer, sizeof(peer));
    } else {
      auto *a = reinterpret_cast<struct sockaddr_in *>(&ss);
      inet_ntop(AF_INET, &a->sin_addr, peer, sizeof(peer));
    }
  }
  // IPv4 clients reach the dual-stack listener as IPv4-mapped
  // (::ffff:a.b.c.d) — show the bare v4 address.
  const char *peer_str = peer;
  if (strncasecmp(peer_str, "::ffff:", 7) == 0) peer_str += 7;
  ESP_LOGW(TAG, "OTA STARTED from %s (content_len=%d)", peer_str,
           req->content_len);

  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  if (target == nullptr) {
    ESP_LOGE(TAG, "no OTA partition available");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "no OTA partition");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "writing to partition %s @ 0x%08lx (%lu bytes)", target->label,
           target->address, target->size);

  // Free the radio for the transfer: drop the SoftAP and pause the BLE scan
  // so coex can't starve the upload. Restored on any failure return below; on
  // success the reboot restores them. (No-op if main wired no hooks.)
  if (g_quiesce_begin) g_quiesce_begin();

  esp_ota_handle_t handle = 0;
  // Size=OTA_SIZE_UNKNOWN lets the OTA layer figure it out from the
  // image header — works even if Content-Length is wrong/missing.
  esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
    if (g_quiesce_end) g_quiesce_end();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "ota_begin failed");
    return ESP_FAIL;
  }

  // Reading on the stack would blow our 8 KiB HTTP task stack; static
  // is fine since only one OTA can run at a time.
  static uint8_t buf[CHUNK];
  size_t total = 0;
  while (true) {
    int n = httpd_req_recv(req, reinterpret_cast<char *>(buf), sizeof(buf));
    if (n == 0) break;  // clean EOF
    if (n < 0) {
      if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
      ESP_LOGE(TAG, "recv failed: %d", n);
      esp_ota_abort(handle);
      if (g_quiesce_end) g_quiesce_end();
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
      return ESP_FAIL;
    }
    err = esp_ota_write(handle, buf, n);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_write @ %u: %s", static_cast<unsigned>(total),
               esp_err_to_name(err));
      esp_ota_abort(handle);
      if (g_quiesce_end) g_quiesce_end();
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "ota_write failed");
      return ESP_FAIL;
    }
    total += static_cast<size_t>(n);
  }

  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end: %s (bad image?)", esp_err_to_name(err));
    if (g_quiesce_end) g_quiesce_end();
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "ota_end failed — bad image?");
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
    if (g_quiesce_end) g_quiesce_end();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "set_boot failed");
    return ESP_FAIL;
  }

  char msg[80];
  std::snprintf(msg, sizeof(msg), "ok: wrote %u bytes to %s, rebooting\n",
                static_cast<unsigned>(total), target->label);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);

  ESP_LOGI(TAG, "OTA complete (%u bytes) → rebooting into %s",
           static_cast<unsigned>(total), target->label);
  // Give the HTTP response a chance to drain before pulling the rug.
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}
#endif  // CONFIG_NBP_OTA

}  // namespace

void start() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 8192;
  // OTA POST can take 30+ seconds over WiFi; bump from the 5s default.
  cfg.recv_wait_timeout = 30;
  cfg.send_wait_timeout = 30;
  cfg.lru_purge_enable = true;
  // Default 8 handlers is exactly what we needed for the bring-up set
  // (/update, /, /stats.json, /log, /level GET+POST, /reboot, /trace).
  // We're now well past that: /favicon.svg, /txpower, /cpufreq, /scan,
  // /wifips, /hostname, /advitvl (all GET+POST pairs), /devices, /clone
  // GET+POST (clone absorbed /passkey), plus /nat GET+POST and /portmap
  // POST when the NAT router is built in — ~28 routes today. Each
  // unregistered handler is a silent 404 in production (we saw
  // /devices drop at boot), so size with a comfortable margin and add
  // a runtime assert at registration time.
  cfg.max_uri_handlers = 40;
  // CONFIG_LWIP_MAX_SOCKETS=16 leaves the api_server (1 listen + up to
  // 4 clients), mDNS and clone.mirror plenty of room alongside the
  // dashboard. Bumping httpd here to 5 lets a phone + a desktop browser
  // both poll /stats.json in parallel without httpd_accept_conn ENFILE.
  cfg.max_open_sockets = 5;

  if (httpd_start(&g_srv, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    g_srv = nullptr;
    return;
  }

  // Suppress 'httpd_txrx: error in recv : 104/113' WARN spam. These fire
  // on every peer-initiated socket close (ECONNRESET / ENOCONN) — both
  // are normal for a server handling polling clients (the dashboard
  // closes keep-alive connections between fetch bursts) and for any
  // recv in flight when WiFi drops. The IDF httpd has no per-errno
  // filter; coarse-level mute is the only knob. Real bugs in this path
  // would surface via ESP_LOGE (e.g. 'httpd_start failed') instead.
  esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

#if CONFIG_NBP_OTA
  httpd_uri_t update = {.uri = "/update",
                        .method = HTTP_POST,
                        .handler = &update_post,
                        .user_ctx = nullptr};
  httpd_register_uri_handler(g_srv, &update);

  ESP_LOGI(TAG, "OTA endpoint at http://%s.local/update", proxy::hostname());
#else
  ESP_LOGI(TAG, "OTA endpoint disabled (NBP_OTA=n); httpd up for dashboard");
#endif
}

httpd_handle_t handle() { return g_srv; }

void set_quiesce_hooks(QuiesceFn begin, QuiesceFn end) {
  g_quiesce_begin = begin;
  g_quiesce_end = end;
}

}  // namespace ota
