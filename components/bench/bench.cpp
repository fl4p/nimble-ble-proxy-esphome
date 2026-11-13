// Network/heap benchmark server, CONFIG_NBP_BENCH (default n).
//
// Its own task and its own listener (CONFIG_NBP_BENCH_PORT, default 8081)
// rather than routes on the dashboard httpd: that server handles one request
// at a time, so a long bench transfer would block the /stats.json polling
// that measures the heap during the test.
//
//   GET  /bench?bytes=N    streams N bytes (Content-Length, no chunking)
//   POST /bench            drains the request body, replies JSON
//   GET  /benchmin         heap now / low-water / largest block, UDP sink
//                          counters; ?reset=1 restarts the low-water mark
//                          and zeroes the counters after reporting them
//
// UDP sink on CONFIG_NBP_BENCH_UDP_PORT (default 5009): datagrams are read
// and counted, nothing is sent back (a closed port would answer each one
// with ICMP and load the TX path as well).
//
// The low-water mark uses heap_caps_monitor_local_minimum_free_size_start(),
// which resets every heap's minimum_free_bytes while keeping the since-boot
// value aside. So in a bench build /stats.json `heap_min` is the minimum
// since the last ?reset=1, not since boot.
//
// Sockets: one TCP listener and one UDP socket, plus one accepted
// connection at a time (CONFIG_LWIP_MAX_SOCKETS is 16). One connection at a
// time also means the UDP sink is not read while a TCP transfer runs.
#include "bench.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace bench {
namespace {

constexpr const char *TAG = "bench";
constexpr size_t kBufSize = 2048;
constexpr size_t kMaxHeader = 1024;
constexpr uint64_t kMaxBytes = 256ull * 1024 * 1024;

uint8_t g_buf[kBufSize];
char g_hdr[kMaxHeader + 1];

uint32_t g_udp_pkts = 0;
uint64_t g_udp_bytes = 0;
int64_t g_udp_first_us = 0;
int64_t g_udp_last_us = 0;
uint64_t g_tcp_rx = 0;
uint64_t g_tcp_tx = 0;
int64_t g_window_start_us = 0;

bool send_all(int fd, const void *data, size_t len) {
  auto p = static_cast<const uint8_t *>(data);
  while (len > 0) {
    int n = send(fd, p, len, 0);
    if (n <= 0) return false;
    p += n;
    len -= static_cast<size_t>(n);
    g_tcp_tx += static_cast<uint64_t>(n);
  }
  return true;
}

void reply(int fd, const char *status, const char *type, const char *body) {
  char head[160];
  int n = std::snprintf(head, sizeof(head),
                        "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
                        "Content-Length: %u\r\nConnection: close\r\n\r\n",
                        status, type, static_cast<unsigned>(std::strlen(body)));
  if (send_all(fd, head, static_cast<size_t>(n))) send_all(fd, body, std::strlen(body));
}

// Value of `key=` in the query string, or `def`.
uint64_t query_u64(const char *path, const char *key, uint64_t def) {
  const char *q = std::strchr(path, '?');
  if (q == nullptr) return def;
  size_t klen = std::strlen(key);
  for (const char *p = q + 1; *p != '\0';) {
    if (std::strncmp(p, key, klen) == 0 && p[klen] == '=') {
      return std::strtoull(p + klen + 1, nullptr, 10);
    }
    const char *amp = std::strchr(p, '&');
    if (amp == nullptr) break;
    p = amp + 1;
  }
  return def;
}

bool path_is(const char *path, const char *want) {
  size_t n = std::strlen(want);
  return std::strncmp(path, want, n) == 0 && (path[n] == '\0' || path[n] == '?');
}

void handle_stats(int fd, const char *path) {
  char body[384];
  int64_t now = esp_timer_get_time();
  std::snprintf(
      body, sizeof(body),
      "{\"heap\":%u,\"heap_min\":%u,\"heap_largest\":%u,\"window_ms\":%" PRId64
      ",\"udp_pkts\":%" PRIu32 ",\"udp_bytes\":%" PRIu64
      ",\"udp_span_ms\":%" PRId64 ",\"tcp_rx\":%" PRIu64 ",\"tcp_tx\":%" PRIu64 "}",
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
      (now - g_window_start_us) / 1000, g_udp_pkts, g_udp_bytes,
      g_udp_pkts > 1 ? (g_udp_last_us - g_udp_first_us) / 1000 : 0, g_tcp_rx,
      g_tcp_tx);
  reply(fd, "200 OK", "application/json", body);
  if (query_u64(path, "reset", 0) != 0) {
    // values[] was allocated by the first start() in task start, so this
    // only resets the per-heap minimum; it cannot fail for want of heap.
    heap_caps_monitor_local_minimum_free_size_start();
    g_udp_pkts = 0;
    g_udp_bytes = 0;
    g_udp_first_us = g_udp_last_us = 0;
    g_tcp_rx = g_tcp_tx = 0;
    g_window_start_us = esp_timer_get_time();
  }
}

void handle_get(int fd, const char *path) {
  uint64_t n = query_u64(path, "bytes", 0);
  if (n == 0 || n > kMaxBytes) {
    reply(fd, "400 Bad Request", "text/plain", "bytes=1..268435456\n");
    return;
  }
  char head[160];
  int h = std::snprintf(head, sizeof(head),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                        "Content-Length: %" PRIu64 "\r\nConnection: close\r\n\r\n",
                        n);
  if (!send_all(fd, head, static_cast<size_t>(h))) return;
  while (n > 0) {
    size_t chunk = n < kBufSize ? static_cast<size_t>(n) : kBufSize;
    if (!send_all(fd, g_buf, chunk)) return;
    n -= chunk;
  }
}

// `already` = body bytes that arrived with the header.
void handle_post(int fd, uint64_t content_len, size_t already) {
  int64_t t0 = esp_timer_get_time();
  uint64_t got = already;
  g_tcp_rx += already;
  while (got < content_len) {
    int r = recv(fd, g_buf, kBufSize, 0);
    if (r <= 0) return;
    got += static_cast<uint64_t>(r);
    g_tcp_rx += static_cast<uint64_t>(r);
  }
  char body[96];
  std::snprintf(body, sizeof(body), "{\"bytes\":%" PRIu64 ",\"ms\":%" PRId64 "}\n", got,
                (esp_timer_get_time() - t0) / 1000);
  reply(fd, "200 OK", "application/json", body);
}

void handle_conn(int fd) {
  timeval tv = {.tv_sec = 10, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  size_t len = 0;
  char *end = nullptr;
  while (len < kMaxHeader) {
    int r = recv(fd, g_hdr + len, kMaxHeader - len, 0);
    if (r <= 0) return;
    len += static_cast<size_t>(r);
    g_hdr[len] = '\0';
    end = std::strstr(g_hdr, "\r\n\r\n");
    if (end != nullptr) break;
  }
  if (end == nullptr) {
    reply(fd, "431 Request Header Fields Too Large", "text/plain", "header\n");
    return;
  }
  size_t body_in_hdr = len - static_cast<size_t>(end + 4 - g_hdr);

  uint64_t content_len = 0;
  bool expect_continue = false;
  for (char *line = std::strstr(g_hdr, "\r\n"); line != nullptr && line < end;
       line = std::strstr(line + 2, "\r\n")) {
    if (strncasecmp(line + 2, "Content-Length:", 15) == 0) {
      content_len = std::strtoull(line + 2 + 15, nullptr, 10);
    } else if (strncasecmp(line + 2, "Expect: 100-continue", 20) == 0) {
      expect_continue = true;
    }
  }
  // curl sends `Expect: 100-continue` for large bodies and otherwise waits
  // 1 s before sending, which would show up as lost throughput.
  if (expect_continue && !send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25)) return;
  *end = '\0';
  char *sp1 = std::strchr(g_hdr, ' ');
  if (sp1 == nullptr) return;
  *sp1 = '\0';
  char *path = sp1 + 1;
  char *sp2 = std::strchr(path, ' ');
  if (sp2 != nullptr) *sp2 = '\0';

  if (std::strcmp(g_hdr, "GET") == 0 && path_is(path, "/bench")) {
    handle_get(fd, path);
  } else if (std::strcmp(g_hdr, "POST") == 0 && path_is(path, "/bench")) {
    handle_post(fd, content_len, body_in_hdr);
  } else if (std::strcmp(g_hdr, "GET") == 0 && path_is(path, "/benchmin")) {
    handle_stats(fd, path);
  } else {
    reply(fd, "404 Not Found", "text/plain", "not found\n");
  }
}

void drain_udp(int fd) {
  for (;;) {
    int r = recv(fd, g_buf, kBufSize, MSG_DONTWAIT);
    if (r < 0) return;
    int64_t now = esp_timer_get_time();
    if (g_udp_pkts == 0) g_udp_first_us = now;
    g_udp_last_us = now;
    ++g_udp_pkts;
    g_udp_bytes += static_cast<uint64_t>(r);
  }
}

void task(void *) {
  int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int ufd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (lfd < 0 || ufd < 0) {
    ESP_LOGE(TAG, "socket() failed: errno %d", errno);
    if (lfd >= 0) close(lfd);
    if (ufd >= 0) close(ufd);
    vTaskDelete(nullptr);
    return;
  }
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(CONFIG_NBP_BENCH_PORT);
  sockaddr_in u = a;
  u.sin_port = htons(CONFIG_NBP_BENCH_UDP_PORT);
  if (bind(lfd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || listen(lfd, 1) != 0 ||
      bind(ufd, reinterpret_cast<sockaddr *>(&u), sizeof(u)) != 0) {
    ESP_LOGE(TAG, "bind/listen failed: errno %d", errno);
    close(lfd);
    close(ufd);
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGW(TAG, "bench server on tcp:%d udp:%d (test build)", CONFIG_NBP_BENCH_PORT,
           CONFIG_NBP_BENCH_UDP_PORT);

  for (;;) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(lfd, &rf);
    FD_SET(ufd, &rf);
    timeval tv = {.tv_sec = 1, .tv_usec = 0};
    int n = select((lfd > ufd ? lfd : ufd) + 1, &rf, nullptr, nullptr, &tv);
    if (n <= 0) continue;
    if (FD_ISSET(ufd, &rf)) drain_udp(ufd);
    if (FD_ISSET(lfd, &rf)) {
      int fd = accept(lfd, nullptr, nullptr);
      if (fd >= 0) {
        handle_conn(fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
      }
    }
  }
}

}  // namespace

void start() {
  for (size_t i = 0; i < kBufSize; ++i) g_buf[i] = static_cast<uint8_t>('a' + i % 26);
  // First start() allocates its per-heap snapshot array; later ones reuse it.
  heap_caps_monitor_local_minimum_free_size_start();
  g_window_start_us = esp_timer_get_time();
  if (xTaskCreate(task, "bench", 4096, nullptr, 5, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "xTaskCreate failed");
  }
}

}  // namespace bench
