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
//   stall_strict  the same with non-lossy frames to the stalled client: they
//                 queue in its TX ring until it is full, then the client is
//                 reaped (the producer never waits).
//   stall_isolation  one client stalled, one healthy, notifies to both from
//                 one task: every send call stays short, the healthy client
//                 gets all of its frames, only the stalled one is reaped.
//   fd_reuse      a client reaped with a full ring; the next client gets the
//                 same fd and its first frame must be its HelloResponse.
//   bulk_wait     a GetServices-sized burst (16 x 2054 B frames) from a
//                 client's own connection task into a slowly read socket,
//                 while host-priority notifies (max size) go to the same
//                 client: the burst waits for ring room and leaves the
//                 notify reserve free, so no notify is refused and nobody
//                 is reaped.
//   bulk_reserve  the same, deterministic: W stalled (input hook), a burst of
//                 500 B frames from W's task fills lwIP and then the ring up
//                 to the notify reserve and waits; two max-size notifies
//                 from host priority must still be queued; W recovers once
//                 the stall ends and gets everything.
//   bulk_adverts  a GetServices burst (16 x 2054 B) to a slow but draining W
//                 (256 B per tick, ~25 KB/s) while 600 B advert batches are
//                 broadcast every tick (~60 KB/s, from above api_tx's
//                 priority). Adverts alone keep W's ring near half full, and
//                 a bulk frame fits only below RING - reserve - 2054 (on the
//                 C3: an empty ring). The waiting burst must make adverts for
//                 W stop (dropped) so the ring empties, or it never gets in
//                 and W is reaped at the 20 s deadline.
//   bulk_gate     the same, deterministic: W stalled, its ring holds a few
//                 notifies (more than a bulk frame leaves room for, less than
//                 half the ring) and the burst waits. Advert batches
//                 broadcast now must be refused for W (ring unchanged) and
//                 still reach B; after the stall ends the burst completes.
//   coalesce      frames per send() / per TCP segment for a burst and a
//                 paced stream of notify-sized frames.
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

#include "pb_encode.h"

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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
std::atomic<uint16_t> g_drop_port{0};  // network order; 0 = drop nothing
std::atomic<uint32_t> g_dropped{0};
// Count (don't drop) data segments to this port: what one TCP segment
// carries, for the coalescing measurement.
std::atomic<uint16_t> g_count_port{0};
std::atomic<uint32_t> g_count_segs{0};
std::atomic<uint32_t> g_count_bytes{0};
}  // namespace

// lwIP IP4 input hook (nbp_lwip_hooks.h, self-test builds only). Runs in the
// tcpip thread. Eats TCP segments whose destination port is g_drop_port.
extern "C" int nbp_selftest_ip4_input(struct pbuf *p, struct netif *) {
  const uint16_t port = g_drop_port.load(std::memory_order_relaxed);
  if ((port == 0 && g_count_port.load(std::memory_order_relaxed) == 0) ||
      p == nullptr || p->len < IP_HLEN) {
    return 0;
  }
  const auto *ip = static_cast<const ip_hdr *>(p->payload);
  if (IPH_PROTO(ip) != IP_PROTO_TCP) return 0;
  const uint16_t hl = IPH_HL_BYTES(ip);
  if (p->len < hl + TCP_HLEN) return 0;
  // (port 0 = drop nothing, but the counter below may still be on)
  const auto *tcp = reinterpret_cast<const tcp_hdr *>(
      static_cast<const uint8_t *>(p->payload) + hl);
  const uint16_t cport = g_count_port.load(std::memory_order_relaxed);
  if (cport != 0 && tcp->dest == cport) {
    const int data = lwip_ntohs(IPH_LEN(ip)) - hl - TCPH_HDRLEN_BYTES(tcp);
    if (data > 0) {
      g_count_segs.fetch_add(1, std::memory_order_relaxed);
      g_count_bytes.fetch_add(static_cast<uint32_t>(data), std::memory_order_relaxed);
    }
  }
  if (tcp->dest != port) return 0;
  g_dropped.fetch_add(1, std::memory_order_relaxed);
  pbuf_free(p);
  return 1;
}

