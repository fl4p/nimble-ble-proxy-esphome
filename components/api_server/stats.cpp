#include "stats.h"

#include "ble_backend.h"
#include "connection.h"
#include "driver/temperature_sensor.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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

  xSemaphoreTake(g_log_mutex, portMAX_DELAY);
  uint32_t seq = g_log_seq;
  // If the client is way behind, clamp to the oldest byte we still hold.
  uint32_t backlog;
  if (since > seq) {
    // Counter wrap or client seq came from a previous boot; reset.
    since = seq;
    backlog = 0;
  } else if (seq - since > LOG_RING_SIZE) {
    since = seq - LOG_RING_SIZE;
    backlog = LOG_RING_SIZE;
  } else {
    backlog = seq - since;
  }

  char hdr[32];
  std::snprintf(hdr, sizeof(hdr), "%lu", static_cast<unsigned long>(seq));

  if (backlog == 0) {
    xSemaphoreGive(g_log_mutex);
    httpd_resp_set_hdr(req, "X-Log-Seq", hdr);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "", 0);
  }

  // Stream in two slices (handling wrap-around) via chunked encoding so
  // we never need a single contiguous malloc — fragmented heap with a
  // 64 KiB ring would otherwise fail the alloc and lose the response.
  // A small bounce buffer copies under the mutex; sends happen outside.
  size_t start = since % LOG_RING_SIZE;
  size_t first_len = std::min(static_cast<size_t>(backlog), LOG_RING_SIZE - start);
  size_t second_len = backlog - first_len;

  constexpr size_t CHUNK = 1024;
  static char bounce[CHUNK];

  httpd_resp_set_hdr(req, "X-Log-Seq", hdr);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");

  esp_err_t r = ESP_OK;
  auto send_range = [&](const char *base, size_t len) {
    while (len > 0 && r == ESP_OK) {
      size_t take = len < CHUNK ? len : CHUNK;
      std::memcpy(bounce, base, take);
      xSemaphoreGive(g_log_mutex);
      r = httpd_resp_send_chunk(req, bounce, take);
      xSemaphoreTake(g_log_mutex, portMAX_DELAY);
      base += take;
      len -= take;
    }
  };
  send_range(g_log_ring + start, first_len);
  if (r == ESP_OK && second_len > 0) {
    send_range(g_log_ring, second_len);
  }
  xSemaphoreGive(g_log_mutex);

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
  int n = std::snprintf(buf, sizeof(buf), "{\"nimble\":%d}",
                        static_cast<int>(g_current_nimble_level));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t level_post(httpd_req_t *req) {
  // Accept the level in a query string: POST /level?nimble=2 .
  // Keeps the handler trivial — no body parsing needed.
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "missing query");
  }
  char val[8];
  if (httpd_query_key_value(query, "nimble", val, sizeof(val)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "missing nimble=");
  }
  int parsed = std::atoi(val);
  if (parsed < ESP_LOG_NONE || parsed > ESP_LOG_VERBOSE) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "level out of range");
  }
  auto lvl = static_cast<esp_log_level_t>(parsed);
  esp_err_t err = nvs_write_level(lvl);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "nvs write failed: %s", esp_err_to_name(err));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "nvs write failed");
  }
  apply_level(lvl);
  ESP_LOGI(TAG, "NimBLE log level set to %d (persisted)",
           static_cast<int>(lvl));
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
  int n = std::snprintf(buf, sizeof(buf), "{\"passkey\":%06lu}",
                        static_cast<unsigned long>(
                            ble_backend::connection::get_passkey()));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t passkey_post(httpd_req_t *req) {
  char query[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  char val[8];
  if (httpd_query_key_value(query, "val", val, sizeof(val)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing val=");
  }
  long parsed = std::strtol(val, nullptr, 10);
  if (parsed < 0 || parsed > 999999) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "passkey must be 0..999999");
  }
  uint32_t pin = static_cast<uint32_t>(parsed);
  esp_err_t err = nvs_write_passkey(pin);
  if (err != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "nvs write failed");
  }
  ble_backend::connection::set_passkey(pin);
  ESP_LOGI(TAG, "BLE passkey set to %06lu (persisted)",
           static_cast<unsigned long>(pin));
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

