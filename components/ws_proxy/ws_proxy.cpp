#include "ws_proxy.h"

#ifdef CONFIG_NBP_WS_PROXY

#include "origin_check.h"
#include "proxy_config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <new>

#if !CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT || \
    !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "ws_proxy needs HTTPD_WS_{PRE,POST}_HANDSHAKE_CB_SUPPORT (IDF release/v5.5 or v6.0.3+); NBP_WS_PROXY selects them"
#endif

namespace ws_proxy {

namespace {

constexpr const char *TAG = "ws_proxy";

// Per-browser cost is real: one loopback TCP socket here + one accepted
// socket and an 8 KiB connection_task inside api_server + the pump task
// below. Cap concurrent bridges so a misbehaving page can't exhaust the
// 16-socket LWIP budget shared with httpd, mDNS and clone.
constexpr uint8_t MAX_WS_SESSIONS = 2;

// Pump task only does recv() + httpd_ws_send_frame_async(); 4 KiB is ample.
constexpr size_t PUMP_STACK = 4096;
constexpr UBaseType_t PUMP_PRIO = 5;  // matches api_server tasks

// Inbound (browser → device) frame cap. ESPHome client→server messages
// are small (Hello / Connect / GATT writes); MAX_MESSAGE_SIZE is the
// protocol ceiling, so a larger WS frame is malformed → drop the link.
constexpr size_t WS_RX_MAX = proxy::MAX_MESSAGE_SIZE;

std::atomic<uint8_t> g_sessions{0};
std::atomic<uint32_t> g_origin_refused{0};

struct WsSession {
  httpd_handle_t hd;
  int ws_fd;    // browser-facing WebSocket socket (owned by httpd)
  int loop_fd;  // loopback TCP socket into api_server (owned by us)
  std::atomic<bool> closing;
  SemaphoreHandle_t pump_done;  // given by pump on exit; taken by session_free
};

// Open a loopback TCP connection to the local api_server listener.
// api_server binds INADDR_ANY:API_PORT, so 127.0.0.1 reaches it via the
// lwIP loopback path (CONFIG_LWIP_NETIF_LOOPBACK=y).
int connect_loopback() {
  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(proxy::API_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ESP_LOGW(TAG, "loopback connect to :%u failed: errno=%d", proxy::API_PORT,
             errno);
    ::close(fd);
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}

// Pump task: loopback (api_server) → browser. One per session, started
// after the 101 went out. api_server sends nothing unsolicited on connect,
// so this recv() blocks until the browser's first request has round-tripped.
void pump_task(void *arg) {
  auto *s = static_cast<WsSession *>(arg);
  uint8_t buf[1024];
  while (!s->closing.load(std::memory_order_acquire)) {
    ssize_t n = ::recv(s->loop_fd, buf, sizeof(buf), 0);
    if (n <= 0) break;  // loopback EOF/error, or woken by session_free shutdown
    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = buf;
    frame.len = static_cast<size_t>(n);
    if (httpd_ws_send_frame_async(s->hd, s->ws_fd, &frame) != ESP_OK) {
      break;  // browser gone
    }
  }
  // If the loopback side closed first (i.e. session_free didn't initiate
  // this), ask httpd to tear down the WS session; that invokes
  // session_free, which waits on pump_done (given just below) before
  // freeing. After the give we touch nothing in `s`.
  if (!s->closing.load(std::memory_order_acquire)) {
    httpd_sess_trigger_close(s->hd, s->ws_fd);
  }
  xSemaphoreGive(s->pump_done);
  vTaskDelete(nullptr);
}

// Called by httpd when the WS socket closes (browser disconnect, CLOSE
// frame, error, or our own httpd_sess_trigger_close). Single teardown
// point — joins the pump before freeing so there's no use-after-free.
void session_free(void *ctx) {
  auto *s = static_cast<WsSession *>(ctx);
  if (s == nullptr) return;
  s->closing.store(true, std::memory_order_release);
  ::shutdown(s->loop_fd, SHUT_RDWR);  // unblock the pump's recv()
  xSemaphoreTake(s->pump_done, portMAX_DELAY);  // pump has now exited
  ::close(s->loop_fd);
  vSemaphoreDelete(s->pump_done);
  uint8_t left = g_sessions.fetch_sub(1, std::memory_order_acq_rel) - 1;
  ESP_LOGI(TAG, "ws session closed (fd=%d, active=%u)", s->ws_fd, left);
  delete s;
}

// Forward one fully-received inbound payload to the loopback socket.
bool forward_to_loopback(int fd, const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t r = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (r < 0) return false;
    sent += static_cast<size_t>(r);
  }
  return true;
}

// Header values are copied into fixed buffers; a value that does not fit
// is refused rather than truncated (a truncated Origin could match a
// shorter Host).
constexpr size_t HDR_MAX = 128;

// Read header `name` into `buf`. Returns buf, nullptr when absent or empty,
// or sets *too_long and returns nullptr when it does not fit.
const char *read_hdr(httpd_req_t *req, const char *name, char *buf,
                     size_t cap, bool *too_long) {
  size_t len = httpd_req_get_hdr_value_len(req, name);
  if (len == 0) return nullptr;
  if (len >= cap) {
    *too_long = true;
    return nullptr;
  }
  if (httpd_req_get_hdr_value_str(req, name, buf, cap) != ESP_OK) {
    *too_long = true;
    return nullptr;
  }
  return buf;
}

esp_err_t refuse(httpd_req_t *req, const char *status, const char *msg) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
  // Non-OK makes httpd skip the 101 and close the socket. Nothing has
  // been allocated yet, so there is nothing to free.
  return ESP_FAIL;
}

// Runs on the HTTP GET before httpd answers 101. Origin policy and the
// session cap both live here so a refused client never gets a WebSocket,
// a loopback socket or a pump task.
esp_err_t pre_handshake(httpd_req_t *req) {
  char origin_buf[HDR_MAX];
  char host_buf[HDR_MAX];
  bool too_long = false;
  const char *origin = read_hdr(req, "Origin", origin_buf, sizeof(origin_buf),
                                &too_long);
  const char *host = read_hdr(req, "Host", host_buf, sizeof(host_buf),
                              &too_long);
  origin::Verdict v = too_long ? origin::Verdict::kRejectMalformed
                               : origin::check(origin, host,
                                               CONFIG_NBP_WS_ORIGIN_ALLOWLIST);
  if (!origin::accepted(v)) {
    g_origin_refused.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGW(TAG, "refusing ws /api: %s (Origin '%s', Host '%s')",
             origin::name(v), origin ? origin : (too_long ? "<long>" : "-"),
             host ? host : "-");
    return refuse(req, "403 Forbidden", "ws /api: origin not allowed\n");
  }
  if (g_sessions.load(std::memory_order_acquire) >= MAX_WS_SESSIONS) {
    ESP_LOGW(TAG, "refusing ws: %u sessions already active", MAX_WS_SESSIONS);
    return refuse(req, "503 Service Unavailable", "ws /api: busy\n");
  }
  ESP_LOGD(TAG, "ws /api accepted: %s", origin::name(v));
  return ESP_OK;
}

// Runs after the 101 went out: stand up the loopback bridge. Since
// esp_http_server's 2026-02 rework (release/v5.5, v6.0.3) the URI handler
// is no longer called for the handshake GET, so this is the only place the
// session can be created. On failure httpd closes the new socket; the
// session context is attached only on success, so nothing leaks.
esp_err_t post_handshake(httpd_req_t *req) {
  int loop_fd = connect_loopback();
  if (loop_fd < 0) return ESP_FAIL;

  auto *s = new (std::nothrow) WsSession{};
  if (s == nullptr) {
    ::close(loop_fd);
    return ESP_FAIL;
  }
  s->hd = req->handle;
  s->ws_fd = httpd_req_to_sockfd(req);
  s->loop_fd = loop_fd;
  s->closing.store(false, std::memory_order_release);
  s->pump_done = xSemaphoreCreateBinary();
  if (s->pump_done == nullptr) {
    ::close(loop_fd);
    delete s;
    return ESP_FAIL;
  }
  if (xTaskCreate(&pump_task, "ws_pump", PUMP_STACK, s, PUMP_PRIO, nullptr) !=
      pdPASS) {
    vSemaphoreDelete(s->pump_done);
    ::close(loop_fd);
    delete s;
    return ESP_FAIL;
  }

  req->sess_ctx = s;
  req->free_ctx = &session_free;
  uint8_t active = g_sessions.fetch_add(1, std::memory_order_acq_rel) + 1;
  ESP_LOGI(TAG, "ws session open (fd=%d, active=%u) -> loopback fd=%d",
           s->ws_fd, active, loop_fd);
  return ESP_OK;
}

esp_err_t ws_handler(httpd_req_t *req) {
  // Only ever called for inbound WS frames (browser → device); the
  // handshake is handled by pre_/post_handshake above.
  auto *s = static_cast<WsSession *>(req->sess_ctx);
  if (s == nullptr) return ESP_FAIL;

  // First recv with max_len=0 fills frame.len + frame.type only. Control
  // frames (PING/PONG/CLOSE) are handled internally by httpd
  // (handle_ws_control_frames=false), so we only ever see data frames.
  httpd_ws_frame_t frame{};
  esp_err_t e = httpd_ws_recv_frame(req, &frame, 0);
  if (e != ESP_OK) return e;
  if (frame.len == 0) return ESP_OK;
  if (frame.len > WS_RX_MAX) {
    ESP_LOGW(TAG, "inbound ws frame %u > %u cap; closing",
             static_cast<unsigned>(frame.len), static_cast<unsigned>(WS_RX_MAX));
    return ESP_FAIL;
  }

  uint8_t buf[WS_RX_MAX];
  frame.payload = buf;
  e = httpd_ws_recv_frame(req, &frame, WS_RX_MAX);
  if (e != ESP_OK) return e;

  if (!forward_to_loopback(s->loop_fd, buf, frame.len)) {
    ESP_LOGW(TAG, "loopback send failed; closing ws");
    return ESP_FAIL;
  }
  return ESP_OK;
}

}  // namespace

