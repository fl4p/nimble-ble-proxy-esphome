#include "stats.h"

#include "ble_backend.h"
#include "connection.h"
#include "driver/temperature_sensor.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#if CONFIG_NBP_WIFI
#include "esp_wifi.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "proxy_config.h"
#include "scanner.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace api_server::stats {

namespace {

constexpr const char *TAG = "stats";

std::atomic<uint32_t> g_reads{0};
std::atomic<uint32_t> g_writes{0};
std::atomic<uint32_t> g_notifies{0};

#if CONFIG_NBP_WEB_CONSOLE
// ---- log ring buffer ----
//
// esp_log_set_vprintf installs a process-wide hook called from any
// task that does ESP_LOGx. We mirror each formatted line into a flat
// ring buffer indexed by a monotonic `g_log_seq` (total bytes ever
// written). `/log?since=N` returns the slice [N, g_log_seq). When the
// client falls behind by more than RING_SIZE, we resync from the
// oldest byte still resident.
//
// The hook keeps calling the original vprintf so UART/JTAG output is
// preserved — this is purely a tee.

// Bigger ring when NimBLE is compiled at DEBUG — DEBUG output during
// connect/discovery rolls a small ring in well under a second.
#if defined(CONFIG_BT_NIMBLE_LOG_LEVEL_DEBUG) || defined(CONFIG_NIMBLE_CPP_LOG_LEVEL_DEBUG)
constexpr size_t LOG_RING_SIZE = 65536;
#else
constexpr size_t LOG_RING_SIZE = 8192;
#endif
char g_log_ring[LOG_RING_SIZE];
uint32_t g_log_seq = 0;
SemaphoreHandle_t g_log_mutex = nullptr;
vprintf_like_t g_old_vprintf = nullptr;

void log_ring_append(const char *data, size_t len) {
  if (len == 0 || g_log_mutex == nullptr) return;
  // If a single line is bigger than the whole ring, keep only the tail.
  if (len > LOG_RING_SIZE) {
    data += len - LOG_RING_SIZE;
    len = LOG_RING_SIZE;
  }
  xSemaphoreTake(g_log_mutex, portMAX_DELAY);
  size_t pos = g_log_seq % LOG_RING_SIZE;
  size_t first = std::min(len, LOG_RING_SIZE - pos);
  std::memcpy(g_log_ring + pos, data, first);
  if (len > first) {
    std::memcpy(g_log_ring, data + first, len - first);
  }
  g_log_seq += len;
  xSemaphoreGive(g_log_mutex);
}

int log_vprintf(const char *fmt, va_list argptr) {
  // va_copy before consuming argptr in old_vprintf; vsnprintf into a
  // stack scratch — accept truncation for lines >sizeof(buf), no malloc
  // in a logging path.
  char buf[256];
  va_list ap;
  va_copy(ap, argptr);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    size_t copy = (n >= static_cast<int>(sizeof(buf))) ? sizeof(buf) - 1
                                                       : static_cast<size_t>(n);
    log_ring_append(buf, copy);
  }
  return g_old_vprintf ? g_old_vprintf(fmt, argptr) : std::vprintf(fmt, argptr);
}

esp_err_t log_get(httpd_req_t *req) {
  uint32_t since = 0;
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[32];
    if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
      since = static_cast<uint32_t>(std::strtoul(val, nullptr, 10));
    }
  }

  constexpr size_t CHUNK = 1024;
  static char bounce[CHUNK];

  // First slice snapshots g_log_seq. The header advertises that snapshot
  // so the client's next poll resumes from a deterministic boundary; new
  // bytes arriving during the drain loop are picked up on the next poll.
  uint32_t target_seq = 0;
  size_t n = build_log_slice(&since, bounce, sizeof(bounce), &target_seq);

  char hdr[32];
  std::snprintf(hdr, sizeof(hdr), "%lu",
                static_cast<unsigned long>(target_seq));
  httpd_resp_set_hdr(req, "X-Log-Seq", hdr);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");

  if (n == 0) {
    return httpd_resp_send(req, "", 0);
  }

  esp_err_t r = httpd_resp_send_chunk(req, bounce, n);
  since += n;
  while (r == ESP_OK && since < target_seq) {
    uint32_t ignore;
    n = build_log_slice(&since, bounce, sizeof(bounce), &ignore);
    if (n == 0) break;
    r = httpd_resp_send_chunk(req, bounce, n);
    since += n;
  }
  if (r == ESP_OK) {
    r = httpd_resp_send_chunk(req, nullptr, 0);  // terminator
  }
  return r;
}
#endif  // CONFIG_NBP_WEB_CONSOLE

// ---- NimBLE log-level override (NVS-persisted) ----
//
// One slot, one knob: a single esp_log_level applied to all the
// NimBLE-Cpp tags we know about. Stored in NVS namespace "stats" as
// key "nimble_lvl" (int8). Default = WARN, which is what we had
// hard-coded for NimBLEScan before.

