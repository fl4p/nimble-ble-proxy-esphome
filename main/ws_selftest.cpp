// QEMU-only self-test of the browser-facing CSRF guards (idf.py -B
// build-qemu-ws -DIDF_TARGET=esp32s3 -DNBP_WS_SELFTEST=1
// -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wstest" build,
// then `idf.py -B build-qemu-ws qemu`). Never in firmware: compiled only with
// that define, and it takes over app_main.
//
// Build with the sdkconfig.defaults.wstest overlay (see that file).
//
// Runs the real dashboard httpd (ota::start), the real api_server and the
// ws_proxy bridge on lwIP loopback, then drives them with raw sockets the
// way a browser would. Each step prints `WS-SELFTEST <name>: PASS|FAIL`.

#if NBP_WS_SELFTEST

#include "ws_selftest.h"

#include "api_server.h"
#include "connection.h"
#include "ota.h"
#include "stats.h"
#include "ws_proxy.h"

#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !CONFIG_NBP_WS_PROXY
#error "NBP_WS_SELFTEST needs CONFIG_NBP_WS_PROXY=y (the esp32s3 default)"
#endif

namespace ws_selftest {

namespace {

int g_fail = 0;

void result(const char *name, bool ok, const char *fmt = "", ...)
    __attribute__((format(printf, 3, 4)));
void result(const char *name, bool ok, const char *fmt, ...) {
  char detail[200];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);
  if (!ok) ++g_fail;
  std::printf("WS-SELFTEST %s: %s %s\n", name, ok ? "PASS" : "FAIL", detail);
}

int connect_httpd() {
  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return -1;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(80);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0) {
    ::close(fd);
    return -1;
  }
  timeval tv{.tv_sec = 3, .tv_usec = 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

// Reads the response head; returns the status code (0 on none).
int read_status(int fd) {
  char buf[512];
  size_t n = 0;
  while (n < sizeof(buf) - 1) {
    ssize_t r = ::recv(fd, buf + n, sizeof(buf) - 1 - n, 0);
    if (r <= 0) break;
    n += static_cast<size_t>(r);
    buf[n] = '\0';
    if (std::strstr(buf, "\r\n\r\n")) break;
  }
  buf[n] = '\0';
  if (std::strncmp(buf, "HTTP/1.1 ", 9) != 0) return 0;
  return std::atoi(buf + 9);
}

// WebSocket handshake to /api with an optional Origin. Returns the status;
// *out_fd keeps the socket open on 101 (caller closes), else -1.
int ws_open(const char *origin, const char *host, int *out_fd) {
  *out_fd = -1;
  int fd = connect_httpd();
  if (fd < 0) return -1;
  char req[400];
  int n = std::snprintf(
      req, sizeof(req),
      "GET /api HTTP/1.1\r\nHost: %s\r\nConnection: Upgrade\r\n"
      "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n%s%s%s\r\n",
      host, origin ? "Origin: " : "", origin ? origin : "",
      origin ? "\r\n" : "");
  ::send(fd, req, n, 0);
  int st = read_status(fd);
  if (st == 101) {
    *out_fd = fd;
  } else {
    ::close(fd);
  }
  return st;
}

// After the server refuses, it must close the socket (no bridge behind it).
bool peer_closed(int fd) {
  char c;
  ssize_t r = ::recv(fd, &c, 1, 0);
  return r == 0;
}

int http_post(const char *path, bool confirm) {
  int fd = connect_httpd();
  if (fd < 0) return -1;
  char req[300];
  int n = std::snprintf(req, sizeof(req),
                        "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                        "Content-Length: 0\r\n%s\r\n",
                        path, confirm ? "X-NBP-Confirm: yes\r\n" : "");
  ::send(fd, req, n, 0);
  int st = read_status(fd);
  ::close(fd);
  return st;
}

// Masked client WS binary frame carrying one plaintext ESPHome message
// (0x00, len, type) with an empty body: HelloRequest = 1.
void ws_send_hello(int fd) {
  const uint8_t payload[3] = {0x00, 0x00, 0x01};
  uint8_t f[2 + 4 + sizeof(payload)];
  f[0] = 0x82;  // FIN + binary
  f[1] = 0x80 | sizeof(payload);
  const uint8_t mask[4] = {1, 2, 3, 4};
  std::memcpy(f + 2, mask, 4);
  for (size_t i = 0; i < sizeof(payload); ++i) f[6 + i] = payload[i] ^ mask[i % 4];
  ::send(fd, f, sizeof(f), 0);
}

bool recv_all(int fd, uint8_t *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::recv(fd, buf + got, n - got, 0);
    if (r <= 0) return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

// Reads one server WS frame (httpd sends header and payload as separate
// segments); returns the ESPHome message type of the first message in it,
// or -1.
int ws_recv_type(int fd) {
  uint8_t hdr[4];
  if (!recv_all(fd, hdr, 2) || (hdr[0] & 0x0f) != 0x2) return -1;
  size_t len = hdr[1] & 0x7f;
  if (len == 126) {
    if (!recv_all(fd, hdr + 2, 2)) return -1;
    len = (static_cast<size_t>(hdr[2]) << 8) | hdr[3];
  } else if (len == 127) {
    return -1;
  }
  uint8_t buf[512];
  if (len < 3 || len > sizeof(buf) || !recv_all(fd, buf, len)) return -1;
  // ESPHome plaintext: 0x00, varint length, varint type.
  if (buf[0] != 0x00) return -1;
  size_t off = 1;
  while (off < len && (buf[off] & 0x80)) ++off;  // skip the length varint
  ++off;
  return off < len ? buf[off] : -1;
}

uint32_t refused_count() {
  char b[96];
  ws_proxy::append_stats_json(b, sizeof(b));
  const char *p = std::strstr(b, "\"ws_origin_refused\":");
  return p ? static_cast<uint32_t>(std::strtoul(p + 20, nullptr, 10)) : 0xffffffff;
}

unsigned sessions() {
  char b[96];
  ws_proxy::append_stats_json(b, sizeof(b));
  const char *p = std::strstr(b, "\"ws_sessions\":");
  return p ? static_cast<unsigned>(std::strtoul(p + 14, nullptr, 10)) : 999;
}

void step_refused(const char *name, const char *origin, const char *host) {
  uint32_t before = refused_count();
  // Hand-rolled rather than ws_open() so the close can be checked too.
  int s = connect_httpd();
  if (s < 0) {
    result(name, false, "connect failed");
    return;
  }
  char req[400];
  int n = std::snprintf(
      req, sizeof(req),
      "GET /api HTTP/1.1\r\nHost: %s\r\nConnection: Upgrade\r\n"
      "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nOrigin: %s\r\n\r\n",
      host, origin);
  ::send(s, req, n, 0);
  int st = read_status(s);
  bool closed = peer_closed(s);
  ::close(s);
  vTaskDelay(pdMS_TO_TICKS(50));
  result(name, st == 403 && closed && refused_count() == before + 1 &&
                   sessions() == 0,
         "status %d, closed %d, refused %lu -> %lu, sessions %u", st, closed,
         static_cast<unsigned long>(before),
         static_cast<unsigned long>(refused_count()), sessions());
}

void step_accepted(const char *name, const char *origin, const char *host) {
  int fd;
  int st = ws_open(origin, host, &fd);
  if (st != 101) {
    result(name, false, "status %d", st);
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(50));
  unsigned live = sessions();
  ws_send_hello(fd);
  int type = ws_recv_type(fd);
  ::close(fd);
  vTaskDelay(pdMS_TO_TICKS(300));
  unsigned after = sessions();
  // HelloResponse = 2: the loopback bridge carried a real API round-trip.
  result(name, live == 1 && type == 2 && after == 0,
         "101, sessions %u, reply type %d, sessions after close %u", live, type,
         after);
}

}  // namespace

void run() {
  ESP_ERROR_CHECK(esp_netif_init());
  // Only the slot-table mutex: a closing API client releases its
  // (nonexistent) BLE links through it. NimBLE itself stays off.
  ble_backend::connection::init();
  ota::start();
  api_server::stats::register_endpoints(ota::handle());
  api_server::start();
  ws_proxy::register_endpoint(ota::handle());
  vTaskDelay(pdMS_TO_TICKS(300));

  // Hold one plain API client for the whole run. Without NimBLE, the last
  // API client leaving calls scanner::stop_forwarding() on a mutex that
  // only scanner::init() (which needs NimBLE) creates.
  int sentinel = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(6053);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::connect(sentinel, reinterpret_cast<sockaddr *>(&a), sizeof(a));
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  step_refused("forged-origin", "http://evil.example", "127.0.0.1");
  step_refused("null-origin", "null", "127.0.0.1");
  step_refused("lookalike-origin", "http://127.0.0.1.evil.example", "127.0.0.1");
  step_refused("port-mismatch", "http://127.0.0.1:8080", "127.0.0.1");
  step_accepted("no-origin", nullptr, "127.0.0.1");
  step_accepted("same-origin", "http://127.0.0.1", "127.0.0.1");
  step_accepted("same-origin-hostname", "http://Nimble-Proxy.local",
                "nimble-proxy.local");

  // Session cap: two bridges open, the third handshake gets 503, not 101.
  // Needs sdkconfig.defaults.wstest (3 API slots: sentinel + 2 bridges).
#if CONFIG_NBP_API_MAX_CLIENTS < 3
  result("session-cap", false, "build with sdkconfig.defaults.wstest");
#else
  {
    int a, b, c;
    int sa = ws_open(nullptr, "127.0.0.1", &a);
    int sb = ws_open(nullptr, "127.0.0.1", &b);
    int sc = ws_open(nullptr, "127.0.0.1", &c);
    if (c >= 0) ::close(c);
    if (a >= 0) ::close(a);
    if (b >= 0) ::close(b);
    vTaskDelay(pdMS_TO_TICKS(300));
    result("session-cap", sa == 101 && sb == 101 && sc == 503 && sessions() == 0,
           "%d %d %d, sessions after %u", sa, sb, sc, sessions());
  }
#endif

  // A refused handshake leaks nothing. Both ends of every test connection
  // live on this device, so each closed socket leaves a TIME_WAIT PCB on
  // the heap for 2 MSL. A plain GET loop pays the same; compare against it
  // and require the refused loop to stop costing heap once lwIP recycles
  // TIME_WAIT PCBs.
  {
    auto heap = [] { return heap_caps_get_free_size(MALLOC_CAP_DEFAULT); };
    auto refused30 = [] {
      for (int i = 0; i < 30; ++i) {
        int fd;
        ws_open("http://evil.example", "127.0.0.1", &fd);
        if (fd >= 0) ::close(fd);
      }
      vTaskDelay(pdMS_TO_TICKS(300));
    };
    auto get30 = [] {
      for (int i = 0; i < 30; ++i) {
        int fd = connect_httpd();
        if (fd < 0) continue;
        const char r[] = "GET /level HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        ::send(fd, r, sizeof(r) - 1, 0);
        read_status(fd);
        ::close(fd);
      }
      vTaskDelay(pdMS_TO_TICKS(300));
    };
    UBaseType_t tasks0 = uxTaskGetNumberOfTasks();
    long h0 = heap();
    get30();
    long h1 = heap();
    get30();
    long h2 = heap();
    refused30();
    long h3 = heap();
    refused30();
    long h4 = heap();
    refused30();
    long h5 = heap();
    UBaseType_t tasks1 = uxTaskGetNumberOfTasks();
    long last = h4 - h5;
    result("refused-no-leak",
           tasks1 == tasks0 && last < 256 && sessions() == 0,
           "heap cost per 30: GET %ld, %ld; refused ws %ld, %ld, %ld B; "
           "tasks %u -> %u",
           h0 - h1, h1 - h2, h2 - h3, h3 - h4, last,
           static_cast<unsigned>(tasks0), static_cast<unsigned>(tasks1));
  }

  // POST confirm header (include/http_confirm.h).
  int s0 = http_post("/level?nimble=3", false);
  int s1 = http_post("/reboot", false);
  int s2 = http_post("/update", false);
  int s3 = http_post("/level?nimble=3", true);
  result("post-confirm",
         s0 == 400 && s1 == 400 && s2 == 400 && s3 == 200,
         "/level w/o %d, /reboot w/o %d, /update w/o %d, /level with %d", s0,
         s1, s2, s3);

  (void)sentinel;  // stays open: see above
  std::printf("WS-SELFTEST summary: %d failure(s)\n", g_fail);
  for (;;) vTaskDelay(portMAX_DELAY);
}

}  // namespace ws_selftest

#endif  // NBP_WS_SELFTEST
