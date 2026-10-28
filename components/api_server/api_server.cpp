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

// Every send to a client runs under g_tx_mutex, and send_async is called
// from the NimBLE host task. Without a bound, one client that stops
// reading (stalled WiFi, suspended process) fills its lwIP send buffer
// and blocks send() forever: the host task blocks on g_tx_mutex, and BLE
// stops for every client. With SO_SNDTIMEO a send that makes no progress
// for this long fails, and the failing fd is reaped (shutdown → its
// connection_task exits → its BLE links are released). Long enough to
// ride out TCP retransmission backoff on a lossy but live link.
constexpr int CLIENT_SEND_TIMEOUT_S = 5;

// TCP keepalive so a half-open client (host crashed or lost power, no
// FIN/RST) is detected even when we send it nothing: ~30 s idle + 3 × 10 s
// probes → recv() fails, the connection_task exits and releases the
// client's BLE links instead of holding them indefinitely.
constexpr int CLIENT_KEEPIDLE_S = 30;
constexpr int CLIENT_KEEPINTVL_S = 10;
constexpr int CLIENT_KEEPCNT = 3;

bool g_started = false;

// Active clients. fd -1 in unused slots. Guarded by g_clients_mutex.
// Read by send_async (broadcast), send_to (targeted) and
// has_active_client.
//
// `id` is a monotonic per-connection id (never 0, never reused while the
// device is up). BLE links are owned by id, not fd: lwIP reissues the
// lowest free fd, so a reconnecting client usually gets the fd the old
// one had, and an fd-keyed owner would hand it the old client's links
// and late callbacks.
struct ClientEntry {
  int fd = -1;
  uint32_t id = 0;
};
std::array<ClientEntry, proxy::MAX_API_CLIENTS> g_clients;
std::atomic<uint32_t> g_next_client_id{1};
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
      // EAGAIN/EWOULDBLOCK here = SO_SNDTIMEO expired with no progress:
      // the client stopped reading. Caller reaps the fd.
      ESP_LOGW(TAG, "send fd=%d failed: errno=%d%s", fd, errno,
               (errno == EAGAIN || errno == EWOULDBLOCK) ? " (send timeout)"
                                                         : "");
      return false;
    }
    sent += static_cast<size_t>(r);
  }
  return true;
}

// Add/remove a client fd from the active set. Returns the client's id,
// or 0 if no slot is free; in that case the caller should reject the
// connection.
uint32_t register_client(int fd) {
  uint32_t id = 0;
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (auto &s : g_clients) {
    if (s.fd < 0) {
      id = g_next_client_id.fetch_add(1, std::memory_order_relaxed);
      if (id == 0) id = g_next_client_id.fetch_add(1, std::memory_order_relaxed);
      s.fd = fd;
      s.id = id;
      break;
    }
  }
  xSemaphoreGive(g_clients_mutex);
  if (id != 0) g_active_count.fetch_add(1, std::memory_order_release);
  return id;
}

// Caller holds g_tx_mutex (see connection_task), so no send_async /
// send_to is mid-write to this fd when it leaves the set and is closed.
void unregister_client(int fd) {
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (auto &s : g_clients) {
    if (s.fd == fd) {
      s.fd = -1;
      s.id = 0;
      break;
    }
  }
  xSemaphoreGive(g_clients_mutex);
  g_active_count.fetch_sub(1, std::memory_order_release);
}