esp_err_t apply_cpu_freq_mhz(int mhz) {
  // Pin min=max so the CPU is clamped exactly. light_sleep off — a
  // continuously-active scanner makes the sleep entry/exit overhead
  // not worth its modest win.
  esp_pm_config_t cfg = {
      .max_freq_mhz = mhz,
      .min_freq_mhz = mhz,
      .light_sleep_enable = false,
  };
  return esp_pm_configure(&cfg);
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
  return esp_wifi_set_max_tx_power(static_cast<int8_t>(dbm * 4));
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
  int n = std::snprintf(buf, sizeof(buf), "{\"wifi\":%d,\"ble\":%d}",
                        static_cast<int>(g_wifi_tx_dbm),
                        static_cast<int>(g_ble_tx_dbm));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t cpufreq_get(httpd_req_t *req) {
  char buf[32];
  int n = std::snprintf(buf, sizeof(buf), "{\"mhz\":%d}", g_cpu_freq_mhz);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t cpufreq_post(httpd_req_t *req) {
  char query[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  char val[8];
  if (httpd_query_key_value(query, "mhz", val, sizeof(val)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing mhz=");
  }
  int mhz = std::atoi(val);
  if (mhz != 80 && mhz != 160 && mhz != 240) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "mhz must be 80, 160, or 240");
  }
  esp_err_t err = apply_cpu_freq_mhz(mhz);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_pm_configure failed: %s", esp_err_to_name(err));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "cpu freq apply failed");
  }
  g_cpu_freq_mhz = mhz;
  nvs_write_i8(NVS_CPU_FREQ_KEY, static_cast<int8_t>(mhz / 10));
  ESP_LOGI(TAG, "cpu freq -> %d MHz (persisted)", mhz);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}

esp_err_t txpower_post(httpd_req_t *req) {
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  char val[8];
  bool changed = false;
  if (httpd_query_key_value(query, "wifi", val, sizeof(val)) == ESP_OK) {
    int dbm = std::atoi(val);
    if (dbm < 2 || dbm > 21) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wifi 2..21");
    }
    if (dbm != g_wifi_tx_dbm) {
      int8_t new_dbm = static_cast<int8_t>(dbm);
      if (apply_wifi_tx_dbm(new_dbm) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "wifi tx apply failed");
      }
      g_wifi_tx_dbm = new_dbm;
      nvs_write_i8(NVS_WIFI_TX_KEY, g_wifi_tx_dbm);
      ESP_LOGI(TAG, "wifi tx -> %d dBm (persisted)",
               static_cast<int>(g_wifi_tx_dbm));
      changed = true;
    }
  }
  if (httpd_query_key_value(query, "ble", val, sizeof(val)) == ESP_OK) {
    int dbm = std::atoi(val);
    if (dbm < -12 || dbm > 9) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ble -12..9");
    }
    if (dbm != g_ble_tx_dbm) {
      if (apply_ble_tx_dbm(dbm) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "ble tx apply failed");
      }
      g_ble_tx_dbm = static_cast<int8_t>(dbm);
      nvs_write_i8(NVS_BLE_TX_KEY, g_ble_tx_dbm);
      ESP_LOGI(TAG, "ble tx -> %d dBm (persisted)",
               static_cast<int>(g_ble_tx_dbm));
      changed = true;
    }
  }
  if (!changed) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "no params or no change");
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}

// Diagnostic capture mode. /trace?on=1 silences scanner noise and
// pauses scanning so the 64 KiB log ring isn't flooded with "New
// advertiser" lines during a BMS bring-up, then resets log_seq so the
// next `/log?since=0` returns a clean trace starting after `on=1`.
// /trace?on=0 restores the persisted NimBLE level and resumes scanning.
esp_err_t trace_post(httpd_req_t *req) {
  char query[32];
  char val[8];
  bool on = true;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "on", val, sizeof(val)) == ESP_OK) {
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
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, on ? "{\"trace\":true}" : "{\"trace\":false}",
                         HTTPD_RESP_USE_STRLEN);
}

