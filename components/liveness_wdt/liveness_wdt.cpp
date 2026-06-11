#include "liveness_wdt.h"

#include "sdkconfig.h"

#ifdef CONFIG_NBP_LIVENESS_WDT

#include "wifi_sta.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include <atomic>
#include <cerrno>
#include <cstring>

namespace liveness_wdt {

namespace {

constexpr const char *TAG = "liveness";
constexpr const char *NVS_NS = "livewdt";

// Don't arm until the network has had a fair chance to come up — skips the
// boot/first-associate transient. Also re-checked against the once-connected latch.
constexpr uint32_t GRACE_S = 90;
// Base scheduler tick: probes run every `interval_s`, but the task wakes this
// often so the forced self-test reboots promptly and config changes apply soon.
constexpr uint32_t TICK_MS = 2000;
// Per-target connect timeout. Generous enough for ARP + a starved radio without
// letting the task stall: select() bounds it hard.
constexpr int CONNECT_TIMEOUT_MS = 3000;

// Secondary probe target: a known-open host on the LAN the router bridges to
// (the MQTT broker). The primary target is the STA's own default gateway, read
// at runtime. A cycle fails only if BOTH are unreachable.
constexpr const char *BROKER_IP = "192.168.1.200";
constexpr uint16_t BROKER_PORT = 1882;

std::atomic<bool> g_started{false};
std::atomic<bool> g_enabled{true};
std::atomic<uint32_t> g_interval_s{CONFIG_NBP_LIVENESS_INTERVAL_SECS};
std::atomic<uint32_t> g_threshold{CONFIG_NBP_LIVENESS_THRESHOLD};
std::atomic<uint32_t> g_failures{0};
std::atomic<uint32_t> g_last_ok_s{0};
std::atomic<bool> g_have_ok{false};
std::atomic<bool> g_force_test{false};

// Rolling WiFi-quality history: one sample per probe cycle. 120 @ the 30 s
// default = one hour. ~720 B; guarded by a mutex (httpd reader vs probe writer).
constexpr size_t HIST_LEN = 120;
WifiSample g_hist[HIST_LEN];
size_t g_hist_head = 0;     // next slot to write
uint32_t g_hist_total = 0;  // total samples ever recorded
SemaphoreHandle_t g_hist_mtx = nullptr;

uint32_t uptime_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

// Clamp a microsecond duration into the uint16 ms field (0xFFFF reserved for loss).
uint16_t clamp_rtt_ms(int64_t us) {
  int64_t ms = us / 1000;
  if (ms < 0) ms = 0;
  if (ms > 0xFFFE) ms = 0xFFFE;
  return static_cast<uint16_t>(ms);
}

void load_nvs() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  uint8_t en = 0;
  uint32_t v = 0;
  if (nvs_get_u8(h, "en", &en) == ESP_OK) g_enabled.store(en != 0);
  if (nvs_get_u32(h, "intv", &v) == ESP_OK && v >= 5 && v <= 3600) g_interval_s.store(v);
  if (nvs_get_u32(h, "thr", &v) == ESP_OK && v >= 1 && v <= 60) g_threshold.store(v);
  nvs_close(h);
}

void save_nvs() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "en", g_enabled.load() ? 1 : 0);
  nvs_set_u32(h, "intv", g_interval_s.load());
  nvs_set_u32(h, "thr", g_threshold.load());
  nvs_commit(h);
  nvs_close(h);
}

// Non-blocking TCP connect with a hard select() timeout. Returns true if the
// host is reachable — a refused connection (RST) counts as reachable, since it
// still proves the L3/ARP path works. Only a timeout or a no-route error means
// unreachable. The task can never block here.
bool reachable(uint32_t ip_be, uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return true;  // local resource hiccup — don't blame the network

  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = ip_be;

  bool ok = true;
  int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  if (rc == 0) {
    ok = true;  // connected immediately
  } else if (errno == EINPROGRESS) {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    timeval tv{.tv_sec = CONNECT_TIMEOUT_MS / 1000, .tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000};
    int sel = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
    if (sel <= 0) {
      ok = false;  // timeout (or select error) → unreachable
    } else {
      int soerr = 0;
      socklen_t len = sizeof(soerr);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
      // Reached the host unless the stack reports no route / timeout.
      ok = !(soerr == EHOSTUNREACH || soerr == ENETUNREACH || soerr == ETIMEDOUT ||
             soerr == EHOSTDOWN || soerr == ENETDOWN);
    }
  } else {
    ok = !(errno == EHOSTUNREACH || errno == ENETUNREACH || errno == ETIMEDOUT);
  }
  ::close(fd);
  return ok;
}

// Probe the STA default gateway (port 80) and the broker. Reachable if either
// responds; *rtt_ms gets the connect latency of whichever answered (0xFFFF on
// total failure).
bool probe_cycle(uint16_t *rtt_ms) {
  uint32_t gw_be = 0;
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t info = {};
  if (sta != nullptr && esp_netif_get_ip_info(sta, &info) == ESP_OK) gw_be = info.gw.addr;

  if (gw_be != 0) {
    int64_t t0 = esp_timer_get_time();
    if (reachable(gw_be, 80)) {
      *rtt_ms = clamp_rtt_ms(esp_timer_get_time() - t0);
      return true;
    }
  }
  uint32_t broker_be = ::inet_addr(BROKER_IP);
  if (broker_be != INADDR_NONE) {
    int64_t t0 = esp_timer_get_time();
    if (reachable(broker_be, BROKER_PORT)) {
      *rtt_ms = clamp_rtt_ms(esp_timer_get_time() - t0);
      return true;
    }
  }
  *rtt_ms = 0xFFFF;
  return false;
}

