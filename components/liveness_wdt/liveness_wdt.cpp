#include "liveness_wdt.h"
#include "liveness_policy.h"

#include "sdkconfig.h"

#ifdef CONFIG_NBP_LIVENESS_WDT

#include "wifi_sta.h"

#include "esp_attr.h"
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

#include <algorithm>
#include <atomic>
#include <cstdlib>
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

// Optional secondary probe target ("ip:port", Kconfig). Empty by default:
// v0.2.3 hard-coded a lab broker here, which on any other LAN never
// answered (issue #2). The primary target is the STA's default gateway.
constexpr const char *EXTRA_TARGET = CONFIG_NBP_LIVENESS_EXTRA_TARGET;

// Data from an API client within this many seconds (and at least one probe
// interval) also proves the LAN path. HA's keepalive ping is every 20 s.
constexpr uint32_t CLIENT_RX_FRESH_S = 60;
// ...but only from a session that has stayed open this long (and at least two
// probe intervals). On a degraded uplink (2026-11-16, .231: -80 dBm, 45 % ping
// loss) batmon lost its session and reconnected every ~40 s, and each
// handshake looked like fresh traffic, so the watchdog never fired.
constexpr uint32_t CLIENT_SESSION_MIN_S = 60;

int64_t (*g_client_rx_us)(int64_t min_age_us) = nullptr;

// EXTRA_TARGET, parsed once at start(); ip 0 = none.
uint32_t g_extra_ip_be = 0;
uint16_t g_extra_port = 0;

// Survives esp_restart (not power-on): set just before a liveness reboot so
// the next boot starts armed. Otherwise a wedge that comes back before the
// first probe (~90 s, e.g. SoftAP clients resuming heavy forwarding) would
// leave the device unarmed and stuck. g_carry_n counts liveness reboots in
// a row with no probe success in between; each one doubles the threshold
// (up to 32x, ~1.6 h at the defaults); after that the next boot starts
// unarmed. So a device that moved to a LAN whose gateway ignores port 80
// backs off and then stops instead of looping. A successful probe resets
// the backoff. Checked with a magic word.
__NOINIT_ATTR uint32_t g_armed_carry;
__NOINIT_ATTR uint32_t g_carry_n;
constexpr uint32_t ARMED_CARRY_MAGIC = 0x4C564152u;  // "LVAR"
constexpr uint32_t CARRY_MAX_SHIFT = 5;
uint32_t g_backoff_shift = 0;  // this boot's threshold multiplier (log2)
bool g_have_probe_ok = false;  // a probe target answered this boot

std::atomic<bool> g_started{false};
std::atomic<bool> g_enabled{true};
std::atomic<uint32_t> g_interval_s{CONFIG_NBP_LIVENESS_INTERVAL_SECS};
std::atomic<uint32_t> g_threshold{CONFIG_NBP_LIVENESS_THRESHOLD};
std::atomic<uint32_t> g_failures{0};
std::atomic<uint32_t> g_last_ok_s{0};
std::atomic<bool> g_have_ok{false};
// Set once a probe target (not client traffic) answered this boot. Reboots
// are armed only after that: a target that never answered proves nothing
// about a wedge, and on a LAN whose gateway filters port 80 an unarmed
// watchdog would otherwise reboot every few minutes whenever HA is away.
std::atomic<bool> g_armed{false};
std::atomic<bool> g_force_test{false};

// Rolling WiFi-quality history: one sample per probe cycle. 120 @ the 30 s
// default = one hour. ~720 B; guarded by a mutex (httpd reader vs probe writer).
constexpr size_t HIST_LEN = 120;
WifiSample g_hist[HIST_LEN];
size_t g_hist_head = 0;     // next slot to write
uint32_t g_hist_total = 0;  // total samples ever recorded
SemaphoreHandle_t g_hist_mtx = nullptr;

// Window decider (liveness_policy.h). Touched only by the liveness task;
// get_state() reads the published counters below.
policy::Decider g_decider;
std::atomic<uint32_t> g_window{0};
std::atomic<uint32_t> g_reconnects{0};

// Heartbeat: the task stamps this every tick. lwIP core locking is off, so
// the probe's socket calls go through the tcpip thread's mailbox, and a
// wedged tcpip thread would block this task forever with nothing else
// watching (the IDF task WDT only watches the idle tasks and only logs). An
// esp_timer check, which does not touch lwIP, reboots if the stamp goes
// stale. Bound: one tick + two 3 s connects + an API txq query is well under
// a minute; the limit is generous so a slow but alive task never trips it.
constexpr int64_t HEARTBEAT_STALE_US = 5LL * 60 * 1000000;
constexpr int64_t HEARTBEAT_CHECK_US = 30LL * 1000000;
std::atomic<int64_t> g_heartbeat_us{0};
esp_timer_handle_t g_heartbeat_timer = nullptr;

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