constexpr const char *NVS_NS = "stats";
constexpr const char *NVS_LEVEL_KEY = "nimble_lvl";
constexpr esp_log_level_t DEFAULT_NIMBLE_LEVEL = ESP_LOG_WARN;
#ifdef CONFIG_NBP_SMP
constexpr const char *NVS_PASSKEY_KEY = "ble_passkey";
constexpr uint32_t DEFAULT_PASSKEY = 123456;
#endif

// Every NimBLE-Cpp log tag we've observed. Adding more is harmless;
// esp_log_level_set just stores the mapping. Split into "scan" and
// "core" so /trace can silence scanner noise while keeping host/client
// debug output flowing to the log ring.
constexpr const char *NIMBLE_SCAN_TAGS[] = {
    "NimBLEScan", "NimBLEAdvertisedDevice",
};
constexpr const char *NIMBLE_CORE_TAGS[] = {
    "NimBLE", "NimBLEDevice", "NimBLEClient",
    "NimBLERemoteCharacteristic",
};

esp_log_level_t g_current_nimble_level = DEFAULT_NIMBLE_LEVEL;

void apply_level(esp_log_level_t lvl) {
  for (const char *tag : NIMBLE_CORE_TAGS) esp_log_level_set(tag, lvl);
  // NimBLE-Cpp's scanner logs "New advertiser: <mac>" at INFO on every
  // advert with wantDuplicates=true — instantly floods the console at
  // INFO+. Cap scan tags at WARN regardless of the user-picked level;
  // they only become more verbose via /trace ON's explicit override.
  esp_log_level_t scan_lvl = (lvl < ESP_LOG_WARN) ? lvl : ESP_LOG_WARN;
  for (const char *tag : NIMBLE_SCAN_TAGS) esp_log_level_set(tag, scan_lvl);
  g_current_nimble_level = lvl;
}

#if CONFIG_NBP_WEB_CONSOLE
void log_ring_reset() {
  if (g_log_mutex == nullptr) return;
  xSemaphoreTake(g_log_mutex, portMAX_DELAY);
  g_log_seq = 0;
  xSemaphoreGive(g_log_mutex);
}
#endif

esp_err_t nvs_read_level(esp_log_level_t *out) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
  if (err != ESP_OK) return err;
  int8_t v = 0;
  err = nvs_get_i8(h, NVS_LEVEL_KEY, &v);
  nvs_close(h);
  if (err != ESP_OK) return err;
  *out = static_cast<esp_log_level_t>(v);
  return ESP_OK;
}

esp_err_t nvs_write_level(esp_log_level_t lvl) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_i8(h, NVS_LEVEL_KEY, static_cast<int8_t>(lvl));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

esp_err_t level_get(httpd_req_t *req) {
  char buf[32];
  size_t n = build_level_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t level_post(httpd_req_t *req) {
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  const char *err = handle_level_set(query);
  if (err) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}

#ifdef CONFIG_NBP_SMP
// SMP passkey (NVS-persisted). Default 123456 — covers most Victron
// SmartShunts. /passkey?val=000000 swaps at runtime.

esp_err_t nvs_read_passkey(uint32_t *out) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
  if (err != ESP_OK) return err;
  err = nvs_get_u32(h, NVS_PASSKEY_KEY, out);
  nvs_close(h);
  return err;
}

esp_err_t nvs_write_passkey(uint32_t pin) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_u32(h, NVS_PASSKEY_KEY, pin);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

