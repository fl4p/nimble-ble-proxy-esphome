// QEMU-only OOM self-test (idf.py -DNBP_OOM_SELFTEST=1 …). Never in
// firmware: compiled only with that define, and it takes over app_main.
//
// Runs the real api_server on lwIP loopback (no WiFi, no BLE in QEMU), makes
// the heap actually run out by allocating it away ("hog"), and checks that
// the GetServices path and the API send path return errors instead of
// aborting. Every step prints `OOM-SELFTEST <name>: PASS|FAIL <detail>`; the
// last step deliberately exhausts the heap under a throwing `new` and must
// end in the "OOM: operator new" abort.

#if NBP_OOM_SELFTEST

#include "oom_selftest.h"

#include "api_proto.h"
#include "api_server.h"
#include "frame_codec.h"
#include "oom_guard.h"
#include "proxy_config.h"

#include "../ble_backend/connection.h"
#include "../ble_backend/gatt_discovery.h"
#include "../ble_backend/publish.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace api_server::oom_selftest {

namespace {

constexpr const char *TAG = "oomtest";
int g_fail = 0;

void result(const char *name, bool ok, const char *fmt = "", ...)
    __attribute__((format(printf, 3, 4)));
void result(const char *name, bool ok, const char *fmt, ...) {
  char detail[160];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);
  if (!ok) ++g_fail;
  std::printf("OOM-SELFTEST %s: %s %s\n", name, ok ? "PASS" : "FAIL", detail);
}

// ---- heap hog: take the heap away for real ----
constexpr int kHogMax = 512;
void *g_hog[kHogMax];
int g_hog_n = 0;

size_t free_now() { return heap_caps_get_free_size(MALLOC_CAP_DEFAULT); }
size_t largest_now() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
}

// Allocate until at most `keep` bytes are free (keep = 0: until not even a
// 16 B block is left). Uses heap_caps_malloc directly, never new.
void hog(size_t keep) {
  while (g_hog_n < kHogMax) {
    const size_t f = free_now();
    if (f <= keep) break;
    size_t want = largest_now();
    if (keep > 0 && want > f - keep) want = f - keep;
    if (want < 16) break;
    // TLSF overhead can make an exact "largest" request fail; back off.
    void *p = nullptr;
    while (want >= 16 && (p = heap_caps_malloc(want, MALLOC_CAP_DEFAULT)) == nullptr) {
      want -= want / 8 + 8;
    }
    if (p == nullptr) break;
    g_hog[g_hog_n++] = p;
  }
}

void unhog() {
  while (g_hog_n > 0) heap_caps_free(g_hog[--g_hog_n]);
}

void unhog_cb(void *) { unhog(); }

esp_timer_handle_t g_unhog_timer = nullptr;

// Release the hog from the esp_timer task in `ms`, while the test task is
// blocked inside the allocation under test.
void unhog_after(uint32_t ms) {
  esp_timer_start_once(g_unhog_timer, static_cast<uint64_t>(ms) * 1000);
}

// The test task may only touch the hog once the timer is done with it.
void wait_unhog_timer() {
  while (esp_timer_is_active(g_unhog_timer)) vTaskDelay(pdMS_TO_TICKS(10));
}

// Keep the compiler from eliding or folding a new/delete pair.
inline void keep(const void *p) { asm volatile("" : : "r"(p) : "memory"); }

// ---- loopback API client ----
int g_fd = -1;

ssize_t sock_read(void *ctx, uint8_t *buf, size_t n) {
  return ::recv(*static_cast<int *>(ctx), buf, n, 0);
}

bool send_frame(uint16_t type, const pb_msgdesc_t *desc, const void *msg) {
  uint8_t buf[frame_codec::MAX_HEADER_LEN + 64];
  pb_ostream_t s = pb_ostream_from_buffer(buf + frame_codec::MAX_HEADER_LEN,
                                          sizeof(buf) - frame_codec::MAX_HEADER_LEN);
  if (!pb_encode(&s, desc, msg)) return false;
  size_t start = frame_codec::prepend_header(buf, type, s.bytes_written);
  size_t total = (frame_codec::MAX_HEADER_LEN - start) + s.bytes_written;
  return ::send(g_fd, buf + start, total, 0) == static_cast<ssize_t>(total);
}

uint8_t g_rx[proxy::MAX_MESSAGE_SIZE];
bool read_frame(uint16_t *type, size_t *len) {
  return frame_codec::read_frame(&sock_read, &g_fd, g_rx, sizeof(g_rx), type,
                                 len) == frame_codec::Error::Ok;
}

