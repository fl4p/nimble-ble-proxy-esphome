#include "api_server.h"

#include "api_proto.h"
#include "bt_handlers.h"
#include "frame_codec.h"
#include "handshake.h"
#include "proxy_config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace api_server {

namespace {

constexpr const char *TAG = "api_server";
constexpr size_t LISTENER_STACK = 4096;
constexpr size_t CONN_STACK = 8192;
constexpr UBaseType_t LISTENER_PRIO = 5;
constexpr UBaseType_t CONN_PRIO = 5;

bool g_started = false;

// Active client fds. -1 in unused slots. Guarded by g_clients_mutex.
// Read by send_async (broadcast) and has_active_client.
std::array<int, proxy::MAX_API_CLIENTS> g_client_fds;
SemaphoreHandle_t g_clients_mutex = nullptr;
std::atomic<uint8_t> g_active_count{0};

// TX serialization — protects the shared g_tx_buf staging area used by
// both per-connection dispatch and async senders (BLE callbacks).
SemaphoreHandle_t g_tx_mutex = nullptr;

// Shared payload-encode area. Senders hold g_tx_mutex while writing
// here and shipping the bytes onto whichever socket(s).
uint8_t g_tx_buf[frame_codec::MAX_HEADER_LEN + proxy::MAX_MESSAGE_SIZE];

ssize_t socket_read(void *ctx, uint8_t *buf, size_t n) {
  int fd = *static_cast<int *>(ctx);
  return ::recv(fd, buf, n, 0);
}

// Write the payload in g_tx_buf to a specific fd. Caller holds g_tx_mutex.
bool send_locked_to(int fd, uint16_t msg_type, size_t payload_len) {
  size_t start = frame_codec::prepend_header(g_tx_buf, msg_type, payload_len);
  size_t total = (frame_codec::MAX_HEADER_LEN - start) + payload_len;
  const uint8_t *p = g_tx_buf + start;
  size_t sent = 0;
  while (sent < total) {
    ssize_t r = ::send(fd, p + sent, total - sent, MSG_NOSIGNAL);
    if (r < 0) {
      ESP_LOGW(TAG, "send fd=%d failed: errno=%d", fd, errno);
      return false;
    }
    sent += static_cast<size_t>(r);
  }
  return true;
}

// Add/remove a client fd from the active set. Returns false if no slot
// is free; in that case the caller should reject the connection.
bool register_client(int fd) {
  bool ok = false;
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (auto &s : g_client_fds) {
    if (s < 0) {
      s = fd;
      ok = true;
      break;
    }
  }
  xSemaphoreGive(g_clients_mutex);
  if (ok) g_active_count.fetch_add(1, std::memory_order_release);
  return ok;
}

void unregister_client(int fd) {
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (auto &s : g_client_fds) {
    if (s == fd) {
      s = -1;
      break;
    }
  }
  xSemaphoreGive(g_clients_mutex);
  g_active_count.fetch_sub(1, std::memory_order_release);
}

bool dispatch_one(int fd, uint8_t *rx_payload, size_t rx_cap,
                  bt_handlers::ClientSubs *subs) {
  uint16_t msg_type = 0;
  size_t payload_len = 0;
  frame_codec::Error e = frame_codec::read_frame(socket_read, &fd, rx_payload,
                                                 rx_cap, &msg_type, &payload_len);
  if (e != frame_codec::Error::Ok) {
    if (e != frame_codec::Error::Eof) {
      ESP_LOGW(TAG, "fd=%d read_frame error: %d", fd, static_cast<int>(e));
    }
    return false;
  }

  // Handshake holds the TX mutex (encodes directly into g_tx_buf and
  // ships it before releasing). BT handlers do NOT — they use
  // api_server::send_async for any wire output, which acquires the mutex
  // itself. Holding the mutex through bt_handlers::handle would deadlock
  // if a handler triggered an inline ble_backend callback that then tried
  // to send_async.
  bool keep_open = true;
  bool is_handshake = false;
  {
    xSemaphoreTake(g_tx_mutex, portMAX_DELAY);
    uint8_t *resp_payload = &g_tx_buf[frame_codec::MAX_HEADER_LEN];
    size_t resp_cap = sizeof(g_tx_buf) - frame_codec::MAX_HEADER_LEN;
    handshake::Result hr = handshake::handle(msg_type, rx_payload, payload_len,
                                             resp_payload, resp_cap);
    if (hr.handled) {
      is_handshake = true;
      if (hr.response_type != 0) {
        if (!send_locked_to(fd, hr.response_type, hr.payload_len)) keep_open = false;
      }
      if (hr.close_after) keep_open = false;
    }
    xSemaphoreGive(g_tx_mutex);
  }
  if (is_handshake) return keep_open;

  bt_handlers::Context bctx{fd, subs};
  if (!bt_handlers::handle(msg_type, rx_payload, payload_len, bctx)) {
    ESP_LOGD(TAG, "fd=%d unhandled msg_type=%u (len=%zu)", fd, msg_type,
             payload_len);
  }
  return true;
}

// Per-connection task. One per active client; exits on disconnect.
void connection_task(void *arg) {
  int client_fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  ESP_LOGI(TAG, "client connected (fd=%d, total=%u)", client_fd,
           g_active_count.load(std::memory_order_acquire));

  // Per-connection state. The RX buffer and ClientSubs both live on
  // this task's stack — gone when the task exits.
  uint8_t rx_payload[proxy::MAX_MESSAGE_SIZE];
  bt_handlers::ClientSubs subs;

  while (dispatch_one(client_fd, rx_payload, sizeof(rx_payload), &subs)) {
    // loop
  }

  // Release this client's subscription refs FIRST so any in-flight
  // async sends to other clients don't keep targeting our (now-closing)
  // fd. on_last_client_disconnect handles the global GATT teardown.
  bt_handlers::on_client_disconnect(subs);
  unregister_client(client_fd);
  ESP_LOGI(TAG, "client disconnected (fd=%d, remaining=%u)", client_fd,
           g_active_count.load(std::memory_order_acquire));
  if (g_active_count.load(std::memory_order_acquire) == 0) {
    bt_handlers::on_last_client_disconnect();
  }

  ::shutdown(client_fd, SHUT_RDWR);
  ::close(client_fd);
  vTaskDelete(nullptr);
}

void listener_task(void *) {
  int listen_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_fd < 0) {
    ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
    vTaskDelete(nullptr);
    return;
  }
  int one = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(proxy::API_PORT);
  if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
    ::close(listen_fd);
    vTaskDelete(nullptr);
    return;
  }
  if (::listen(listen_fd, proxy::MAX_API_CLIENTS) < 0) {
    ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
    ::close(listen_fd);
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(TAG, "listening on :%u (max_clients=%u)", proxy::API_PORT,
           proxy::MAX_API_CLIENTS);

  while (true) {
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &plen);
    if (client_fd < 0) {
      ESP_LOGW(TAG, "accept() failed: errno=%d", errno);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    // Visibility: print the moment accept() returns, before any task
    // scheduling delay. If "accepted fd=X" appears with no matching
    // "client connected" later, xTaskCreate was the holdup.
    ESP_LOGI(TAG, "accepted fd=%d peer=%s:%u", client_fd,
             inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (!register_client(client_fd)) {
      ESP_LOGW(TAG, "no client slot; closing fd=%d", client_fd);
      ::close(client_fd);
      continue;
    }

    char name[24];
    std::snprintf(name, sizeof(name), "api_client%d", client_fd);
    if (xTaskCreate(&connection_task, name, CONN_STACK,
                    reinterpret_cast<void *>(static_cast<intptr_t>(client_fd)),
                    CONN_PRIO, nullptr) != pdPASS) {
      ESP_LOGE(TAG, "xTaskCreate for fd=%d failed", client_fd);
      unregister_client(client_fd);
      ::close(client_fd);
    }
  }
}

}  // namespace

