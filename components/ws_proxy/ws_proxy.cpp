#include "ws_proxy.h"

#ifdef CONFIG_NBP_WS_PROXY

#include "proxy_config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <atomic>
#include <cerrno>
#include <new>

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

// Pump task: loopback (api_server) → browser. One per session. api_server
// sends nothing unsolicited on connect, so this recv() blocks until the
// browser's first request has round-tripped — well after the WS handshake
// completes, avoiding any interleave with the HTTP 101 response.
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

esp_err_t ws_handler(httpd_req_t *req) {
  // First invocation = the HTTP GET upgrade. esp_http_server completes the
  // WebSocket handshake itself; we just stand up the loopback bridge.
  if (req->method == HTTP_GET) {
    if (g_sessions.load(std::memory_order_acquire) >= MAX_WS_SESSIONS) {
      ESP_LOGW(TAG, "refusing ws: %u sessions already active", MAX_WS_SESSIONS);
      return ESP_FAIL;  // server replies 500; browser sees a failed open
    }
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

  // Subsequent invocations = inbound WS frames (browser → device).
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
  httpd_uri_t ws = {.uri = "/api",
                    .method = HTTP_GET,
                    .handler = &ws_handler,
                    .user_ctx = nullptr,
                    .is_websocket = true,
                    .handle_ws_control_frames = false,
                    .supported_subprotocol = nullptr};
  if (httpd_register_uri_handler(srv, &ws) != ESP_OK) {
    ESP_LOGE(TAG, "failed to register /api ws route");
    return;
  }
  ESP_LOGI(TAG, "esphome-over-websocket bridge at ws:///api (-> :%u)",
           proxy::API_PORT);
}

}  // namespace ws_proxy

#endif  // CONFIG_NBP_WS_PROXY