esp_err_t passkey_get(httpd_req_t *req) {
  char buf[32];
  size_t n = build_passkey_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t passkey_post(httpd_req_t *req) {
  char query[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  const char *err = handle_passkey_set(query);
  if (err) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}
#endif  // CONFIG_NBP_SMP

// ---- TX power (NVS-persisted) ----
//
// WiFi: esp_wifi_set_max_tx_power takes int8 in 0.25 dBm units. Dropdown
// values are dBm, converted on apply. Setting too low risks dropping
// the LAN connection — see wifi_tx_revert_check for the safety net.
//
// BLE: esp_ble_tx_power_set with ESP_BLE_PWR_TYPE_DEFAULT maps to the
// nearest 3 dBm step the controller supports. No connectivity safety
// needed (LAN is unaffected).

constexpr const char *NVS_WIFI_TX_KEY = "wifi_tx";
constexpr const char *NVS_BLE_TX_KEY = "ble_tx";
constexpr const char *NVS_CPU_FREQ_KEY = "cpu_freq";
constexpr int8_t DEFAULT_WIFI_TX_DBM = 20;
constexpr int8_t DEFAULT_BLE_TX_DBM = 9;
constexpr int DEFAULT_CPU_FREQ_MHZ = 240;

int8_t g_wifi_tx_dbm = DEFAULT_WIFI_TX_DBM;
int8_t g_ble_tx_dbm = DEFAULT_BLE_TX_DBM;
int g_cpu_freq_mhz = DEFAULT_CPU_FREQ_MHZ;

bool g_light_sleep = false;

// Runtime-only: set by handle_txpower_set when wifi=0. Not persisted —
// a reboot brings WiFi back at the NVS-stored dBm so the device can't
// be bricked over BLE.
bool g_wifi_off = false;

esp_err_t apply_cpu_freq_mhz(int mhz, bool light_sleep) {
  // Pin min=max so the CPU is clamped exactly. light_sleep lets the
  // SoC coast between bursts of work — measurable temp drop on the
  // scanner workload, no behavioural change since any peripheral
  // interrupt wakes the cores.
  esp_pm_config_t cfg = {
      .max_freq_mhz = mhz,
      .min_freq_mhz = mhz,
      .light_sleep_enable = light_sleep,
  };
  esp_err_t err = esp_pm_configure(&cfg);
  if (err == ESP_OK) g_light_sleep = light_sleep;
  return err;
}

esp_power_level_t dbm_to_ble_lvl(int dbm) {
  if (dbm <= -12) return ESP_PWR_LVL_N12;
  if (dbm <= -9) return ESP_PWR_LVL_N9;
  if (dbm <= -6) return ESP_PWR_LVL_N6;
  if (dbm <= -3) return ESP_PWR_LVL_N3;
  if (dbm <= 0) return ESP_PWR_LVL_N0;
  if (dbm <= 3) return ESP_PWR_LVL_P3;
  if (dbm <= 6) return ESP_PWR_LVL_P6;
  return ESP_PWR_LVL_P9;
}

esp_err_t apply_ble_tx_dbm(int dbm) {
  return esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, dbm_to_ble_lvl(dbm));
}

esp_err_t apply_wifi_tx_dbm(int dbm) {
#if CONFIG_NBP_WIFI
  return esp_wifi_set_max_tx_power(static_cast<int8_t>(dbm * 4));
#else
  (void)dbm;
  return ESP_OK;
#endif
}

// Stop the WiFi radio entirely. Tears down the STA connection and turns
// the PHY off — the dashboard becomes unreachable over HTTP, but a BLE
// client (ble_httpd) keeps working. Idempotent.
esp_err_t apply_wifi_off() {
#if CONFIG_NBP_WIFI
  esp_err_t err = esp_wifi_stop();
  if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED) {
    return ESP_OK;
  }
  return err;
#else
  return ESP_OK;
#endif
}

esp_err_t nvs_read_i8(const char *key, int8_t *out) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
  if (err != ESP_OK) return err;
  err = nvs_get_i8(h, key, out);
  nvs_close(h);
  return err;
}

esp_err_t nvs_write_i8(const char *key, int8_t v) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_i8(h, key, v);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