esp_err_t reboot_post(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "rebooting\n", HTTPD_RESP_USE_STRLEN);
  ESP_LOGI(TAG, "reboot requested via /reboot");
  // Let the response drain and the TCP FIN reach the client before we
  // yank the rug — same pattern as the OTA handler.
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
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
  unsigned in_use = proxy::MAX_CONNECTIONS -
                    ble_backend::connection::free_slots();
  int cpu0 = 0, cpu1 = 0;
  sample_cpu_pct(&cpu0, &cpu1);
  float temp_c = read_temp_c();
  int n = std::snprintf(
      buf, sizeof(buf),
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
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

#if CONFIG_NBP_DEVICES_PANEL
esp_err_t devices_get(httpd_req_t *req) {
  // Snapshot under the scanner mutex, then format outside it.
  static ble_backend::scanner::DeviceRow snap[64];
  size_t n = ble_backend::scanner::snapshot_devices(snap, 64);
  uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

  // httpd serializes requests on a single worker, so a static buffer
  // is safe and avoids putting 6 KiB on the worker stack.
  static char out[6144];
  char *p = out;
  char *const end = out + sizeof(out);
  auto rem = [&]() -> size_t { return end > p ? size_t(end - p) : 0; };
  auto bump = [&](int w) {
    if (w > 0) p += size_t(w) < rem() ? size_t(w) : rem();
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
    // JSON-escape the name: backslash " and \, \u-escape controls.
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

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out, p - out);
}
#endif  // CONFIG_NBP_DEVICES_PANEL

esp_err_t root_get(httpd_req_t *req) {
  // uPlot loaded from jsdelivr. Page polls /stats.json each second and
  // plots the per-second delta over a 120-sample (2 min) window.
  static const char page[] =
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<title>nimble-ble-proxy</title>"
      "<link rel=stylesheet "
      "href=\"https://cdn.jsdelivr.net/npm/uplot@1.6.31/dist/uPlot.min.css\">"
      "<style>"
      "html,body{box-sizing:border-box}*,*::before,*::after{box-sizing:inherit}"
      "body{font:14px system-ui;margin:0;padding:1em;color:#eee;background:#111}"
      "h1,h2{font-size:1.1em;margin:0 0 1em}"
      "h2{margin-top:1.5em}"
      // Charts are uPlot-sized in JS to match the wrapper's inner
      // width (clamped to 900). max-width:100% keeps the wrapper from
      // overflowing the body on narrow viewports.
      "#chart,#chart2{background:#1a1a1a;padding:.5em;border-radius:6px;"
      "display:block;max-width:100%}"
      "#chart2{margin-top:.5em}"
#if CONFIG_NBP_WEB_CONSOLE
      "#console{background:#0a0a0a;color:#d4d4d4;"
      "font:11px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;"
      "padding:.5em;border-radius:6px;width:100%;max-width:920px;"
      "height:300px;overflow-y:auto;white-space:pre-wrap;"
      "word-break:break-all;margin:0;border:1px solid #222}"
#endif
      "footer{margin-top:1em;color:#888;font-size:.85em}"
      "code{color:#bbb}"
      // flex-wrap so the controls stack on narrow viewports instead of
      // overflowing the screen edge.
      "#controls{margin:1em 0;display:flex;flex-wrap:wrap;gap:.6em 1em;"
      "align-items:center}"
      "#controls label{color:#aaa;display:inline-flex;align-items:center;"
      "gap:.3em}"
      "#controls select,#controls button{"
      "background:#1a1a1a;color:#eee;border:1px solid #333;"
      "border-radius:4px;padding:.3em .6em;font:inherit;cursor:pointer}"
      "#controls button:hover{background:#2a2a2a}"
      "#controls button.danger{border-color:#7f1d1d;color:#fca5a5}"
      "#controls button.danger:hover{background:#3b0a0a}"
#if CONFIG_NBP_DEVICES_PANEL
      // Wrap the table in a horizontally-scrollable container so it
      // doesn't push the page wider than the viewport on mobile.
      ".devwrap{overflow-x:auto;max-width:100%}"
      "table#devices{border-collapse:collapse;font-size:12px;color:#ddd;"
      "margin-bottom:1em;min-width:480px}"
      "table#devices th,table#devices td{padding:.2em .6em;"
      "border-bottom:1px solid #222;text-align:left}"
      "table#devices th{color:#888;font-weight:normal}"
      "table#devices .r{text-align:right;font-variant-numeric:tabular-nums}"
      "table#devices tr.stale{opacity:.4}"
#endif
      "@media(max-width:640px){body{padding:.5em;font-size:13px}"
      "h1{font-size:1em}#controls{gap:.5em}}"
      "</style></head><body>"
      "<h1>nimble-ble-proxy &mdash; BLE activity/s</h1>"
      "<div id=chart></div>"
      "<br><div id=chart2></div>"
#if CONFIG_NBP_DEVICES_PANEL
      "<h2>devices seen</h2>"
      "<div class=devwrap><table id=devices><thead><tr>"
      "<th>MAC</th><th>name</th><th class=r>RSSI</th>"
      "<th class=r>adv/s</th><th class=r>total</th><th class=r>age</th>"
      "</tr></thead><tbody></tbody></table></div>"
#endif
      "<div id=controls>"
      "<label>NimBLE log: "
      "<select id=lvl>"
      "<option value=0>NONE</option>"
      "<option value=1>ERROR</option>"
      "<option value=2>WARN</option>"
      "<option value=3>INFO</option>"
      "<option value=4>DEBUG</option>"
      "<option value=5>VERBOSE</option>"
      "</select></label>"
      "<label>WiFi TX: <select id=wtx>"
      "<option value=8>8</option><option value=11>11</option>"
      "<option value=14>14</option><option value=17>17</option>"
      "<option value=20>20</option><option value=21>21</option>"
      "</select> dBm</label>"
      "<label>BLE TX: <select id=btx>"
      "<option value=-12>-12</option><option value=-9>-9</option>"
      "<option value=-6>-6</option><option value=-3>-3</option>"
      "<option value=0>0</option><option value=3>3</option>"
      "<option value=6>6</option><option value=9>9</option>"
      "</select> dBm</label>"
      "<label>CPU: <select id=cpu>"
      "<option value=80>80</option><option value=160>160</option>"
      "<option value=240>240</option>"
      "</select> MHz</label>"
      "<button id=reboot class=danger>reboot device</button>"
      "</div>"
#if CONFIG_NBP_WEB_CONSOLE
      "<h2>device log</h2>"
      "<pre id=console></pre>"
#endif
      "<footer>OTA: <code>curl --data-binary @firmware.bin "
      "http://&lt;host&gt;/update</code></footer>"
      "<script src=\"https://cdn.jsdelivr.net/npm/uplot@1.6.31/dist/"
      "uPlot.iife.min.js\"></script>"
#if CONFIG_NBP_WEB_CONSOLE
      "<script src=\"https://cdn.jsdelivr.net/npm/ansi_up@5/ansi_up.js\">"
      "</script>"
#endif
      "<script>"
      "const N=120,t=[],r=[],w=[],n=[],a=[],c=[],h=[],"
      "c0=[],c1=[],tc=[],tcA=[];"
      "for(let i=0;i<N;i++){t.push(i-N+1);r.push(null);w.push(null);"
      "n.push(null);a.push(null);c.push(null);h.push(null);"
      "c0.push(null);c1.push(null);tc.push(null);tcA.push(null);}"
      // Simple moving average over the last K non-null samples of arr.
      // Returns null until at least one sample is in-window.
      "const ma=(arr,k)=>{let s=0,c=0;for(let i=Math.max(0,arr.length-k);"
      "i<arr.length;i++){if(arr[i]!=null){s+=arr[i];c++;}}"
      "return c?s/c:null;};"
      "const fmt1=(u,v)=>v==null?'--':v.toFixed(1);"
      // Chart canvas matches the wrapper's inner width (clamped to
      // 900). Reading the live element's clientWidth — minus the
      // wrapper's own padding — avoids overshooting on small screens.
      "const chartW=el=>Math.min(900,el.clientWidth-16);"
      "const chartEl=document.getElementById('chart'),"
      "chart2El=document.getElementById('chart2');"
      "const u=new uPlot({width:chartW(chartEl),height:320,"
      "scales:{x:{time:false},y:{},kb:{}},"
      "axes:[{stroke:'#aaa',grid:{stroke:'#333'}},"
      "{stroke:'#aaa',grid:{stroke:'#333'}},"
      // Explicit size: 5-char value labels ('999 KB') + a bit of margin.
      "{side:1,scale:'kb',stroke:'#9ca3af',grid:{show:false},size:52,"
      "values:(u,vs)=>vs.map(v=>v+' KB')}],"
      "series:[{label:'t (s ago)'},"
      "{label:'reads/s',stroke:'#4ade80',width:2,value:fmt1},"
      "{label:'writes/s',stroke:'#60a5fa',width:2,value:fmt1},"
      "{label:'notifies/s',stroke:'#f472b6',width:2,value:fmt1},"
      "{label:'adverts/s',stroke:'#fbbf24',width:2,value:fmt1},"
      "{label:'conns',stroke:'#a78bfa',width:2},"
      "{label:'heap',scale:'kb',stroke:'#9ca3af',width:2,dash:[4,4]}]},"
      "[t,r,w,n,a,c,h],document.getElementById('chart'));"
      // Second chart: per-core CPU% (left axis 0-100) and chip temp °C
      // (right axis, separate 'temp' scale). Separate from the rate
      // chart because the units (%, °C) don't share a sensible axis.
      "const u2=new uPlot({width:chartW(chart2El),height:200,"
      "scales:{x:{time:false},y:{range:[0,100]},temp:{}},"
      "axes:[{stroke:'#aaa',grid:{stroke:'#333'}},"
      "{stroke:'#aaa',grid:{stroke:'#333'},"
      "values:(u,vs)=>vs.map(v=>v+'%')},"
      "{side:1,scale:'temp',stroke:'#22d3ee',grid:{show:false},size:42,"
      "values:(u,vs)=>vs.map(v=>v.toFixed(0)+'\\u00B0C')}],"
      "series:[{label:'t (s ago)'},"
      "{label:'cpu0%',stroke:'#ef4444',width:2},"
      "{label:'cpu1%',stroke:'#f97316',width:2},"
      // 10-sample moving average over s.temp_c; raw samples are kept
      // in `tc` only as the MA's input, not plotted.
      "{label:'temp\\u00B0C',scale:'temp',stroke:'#22d3ee',width:2,"
      "value:fmt1}]},"
      "[t,c0,c1,tcA],document.getElementById('chart2'));"
      "let prev=null,prevT=null;"
      "const d=(cur,p,dt)=>{const v=(cur-p)/dt;return v<0?null:v;};"
      "async function tick(){"
      "try{const now=performance.now()/1000;"
      "const s=await(await fetch('/stats.json')).json();"
      "if(prev){const dt=now-prevT;"
      "r.shift();w.shift();n.shift();a.shift();c.shift();h.shift();"
      "c0.shift();c1.shift();tc.shift();tcA.shift();"
      "r.push(d(s.reads,prev.reads,dt));"
      "w.push(d(s.writes,prev.writes,dt));"
      "n.push(d(s.notifies,prev.notifies,dt));"
      "a.push(d(s.adverts,prev.adverts,dt));"
      "c.push(s.connections);"
      "h.push(Math.round(s.heap/1024));"
      "c0.push(s.cpu0);c1.push(s.cpu1);tc.push(s.temp_c);"
      "tcA.push(ma(tc,10));"
      "u.setData([t,r,w,n,a,c,h]);"
      "u2.setData([t,c0,c1,tcA]);}"
      "prev=s;prevT=now;}catch(e){}}"
      "setInterval(tick,1000);tick();"
#if CONFIG_NBP_WEB_CONSOLE
      "let logSeq=0;const con=document.getElementById('console');"
      // ansi_up output is HTML-safe (input is escape_html'd then wrapped
      // in <span>s); we still avoid innerHTML by inserting via a Range
      // fragment and trim by removing leading child nodes.
      "const au=new AnsiUp();au.use_classes=false;"
      "async function pollLog(){"
      "try{const r=await fetch('/log?since='+logSeq);"
      "logSeq=parseInt(r.headers.get('X-Log-Seq')||logSeq,10);"
      "const txt=await r.text();"
      "if(txt){"
      "const atBottom=con.scrollHeight-con.scrollTop-con.clientHeight<20;"
      "const frag=document.createRange()"
      ".createContextualFragment(au.ansi_to_html(txt));"
      "con.appendChild(frag);"
      "while(con.childNodes.length>3000)con.removeChild(con.firstChild);"
      "if(atBottom)con.scrollTop=con.scrollHeight;"
      "}}catch(e){}}"
      "setInterval(pollLog,500);pollLog();"
#endif
      "const lvl=document.getElementById('lvl');"
      "fetch('/level').then(r=>r.json()).then(j=>{lvl.value=j.nimble;});"
      "lvl.onchange=()=>{"
      "fetch('/level?nimble='+lvl.value,{method:'POST'})"
      ".then(r=>{if(!r.ok)alert('level update failed');});};"
      // TX power: dropdown preselected from /txpower, change POSTs the
      // new value. WiFi <= 8 dBm is risky (may drop the LAN link), so
      // confirm; device-side has an 8 s revert safety net regardless.
      "const wtx=document.getElementById('wtx');"
      "const btx=document.getElementById('btx');"
      "fetch('/txpower').then(r=>r.json()).then(j=>{"
      "wtx.value=j.wifi;btx.value=j.ble;});"
      "wtx.onchange=()=>{"
      "fetch('/txpower?wifi='+wtx.value,{method:'POST'})"
      ".then(r=>{if(!r.ok)alert('wifi tx update failed');});};"
      "btx.onchange=()=>{"
      "fetch('/txpower?ble='+btx.value,{method:'POST'})"
      ".then(r=>{if(!r.ok)alert('ble tx update failed');});};"
      "const cpu=document.getElementById('cpu');"
      "fetch('/cpufreq').then(r=>r.json()).then(j=>{cpu.value=j.mhz;});"
      "cpu.onchange=()=>{"
      "fetch('/cpufreq?mhz='+cpu.value,{method:'POST'})"
      ".then(r=>{if(!r.ok)alert('cpu freq update failed');});};"
      // Re-fit charts on viewport resize / orientation change.
      "addEventListener('resize',()=>{"
      "u.setSize({width:chartW(chartEl),height:320});"
      "u2.setSize({width:chartW(chart2El),height:200});});"
      "document.getElementById('reboot').onclick=()=>{"
      "if(!confirm('Reboot device?'))return;"
      "fetch('/reboot',{method:'POST'})"
#if CONFIG_NBP_WEB_CONSOLE
      ".then(()=>{con.appendChild(document.createTextNode("
      "'\\n[client] reboot requested, waiting for device...\\n'));})"
#endif
      ";};"
#if CONFIG_NBP_DEVICES_PANEL
      // Per-device adv/s computed from delta of `count` between polls,
      // same pattern as the global rates. devPrev gets rebuilt each
      // tick so it can't grow unbounded as the LRU evicts entries.
      "let devPrev={};"
      "async function pollDevices(){try{"
      "const d=(await(await fetch('/devices')).json()).devices;"
      "const now=performance.now()/1000;const next={};"
      "d.sort((a,b)=>b.count-a.count);"
      "const tb=document.querySelector('#devices tbody');tb.textContent='';"
      "for(const x of d){const p=devPrev[x.addr];let rate='--';"
      "if(p){const dt=now-p.t;if(dt>0){const v=(x.count-p.count)/dt;"
      "if(v>=0)rate=v.toFixed(1);}}"
      "next[x.addr]={count:x.count,t:now};"
      "const tr=document.createElement('tr');"
      "if(x.age>10000)tr.className='stale';"
      "const cells=[[x.addr,''],[x.name||'',''],"
      "[x.rssi,'r'],[rate,'r'],[x.count,'r'],"
      "[(x.age/1000).toFixed(1)+'s','r']];"
      "for(const [v,cls] of cells){const td=document.createElement('td');"
      "if(cls)td.className=cls;td.textContent=v;tr.appendChild(td);}"
      "tb.appendChild(tr);}devPrev=next;}catch(e){}}"
      "setInterval(pollDevices,1000);pollDevices();"
#endif
      "</script></body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, page, sizeof(page) - 1);
}

}  // namespace

void record_read() { g_reads.fetch_add(1, std::memory_order_relaxed); }
void record_write() { g_writes.fetch_add(1, std::memory_order_relaxed); }
void record_notify() { g_notifies.fetch_add(1, std::memory_order_relaxed); }

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
  esp_err_t err = apply_cpu_freq_mhz(g_cpu_freq_mhz);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_pm_configure(%d MHz) failed: %s",
             g_cpu_freq_mhz, esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "CPU freq applied: %d MHz", g_cpu_freq_mhz);
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

}  // namespace api_server::stats
