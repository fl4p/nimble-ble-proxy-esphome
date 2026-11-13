// QEMU-only heap self-test (idf.py -DNBP_HEAP_SELFTEST=1 …). Never in
// firmware: compiled only with that define, and it takes over app_main.
//
// Runs the real api_server on lwIP loopback (no WiFi, no BLE in QEMU) and
// checks the heap-pressure fixes of 2026-11:
//   boot_race     GET /stats.json's builder before BLE init (null mutex).
//   refuse_extra  a client beyond NBP_API_MAX_CLIENTS is reset at once.
//   stall_lossy   client A stops ACKing (an lwIP input hook drops every
//                 segment to its port, so data is sent and never ACKed, as
//                 on a stalled WiFi link) while advert batches go to all and
//                 notifications to client B: B keeps receiving, A's queue
//                 stays at the cap, A is reaped, the heap stays bounded.
//   stall_strict  the same with non-lossy frames to the stalled client: the
//                 send waits, then reaps it at the deadline.
// Every step prints `HEAP-SELFTEST <name>: PASS|FAIL <detail>`; numbers are
// printed as `HEAP-MEASURE <what> <value>`. Build with
// -DNBP_API_CLIENT_TXQ_CAP=0 in sdkconfig to measure the uncapped behaviour.

#if NBP_HEAP_SELFTEST

#include "heap_selftest.h"

#include "api_proto.h"
#include "api_server.h"
#include "frame_codec.h"
#include "proxy_config.h"
#include "stats.h"

#include "../ble_backend/connection.h"
#include "../ble_backend/publish.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip.h"
#include "lwip/ip4.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "lwip/sockets.h"

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
std::atomic<uint16_t> g_drop_port{0};  // network order; 0 = drop nothing
std::atomic<uint32_t> g_dropped{0};
}  // namespace

// lwIP IP4 input hook (nbp_lwip_hooks.h, self-test builds only). Runs in the
// tcpip thread. Eats TCP segments whose destination port is g_drop_port.
extern "C" int nbp_selftest_ip4_input(struct pbuf *p, struct netif *) {
  const uint16_t port = g_drop_port.load(std::memory_order_relaxed);
  if (port == 0 || p == nullptr || p->len < IP_HLEN) return 0;
  const auto *ip = static_cast<const ip_hdr *>(p->payload);
  if (IPH_PROTO(ip) != IP_PROTO_TCP) return 0;
  const uint16_t hl = IPH_HL_BYTES(ip);
  if (p->len < hl + TCP_HLEN) return 0;
  const auto *tcp = reinterpret_cast<const tcp_hdr *>(
      static_cast<const uint8_t *>(p->payload) + hl);
  if (tcp->dest != port) return 0;
  g_dropped.fetch_add(1, std::memory_order_relaxed);
  pbuf_free(p);
  return 1;
}

namespace api_server::heap_selftest {

namespace {

constexpr const char *TAG = "heaptest";
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
  std::printf("HEAP-SELFTEST %s: %s %s\n", name, ok ? "PASS" : "FAIL", detail);
}

void measure(const char *what, long v) {
  std::printf("HEAP-MEASURE %s %ld\n", what, v);
}

size_t free_now() { return heap_caps_get_free_size(MALLOC_CAP_DEFAULT); }
size_t largest_now() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
}

int connect_loopback() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(proxy::API_PORT);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

uint16_t local_port_net(int fd) {
  sockaddr_in a{};
  socklen_t l = sizeof(a);
  ::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &l);
  return a.sin_port;
}

