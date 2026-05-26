#include "ota.h"

#include "proxy_config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

namespace ota {

namespace {

constexpr const char *TAG = "ota";

// Chunk size for streaming POST body → OTA flash writes. Small enough
// to keep stack/heap pressure low; large enough to amortize HTTP recv
// overhead. 4 KiB matches the SPI flash sector size, which is what
// esp_ota_write internally aligns to.
constexpr size_t CHUNK = 4096;

esp_err_t root_get(httpd_req_t *req) {
  // Plain-text help page. Keeping it tiny avoids the multipart-form
  // parsing we'd need for browser file uploads.
  static const char body[] =
      "nimble-ble-proxy OTA endpoint\n"
      "POST a raw firmware image (.bin) to /update.\n"
      "\n"
      "  curl --data-binary @nimble_ble_proxy.bin \\\n"
      "       http://<host>/update\n";
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, body, sizeof(body) - 1);
}

esp_err_t update_post(httpd_req_t *req) {
  ESP_LOGI(TAG, "OTA upload starting, content_len=%d", req->content_len);

  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  if (target == nullptr) {
    ESP_LOGE(TAG, "no OTA partition available");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "no OTA partition");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "writing to partition %s @ 0x%08lx (%lu bytes)", target->label,
           target->address, target->size);

  esp_ota_handle_t handle = 0;
  // Size=OTA_SIZE_UNKNOWN lets the OTA layer figure it out from the
  // image header — works even if Content-Length is wrong/missing.
  esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
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
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
      return ESP_FAIL;
    }
    err = esp_ota_write(handle, buf, n);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_write @ %u: %s", static_cast<unsigned>(total),
               esp_err_to_name(err));
      esp_ota_abort(handle);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "ota_write failed");
      return ESP_FAIL;
    }
    total += static_cast<size_t>(n);
  }

  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end: %s (bad image?)", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "ota_end failed — bad image?");
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
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

}  // namespace

void start() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 8192;
  // OTA POST can take 30+ seconds over WiFi; bump from the 5s default.
  cfg.recv_wait_timeout = 30;
  cfg.send_wait_timeout = 30;
  cfg.lru_purge_enable = true;
  // LWIP_MAX_SOCKETS=8 leaves us 5 user sockets after the HTTPD's 3
  // internal ones; default 7 would fail to start. OTA only needs 1
  // concurrent client.
  cfg.max_open_sockets = 3;

  httpd_handle_t srv = nullptr;
  if (httpd_start(&srv, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    return;
  }

  httpd_uri_t root = {.uri = "/",
                      .method = HTTP_GET,
                      .handler = &root_get,
                      .user_ctx = nullptr};
  httpd_uri_t update = {.uri = "/update",
                        .method = HTTP_POST,
                        .handler = &update_post,
                        .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &root);
  httpd_register_uri_handler(srv, &update);

  ESP_LOGI(TAG, "OTA endpoint at http://%s.local/update", proxy::HOSTNAME);
}

}  // namespace ota