// Shut down fds whose send failed so their connection_task's recv()
// wakes and runs the normal cleanup path (unregister + close +
// vTaskDelete). We do NOT close here — that would race with the
// connection_task and risk operating on a recycled fd.
//
// Re-check membership under g_clients_mutex before shutdown so we don't
// tear down an fd the connection_task already retired.
void reap_dead(const int *dead, uint8_t n_dead) {
  if (n_dead == 0) return;
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (uint8_t i = 0; i < n_dead; ++i) {
    bool still_ours = false;
    for (const auto &c : g_clients) {
      if (c.fd == dead[i]) {
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

bool dispatch_one(int fd, uint32_t client_id, uint8_t *rx_payload,
                  size_t rx_cap, bt_handlers::ClientSubs *subs) {
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

  bt_handlers::Context bctx{fd, client_id, subs};
  if (!bt_handlers::handle(msg_type, rx_payload, payload_len, bctx)) {
    ESP_LOGD(TAG, "fd=%d unhandled msg_type=%u (len=%zu)", fd, msg_type,
             payload_len);
  }
  return true;
}

// Id registered for `fd` (registered before its task is created, so
// always present while the task runs).
uint32_t id_for_fd(int fd) {
  uint32_t id = 0;
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (const auto &c : g_clients) {
    if (c.fd == fd) {
      id = c.id;
      break;
    }
  }
  xSemaphoreGive(g_clients_mutex);
  return id;
}

// Per-connection task. One per active client; exits on disconnect.
void connection_task(void *arg) {
  int client_fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  const uint32_t client_id = id_for_fd(client_fd);
  ESP_LOGI(TAG, "client connected (fd=%d, id=%lu, total=%u)", client_fd,
           static_cast<unsigned long>(client_id),
           g_active_count.load(std::memory_order_acquire));

  // Per-connection state. The RX buffer and ClientSubs both live on
  // this task's stack — gone when the task exits.
  uint8_t rx_payload[proxy::MAX_MESSAGE_SIZE];
  bt_handlers::ClientSubs subs;

  while (dispatch_one(client_fd, client_id, rx_payload, sizeof(rx_payload),
                      &subs)) {
    // loop
  }

  // Release this client's subscription refs FIRST so any in-flight
  // async sends to other clients don't keep targeting our (now-closing)
  // fd.
  bt_handlers::on_client_disconnect(subs);
  // Leave the client set under g_tx_mutex: any broadcast/targeted send
  // that snapshotted our fd finishes first, and none starts afterwards,
  // so nothing writes to this fd once it is closed and possibly reissued
  // to a new client. Lock order g_tx_mutex → g_clients_mutex matches
  // send_async / send_to.
  xSemaphoreTake(g_tx_mutex, portMAX_DELAY);
  unregister_client(client_fd);
  xSemaphoreGive(g_tx_mutex);
  ESP_LOGI(TAG, "client disconnected (fd=%d, id=%lu, remaining=%u)",
           client_fd, static_cast<unsigned long>(client_id),
           g_active_count.load(std::memory_order_acquire));
  // Drop exactly the BLE links this client opened, even though other
  // clients stay connected. After unregister, so any state callback for
  // them finds no recipient; the remaining clients get ConnectionsFree.
  bt_handlers::on_client_links_release(client_id);
  // Backstop: with nobody left, drop everything (incl. anything orphaned).
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
    timeval snd_to{};
    snd_to.tv_sec = CLIENT_SEND_TIMEOUT_S;
    ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
    int keepidle = CLIENT_KEEPIDLE_S;
    int keepintvl = CLIENT_KEEPINTVL_S;
    int keepcnt = CLIENT_KEEPCNT;
    ::setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    uint32_t client_id = register_client(client_fd);
    if (client_id == 0) {
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
    for (const auto &c : g_clients) {
      if (c.fd >= 0) fds_snapshot[n_fds++] = c.fd;
    }
    xSemaphoreGive(g_clients_mutex);
    for (uint8_t i = 0; i < n_fds; ++i) {
      if (send_locked_to(fds_snapshot[i], msg_type, n)) {
        any_sent = true;
      } else {
        // Peer is gone (EPIPE / ECONNRESET / EBADF). Without reaping,
        // the fd lingers in g_clients and the connection_task's
        // recv() may stay blocked until lwIP-level RST timeout — long
        // enough to leak the lwIP socket and trip ENFILE on accept().
        dead[n_dead++] = fds_snapshot[i];
      }
      // Note: prepend_header rewrites the header in g_tx_buf each call,
      // but the payload area is unchanged so re-sending is safe.
    }
  }
  xSemaphoreGive(g_tx_mutex);
  reap_dead(dead, n_dead);
  return any_sent;
}

bool send_to(uint32_t client_id, uint16_t msg_type, EncodeFn encode,
             void *ctx) {
  if (client_id == 0) return false;
  if (g_tx_mutex == nullptr) return false;

  xSemaphoreTake(g_tx_mutex, portMAX_DELAY);
  // Resolve id → fd under g_tx_mutex: connection_task unregisters while
  // holding it, so the fd cannot leave the set (and be closed/reissued)
  // until this send is done.
  int fd = -1;
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (const auto &c : g_clients) {
    if (c.fd >= 0 && c.id == client_id) {
      fd = c.fd;
      break;
    }
  }
  xSemaphoreGive(g_clients_mutex);
  bool sent = false;
  int dead = -1;
  if (fd >= 0) {
    uint8_t *resp_payload = &g_tx_buf[frame_codec::MAX_HEADER_LEN];
    size_t resp_cap = sizeof(g_tx_buf) - frame_codec::MAX_HEADER_LEN;
    size_t n = encode ? encode(ctx, resp_payload, resp_cap) : 0;
    if (n > 0) {
      sent = send_locked_to(fd, msg_type, n);
      if (!sent) dead = fd;
    }
  }
  xSemaphoreGive(g_tx_mutex);
  if (dead >= 0) reap_dead(&dead, 1);
  return sent;
}

void start() {
  if (g_started) return;
  g_started = true;
  g_tx_mutex = xSemaphoreCreateMutex();
  g_clients_mutex = xSemaphoreCreateMutex();
  for (auto &s : g_clients) s = ClientEntry{};
  xTaskCreate(&listener_task, "api_listener", LISTENER_STACK, nullptr,
              LISTENER_PRIO, nullptr);
}

}  // namespace api_server