#if CONFIG_IDF_TARGET_ESP32C3
// QEMU has no ADC: skip the boot-time ADC self-calibration, which would spin
// forever (root CMakeLists.txt wraps it for this build only).
extern "C" void __wrap_adc_calc_hw_calibration_code(int, int) {}
#endif

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
  // At the NimBLE host task's priority (21), as in stall_isolation: the
  // tick measures the send calls, not preemption by api_tx or tcpip.
  const UBaseType_t prio0 = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, 21);
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
  vTaskPrioritySet(nullptr, prio0);
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
  // The ring is static (.bss), so it is not in the heap drop.
  const long bound = static_cast<long>(proxy::API_CLIENT_TXQ_CAP) + 6 * 1024;
  measure("stall_lossy_ring_drops", static_cast<long>(s1.drops - s0.drops));
  // The send calls never wait on the stalled client: a tick (two send
  // calls) stays at the scheduler's resolution, not the old 20 s.
  result("stall_lossy",
         !capped || (reaped_ms > 0 && s1.n_clients == 1 && s1.reaps > s0.reaps &&
                     b_got >= static_cast<uint32_t>(notifies_sent) * 9 / 10 &&
                     notifies_sent >= 270 && drop <= bound && max_tick_ms <= 20),
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
  // The frames queue in lwIP (up to the cap / TCP_SND_BUF) and then in the
  // ring; the first that does not fit reaps the client at once, well before
  // the 20 s stall deadline.
  result("stall_strict",
         c == 2 && id_a2 != 0 && failed_ms > 0 && failed_ms < 20000 &&
             s1.n_clients == 1 && s1.ring_reaps == s0.ring_reaps + 1 &&
             (!capped || drop <= bound),
         "client %lu: %d frames sent, send_to failed after %lld ms; clients "
         "%u; heap drop %ld (bound %ld)",
         static_cast<unsigned long>(id_a2), sent,
         static_cast<long long>(failed_ms), s1.n_clients, drop, bound);
  g_drop_port.store(0);
  ::close(fd);
}