esp_err_t txpower_get(httpd_req_t *req) {
  char buf[32];
  size_t n = build_txpower_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t cpufreq_get(httpd_req_t *req) {
  char buf[32];
  size_t n = build_cpufreq_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t cpufreq_post(httpd_req_t *req) {
  char query[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  const char *err = handle_cpufreq_set(query);
  if (err) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}

esp_err_t txpower_post(httpd_req_t *req) {
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  const char *err = handle_txpower_set(query);
  if (err) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}

esp_err_t trace_post(httpd_req_t *req) {
  char query[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    query[0] = '\0';
  }
  bool on = handle_trace_set(query);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, on ? "{\"trace\":true}" : "{\"trace\":false}",
                         HTTPD_RESP_USE_STRLEN);
}

esp_err_t reboot_post(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "rebooting\n", HTTPD_RESP_USE_STRLEN);
  schedule_reboot();
  return ESP_OK;
}

// ---- CPU + chip temperature ----
//
// CPU% is computed from per-core IDLE-task run-time counters between
// /stats.json polls. httpd serializes requests, so the static "last
// sample" state is safe. The result is the busy fraction since the
// previous call — clients polling at 1 Hz get a 1 s window; multiple
// clients see slightly jittered windows but each measurement is still
// internally consistent.
//
// Temperature uses the ESP32-S3 internal silicon-temperature sensor
// (±1-2 °C absolute, fine for trend visibility).

temperature_sensor_handle_t g_temp_handle = nullptr;
bool g_temp_init_done = false;

void ensure_temp_sensor() {
  if (g_temp_init_done) return;
  g_temp_init_done = true;
  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  if (temperature_sensor_install(&cfg, &g_temp_handle) != ESP_OK ||
      temperature_sensor_enable(g_temp_handle) != ESP_OK) {
    ESP_LOGW(TAG, "temp sensor unavailable");
    g_temp_handle = nullptr;
  }
}

float read_temp_c() {
  ensure_temp_sensor();
  if (g_temp_handle == nullptr) return 0.0f;
  float c = 0.0f;
  if (temperature_sensor_get_celsius(g_temp_handle, &c) != ESP_OK) return 0.0f;
  return c;
}

void sample_cpu_pct(int *cpu0, int *cpu1) {
  // Persisted across calls; safe because httpd serializes requests.
  static uint64_t prev_us = 0;       // wall-clock anchor
  static uint32_t prev_idle[2] = {0, 0};

  TaskStatus_t tasks[40];
  uint32_t total_unused = 0;
  UBaseType_t n = uxTaskGetSystemState(tasks, 40, &total_unused);
  if (n == 0) {
    *cpu0 = *cpu1 = 0;
    return;
  }

  // Identify idle tasks by name ("IDLE0" / "IDLE1" on the SMP port).
  // The handle returned by xTaskGetIdleTaskHandleForCore isn't always
  // matchable against TaskStatus_t.xHandle on every IDF revision; name
  // matching is more robust.
  uint32_t idle_now[2] = {0, 0};
  for (UBaseType_t i = 0; i < n; ++i) {
    const char *nm = tasks[i].pcTaskName ? tasks[i].pcTaskName : "";
    if (nm[0] == 'I' && nm[1] == 'D' && nm[2] == 'L' && nm[3] == 'E') {
      if (nm[4] == '0' || nm[4] == '\0') idle_now[0] = tasks[i].ulRunTimeCounter;
      else if (nm[4] == '1') idle_now[1] = tasks[i].ulRunTimeCounter;
    }
  }

  // Use esp_timer as the wall-clock denominator (1 µs resolution). The
  // runtime-stats counter wraps every ~71 min on its own u32 base, but
  // taking deltas via subtraction makes wrap a non-issue. esp_timer is
  // 64-bit so no wrap during a session.
  uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
  uint64_t elapsed_us = (prev_us == 0) ? 0 : (now_us - prev_us);

  int out[2] = {0, 0};
  if (elapsed_us > 0) {
    for (int k = 0; k < 2; ++k) {
      uint32_t idle_delta = idle_now[k] - prev_idle[k];  // wraps fine
      uint64_t idle_us = idle_delta;
      if (idle_us > elapsed_us) idle_us = elapsed_us;
      uint64_t busy_us = elapsed_us - idle_us;
      out[k] = static_cast<int>((100ULL * busy_us) / elapsed_us);
    }
  }
  prev_us = now_us;
  prev_idle[0] = idle_now[0];
  prev_idle[1] = idle_now[1];
  *cpu0 = out[0];
  *cpu1 = out[1];
}

esp_err_t stats_get(httpd_req_t *req) {
  char buf[256];
  size_t n = build_stats_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t scan_get(httpd_req_t *req) {
  char buf[48];
  size_t n = build_scan_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t scan_post(httpd_req_t *req) {
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  const char *err = handle_scan_set(query);
  if (err) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}

#if CONFIG_NBP_DEVICES_PANEL
esp_err_t devices_get(httpd_req_t *req) {
  // httpd serializes requests on a single worker, so a static buffer
  // is safe and avoids putting 6 KiB on the worker stack.
  static char out[6144];
  size_t n = build_devices_json(out, sizeof(out));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out, n);
}
#endif  // CONFIG_NBP_DEVICES_PANEL

#if CONFIG_NBP_WIFI
// Dashboard HTML lives in web/index.html — embedded via EMBED_FILES in
// this component's CMakeLists.txt (also gated on CONFIG_NBP_WIFI so we
// don't carry the bytes in BLE-only builds). The same file is served
// to an off-device Web Bluetooth client; the page auto-detects which
// transport to use.
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

esp_err_t root_get(httpd_req_t *req) {
  const size_t n = static_cast<size_t>(index_html_end - index_html_start);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(
      req, reinterpret_cast<const char *>(index_html_start), n);
}
#endif  // CONFIG_NBP_WIFI

}  // namespace

void record_read() { g_reads.fetch_add(1, std::memory_order_relaxed); }
void record_write() { g_writes.fetch_add(1, std::memory_order_relaxed); }
void record_notify() { g_notifies.fetch_add(1, std::memory_order_relaxed); }

size_t build_stats_json(char *buf, size_t cap) {
  unsigned in_use = proxy::MAX_CONNECTIONS -
                    ble_backend::connection::free_slots();
  int cpu0 = 0, cpu1 = 0;
  sample_cpu_pct(&cpu0, &cpu1);
  float temp_c = read_temp_c();
  int n = std::snprintf(
      buf, cap,
      "{\"reads\":%lu,\"writes\":%lu,\"notifies\":%lu,\"adverts\":%lu,"
      "\"connections\":%u,\"heap\":%lu,"
      "\"notify_rx\":%lu,\"last_notify_handle\":%u,"
      "\"cpu0\":%d,\"cpu1\":%d,\"temp_c\":%.1f}",
      static_cast<unsigned long>(g_reads.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(g_writes.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(g_notifies.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(ble_backend::scanner::adv_count()),
      in_use,
      static_cast<unsigned long>(esp_get_free_heap_size()),
      static_cast<unsigned long>(ble_backend::notify_rx_total()),
      ble_backend::last_notify_handle(),
      cpu0, cpu1, static_cast<double>(temp_c));
  if (n < 0) return 0;
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

#if CONFIG_NBP_WEB_CONSOLE
size_t build_log_slice(uint32_t *since_inout, char *buf, size_t cap,
                       uint32_t *out_seq) {
  if (g_log_mutex == nullptr) {
    if (out_seq) *out_seq = 0;
    return 0;
  }
  xSemaphoreTake(g_log_mutex, portMAX_DELAY);
  uint32_t seq = g_log_seq;
  uint32_t since = *since_inout;
  if (since > seq) {
    since = seq;  // client reboot or counter ahead — reset to current.
  } else if (seq - since > LOG_RING_SIZE) {
    since = seq - LOG_RING_SIZE;  // client too far behind; drop older.
  }
  *since_inout = since;

  uint32_t backlog = seq - since;
  if (backlog == 0 || cap == 0) {
    xSemaphoreGive(g_log_mutex);
    if (out_seq) *out_seq = seq;
    return 0;
  }
  size_t start = since % LOG_RING_SIZE;
  size_t contig = std::min(static_cast<size_t>(backlog),
                           LOG_RING_SIZE - start);
  size_t take = std::min(contig, cap);
  std::memcpy(buf, g_log_ring + start, take);
  xSemaphoreGive(g_log_mutex);
  if (out_seq) *out_seq = seq;
  return take;
}
#endif

// ---- Per-endpoint helpers (shared by httpd + BLE transports) ----

size_t build_level_json(char *buf, size_t cap) {
  int n = std::snprintf(buf, cap, "{\"nimble\":%d}",
                        static_cast<int>(g_current_nimble_level));
  if (n < 0) return 0;
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

const char *handle_level_set(const char *query) {
  char val[8];
  if (httpd_query_key_value(query, "nimble", val, sizeof(val)) != ESP_OK) {
    return "missing nimble=";
  }
  int parsed = std::atoi(val);
  if (parsed < ESP_LOG_NONE || parsed > ESP_LOG_VERBOSE) {
    return "level out of range";
  }
  auto lvl = static_cast<esp_log_level_t>(parsed);
  if (nvs_write_level(lvl) != ESP_OK) return "nvs write failed";
  apply_level(lvl);
  ESP_LOGI(TAG, "NimBLE log level set to %d (persisted)",
           static_cast<int>(lvl));
  return nullptr;
}

size_t build_txpower_json(char *buf, size_t cap) {
  // wifi==0 in the wire format means "WiFi not active" — either runtime
  // off (via handle_txpower_set) or compile-time absent. Either way the
  // dashboard disables the TX-power dropdown on seeing 0.
#if CONFIG_NBP_WIFI
  int wifi = g_wifi_off ? 0 : static_cast<int>(g_wifi_tx_dbm);
#else
  int wifi = 0;
#endif
  int n = std::snprintf(buf, cap, "{\"wifi\":%d,\"ble\":%d}", wifi,
                        static_cast<int>(g_ble_tx_dbm));
  if (n < 0) return 0;
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

const char *handle_txpower_set(const char *query) {
  char val[8];
  bool changed = false;
  if (httpd_query_key_value(query, "wifi", val, sizeof(val)) == ESP_OK) {
    int dbm = std::atoi(val);
    if (dbm == 0) {
      if (!g_wifi_off) {
        if (apply_wifi_off() != ESP_OK) return "wifi stop failed";
        g_wifi_off = true;
        ESP_LOGI(TAG, "wifi off (runtime; reboot to re-enable)");
        changed = true;
      }
    } else {
      if (dbm < 2 || dbm > 21) return "wifi 0 or 2..21";
      if (g_wifi_off) return "wifi off; reboot to re-enable";
      if (dbm != g_wifi_tx_dbm) {
        int8_t new_dbm = static_cast<int8_t>(dbm);
        if (apply_wifi_tx_dbm(new_dbm) != ESP_OK) return "wifi tx apply failed";
        g_wifi_tx_dbm = new_dbm;
        nvs_write_i8(NVS_WIFI_TX_KEY, g_wifi_tx_dbm);
        ESP_LOGI(TAG, "wifi tx -> %d dBm (persisted)",
                 static_cast<int>(g_wifi_tx_dbm));
        changed = true;
      }
    }
  }
  if (httpd_query_key_value(query, "ble", val, sizeof(val)) == ESP_OK) {
    int dbm = std::atoi(val);
    if (dbm < -12 || dbm > 9) return "ble -12..9";
    if (dbm != g_ble_tx_dbm) {
      if (apply_ble_tx_dbm(dbm) != ESP_OK) return "ble tx apply failed";
      g_ble_tx_dbm = static_cast<int8_t>(dbm);
      nvs_write_i8(NVS_BLE_TX_KEY, g_ble_tx_dbm);
      ESP_LOGI(TAG, "ble tx -> %d dBm (persisted)",
               static_cast<int>(g_ble_tx_dbm));
      changed = true;
    }
  }
  if (!changed) return "no params or no change";
  return nullptr;
}

size_t build_cpufreq_json(char *buf, size_t cap) {
  int n = std::snprintf(buf, cap, "{\"mhz\":%d,\"ls\":%s}",
                        g_cpu_freq_mhz, g_light_sleep ? "true" : "false");
  if (n < 0) return 0;
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

const char *handle_cpufreq_set(const char *query) {
  char val[8];
  int mhz = g_cpu_freq_mhz;
  bool light_sleep = g_light_sleep;
  bool any = false;
  if (httpd_query_key_value(query, "mhz", val, sizeof(val)) == ESP_OK) {
    mhz = std::atoi(val);
    if (mhz != 80 && mhz != 160 && mhz != 240) {
      return "mhz must be 80, 160, or 240";
    }
    any = true;
  }
  if (httpd_query_key_value(query, "ls", val, sizeof(val)) == ESP_OK) {
    light_sleep = (std::atoi(val) != 0);
    any = true;
  }
  if (!any) return "missing mhz= or ls=";
  if (apply_cpu_freq_mhz(mhz, light_sleep) != ESP_OK) {
    return "cpu freq apply failed";
  }
  g_cpu_freq_mhz = mhz;
  nvs_write_i8(NVS_CPU_FREQ_KEY, static_cast<int8_t>(mhz / 10));
  nvs_write_i8("cpu_ls", light_sleep ? 1 : 0);
  ESP_LOGI(TAG, "cpu -> %d MHz ls=%d (persisted)", mhz, light_sleep ? 1 : 0);
  return nullptr;
}

#ifdef CONFIG_NBP_SMP
size_t build_passkey_json(char *buf, size_t cap) {
  int n = std::snprintf(buf, cap, "{\"passkey\":%06lu}",
                        static_cast<unsigned long>(
                            ble_backend::connection::get_passkey()));
  if (n < 0) return 0;
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

const char *handle_passkey_set(const char *query) {
  char val[8];
  if (httpd_query_key_value(query, "val", val, sizeof(val)) != ESP_OK) {
    return "missing val=";
  }
  long parsed = std::strtol(val, nullptr, 10);
  if (parsed < 0 || parsed > 999999) return "passkey must be 0..999999";
  uint32_t pin = static_cast<uint32_t>(parsed);
  if (nvs_write_passkey(pin) != ESP_OK) return "nvs write failed";
  ble_backend::connection::set_passkey(pin);
  ESP_LOGI(TAG, "BLE passkey set to %06lu (persisted)",
           static_cast<unsigned long>(pin));
  return nullptr;
}
#endif  // CONFIG_NBP_SMP

#if CONFIG_NBP_DEVICES_PANEL
size_t build_devices_json(char *buf, size_t cap) {
  static ble_backend::scanner::DeviceRow snap[64];
  size_t n = ble_backend::scanner::snapshot_devices(snap, 64);
  uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

  char *p = buf;
  char *const end = buf + cap;
  auto rem = [&]() -> size_t { return end > p ? size_t(end - p) : 0; };
  // snprintf writes a NUL terminator at position size-1 when the input
  // is longer than `size`. Advancing p by rem() in that case would
  // land us on the NUL — and we'd return it to the caller. Cap the
  // advance at rem()-1 on truncation so the NUL never gets counted.
  auto bump = [&](int w) {
    if (w <= 0) return;
    size_t r = rem();
    if (r == 0) return;
    p += static_cast<size_t>(w) < r ? static_cast<size_t>(w) : r - 1;
  };

  bump(std::snprintf(p, rem(), "{\"devices\":["));
  for (size_t i = 0; i < n; ++i) {
    const auto &r = snap[i];
    uint8_t b[6];
    for (int k = 0; k < 6; ++k) b[k] = (r.addr >> ((5 - k) * 8)) & 0xff;
    bump(std::snprintf(
        p, rem(),
        "%s{\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"type\":%u,\"rssi\":%d,\"count\":%lu,\"age\":%lu,\"name\":\"",
        i ? "," : "", b[0], b[1], b[2], b[3], b[4], b[5],
        static_cast<unsigned>(r.addr_type), static_cast<int>(r.rssi),
        static_cast<unsigned long>(r.adv_count),
        static_cast<unsigned long>(now_ms - r.last_ms)));
    for (const char *q = r.name; *q && rem() > 8; ++q) {
      char c = *q;
      if (c == '"' || c == '\\') {
        *p++ = '\\'; *p++ = c;
      } else if (static_cast<unsigned char>(c) < 0x20) {
        bump(std::snprintf(p, rem(), "\\u%04x",
                           static_cast<unsigned>(static_cast<unsigned char>(c))));
      } else {
        *p++ = c;
      }
    }
    if (rem() >= 2) { *p++ = '"'; *p++ = '}'; }
  }
  if (rem() >= 2) { *p++ = ']'; *p++ = '}'; }
  return static_cast<size_t>(p - buf);
}
#endif

bool handle_trace_set(const char *query) {
  char val[8];
  bool on = true;
  if (httpd_query_key_value(query, "on", val, sizeof(val)) == ESP_OK) {
    on = (std::atoi(val) != 0);
  }
  if (on) {
    for (const char *tag : NIMBLE_SCAN_TAGS) esp_log_level_set(tag, ESP_LOG_ERROR);
    for (const char *tag : NIMBLE_CORE_TAGS) esp_log_level_set(tag, ESP_LOG_DEBUG);
    ble_backend::scanner::pause();
#if CONFIG_NBP_WEB_CONSOLE
    log_ring_reset();
#endif
    ESP_LOGI(TAG, "trace ON: scan paused, core=DEBUG, scan-tags=ERROR");
  } else {
    apply_level(g_current_nimble_level);
    ble_backend::scanner::resume();
    ESP_LOGI(TAG, "trace OFF: scan resumed, levels restored to %d",
             static_cast<int>(g_current_nimble_level));
  }
  return on;
}

size_t build_scan_json(char *buf, size_t cap) {
  uint16_t window = 0, interval = 0;
  ble_backend::scanner::get_duty(&window, &interval);
  int n = std::snprintf(buf, cap, "{\"window\":%u,\"interval\":%u}",
                        static_cast<unsigned>(window),
                        static_cast<unsigned>(interval));
  if (n < 0) return 0;
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

const char *handle_scan_set(const char *query) {
  char val[8];
  uint16_t cur_win = 0, cur_int = 0;
  ble_backend::scanner::get_duty(&cur_win, &cur_int);
  long window = cur_win, interval = cur_int;
  bool any = false;
  if (httpd_query_key_value(query, "window", val, sizeof(val)) == ESP_OK) {
    window = std::strtol(val, nullptr, 10);
    any = true;
  }
  if (httpd_query_key_value(query, "interval", val, sizeof(val)) == ESP_OK) {
    interval = std::strtol(val, nullptr, 10);
    any = true;
  }
  if (!any) return "missing window= or interval=";
  if (window < 20 || window > 10000) return "window 20..10000 ms";
  if (interval < 20 || interval > 10000) return "interval 20..10000 ms";
  if (window > interval) return "window must be <= interval";

  // NVS persist (one u16 per key) BEFORE applying so a bad apply
  // doesn't leave stale state behind.
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u16(h, "scan_win", static_cast<uint16_t>(window));
    nvs_set_u16(h, "scan_int", static_cast<uint16_t>(interval));
    nvs_commit(h);
    nvs_close(h);
  }
  ble_backend::scanner::set_duty(static_cast<uint16_t>(window),
                                 static_cast<uint16_t>(interval));
  return nullptr;
}

void apply_scan_from_nvs() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  uint16_t window = 0, interval = 0;
  bool have_w = (nvs_get_u16(h, "scan_win", &window) == ESP_OK);
  bool have_i = (nvs_get_u16(h, "scan_int", &interval) == ESP_OK);
  nvs_close(h);
  if (!have_w || !have_i) return;  // keep proxy:: defaults
  if (window < 20 || window > interval || interval > 10000) return;
  ble_backend::scanner::set_duty(window, interval);
  ESP_LOGI(TAG, "scan duty from NVS: window=%u interval=%u", window, interval);
}

void schedule_reboot() {
  // One-shot esp_timer so the caller can finish sending its response
  // (HTTP TCP FIN / BLE notify drain) before the radio goes down.
  static esp_timer_handle_t timer = nullptr;
  if (timer == nullptr) {
    esp_timer_create_args_t args = {
        .callback = [](void *) { esp_restart(); },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "reboot",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&args, &timer);
  }
  ESP_LOGI(TAG, "reboot scheduled in 500 ms");
  esp_timer_start_once(timer, 500000);
}

void apply_log_overrides_from_nvs() {
  esp_log_level_t lvl;
  if (nvs_read_level(&lvl) == ESP_OK) {
    apply_level(lvl);
    ESP_LOGI(TAG, "NimBLE log level from NVS: %d", static_cast<int>(lvl));
  } else {
    apply_level(DEFAULT_NIMBLE_LEVEL);
    ESP_LOGI(TAG, "NimBLE log level default: %d",
             static_cast<int>(DEFAULT_NIMBLE_LEVEL));
  }
#ifdef CONFIG_NBP_SMP
  uint32_t pin = DEFAULT_PASSKEY;
  if (nvs_read_passkey(&pin) == ESP_OK) {
    ESP_LOGI(TAG, "BLE passkey from NVS: %06lu",
             static_cast<unsigned long>(pin));
  } else {
    ESP_LOGI(TAG, "BLE passkey default: %06lu",
             static_cast<unsigned long>(pin));
  }
  ble_backend::connection::set_passkey(pin);
#endif
}

void apply_tx_power_from_nvs() {
  int8_t v;
  if (nvs_read_i8(NVS_WIFI_TX_KEY, &v) == ESP_OK) g_wifi_tx_dbm = v;
  if (nvs_read_i8(NVS_BLE_TX_KEY, &v) == ESP_OK) g_ble_tx_dbm = v;
  apply_wifi_tx_dbm(g_wifi_tx_dbm);
  apply_ble_tx_dbm(g_ble_tx_dbm);
  ESP_LOGI(TAG, "TX power applied: wifi=%d dBm, ble=%d dBm",
           static_cast<int>(g_wifi_tx_dbm),
           static_cast<int>(g_ble_tx_dbm));
}

void apply_cpu_freq_from_nvs() {
  int8_t stored = 0;
  // Stored as MHz/10 so it fits in int8 (24 -> 240 MHz, 16 -> 160 MHz, etc.).
  if (nvs_read_i8(NVS_CPU_FREQ_KEY, &stored) == ESP_OK) {
    int mhz = static_cast<int>(stored) * 10;
    if (mhz == 80 || mhz == 160 || mhz == 240) g_cpu_freq_mhz = mhz;
  }
  int8_t ls = 0;
  if (nvs_read_i8("cpu_ls", &ls) == ESP_OK) g_light_sleep = (ls != 0);
  esp_err_t err = apply_cpu_freq_mhz(g_cpu_freq_mhz, g_light_sleep);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_pm_configure(%d MHz ls=%d) failed: %s",
             g_cpu_freq_mhz, g_light_sleep ? 1 : 0, esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "CPU applied: %d MHz, light sleep=%d",
           g_cpu_freq_mhz, g_light_sleep ? 1 : 0);
}

#if CONFIG_NBP_WEB_CONSOLE
void install_log_hook() {
  if (g_log_mutex != nullptr) return;  // already installed
  g_log_mutex = xSemaphoreCreateMutex();
  g_old_vprintf = esp_log_set_vprintf(&log_vprintf);
  if (g_old_vprintf == nullptr) g_old_vprintf = &std::vprintf;
  ESP_LOGI(TAG, "log hook installed, %u-byte ring",
           static_cast<unsigned>(LOG_RING_SIZE));
}
#endif

#if CONFIG_NBP_WIFI
void register_endpoints(httpd_handle_t srv) {
  if (srv == nullptr) {
    ESP_LOGW(TAG, "no httpd handle, stats UI disabled");
    return;
  }
  httpd_uri_t root = {.uri = "/",
                      .method = HTTP_GET,
                      .handler = &root_get,
                      .user_ctx = nullptr};
  httpd_uri_t stats = {.uri = "/stats.json",
                       .method = HTTP_GET,
                       .handler = &stats_get,
                       .user_ctx = nullptr};
#if CONFIG_NBP_WEB_CONSOLE
  httpd_uri_t log = {.uri = "/log",
                     .method = HTTP_GET,
                     .handler = &log_get,
                     .user_ctx = nullptr};
#endif
  httpd_uri_t level_g = {.uri = "/level",
                         .method = HTTP_GET,
                         .handler = &level_get,
                         .user_ctx = nullptr};
  httpd_uri_t level_p = {.uri = "/level",
                         .method = HTTP_POST,
                         .handler = &level_post,
                         .user_ctx = nullptr};
  httpd_uri_t reboot = {.uri = "/reboot",
                        .method = HTTP_POST,
                        .handler = &reboot_post,
                        .user_ctx = nullptr};
  httpd_uri_t trace = {.uri = "/trace",
                       .method = HTTP_POST,
                       .handler = &trace_post,
                       .user_ctx = nullptr};
#ifdef CONFIG_NBP_SMP
  httpd_uri_t passkey_g = {.uri = "/passkey",
                           .method = HTTP_GET,
                           .handler = &passkey_get,
                           .user_ctx = nullptr};
  httpd_uri_t passkey_p = {.uri = "/passkey",
                           .method = HTTP_POST,
                           .handler = &passkey_post,
                           .user_ctx = nullptr};
#endif
  httpd_register_uri_handler(srv, &root);
  httpd_register_uri_handler(srv, &stats);
#if CONFIG_NBP_WEB_CONSOLE
  httpd_register_uri_handler(srv, &log);
#endif
  httpd_register_uri_handler(srv, &level_g);
  httpd_register_uri_handler(srv, &level_p);
  httpd_register_uri_handler(srv, &reboot);
  httpd_register_uri_handler(srv, &trace);
  httpd_uri_t txpower_g = {.uri = "/txpower",
                           .method = HTTP_GET,
                           .handler = &txpower_get,
                           .user_ctx = nullptr};
  httpd_uri_t txpower_p = {.uri = "/txpower",
                           .method = HTTP_POST,
                           .handler = &txpower_post,
                           .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &txpower_g);
  httpd_register_uri_handler(srv, &txpower_p);
  httpd_uri_t cpufreq_g = {.uri = "/cpufreq",
                           .method = HTTP_GET,
                           .handler = &cpufreq_get,
                           .user_ctx = nullptr};
  httpd_uri_t cpufreq_p = {.uri = "/cpufreq",
                           .method = HTTP_POST,
                           .handler = &cpufreq_post,
                           .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &cpufreq_g);
  httpd_register_uri_handler(srv, &cpufreq_p);
  httpd_uri_t scan_g = {.uri = "/scan",
                        .method = HTTP_GET,
                        .handler = &scan_get,
                        .user_ctx = nullptr};
  httpd_uri_t scan_p = {.uri = "/scan",
                        .method = HTTP_POST,
                        .handler = &scan_post,
                        .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &scan_g);
  httpd_register_uri_handler(srv, &scan_p);
#ifdef CONFIG_NBP_SMP
  httpd_register_uri_handler(srv, &passkey_g);
  httpd_register_uri_handler(srv, &passkey_p);
#endif
#if CONFIG_NBP_DEVICES_PANEL
  httpd_uri_t devices = {.uri = "/devices",
                         .method = HTTP_GET,
                         .handler = &devices_get,
                         .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &devices);
#endif
  ESP_LOGI(TAG, "stats UI at /");
}
#endif  // CONFIG_NBP_WIFI

}  // namespace api_server::stats