void set_rcv_timeout(int fd, int ms) {
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

uint8_t wait_clients(uint8_t n, int ms) {
  uint8_t c = 0;
  for (int t = 0; t < ms; t += 20) {
    c = api_server::txq_stats().n_clients;
    if (c == n) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return c;
}

// ---- client B: a healthy reader in its own task ----
int g_fd_b = -1;
std::atomic<uint32_t> g_b_frames{0};
std::atomic<uint32_t> g_b_notifies{0};
std::atomic<bool> g_b_run{true};
uint8_t g_b_rx[proxy::MAX_MESSAGE_SIZE];

ssize_t sock_read(void *ctx, uint8_t *buf, size_t n) {
  return ::recv(*static_cast<int *>(ctx), buf, n, 0);
}

void reader_b(void *) {
  while (g_b_run.load()) {
    uint16_t t = 0;
    size_t len = 0;
    const auto e = frame_codec::read_frame(&sock_read, &g_fd_b, g_b_rx,
                                           sizeof(g_b_rx), &t, &len);
    if (e != frame_codec::Error::Ok) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      break;
    }
    g_b_frames.fetch_add(1);
    if (t == proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE) {
      g_b_notifies.fetch_add(1);
    }
  }
  vTaskDelete(nullptr);
}

// ---- payload encoders (content is arbitrary; only framing matters) ----
size_t encode_fill(void *ctx, uint8_t *buf, size_t cap) {
  size_t n = *static_cast<size_t *>(ctx);
  if (n > cap) n = cap;
  std::memset(buf, 0x5A, n);
  return n;
}

size_t kAdvBytes = 600;    // a typical 8-10 advert batch
size_t kNotifyBytes = 60;  // a BMS notification frame

// Heap low-water tracking across a phase (sampled, plus the send calls).
size_t g_phase_min = SIZE_MAX;
void sample() {
  const size_t f = free_now();
  if (f < g_phase_min) g_phase_min = f;
}

void print_tasks() {
  static TaskStatus_t st[32];  // main's stack is small
  UBaseType_t n = uxTaskGetSystemState(st, 32, nullptr);
  for (UBaseType_t i = 0; i < n; ++i) {
    std::printf("HEAP-MEASURE stack_free_min[%s] %u\n", st[i].pcTaskName,
                static_cast<unsigned>(st[i].usStackHighWaterMark));
  }
}

// ---- steps ----

void step_boot_race() {
  // Before connection::init(): g_mutex is null, as in the boot window
  // between httpd start and ble_backend::start(). Used to assert.
  static char buf[1024];
  const size_t n = api_server::stats::build_stats_json(buf, sizeof(buf));
  const uint8_t freeslots = ble_backend::connection::free_slots();
  uint64_t addrs[proxy::MAX_CONNECTIONS];
  const uint8_t in_use =
      ble_backend::connection::in_use_addresses(addrs, proxy::MAX_CONNECTIONS);
  ble_backend::connection::release_owner(1);
  ble_backend::connection::disconnect_unowned();
  const bool cf = ble_backend::connection::client_for(1, 1) == nullptr;
  const bool has_conn0 = std::strstr(buf, "\"connections\":0") != nullptr;
  result("boot_race", n > 0 && has_conn0 && freeslots == proxy::MAX_CONNECTIONS &&
                          in_use == 0 && cf,
         "stats.json %u B, connections:0 %s, free_slots %u, in_use %u",
         static_cast<unsigned>(n), has_conn0 ? "yes" : "no", freeslots, in_use);
  std::printf("HEAP-STATS %s\n", buf);
}

int g_fd_a = -1;

void step_clients_and_refuse() {
  const size_t f0 = free_now();
  g_fd_a = connect_loopback();
  const uint8_t c1 = wait_clients(1, 2000);
  vTaskDelay(pdMS_TO_TICKS(200));
  const size_t f1 = free_now();
  g_fd_b = connect_loopback();
  const uint8_t c2 = wait_clients(2, 2000);
  vTaskDelay(pdMS_TO_TICKS(200));
  const size_t f2 = free_now();
  measure("heap_per_client_a", static_cast<long>(f0) - static_cast<long>(f1));
  measure("heap_per_client_b", static_cast<long>(f1) - static_cast<long>(f2));
  result("two_clients", g_fd_a >= 0 && g_fd_b >= 0 && c1 == 1 && c2 == 2,
         "clients %u then %u; heap %u -> %u -> %u", c1, c2,
         static_cast<unsigned>(f0), static_cast<unsigned>(f1),
         static_cast<unsigned>(f2));

  // Third client: accepted by lwIP, then reset by the listener.
  const uint32_t r0 = api_server::txq_stats().refused;
  const int fd_c = connect_loopback();
  set_rcv_timeout(fd_c, 3000);
  uint8_t b = 0;
  const int64_t t0 = esp_timer_get_time();
  const ssize_t r = fd_c >= 0 ? ::recv(fd_c, &b, 1, 0) : -1;
  const int e = errno;
  const int64_t ms = (esp_timer_get_time() - t0) / 1000;
  if (fd_c >= 0) ::close(fd_c);
  vTaskDelay(pdMS_TO_TICKS(100));
  const api_server::TxqStats t = api_server::txq_stats();
  result("refuse_extra",
         proxy::MAX_API_CLIENTS != 2 ||
             (r <= 0 && ms < 2500 && t.n_clients == 2 && t.refused == r0 + 1),
         "max_clients %u; 3rd client recv=%d errno=%d after %lld ms; clients "
         "%u; refused %lu",
         proxy::MAX_API_CLIENTS, static_cast<int>(r), r < 0 ? e : 0,
         static_cast<long long>(ms), t.n_clients,
         static_cast<unsigned long>(t.refused));
}

// Phase: A stops ACKing; adverts broadcast (lossy) + notifies to B.
void step_stall_lossy() {
  const uint32_t id_b = 2;
  xTaskCreate(&reader_b, "reader_b", 4096, nullptr, 5, nullptr);
  // Warm-up with A healthy: let cwnd open like on a live link.
  // A must read too during warm-up, or its receive window closes first.
  set_rcv_timeout(g_fd_a, 50);
  for (int i = 0; i < 20; ++i) {
    api_server::send_async(proxyapi::MSG_BLUETOOTH_LE_RAW_ADVERTISEMENTS_RESPONSE,
                           &encode_fill, &kAdvBytes);
    uint8_t sink[1024];
    while (::recv(g_fd_a, sink, sizeof(sink), 0) > 0) {
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  vTaskDelay(pdMS_TO_TICKS(300));
  const size_t f_before = free_now();
  const uint32_t b0 = g_b_notifies.load();
  const api_server::TxqStats s0 = api_server::txq_stats();
  g_phase_min = SIZE_MAX;
  g_drop_port.store(local_port_net(g_fd_a));
  const int64_t t0 = esp_timer_get_time();
  int notifies_sent = 0;
  int64_t reaped_ms = -1;
  int64_t max_tick_ms = 0;
  // 30 s: past the 20 s over-cap reap of A.
  for (int i = 0; i < 300; ++i) {
    const int64_t ts = esp_timer_get_time();
    api_server::send_async(proxyapi::MSG_BLUETOOTH_LE_RAW_ADVERTISEMENTS_RESPONSE,
                           &encode_fill, &kAdvBytes);
    sample();
    if (api_server::send_to(id_b, proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE,
                            &encode_fill, &kNotifyBytes)) {
      ++notifies_sent;
    }
    sample();
    const int64_t took = (esp_timer_get_time() - ts) / 1000;
    if (took > max_tick_ms) max_tick_ms = took;
    if (reaped_ms < 0 && api_server::txq_stats().n_clients == 1) {
      reaped_ms = (esp_timer_get_time() - t0) / 1000;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  vTaskDelay(pdMS_TO_TICKS(500));
  const api_server::TxqStats s1 = api_server::txq_stats();
  const uint32_t b_got = g_b_notifies.load() - b0;
  const long drop = static_cast<long>(f_before) - static_cast<long>(g_phase_min);
  measure("stall_lossy_heap_drop", drop);
  measure("stall_lossy_txq_max", static_cast<long>(s1.max_all));
  measure("stall_lossy_max_send_block_ms", static_cast<long>(max_tick_ms));
  measure("stall_lossy_reaped_after_ms", static_cast<long>(reaped_ms));
  measure("stall_lossy_adv_drops", static_cast<long>(s1.drops - s0.drops));
  measure("stall_lossy_segments_dropped_by_hook",
          static_cast<long>(g_dropped.load()));
  const bool capped = proxy::API_CLIENT_TXQ_CAP > 0;
  // Bound: the cap + one frame (2 MSS pbufs) + slack for B's traffic and
  // allocator overhead.
  const long bound = static_cast<long>(proxy::API_CLIENT_TXQ_CAP) + 6 * 1024;
  result("stall_lossy",
         !capped || (reaped_ms > 0 && s1.n_clients == 1 && s1.reaps > s0.reaps &&
                     b_got >= static_cast<uint32_t>(notifies_sent) * 9 / 10 &&
                     notifies_sent >= 270 && drop <= bound),
         "cap %u: A reaped after %lld ms; B got %lu/%d notifies; heap drop %ld "
         "(bound %ld); txq max %lu; longest send tick %lld ms",
         static_cast<unsigned>(proxy::API_CLIENT_TXQ_CAP),
         static_cast<long long>(reaped_ms), static_cast<unsigned long>(b_got),
         notifies_sent, drop, bound, static_cast<unsigned long>(s1.max_all),
         static_cast<long long>(max_tick_ms));
  g_drop_port.store(0);
  ::close(g_fd_a);
  g_fd_a = -1;
}

// Phase: a new client A2 stalls; frames that must not be lost go to it.
void step_stall_strict() {
  vTaskDelay(pdMS_TO_TICKS(500));
  const int fd = connect_loopback();
  const uint8_t c = wait_clients(2, 2000);
  vTaskDelay(pdMS_TO_TICKS(200));
  const api_server::TxqStats s0 = api_server::txq_stats();
  uint32_t id_a2 = 0;
  for (uint8_t i = 0; i < s0.n_clients; ++i) {
    if (s0.clients[i].id != 2) id_a2 = s0.clients[i].id;
  }
  const size_t f_before = free_now();
  g_phase_min = SIZE_MAX;
  g_drop_port.store(local_port_net(fd));
  const int64_t t0 = esp_timer_get_time();
  int64_t failed_ms = -1;
  int sent = 0;
  for (int i = 0; i < 600 && failed_ms < 0; ++i) {
    if (api_server::send_to(id_a2, proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE,
                            &encode_fill, &kNotifyBytes)) {
      ++sent;
    } else {
      failed_ms = (esp_timer_get_time() - t0) / 1000;
    }
    sample();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  vTaskDelay(pdMS_TO_TICKS(500));
  const api_server::TxqStats s1 = api_server::txq_stats();
  const long drop = static_cast<long>(f_before) - static_cast<long>(g_phase_min);
  measure("stall_strict_heap_drop", drop);
  measure("stall_strict_frames_before_fail", sent);
  measure("stall_strict_failed_after_ms", static_cast<long>(failed_ms));
  const long bound = static_cast<long>(proxy::API_CLIENT_TXQ_CAP) + 6 * 1024;
  const bool capped = proxy::API_CLIENT_TXQ_CAP > 0;
  result("stall_strict",
         c == 2 && id_a2 != 0 && failed_ms > 0 && s1.n_clients == 1 &&
             (!capped || drop <= bound),
         "client %lu: %d frames sent, send_to failed after %lld ms; clients "
         "%u; heap drop %ld (bound %ld)",
         static_cast<unsigned long>(id_a2), sent,
         static_cast<long long>(failed_ms), s1.n_clients, drop, bound);
  g_drop_port.store(0);
  ::close(fd);
}

}  // namespace

void run() {
  std::printf("HEAP-SELFTEST start: free %u largest %u min %u\n",
              static_cast<unsigned>(free_now()),
              static_cast<unsigned>(largest_now()),
              static_cast<unsigned>(esp_get_minimum_free_heap_size()));
  step_boot_race();

  ESP_ERROR_CHECK(esp_netif_init());
  ble_backend::connection::init();
  api_server::start();
  ble_backend::publish::install(&api_server::send_async, &api_server::send_to,
                                &api_server::has_active_client);
  vTaskDelay(pdMS_TO_TICKS(300));
  measure("free_after_api_start", static_cast<long>(free_now()));
  measure("largest_after_api_start", static_cast<long>(largest_now()));

  step_clients_and_refuse();
  measure("free_two_clients", static_cast<long>(free_now()));
  step_stall_lossy();
  step_stall_strict();
  print_tasks();
  g_b_run.store(false);
  static char buf[1024];
  api_server::stats::build_stats_json(buf, sizeof(buf));
  std::printf("HEAP-STATS %s\n", buf);
  std::printf("HEAP-SELFTEST summary: %d failure(s); free %u min %u\n", g_fail,
              static_cast<unsigned>(free_now()),
              static_cast<unsigned>(esp_get_minimum_free_heap_size()));
  std::printf("HEAP-SELFTEST done\n");
  for (;;) vTaskDelay(portMAX_DELAY);
}

}  // namespace api_server::heap_selftest

#endif  // NBP_HEAP_SELFTEST