// Non-blocking TCP connect with a hard select() timeout. Returns true only
// when the host answered: a completed handshake, or a refusal (RST), which
// (ECONNRESET in lwIP), which still proves the L3/ARP path. Everything else is unreachable, including
// local failures (no socket, ENOMEM/ENOBUFS, select error): a probe that
// could not run proves nothing, and a heap- or socket-exhausted stack is a
// wedge this watchdog should count, not excuse. The task can never block here.
bool reachable(uint32_t ip_be, uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return false;

  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = ip_be;

  bool ok = false;
  int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  if (rc == 0) {
    ok = true;  // connected immediately
  } else if (errno == EINPROGRESS) {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    timeval tv{.tv_sec = CONNECT_TIMEOUT_MS / 1000, .tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000};
    int sel = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
    if (sel > 0) {
      int soerr = 0;
      socklen_t len = sizeof(soerr);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0) {
        // lwIP reports a SYN answered by RST as ECONNRESET (err_to_errno
        // ERR_RST); it never produces ECONNREFUSED. Both mean "host answered".
        ok = (soerr == 0 || soerr == ECONNRESET || soerr == ECONNREFUSED);
      }
    }
  } else {
    ok = (errno == ECONNRESET || errno == ECONNREFUSED);
  }
  ::close(fd);
  return ok;
}

bool parse_target(const char *spec, uint32_t *ip_be, uint16_t *port) {
  if (spec == nullptr || *spec == '\0') return false;
  const char *colon = std::strrchr(spec, ':');
  if (colon == nullptr || colon == spec) return false;
  char host[16];
  size_t hl = static_cast<size_t>(colon - spec);
  if (hl >= sizeof(host)) return false;
  std::memcpy(host, spec, hl);
  host[hl] = '\0';
  char *end = nullptr;
  long p = std::strtol(colon + 1, &end, 10);
  if (end == colon + 1 || *end != '\0' || p <= 0 || p > 65535) return false;
  uint32_t ip = ::inet_addr(host);
  if (ip == INADDR_NONE) return false;
  *ip_be = ip;
  *port = static_cast<uint16_t>(p);
  return true;
}

// Probe the STA default gateway (port 80), then the optional extra target.
// Reachable if either responds; *rtt_ms gets the connect latency of whichever
// answered (0xFFFF on total failure).
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
  if (g_extra_ip_be != 0) {
    int64_t t0 = esp_timer_get_time();
    if (reachable(g_extra_ip_be, g_extra_port)) {
      *rtt_ms = clamp_rtt_ms(esp_timer_get_time() - t0);
      return true;
    }
  }
  *rtt_ms = 0xFFFF;
  return false;
}