bool has_active_client() {
  return g_active_count.load(std::memory_order_acquire) > 0;
}

bool send_async(uint16_t msg_type, EncodeFn encode, void *ctx) {
  if (g_active_count.load(std::memory_order_acquire) == 0) return false;
  if (g_tx_mutex == nullptr) return false;

  xSemaphoreTake(g_tx_mutex, portMAX_DELAY);
  uint8_t *resp_payload = &g_tx_buf[frame_codec::MAX_HEADER_LEN];
  size_t resp_cap = sizeof(g_tx_buf) - frame_codec::MAX_HEADER_LEN;
  size_t n = encode ? encode(ctx, resp_payload, resp_cap) : 0;
  bool any_sent = false;
  int dead[proxy::MAX_API_CLIENTS];
  uint8_t n_dead = 0;
  if (n > 0) {
    // Snapshot the fd list so the broadcast loop doesn't hold
    // g_clients_mutex while doing socket IO.
    int fds_snapshot[proxy::MAX_API_CLIENTS];
    uint8_t n_fds = 0;
    xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
    for (int fd : g_client_fds) {
      if (fd >= 0) fds_snapshot[n_fds++] = fd;
    }
    xSemaphoreGive(g_clients_mutex);
    for (uint8_t i = 0; i < n_fds; ++i) {
      if (send_locked_to(fds_snapshot[i], msg_type, n)) {
        any_sent = true;
      } else {
        // Peer is gone (EPIPE / ECONNRESET / EBADF). Without reaping,
        // the fd lingers in g_client_fds and the connection_task's
        // recv() may stay blocked until lwIP-level RST timeout — long
        // enough to leak the lwIP socket and trip ENFILE on accept().
        dead[n_dead++] = fds_snapshot[i];
      }
      // Note: prepend_header rewrites the header in g_tx_buf each call,
      // but the payload area is unchanged so re-sending is safe.
    }
  }
  xSemaphoreGive(g_tx_mutex);

  // Force the connection_task's recv() to wake by shutdown()-ing the
  // socket. Its dispatch loop then returns false and runs the normal
  // cleanup path (unregister_client + close + vTaskDelete). We do NOT
  // close here — that would race with the connection_task and risk
  // operating on a recycled fd if another connect lands first.
  //
  // Take g_clients_mutex and re-check membership before shutdown so we
  // don't tear down an fd the connection_task already retired (rare,
  // but the kernel could reissue the same numeric fd to a new client
  // between our send-failure and now).
  if (n_dead > 0) {
    xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < n_dead; ++i) {
      bool still_ours = false;
      for (int fd : g_client_fds) {
        if (fd == dead[i]) {
          still_ours = true;
          break;
        }
      }
      if (still_ours) {
        ::shutdown(dead[i], SHUT_RDWR);
        ESP_LOGW(TAG, "send failed on fd=%d — shutdown to reap", dead[i]);
      }
    }
    xSemaphoreGive(g_clients_mutex);
  }
  return any_sent;
}

void start() {
  if (g_started) return;
  g_started = true;
  g_tx_mutex = xSemaphoreCreateMutex();
  g_clients_mutex = xSemaphoreCreateMutex();
  for (auto &s : g_client_fds) s = -1;
  xTaskCreate(&listener_task, "api_listener", LISTENER_STACK, nullptr,
              LISTENER_PRIO, nullptr);
}

}  // namespace api_server
