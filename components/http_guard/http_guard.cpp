#include "http_guard.h"

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/api.h"
// sockets_priv.h has no extern "C" guard (lwip_socket_dbg_get_socket).
extern "C" {
#include "lwip/priv/sockets_priv.h"
}
#include "lwip/priv/tcpip_priv.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

extern "C" esp_err_t __real_httpd_register_uri_handler(
    httpd_handle_t handle, const httpd_uri_t *uri_handler);
extern "C" esp_err_t __wrap_httpd_register_uri_handler(
    httpd_handle_t handle, const httpd_uri_t *uri_handler);

namespace http_guard {
namespace {

constexpr const char *TAG = "http";

// One slot per registration. httpd copies the httpd_uri_t, so the
// registered user_ctx is the slot; the trampoline swaps the handler's own
// user_ctx back into the request before calling it. Slots are never freed
// (handlers are registered once at boot; the AP-fallback server adds 4).
struct Slot {
  esp_err_t (*fn)(httpd_req_t *);
  void *ctx;
};
constexpr size_t kSlots = 56;
Slot g_slots[kSlots];
std::atomic<size_t> g_used{0};

void abortive(int fd) {
  linger lg{};
  lg.l_onoff = 1;
  lg.l_linger = 0;
  setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
}

esp_err_t trampoline(httpd_req_t *req) {
  auto *s = static_cast<Slot *>(req->user_ctx);
  req->user_ctx = s->ctx;
  const esp_err_t r = s->fn(req);
  req->user_ctx = s;
  if (r != ESP_OK) {
    const int fd = httpd_req_to_sockfd(req);
    ESP_LOGW(TAG, "%s %s failed (%s); free %u largest %u; closing fd=%d "
             "abortively",
             req->method == HTTP_POST ? "POST" : "GET", req->uri,
             esp_err_to_name(r),
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)),
             fd);
    if (fd >= 0) abortive(fd);
  }
  return r;
}

struct QueueCall {
  tcpip_api_call_data base;  // must be first: lwIP passes &base back
  int fd;
  bool ok;
  uint16_t segs;
};

err_t queue_fn(tcpip_api_call_data *c) {
  auto *q = reinterpret_cast<QueueCall *>(c);
  q->ok = false;
  lwip_sock *sock = lwip_socket_dbg_get_socket(q->fd);
  if (sock == nullptr || sock->conn == nullptr ||
      NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_TCP ||
      sock->conn->pcb.tcp == nullptr) {
    return ERR_OK;
  }
  q->segs = sock->conn->pcb.tcp->snd_queuelen;
  q->ok = true;
  return ERR_OK;
}

}  // namespace

bool bulk_ok(httpd_req_t *req) {
  const size_t free_now = esp_get_free_heap_size();
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  if (free_now >= kBulkMinFree && largest >= kBulkMinLargest) return true;
  ESP_LOGW(TAG, "%s: 503, heap low (free %u largest %u)", req->uri,
           static_cast<unsigned>(free_now), static_cast<unsigned>(largest));
  httpd_resp_set_status(req, "503 Service Unavailable");
  httpd_resp_set_hdr(req, "Retry-After", "10");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, "heap low, retry later\n");
  return false;
}

esp_err_t send_chunk_paced(httpd_req_t *req, const char *buf, size_t len) {
  const int fd = httpd_req_to_sockfd(req);
  // The server's send timeout (ota.cpp sets 30 s) bounds a stall.
  constexpr int kPollMs = 10;
  constexpr int kTimeoutMs = 30 * 1000;
  int waited = 0;
  for (;;) {
    QueueCall q{};
    q.fd = fd;
    if (fd < 0 || tcpip_api_call(&queue_fn, &q.base) != ERR_OK || !q.ok ||
        q.segs <= kPaceSegs) {
      break;  // unreadable: let the send report the error
    }
    if (waited >= kTimeoutMs) {
      ESP_LOGW(TAG, "%s: send queue stuck at %u segs for %d ms", req->uri,
               q.segs, kTimeoutMs);
      return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(kPollMs));
    waited += kPollMs;
  }
  return httpd_resp_send_chunk(req, buf, static_cast<ssize_t>(len));
}

}  // namespace http_guard

extern "C" esp_err_t __wrap_httpd_register_uri_handler(
    httpd_handle_t handle, const httpd_uri_t *uri_handler) {
  using http_guard::g_slots;
  using http_guard::g_used;
  using http_guard::kSlots;
  if (uri_handler == nullptr || uri_handler->handler == nullptr) {
    return __real_httpd_register_uri_handler(handle, uri_handler);
  }
  const size_t i = g_used.fetch_add(1);
  if (i >= kSlots) {
    // Unwrapped: works, only without the failure log.
    ESP_LOGW(http_guard::TAG, "no guard slot for %s", uri_handler->uri);
    return __real_httpd_register_uri_handler(handle, uri_handler);
  }
  g_slots[i] = {uri_handler->handler, uri_handler->user_ctx};
  httpd_uri_t u = *uri_handler;
  u.handler = &http_guard::trampoline;
  u.user_ctx = &g_slots[i];
  const esp_err_t r = __real_httpd_register_uri_handler(handle, &u);
  // A failed registration leaves the slot unused; not worth reclaiming.
  return r;
}