// Snapshot RSSI (uplink via STA AP-record, downlink via the weakest associated
// client) and append a sample to the ring. Called once per probe cycle.
void record_sample(bool ok, uint16_t rtt_ms) {
  WifiSample s{};
  s.rtt_ms = ok ? rtt_ms : 0xFFFF;

  wifi_ap_record_t ap{};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s.sta_rssi = ap.rssi;

  wifi_sta_list_t sl{};
  if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK) {
    s.sta_count = static_cast<uint8_t>(sl.num);
    int8_t worst = 0;
    for (int i = 0; i < sl.num; ++i) {
      int8_t r = sl.sta[i].rssi;
      if (i == 0 || r < worst) worst = r;
    }
    s.ap_rssi = worst;
  }

  if (g_hist_mtx == nullptr) return;
  xSemaphoreTake(g_hist_mtx, portMAX_DELAY);
  g_hist[g_hist_head] = s;
  g_hist_head = (g_hist_head + 1) % HIST_LEN;
  ++g_hist_total;
  xSemaphoreGive(g_hist_mtx);
}

[[noreturn]] void do_reboot(const char *why) {
  ESP_LOGE(TAG, "%s — restarting to recover", why);
  vTaskDelay(pdMS_TO_TICKS(150));  // let the log drain to serial/web-console
  esp_restart();
}

[[noreturn]] void task(void *) {
  bool seen_connected = false;
  uint32_t last_probe_s = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(TICK_MS));

    if (g_force_test.exchange(false)) do_reboot("forced liveness self-test");
    if (!g_enabled.load()) continue;

    uint32_t now = uptime_s();
    if (now < GRACE_S) continue;
    if (!seen_connected) {
      if (wifi_sta::is_connected()) seen_connected = true;
      else continue;  // never had a link yet — nothing to recover
    }
    if (now - last_probe_s < g_interval_s.load()) continue;
    last_probe_s = now;

    // A disassociated STA is the native reconnect path's job, not ours.
    // Rebooting on top of an in-progress reconnect — common on a weak link that
    // keeps dropping and re-associating — just throws the progress away. So
    // hold the counter at 0 while the link is down (record the outage for the
    // chart) and reboot only for an associated-but-dead path: a true wedge.
    if (!wifi_sta::is_connected()) {
      g_failures.store(0);
      record_sample(false, 0xFFFF);
      continue;
    }

    uint16_t rtt = 0xFFFF;
    bool ok = probe_cycle(&rtt);
    record_sample(ok, rtt);

    if (ok) {
      g_failures.store(0);
      g_last_ok_s.store(now);
      g_have_ok.store(true);
      continue;
    }

    uint32_t f = g_failures.fetch_add(1) + 1;
    uint32_t thr = g_threshold.load();
    ESP_LOGW(TAG, "upstream LAN unreachable (%u/%u)", (unsigned)f, (unsigned)thr);
    if (f >= thr) do_reboot("upstream LAN unreachable past threshold");
  }
}

}  // namespace

void start() {
  if (g_started.exchange(true)) return;
  load_nvs();
  g_hist_mtx = xSemaphoreCreateMutex();
  xTaskCreate(&task, "liveness", 4096, nullptr, 3, nullptr);
  ESP_LOGI(TAG, "started: interval=%us threshold=%u enabled=%d",
           (unsigned)g_interval_s.load(), (unsigned)g_threshold.load(), g_enabled.load());
}

State get_state() {
  State s{};
  s.enabled = g_enabled.load();
  s.interval_s = g_interval_s.load();
  s.threshold = g_threshold.load();
  s.failures = g_failures.load();
  s.last_ok_age_s = g_have_ok.load() ? (uptime_s() - g_last_ok_s.load()) : UINT32_MAX;
  return s;
}

const char *set_params(int enabled, int interval_s, int threshold) {
  if (interval_s >= 0 && (interval_s < 5 || interval_s > 3600)) return "interval out of range (5..3600)";
  if (threshold >= 0 && (threshold < 1 || threshold > 60)) return "threshold out of range (1..60)";
  if (enabled >= 0) g_enabled.store(enabled != 0);
  if (interval_s >= 0) g_interval_s.store(static_cast<uint32_t>(interval_s));
  if (threshold >= 0) g_threshold.store(static_cast<uint32_t>(threshold));
  save_nvs();
  return nullptr;
}

void force_fail_test() { g_force_test.store(true); }

size_t get_history(WifiSample *out, size_t max, uint32_t *interval_s) {
  if (interval_s) *interval_s = g_interval_s.load();
  if (g_hist_mtx == nullptr || out == nullptr || max == 0) return 0;
  xSemaphoreTake(g_hist_mtx, portMAX_DELAY);
  size_t avail = g_hist_total < HIST_LEN ? g_hist_total : HIST_LEN;
  size_t n = avail < max ? avail : max;
  // oldest index, then skip forward if the caller wants fewer than available.
  size_t oldest = (g_hist_total < HIST_LEN) ? 0 : g_hist_head;
  size_t start = (oldest + (avail - n)) % HIST_LEN;
  for (size_t i = 0; i < n; ++i) out[i] = g_hist[(start + i) % HIST_LEN];
  xSemaphoreGive(g_hist_mtx);
  return n;
}

}  // namespace liveness_wdt

#endif  // CONFIG_NBP_LIVENESS_WDT