// Wait until client `id` is gone from the table (reaped and torn down).
bool wait_gone(uint32_t id, int ms) {
  for (int t = 0; t < ms; t += 20) {
    const api_server::TxqStats s = api_server::txq_stats();
    bool present = false;
    for (uint8_t i = 0; i < s.n_clients; ++i) present = present || s.clients[i].id == id;
    if (!present) return true;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return false;
}

// Id of the connected client that is not `other`.
uint32_t other_id(uint32_t other, int *fd_out = nullptr) {
  const api_server::TxqStats s = api_server::txq_stats();
  for (uint8_t i = 0; i < s.n_clients; ++i) {
    if (s.clients[i].id != other) {
      if (fd_out != nullptr) *fd_out = s.clients[i].fd;
      return s.clients[i].id;
    }
  }
  return 0;
}

// Phase: A3 stalls, B is healthy; one task sends notifies to both, as the
// NimBLE host task does. Nothing it does may wait on A3.
void step_stall_isolation() {
  const uint32_t id_b = 2;
  vTaskDelay(pdMS_TO_TICKS(500));
  const int fd = connect_loopback();
  const uint8_t c = wait_clients(2, 2000);
  vTaskDelay(pdMS_TO_TICKS(200));
  const uint32_t id_a3 = other_id(id_b);
  const api_server::TxqStats s0 = api_server::txq_stats();
  const uint32_t b0 = g_b_notifies.load();
  g_drop_port.store(local_port_net(fd));
  int64_t max_us = 0, failed_ms = -1;
  int to_b = 0, to_a = 0;
  // Send at the NimBLE host task's priority (21): what that task pays per
  // call, without preemption by api_tx (6) or tcpip (18) inflating it.
  const UBaseType_t prio0 = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, 21);
  const int64_t t0 = esp_timer_get_time();
  // Until A3 is reaped, then 2 s more for B alone.
  for (int i = 0; i < 2000; ++i) {
    const int64_t ts = esp_timer_get_time();
    const bool a_ok = failed_ms < 0 &&
        api_server::send_to(id_a3, proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE,
                            &encode_fill, &kNotifyBytes);
    const int64_t ta = esp_timer_get_time() - ts;
    const int64_t tb0 = esp_timer_get_time();
    if (api_server::send_to(id_b, proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE,
                            &encode_fill, &kNotifyBytes)) {
      ++to_b;
    }
    const int64_t tb = esp_timer_get_time() - tb0;
    max_us = std::max(max_us, std::max(ta, tb));
    if (a_ok) ++to_a;
    if (!a_ok && failed_ms < 0) failed_ms = (esp_timer_get_time() - t0) / 1000;
    if (failed_ms >= 0 && (esp_timer_get_time() - t0) / 1000 > failed_ms + 2000) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  vTaskPrioritySet(nullptr, prio0);
  vTaskDelay(pdMS_TO_TICKS(1000));
  const bool gone = wait_gone(id_a3, 3000);
  const api_server::TxqStats s1 = api_server::txq_stats();
  const uint32_t b_got = g_b_notifies.load() - b0;
  measure("isolation_max_send_call_us", static_cast<long>(max_us));
  measure("isolation_a_frames_before_reap", to_a);
  measure("isolation_a_reaped_after_ms", static_cast<long>(failed_ms));
  // One-tick bound (10 ms): the calls take ~0.2-0.7 ms in QEMU but an
  // emulated core can be descheduled by the host for ~2 ms (measured
  // 2060 us once). The old code blocked here for up to 20 s.
  result("stall_isolation",
         c == 2 && id_a3 != 0 && failed_ms > 0 && gone && max_us < 10000 &&
             b_got == static_cast<uint32_t>(to_b) && to_b > 0 &&
             s1.n_clients == 1 && s1.clients[0].id == id_b &&
             s1.ring_reaps == s0.ring_reaps + 1,
         "A3 %lu reaped after %lld ms (%d frames); B got %lu/%d; longest "
         "send call %lld us; clients %u",
         static_cast<unsigned long>(id_a3), static_cast<long long>(failed_ms),
         to_a, static_cast<unsigned long>(b_got), to_b,
         static_cast<long long>(max_us), s1.n_clients);
  g_drop_port.store(0);
  ::close(fd);
}

bool send_hello(int fd) {
  proxyapi_HelloRequest req = proxyapi_HelloRequest_init_zero;
  std::strcpy(req.client_info, "heaptest");
  req.api_version_major = 1;
  req.api_version_minor = 10;
  uint8_t buf[frame_codec::MAX_HEADER_LEN + 96];
  pb_ostream_t s = pb_ostream_from_buffer(buf + frame_codec::MAX_HEADER_LEN,
                                          sizeof(buf) - frame_codec::MAX_HEADER_LEN);
  if (!pb_encode(&s, proxyapi_HelloRequest_fields, &req)) return false;
  const size_t start = frame_codec::prepend_header(
      buf, proxyapi::MSG_HELLO_REQUEST, s.bytes_written);
  const size_t total = (frame_codec::MAX_HEADER_LEN - start) + s.bytes_written;
  return ::send(fd, buf + start, total, 0) == static_cast<ssize_t>(total);
}

// Phase (known-bad check for the ring reset): X stalls with a full ring and
// is reaped; Y connects, gets X's fd, and must see its HelloResponse first,
// not X's leftover notifies.
void step_fd_reuse() {
  const uint32_t id_b = 2;
  vTaskDelay(pdMS_TO_TICKS(500));
  const int fx = connect_loopback();
  wait_clients(2, 2000);
  vTaskDelay(pdMS_TO_TICKS(200));
  int srv_fd_x = -1;
  const uint32_t id_x = other_id(id_b, &srv_fd_x);
  g_drop_port.store(local_port_net(fx));
  int queued = 0;
  for (int i = 0; i < 2000; ++i) {
    if (!api_server::send_to(id_x, proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE,
                             &encode_fill, &kNotifyBytes)) {
      break;
    }
    ++queued;
  }
  const bool gone = wait_gone(id_x, 3000);
  g_drop_port.store(0);
  ::close(fx);
  vTaskDelay(pdMS_TO_TICKS(300));
  const int fy = connect_loopback();
  wait_clients(2, 2000);
  vTaskDelay(pdMS_TO_TICKS(200));
  int srv_fd_y = -1;
  const uint32_t id_y = other_id(id_b, &srv_fd_y);
  set_rcv_timeout(fy, 3000);
  const bool hello_sent = send_hello(fy);
  uint16_t type = 0;
  size_t len = 0;
  static uint8_t rx[proxy::MAX_MESSAGE_SIZE];
  const auto e = frame_codec::read_frame(&sock_read, const_cast<int *>(&fy), rx,
                                         sizeof(rx), &type, &len);
  result("fd_reuse",
         gone && queued > 0 && id_y != 0 && id_y != id_x && srv_fd_y == srv_fd_x &&
             hello_sent && e == frame_codec::Error::Ok &&
             type == proxyapi::MSG_HELLO_RESPONSE,
         "X id %lu fd %d: %d frames queued, reaped %s; Y id %lu fd %d: first "
         "frame type %u (read %d)",
         static_cast<unsigned long>(id_x), srv_fd_x, queued, gone ? "yes" : "no",
         static_cast<unsigned long>(id_y), srv_fd_y, type, static_cast<int>(e));
  ::close(fy);
  wait_gone(id_y, 3000);
}

// ---- bulk_wait ----
constexpr int kBulkFrames = 16;
size_t kBulkBytes = proxy::MAX_MESSAGE_SIZE;  // a full GetServices response
// What bulk_burst() sends (set per step).
std::atomic<int> g_bulk_n{kBulkFrames};
size_t g_bulk_bytes = proxy::MAX_MESSAGE_SIZE;
size_t kDoneBytes = 8;
size_t kMaxNotifyBytes = proxyapi_BluetoothGATTNotifyDataResponse_size;
std::atomic<int> g_bulk_ok{0};
std::atomic<int> g_bulk_fail{0};
std::atomic<bool> g_bulk_done{false};
std::atomic<int64_t> g_bulk_ms{0};

// Client W's slow reader: 1 KiB per tick (~100 KB/s), parses frames itself
// (read_frame's small reads would each cost a tick here).
int g_fd_w = -1;
std::atomic<bool> g_w_run{false};
std::atomic<uint32_t> g_w_bulk{0}, g_w_done{0}, g_w_notifies{0}, g_w_bad{0};
std::atomic<size_t> g_w_chunk{1024};  // bytes reader_w takes per tick

bool parse_varint(const uint8_t *p, size_t n, size_t *pos, uint32_t *v) {
  *v = 0;
  for (int sh = 0; sh < 28 && *pos < n; sh += 7) {
    const uint8_t b = p[(*pos)++];
    *v |= static_cast<uint32_t>(b & 0x7F) << sh;
    if ((b & 0x80) == 0) return true;
  }
  return false;
}

void reader_w(void *) {
  static uint8_t buf[4096];
  size_t have = 0;
  while (g_w_run.load()) {
    const ssize_t r = ::recv(g_fd_w, buf + have,
                             std::min<size_t>(g_w_chunk.load(), sizeof(buf) - have), 0);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (r <= 0) break;
    have += static_cast<size_t>(r);
    size_t off = 0;
    while (true) {
      size_t pos = off;
      uint32_t len = 0, type = 0;
      if (pos >= have) break;
      if (buf[pos++] != 0) {
        g_w_bad.fetch_add(1);
        have = 0;
        off = 0;
        break;
      }
      if (!parse_varint(buf, have, &pos, &len) || !parse_varint(buf, have, &pos, &type) ||
          pos + len > have) {
        break;  // incomplete
      }
      if (type == proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_RESPONSE) g_w_bulk.fetch_add(1);
      if (type == proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_DONE_RESPONSE) g_w_done.fetch_add(1);
      if (type == proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE) g_w_notifies.fetch_add(1);
      off = pos + len;
    }
    std::memmove(buf, buf + off, have - off);
    have -= off;
    vTaskDelay(1);
  }
  vTaskDelete(nullptr);
}

bool send_get_services(int fd, uint64_t address) {
  proxyapi_BluetoothGATTGetServicesRequest req =
      proxyapi_BluetoothGATTGetServicesRequest_init_zero;
  req.address = address;
  uint8_t buf[frame_codec::MAX_HEADER_LEN + 32];
  pb_ostream_t s = pb_ostream_from_buffer(buf + frame_codec::MAX_HEADER_LEN,
                                          sizeof(buf) - frame_codec::MAX_HEADER_LEN);
  if (!pb_encode(&s, proxyapi_BluetoothGATTGetServicesRequest_fields, &req)) return false;
  const size_t start = frame_codec::prepend_header(
      buf, proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_REQUEST, s.bytes_written);
  const size_t total = (frame_codec::MAX_HEADER_LEN - start) + s.bytes_written;
  return ::send(fd, buf + start, total, 0) == static_cast<ssize_t>(total);
}

void step_bulk_wait() {
  const uint32_t id_b = 2;
  vTaskDelay(pdMS_TO_TICKS(500));
  g_fd_w = connect_loopback();
  const uint8_t c = wait_clients(2, 2000);
  vTaskDelay(pdMS_TO_TICKS(200));
  const uint32_t id_w = other_id(id_b);
  set_rcv_timeout(g_fd_w, 100);
  g_w_run.store(true);
  xTaskCreate(&reader_w, "reader_w", 3072, nullptr, 5, nullptr);
  const api_server::TxqStats s0 = api_server::txq_stats();
  const bool req_sent = send_get_services(g_fd_w, kBulkAddress);
  // Host-priority notifier: a max-size notify every 50 ms (10.8 KB/s, well
  // above a BMS's few hundred B/s of ~150 B notifies), as a BMS that
  // notifies while its second link is discovered. The reserve holds two
  // notifies, so this assumes api_tx gets data into lwIP at least every
  // 100 ms; on loopback the reader frees window every ~20 ms.
  const UBaseType_t prio0 = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, 21);
  int sent = 0, refused = 0;
  int64_t max_us = 0;
  uint32_t ring_max = 0;
  const int64_t t0 = esp_timer_get_time();
  int64_t done_at = -1;
  while ((esp_timer_get_time() - t0) / 1000 < 25000) {
    const int64_t ts = esp_timer_get_time();
    if (api_server::send_to(id_w, proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE,
                            &encode_fill, &kMaxNotifyBytes)) {
      ++sent;
    } else {
      ++refused;
    }
    max_us = std::max<int64_t>(max_us, esp_timer_get_time() - ts);
    const api_server::TxqStats sx = api_server::txq_stats();
    for (uint8_t i = 0; i < sx.n_clients; ++i) {
      if (sx.clients[i].id == id_w) ring_max = std::max(ring_max, sx.clients[i].ring);
    }
    if (done_at < 0 && g_bulk_done.load()) done_at = esp_timer_get_time();
    if (done_at >= 0 && esp_timer_get_time() - done_at > 500000) break;
    if (refused > 0) break;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  vTaskPrioritySet(nullptr, prio0);
  // Let the reader catch up.
  for (int t = 0; t < 5000; t += 50) {
    if (g_w_notifies.load() >= static_cast<uint32_t>(sent) &&
        g_w_done.load() >= 1) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  const api_server::TxqStats s1 = api_server::txq_stats();
  bool w_there = false;
  for (uint8_t i = 0; i < s1.n_clients; ++i) w_there = w_there || s1.clients[i].id == id_w;
  measure("bulk_wait_burst_ms", static_cast<long>(g_bulk_ms.load()));
  measure("bulk_wait_notifies_sent", sent);
  measure("bulk_wait_max_notify_call_us", static_cast<long>(max_us));
  measure("bulk_wait_ring_max", static_cast<long>(ring_max));
  result("bulk_wait",
         c == 2 && req_sent && g_bulk_done.load() && g_bulk_ok.load() == kBulkFrames &&
             g_bulk_fail.load() == 0 && refused == 0 && sent > 0 && w_there &&
             s1.ring_reaps == s0.ring_reaps && s1.reaps == s0.reaps &&
             g_w_bulk.load() == static_cast<uint32_t>(kBulkFrames) &&
             g_w_done.load() == 1 && g_w_notifies.load() == static_cast<uint32_t>(sent) &&
             g_w_bad.load() == 0 && max_us < 10000,
         "W %lu: burst %d/%d in %lld ms; %d notifies sent, %d refused; W got "
         "%lu bulk + %lu done + %lu notifies; %s; ring max %lu/%lu; longest "
         "notify call %lld us",
         static_cast<unsigned long>(id_w), g_bulk_ok.load(), kBulkFrames,
         static_cast<long long>(g_bulk_ms.load()), sent, refused,
         static_cast<unsigned long>(g_w_bulk.load()),
         static_cast<unsigned long>(g_w_done.load()),
         static_cast<unsigned long>(g_w_notifies.load()),
         w_there ? "W kept" : "W reaped", static_cast<unsigned long>(ring_max),
         static_cast<unsigned long>(proxy::API_CLIENT_TX_RING),
         static_cast<long long>(max_us));
  g_w_run.store(false);
  vTaskDelay(pdMS_TO_TICKS(300));
  ::close(g_fd_w);
  g_fd_w = -1;
  wait_gone(id_w, 3000);
}

void step_bulk_reserve() {
  const uint32_t id_b = 2;
  vTaskDelay(pdMS_TO_TICKS(500));
  g_bulk_ok.store(0);
  g_bulk_fail.store(0);
  g_bulk_done.store(false);
  g_w_bulk.store(0);
  g_w_done.store(0);
  g_w_notifies.store(0);
  g_w_bad.store(0);
  constexpr int kFrames = 40;  // 40 x 506 B: more than lwIP + the ring hold
  g_bulk_n.store(kFrames);
  g_bulk_bytes = 500;
  g_fd_w = connect_loopback();
  const uint8_t c = wait_clients(2, 2000);
  vTaskDelay(pdMS_TO_TICKS(200));
  const uint32_t id_w = other_id(id_b);
  set_rcv_timeout(g_fd_w, 100);
  const bool req_sent = send_get_services(g_fd_w, kBulkAddress);
  // Let the request reach W's task, then stall W: data goes out, no ACKs.
  vTaskDelay(pdMS_TO_TICKS(100));
  g_drop_port.store(local_port_net(g_fd_w));
  g_w_run.store(true);
  xTaskCreate(&reader_w, "reader_w", 3072, nullptr, 5, nullptr);
  // Wait until the ring stops growing: the burst is blocked on room.
  uint32_t ring = 0, prev = UINT32_MAX;
  for (int t = 0; t < 4000; t += 200) {
    vTaskDelay(pdMS_TO_TICKS(200));
    const api_server::TxqStats sx = api_server::txq_stats();
    for (uint8_t i = 0; i < sx.n_clients; ++i) {
      if (sx.clients[i].id == id_w) ring = sx.clients[i].ring;
    }
    if (ring == prev && ring > 0) break;
    prev = ring;
  }
  const bool waiting = !g_bulk_done.load() && g_bulk_ok.load() < kFrames;
  const api_server::TxqStats s0 = api_server::txq_stats();
  // Two max-size notifies at host priority, back to back: nothing drains.
  const UBaseType_t prio0 = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, 21);
  const bool n1 = api_server::send_to(id_w, proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE,
                                      &encode_fill, &kMaxNotifyBytes);
  const bool n2 = api_server::send_to(id_w, proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE,
                                      &encode_fill, &kMaxNotifyBytes);
  vTaskPrioritySet(nullptr, prio0);
  // End the stall; lwIP retransmits and everything drains.
  g_drop_port.store(0);
  for (int t = 0; t < 15000 && !g_bulk_done.load(); t += 50) vTaskDelay(pdMS_TO_TICKS(50));
  for (int t = 0; t < 5000; t += 50) {
    if (g_w_done.load() >= 1 && g_w_notifies.load() >= 2u) break;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  const api_server::TxqStats s1 = api_server::txq_stats();
  bool w_there = false;
  for (uint8_t i = 0; i < s1.n_clients; ++i) w_there = w_there || s1.clients[i].id == id_w;
  const uint32_t bulk_limit = static_cast<uint32_t>(
      proxy::API_CLIENT_TX_RING - 2 * (frame_codec::MAX_HEADER_LEN + kMaxNotifyBytes));
  measure("bulk_reserve_ring_while_waiting", static_cast<long>(ring));
  result("bulk_reserve",
         c == 2 && req_sent && waiting && ring > 0 && ring <= bulk_limit && n1 && n2 &&
             s1.ring_reaps == s0.ring_reaps && w_there && g_bulk_done.load() &&
             g_bulk_ok.load() == kFrames && g_w_bulk.load() == static_cast<uint32_t>(kFrames) &&
             g_w_done.load() == 1 && g_w_notifies.load() == 2 && g_w_bad.load() == 0,
         "W %lu: burst waiting %s with ring %lu B (bulk limit %lu); 2 notifies "
         "%s/%s; burst %d/%d; W got %lu bulk + %lu done + %lu notifies; %s",
         static_cast<unsigned long>(id_w), waiting ? "yes" : "no",
         static_cast<unsigned long>(ring), static_cast<unsigned long>(bulk_limit),
         n1 ? "queued" : "REFUSED", n2 ? "queued" : "REFUSED", g_bulk_ok.load(), kFrames,
         static_cast<unsigned long>(g_w_bulk.load()),
         static_cast<unsigned long>(g_w_done.load()),
         static_cast<unsigned long>(g_w_notifies.load()),
         w_there ? "W kept" : "W reaped");
  g_w_run.store(false);
  vTaskDelay(pdMS_TO_TICKS(300));
  ::close(g_fd_w);
  g_fd_w = -1;
  wait_gone(id_w, 3000);
}

std::atomic<bool> g_pump_run{false};
std::atomic<uint32_t> g_pumped{0};
size_t kPumpAdvBytes = 600;

void advert_pump(void *) {
  while (g_pump_run.load()) {
    api_server::send_async(proxyapi::MSG_BLUETOOTH_LE_RAW_ADVERTISEMENTS_RESPONSE,
                           &encode_fill, &kPumpAdvBytes);
    g_pumped.fetch_add(1);
    vTaskDelay(1);
  }
  vTaskDelete(nullptr);
}

void step_bulk_adverts() {
  const uint32_t id_b = 2;
  vTaskDelay(pdMS_TO_TICKS(500));
  g_bulk_ok.store(0);
  g_bulk_fail.store(0);
  g_bulk_done.store(false);
  g_w_bulk.store(0);
  g_w_done.store(0);
  g_w_notifies.store(0);
  g_w_bad.store(0);
  g_bulk_n.store(kBulkFrames);
  g_bulk_bytes = proxy::MAX_MESSAGE_SIZE;
  g_fd_w = connect_loopback();
  const uint8_t c = wait_clients(2, 2000);
  vTaskDelay(pdMS_TO_TICKS(200));
  const uint32_t id_w = other_id(id_b);
  set_rcv_timeout(g_fd_w, 100);
  g_w_chunk.store(256);
  g_w_run.store(true);
  xTaskCreate(&reader_w, "reader_w", 3072, nullptr, 5, nullptr);
  // Adverts first, so W is a busy advert subscriber when the burst starts.
  g_pumped.store(0);
  g_pump_run.store(true);
  xTaskCreate(&advert_pump, "adv_pump", 3072, nullptr, 7, nullptr);
  vTaskDelay(pdMS_TO_TICKS(500));
  const api_server::TxqStats s0 = api_server::txq_stats();
  const bool req_sent = send_get_services(g_fd_w, kBulkAddress);
  const int64_t t0 = esp_timer_get_time();
  // 25 s: past the 20 s deadline a starved burst would hit.
  while (!g_bulk_done.load() && (esp_timer_get_time() - t0) / 1000 < 25000) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  vTaskDelay(pdMS_TO_TICKS(300));
  g_pump_run.store(false);
  for (int t = 0; t < 10000 && g_w_done.load() < 1; t += 50) vTaskDelay(pdMS_TO_TICKS(50));
  const api_server::TxqStats s1 = api_server::txq_stats();
  bool w_there = false;
  for (uint8_t i = 0; i < s1.n_clients; ++i) w_there = w_there || s1.clients[i].id == id_w;
  measure("bulk_adverts_burst_ms", static_cast<long>(g_bulk_ms.load()));
  measure("bulk_adverts_adv_drops", static_cast<long>(s1.drops - s0.drops));
  result("bulk_adverts",
         c == 2 && req_sent && g_bulk_done.load() && g_bulk_ok.load() == kBulkFrames &&
             g_bulk_fail.load() == 0 && w_there && s1.ring_reaps == s0.ring_reaps &&
             s1.reaps == s0.reaps && g_w_bulk.load() == static_cast<uint32_t>(kBulkFrames) &&
             g_w_done.load() == 1 && g_w_bad.load() == 0,
         "W %lu: burst %d/%d in %lld ms with %lu advert batches pumped (%lu "
         "dropped); W got %lu bulk + %lu done; %s; ring reaps %lu->%lu",
         static_cast<unsigned long>(id_w), g_bulk_ok.load(), kBulkFrames,
         static_cast<long long>(g_bulk_ms.load()),
         static_cast<unsigned long>(g_pumped.load()),
         static_cast<unsigned long>(s1.drops - s0.drops),
         static_cast<unsigned long>(g_w_bulk.load()),
         static_cast<unsigned long>(g_w_done.load()), w_there ? "W kept" : "W reaped",
         static_cast<unsigned long>(s0.ring_reaps),
         static_cast<unsigned long>(s1.ring_reaps));
  g_w_run.store(false);
  vTaskDelay(pdMS_TO_TICKS(300));
  g_w_chunk.store(1024);
  ::close(g_fd_w);
  g_fd_w = -1;
  wait_gone(id_w, 3000);
}

uint32_t ring_of(uint32_t id) {
  const api_server::TxqStats s = api_server::txq_stats();
  for (uint8_t i = 0; i < s.n_clients; ++i) {
    if (s.clients[i].id == id) return s.clients[i].ring;
  }
  return 0;
}

void step_bulk_gate() {
  const uint32_t id_b = 2;
  vTaskDelay(pdMS_TO_TICKS(500));
  g_bulk_ok.store(0);
  g_bulk_fail.store(0);
  g_bulk_done.store(false);
  g_w_bulk.store(0);
  g_w_done.store(0);
  g_w_notifies.store(0);
  g_w_bad.store(0);
  constexpr int kFrames = 4;
  g_bulk_n.store(kFrames);
  g_bulk_bytes = proxy::MAX_MESSAGE_SIZE;
  // A bulk frame (2054 B) waits while the ring holds more than this; an
  // advert batch (606 B) fits while it holds at most half the ring minus it.
  const uint32_t bulk_thr = static_cast<uint32_t>(
      proxy::API_CLIENT_TX_RING - 2 * (frame_codec::MAX_HEADER_LEN + kMaxNotifyBytes) -
      (frame_codec::MAX_HEADER_LEN + proxy::MAX_MESSAGE_SIZE));
  const uint32_t adv_frame = static_cast<uint32_t>(frame_codec::MAX_HEADER_LEN + kAdvBytes);
  const uint32_t adv_thr = static_cast<uint32_t>(proxy::API_CLIENT_TX_RING / 2) - adv_frame;
  g_fd_w = connect_loopback();
  const uint8_t c = wait_clients(2, 2000);
  vTaskDelay(pdMS_TO_TICKS(200));
  const uint32_t id_w = other_id(id_b);
  set_rcv_timeout(g_fd_w, 100);
  g_w_run.store(true);
  xTaskCreate(&reader_w, "reader_w", 3072, nullptr, 5, nullptr);
  g_drop_port.store(local_port_net(g_fd_w));
  // Notifies until lwIP is full and the ring holds more than bulk_thr.
  int notifies = 0;
  uint32_t ring = 0;
  for (int i = 0; i < 1000 && ring <= bulk_thr; ++i) {
    if (api_server::send_to(id_w, proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE,
                            &encode_fill, &kNotifyBytes)) {
      ++notifies;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    ring = ring_of(id_w);
    // Past api_tx's 20 ms coalescing hold: what is left is held by lwIP
    // being full, not by the hold.
    if (ring > bulk_thr) {
      vTaskDelay(pdMS_TO_TICKS(100));
      ring = ring_of(id_w);
    }
  }
  const bool req_sent = send_get_services(g_fd_w, kBulkAddress);
  vTaskDelay(pdMS_TO_TICKS(300));
  const bool waiting = !g_bulk_done.load() && g_bulk_ok.load() == 0;
  const uint32_t r0 = ring_of(id_w);
  const api_server::TxqStats s0 = api_server::txq_stats();
  const uint32_t b0 = g_b_frames.load();
  constexpr int kAdv = 5;
  // Spaced, so healthy B takes every one (its own ring has room).
  for (int i = 0; i < kAdv; ++i) {
    api_server::send_async(proxyapi::MSG_BLUETOOTH_LE_RAW_ADVERTISEMENTS_RESPONSE,
                           &encode_fill, &kAdvBytes);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  vTaskDelay(pdMS_TO_TICKS(300));
  const uint32_t r1 = ring_of(id_w);
  const api_server::TxqStats s1 = api_server::txq_stats();
  const uint32_t b_got = g_b_frames.load() - b0;
  // Drops not explained by B's misses are W's refusals.
  const uint32_t w_refused = (s1.drops - s0.drops) - (kAdv - std::min<uint32_t>(b_got, kAdv));
  // End the stall: the ring drains and the burst completes.
  g_drop_port.store(0);
  for (int t = 0; t < 20000 && !g_bulk_done.load(); t += 50) vTaskDelay(pdMS_TO_TICKS(50));
  for (int t = 0; t < 5000 && g_w_done.load() < 1; t += 50) vTaskDelay(pdMS_TO_TICKS(50));
  const api_server::TxqStats s2 = api_server::txq_stats();
  bool w_there = false;
  for (uint8_t i = 0; i < s2.n_clients; ++i) w_there = w_there || s2.clients[i].id == id_w;
  measure("bulk_gate_ring_before_adverts", static_cast<long>(r0));
  measure("bulk_gate_ring_after_adverts", static_cast<long>(r1));
  result("bulk_gate",
         c == 2 && req_sent && waiting && r0 > bulk_thr && r0 <= adv_thr && r1 == r0 &&
             w_refused == static_cast<uint32_t>(kAdv) && b_got == static_cast<uint32_t>(kAdv) &&
             g_bulk_done.load() && g_bulk_ok.load() == kFrames && w_there &&
             s2.ring_reaps == s0.ring_reaps && s2.reaps == s0.reaps &&
             g_w_bulk.load() == static_cast<uint32_t>(kFrames) && g_w_done.load() == 1 &&
             g_w_notifies.load() == static_cast<uint32_t>(notifies) && g_w_bad.load() == 0,
         "W %lu: %d notifies, ring %lu B (bulk waits above %lu, adverts fit up "
         "to %lu); burst waiting %s; %d adverts: W ring %lu -> %lu, W refused "
         "%lu, B got %lu; burst %d/%d; W got %lu bulk + %lu done + %lu notifies; %s",
         static_cast<unsigned long>(id_w), notifies, static_cast<unsigned long>(r0),
         static_cast<unsigned long>(bulk_thr), static_cast<unsigned long>(adv_thr),
         waiting ? "yes" : "no", kAdv, static_cast<unsigned long>(r0),
         static_cast<unsigned long>(r1),
         static_cast<unsigned long>(w_refused), static_cast<unsigned long>(b_got),
         g_bulk_ok.load(), kFrames, static_cast<unsigned long>(g_w_bulk.load()),
         static_cast<unsigned long>(g_w_done.load()),
         static_cast<unsigned long>(g_w_notifies.load()), w_there ? "W kept" : "W reaped");
  g_w_run.store(false);
  vTaskDelay(pdMS_TO_TICKS(300));
  ::close(g_fd_w);
  g_fd_w = -1;
  wait_gone(id_w, 3000);
}

// Phase: what the sender's coalescing does on a healthy client (B, which
// reads continuously). Loopback ACKs are delayed by lwIP's delayed-ACK
// timer, as a WiFi peer's are by the air.
void step_coalesce() {
  const uint32_t id_b = 2;
  vTaskDelay(pdMS_TO_TICKS(500));
  g_count_port.store(local_port_net(g_fd_b));
  struct Run {
    const char *name;
    int frames;
    int gap_ms;
  };
  const Run runs[] = {{"burst", 50, 0}, {"paced_5ms", 50, 5}, {"paced_50ms", 20, 50}};
  bool ok = true;
  for (const Run &r : runs) {
    vTaskDelay(pdMS_TO_TICKS(500));
    const api_server::TxqStats s0 = api_server::txq_stats();
    const uint32_t b0 = g_b_notifies.load();
    const uint32_t seg0 = g_count_segs.load(), by0 = g_count_bytes.load();
    for (int i = 0; i < r.frames; ++i) {
      api_server::send_to(id_b, proxyapi::MSG_BLUETOOTH_GATT_NOTIFY_DATA_RESPONSE,
                          &encode_fill, &kNotifyBytes);
      if (r.gap_ms > 0) vTaskDelay(pdMS_TO_TICKS(r.gap_ms));
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    const api_server::TxqStats s1 = api_server::txq_stats();
    const uint32_t frames = s1.frames - s0.frames;
    const uint32_t sends = s1.sends - s0.sends;
    const uint32_t segs = g_count_segs.load() - seg0;
    const uint32_t got = g_b_notifies.load() - b0;
    char what[64];
    std::snprintf(what, sizeof(what), "coalesce_%s_frames_per_send_x100", r.name);
    measure(what, sends ? static_cast<long>(frames * 100 / sends) : -1);
    std::snprintf(what, sizeof(what), "coalesce_%s_frames_per_segment_x100", r.name);
    measure(what, segs ? static_cast<long>(frames * 100 / segs) : -1);
    std::printf("HEAP-MEASURE coalesce_%s frames %lu sends %lu segments %lu "
                "bytes %lu received %lu\n",
                r.name, static_cast<unsigned long>(frames),
                static_cast<unsigned long>(sends), static_cast<unsigned long>(segs),
                static_cast<unsigned long>(g_count_bytes.load() - by0),
                static_cast<unsigned long>(got));
    ok = ok && got == static_cast<uint32_t>(r.frames) && segs > 0 && sends > 0;
  }
  g_count_port.store(0);
  result("coalesce", ok, "every frame delivered; see HEAP-MEASURE coalesce_*");
}

}  // namespace

// Runs on the requesting client's connection task (bt_handlers hook).
void bulk_burst(uint32_t client_id) {
  const int64_t t0 = esp_timer_get_time();
  const int frames = g_bulk_n.load();
  for (int i = 0; i < frames; ++i) {
    if (api_server::send_to(client_id, proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_RESPONSE,
                            &encode_fill, &g_bulk_bytes)) {
      g_bulk_ok.fetch_add(1);
    } else {
      g_bulk_fail.fetch_add(1);
      break;
    }
  }
  api_server::send_to(client_id, proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_DONE_RESPONSE,
                      &encode_fill, &kDoneBytes);
  g_bulk_ms.store((esp_timer_get_time() - t0) / 1000);
  g_bulk_done.store(true);
}

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
  step_stall_isolation();
  step_fd_reuse();
  step_bulk_wait();
  step_bulk_reserve();
  step_bulk_adverts();
  step_bulk_gate();
  step_coalesce();
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