void set_rcv_timeout(int ms) {
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  ::setsockopt(g_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

bool connect_client() {
  g_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(proxy::API_PORT);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(g_fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
    return false;
  }
  for (int i = 0; i < 50 && !api_server::has_active_client(); ++i) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  set_rcv_timeout(10000);
  return api_server::has_active_client();
}

bool get_services_request() {
  proxyapi_BluetoothGATTGetServicesRequest req =
      proxyapi_BluetoothGATTGetServicesRequest_init_zero;
  req.address = ble_backend::gatt_discovery::kOomSelftestAddress;
  return send_frame(proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_REQUEST,
                    proxyapi_BluetoothGATTGetServicesRequest_fields, &req);
}

size_t encode_err(void *ctx, uint8_t *buf, size_t cap) {
  pb_ostream_t s = pb_ostream_from_buffer(buf, cap);
  if (!pb_encode(&s, proxyapi_BluetoothGATTErrorResponse_fields, ctx)) return 0;
  return s.bytes_written;
}

// One frame through api_server::send_to (the targeted send path every
// GATT reply uses) to client id 1, the loopback client.
bool api_send(int32_t marker) {
  proxyapi_BluetoothGATTErrorResponse m = proxyapi_BluetoothGATTErrorResponse_init_zero;
  m.address = 0x5E5E;
  m.error = marker;
  return api_server::send_to(1, proxyapi::MSG_BLUETOOTH_GATT_ERROR_RESPONSE,
                             &encode_err, &m);
}

bool read_error(proxyapi_BluetoothGATTErrorResponse *out, uint16_t *type) {
  size_t len = 0;
  if (!read_frame(type, &len)) return false;
  if (*type != proxyapi::MSG_BLUETOOTH_GATT_ERROR_RESPONSE) return true;
  pb_istream_t s = pb_istream_from_buffer(g_rx, len);
  *out = proxyapi_BluetoothGATTErrorResponse_init_zero;
  return pb_decode(&s, proxyapi_BluetoothGATTErrorResponse_fields, out);
}

void step_get_services_ok() {
  if (!get_services_request()) return result("getservices_ok", false, "send");
  uint16_t t1 = 0, t2 = 0;
  size_t l = 0;
  bool ok = read_frame(&t1, &l) && read_frame(&t2, &l);
  result("getservices_ok",
         ok && t1 == proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_RESPONSE &&
             t2 == proxyapi::MSG_BLUETOOTH_GATT_GET_SERVICES_DONE_RESPONSE,
         "types %u,%u", t1, t2);
}

void step_get_services_oom() {
  const uint32_t c0 = oom_guard::count();
  hog(12 * 1024);  // < sizeof(ServicesEncodeCtx); room for lwIP
  const uint32_t c1 = oom_guard::count();
  const size_t f = free_now(), lg = largest_now();
  bool sent = get_services_request();
  proxyapi_BluetoothGATTErrorResponse e{};
  uint16_t t = 0;
  bool got = sent && read_error(&e, &t);
  const uint32_t c2 = oom_guard::count();
  unhog();
  result("getservices_oom",
         got && t == proxyapi::MSG_BLUETOOTH_GATT_ERROR_RESPONSE &&
             e.error == -3 &&
             e.address == ble_backend::gatt_discovery::kOomSelftestAddress &&
             c2 > c1,
         "free %u largest %u; reply type %u error %ld; oom %lu->%lu->%lu",
         static_cast<unsigned>(f), static_cast<unsigned>(lg), t,
         static_cast<long>(e.error), static_cast<unsigned long>(c0),
         static_cast<unsigned long>(c1), static_cast<unsigned long>(c2));
}

void step_nothrow_new() {
  hog(0);
  const size_t lg = largest_now();
  char *a = new (std::nothrow) char[256];
  keep(a);
  auto u = oom_guard::make_unique<std::array<uint8_t, 512>>();
  keep(u.get());
  const bool ok = a == nullptr && u == nullptr;
  delete[] a;
  unhog();
  result("nothrow_new", ok, "largest %u; new(nothrow) %s, make_unique %s",
         static_cast<unsigned>(lg), a ? "non-null" : "nullptr",
         u ? "non-null" : "nullptr");
}

void step_throwing_new_recovers() {
  const uint32_t r0 = oom_guard::new_recovered();
  unhog_after(300);
  hog(0);
  const int64_t t0 = esp_timer_get_time();
  char *p = new char[1024];  // throwing form: must wait, not abort
  keep(p);
  const int64_t ms = (esp_timer_get_time() - t0) / 1000;
  wait_unhog_timer();
  delete[] p;
  result("throwing_new_waits", p != nullptr && oom_guard::new_recovered() == r0 + 1,
         "waited %lld ms, recovered %lu", static_cast<long long>(ms),
         static_cast<unsigned long>(oom_guard::new_recovered()));
}

void step_send_recovers() {
  unhog_after(3000);
  hog(0);
  const int64_t t0 = esp_timer_get_time();
  const bool sent = api_send(-77);
  const int64_t ms = (esp_timer_get_time() - t0) / 1000;
  wait_unhog_timer();
  proxyapi_BluetoothGATTErrorResponse e{};
  uint16_t t = 0;
  set_rcv_timeout(15000);
  const bool got = read_error(&e, &t) && e.error == -77;
  result("send_heap_recovers", sent && got && api_server::has_active_client(),
         "send_to %s after %lld ms; frame %s; client %s", sent ? "true" : "false",
         static_cast<long long>(ms), got ? "received" : "missing",
         api_server::has_active_client() ? "kept" : "dropped");
}

void step_send_fails() {
  // A second, idle client keeps the reaped one from being the last:
  // last-client teardown stops the BLE scanner, which is not running here.
  const int fd2 = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(proxy::API_PORT);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::connect(fd2, reinterpret_cast<sockaddr *>(&a), sizeof(a));
  vTaskDelay(pdMS_TO_TICKS(200));
  hog(0);
  const int64_t t0 = esp_timer_get_time();
  const bool sent = api_send(-78);
  const int64_t ms = (esp_timer_get_time() - t0) / 1000;
  unhog();
  // Reaped: the server shut the socket down (RST); our recv sees it.
  set_rcv_timeout(5000);
  uint8_t b;
  const ssize_t r = ::recv(g_fd, &b, 1, 0);
  const int e = errno;
  vTaskDelay(pdMS_TO_TICKS(500));
  result("send_heap_exhausted",
         !sent && r <= 0 && api_server::has_active_client(),
         "send_to %s after %lld ms; client 1 recv=%d errno=%d (reaped); "
         "client 2 %s",
         sent ? "true" : "false", static_cast<long long>(ms),
         static_cast<int>(r), r < 0 ? e : 0,
         api_server::has_active_client() ? "still connected" : "gone");
}

}  // namespace

void run() {
  {
    esp_timer_create_args_t a{};
    a.callback = &unhog_cb;
    a.name = "unhog";
    ESP_ERROR_CHECK(esp_timer_create(&a, &g_unhog_timer));
  }
  ESP_ERROR_CHECK(esp_netif_init());
  // Only the slot-table mutex: a reaped client's teardown releases its
  // (nonexistent) BLE links through it. NimBLE itself stays off.
  ble_backend::connection::init();
  api_server::start();
  ble_backend::publish::install(&api_server::send_async, &api_server::send_to,
                                &api_server::has_active_client);
  vTaskDelay(pdMS_TO_TICKS(300));
  ESP_LOGI(TAG, "start: free %u largest %u", static_cast<unsigned>(free_now()),
           static_cast<unsigned>(largest_now()));

  if (!connect_client()) {
    result("connect", false, "loopback client did not register");
  } else {
    step_get_services_ok();
    step_get_services_oom();
    step_get_services_ok();  // and it works again once heap is back
    step_nothrow_new();
    step_throwing_new_recovers();
    step_send_recovers();
    step_send_fails();
  }
  std::printf("OOM-SELFTEST summary: %d failure(s); oom count %lu; free %u\n",
              g_fail, static_cast<unsigned long>(oom_guard::count()),
              static_cast<unsigned>(free_now()));
  // Last: a throwing new with no heap coming back must abort with the
  // oom_guard message (crash_capture records it).
  std::printf("OOM-SELFTEST final: throwing new, heap gone for good -> expect abort\n");
  hog(0);
  char *p = new char[2048];
  keep(p);
  std::printf("OOM-SELFTEST final: FAIL new returned %p\n", static_cast<void *>(p));
  for (;;) vTaskDelay(portMAX_DELAY);
}

}  // namespace api_server::oom_selftest

#endif  // NBP_OOM_SELFTEST