// True if an API session that has been open for at least the probe window
// sent data within the freshness window.
bool client_traffic_fresh() {
  if (g_client_rx_us == nullptr) return false;
  const uint32_t interval_s = g_interval_s.load();
  const uint32_t min_age_s = std::max(CLIENT_SESSION_MIN_S, 2 * interval_s);
  int64_t last = g_client_rx_us(static_cast<int64_t>(min_age_s) * 1000000);
  if (last <= 0) return false;
  uint32_t window_s = std::max(CLIENT_RX_FRESH_S, interval_s + 10);
  return esp_timer_get_time() - last < static_cast<int64_t>(window_s) * 1000000;
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

[[noreturn]] void do_reboot(const char *why, bool carry = true) {
  ESP_LOGE(TAG, "%s — restarting to recover", why);
  // Stop carrying once the backoff is at its cap with still no probe
  // success: the next boot starts unarmed, so a board on a LAN where no
  // probe target answers ends up rebooting at most ~6 times, not forever.
  // The forced self-test carries only after a real probe success, so it
  // exercises this path without starting a backoff chain.
  g_carry_n = g_have_probe_ok ? 0 : g_backoff_shift + 1;
  g_armed_carry = (carry && g_carry_n <= CARRY_MAX_SHIFT) ? ARMED_CARRY_MAGIC : 0;
  vTaskDelay(pdMS_TO_TICKS(150));  // let the log drain to serial/web-console
  esp_restart();
}

// esp_timer task context: must not block or take long. Carries the armed
// state like a probe-driven reboot does.
void heartbeat_check(void *) {
  const int64_t hb = g_heartbeat_us.load();
  if (hb == 0 || !g_enabled.load()) return;
  if (esp_timer_get_time() - hb < HEARTBEAT_STALE_US) return;
  ESP_LOGE(TAG, "liveness task stalled for %llds (blocked in lwIP?); restarting",
           static_cast<long long>((esp_timer_get_time() - hb) / 1000000));
  g_carry_n = g_have_probe_ok ? 0 : g_backoff_shift + 1;
  g_armed_carry = g_carry_n <= CARRY_MAX_SHIFT ? ARMED_CARRY_MAGIC : 0;
  esp_restart();
}

[[noreturn]] void task(void *) {
  bool seen_connected = false;
  uint32_t last_probe_s = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    g_heartbeat_us.store(esp_timer_get_time());

    if (g_force_test.exchange(false)) do_reboot("forced liveness self-test", /*carry=*/g_have_probe_ok);
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
    // disassociated cycles are recorded for the chart but not judged. The
    // window keeps what it had: a link that flaps between bad associations
    // still accumulates toward a trigger (liveness_policy.h).
    if (!wifi_sta::is_connected()) {
      record_sample(false, 0xFFFF);
      continue;
    }

    uint16_t rtt = 0xFFFF;
    const bool probe_ok = probe_cycle(&rtt);
    record_sample(probe_ok, rtt);
    g_heartbeat_us.store(esp_timer_get_time());

    if (probe_ok) {
      g_have_probe_ok = true;
      g_backoff_shift = 0;
      if (!g_armed.exchange(true)) ESP_LOGI(TAG, "probe target answered; armed");
    }
    // A long-lived client session whose sends are being ACKed also proves
    // the path (e.g. a gateway that ignores :80). Asked only when the probe
    // did not pass cleanly. The chart still records the probe.
    const bool probe_clean = probe_ok && rtt <= policy::SLOW_RTT_MS;
    const bool client_ok = !probe_clean && client_traffic_fresh();
    const bool bad = policy::cycle_bad(probe_ok, rtt, client_ok);

    if (!bad) {
      g_last_ok_s.store(now);
      g_have_ok.store(true);
    }

    const uint32_t thr = g_threshold.load() << g_backoff_shift;
    const policy::Action act = g_decider.on_cycle(bad, thr, g_armed.load());
    g_failures.store(g_decider.bad());
    g_window.store(g_decider.window());
    if (bad && g_armed.load()) {
      if (probe_ok) {
        ESP_LOGW(TAG, "upstream LAN slow (%u ms; %u/%u bad of last %u)", (unsigned)rtt,
                 (unsigned)g_decider.bad(), (unsigned)thr, (unsigned)g_decider.window());
      } else {
        ESP_LOGW(TAG, "upstream LAN unreachable (%u/%u bad of last %u)",
                 (unsigned)g_decider.bad(), (unsigned)thr, (unsigned)g_decider.window());
      }
    }
    if (act == policy::Action::RECONNECT) {
      g_reconnects.store(g_decider.reconnects());
      // Re-associate: wifi_sta's STA_DISCONNECTED handler calls
      // esp_wifi_connect() again. Costs seconds and keeps the BLE links; on
      // 2026-11-17 every recovery followed a re-association.
      ESP_LOGE(TAG, "upstream LAN degraded past threshold; forcing WiFi re-association");
      esp_wifi_disconnect();
    } else if (act == policy::Action::REBOOT) {
      do_reboot("upstream LAN still degraded after a WiFi re-association");
    }
  }
}

}  // namespace

void set_client_rx_source(int64_t (*stable_rx_us)(int64_t min_age_us)) { g_client_rx_us = stable_rx_us; }

void start() {
  if (g_started.exchange(true)) return;
  load_nvs();
  if (EXTRA_TARGET[0] != '\0') {
    if (parse_target(EXTRA_TARGET, &g_extra_ip_be, &g_extra_port)) {
      ESP_LOGI(TAG, "extra probe target %s", EXTRA_TARGET);
    } else {
      ESP_LOGW(TAG, "ignoring bad extra probe target '%s' (want ip:port)", EXTRA_TARGET);
    }
  }
  const bool carried = esp_reset_reason() == ESP_RST_SW && g_armed_carry == ARMED_CARRY_MAGIC;
  g_armed_carry = 0;
  if (carried) {
    g_backoff_shift = std::min(g_carry_n, CARRY_MAX_SHIFT);
    g_armed.store(true);
    ESP_LOGW(TAG, "previous boot was a liveness restart; starting armed (threshold x%u)",
             1u << g_backoff_shift);
  }
  g_carry_n = 0;
  g_hist_mtx = xSemaphoreCreateMutex();
  xTaskCreate(&task, "liveness", 4096, nullptr, 3, nullptr);
  const esp_timer_create_args_t hb = {.callback = &heartbeat_check, .arg = nullptr,
                                      .dispatch_method = ESP_TIMER_TASK,
                                      .name = "liveness_hb", .skip_unhandled_events = true};
  if (esp_timer_create(&hb, &g_heartbeat_timer) == ESP_OK) {
    esp_timer_start_periodic(g_heartbeat_timer, HEARTBEAT_CHECK_US);
  } else {
    ESP_LOGW(TAG, "heartbeat timer not created; a blocked liveness task goes unnoticed");
  }
  ESP_LOGI(TAG, "started: interval=%us threshold=%u enabled=%d",
           (unsigned)g_interval_s.load(), (unsigned)g_threshold.load(), g_enabled.load());
}

State get_state() {
  State s{};
  s.enabled = g_enabled.load();
  s.interval_s = g_interval_s.load();
  s.threshold = g_threshold.load();
  s.failures = g_failures.load();
  s.armed = g_armed.load();
  s.window = g_window.load();
  s.reconnects = g_reconnects.load();
  s.slow_ms = policy::SLOW_RTT_MS;
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