void register_endpoint(void *httpd_handle) {
  if (httpd_handle == nullptr) {
    ESP_LOGW(TAG, "no httpd handle; ws bridge disabled");
    return;
  }
  auto srv = static_cast<httpd_handle_t>(httpd_handle);
  httpd_uri_t ws = {};
  ws.uri = "/api";
  ws.method = HTTP_GET;
  ws.handler = &ws_handler;
  ws.is_websocket = true;
  ws.handle_ws_control_frames = false;
  ws.supported_subprotocol = nullptr;
  ws.ws_pre_handshake_cb = &pre_handshake;
  ws.ws_post_handshake_cb = &post_handshake;
  if (httpd_register_uri_handler(srv, &ws) != ESP_OK) {
    ESP_LOGE(TAG, "failed to register /api ws route");
    return;
  }
  ESP_LOGI(TAG, "esphome-over-websocket bridge at ws:///api (-> :%u)",
           proxy::API_PORT);
}

int append_stats_json(char *buf, size_t cap) {
  return std::snprintf(
      buf, cap, ",\"ws_sessions\":%u,\"ws_origin_refused\":%lu",
      static_cast<unsigned>(g_sessions.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(
          g_origin_refused.load(std::memory_order_relaxed)));
}

}  // namespace ws_proxy

#endif  // CONFIG_NBP_WS_PROXY
