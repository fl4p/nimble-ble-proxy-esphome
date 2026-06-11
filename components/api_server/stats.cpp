#include "stats.h"

#include "ble_backend.h"
#include "bthome.h"
#include "connection.h"
#include "soc/soc_caps.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif
#if CONFIG_NBP_BLE
#include "esp_bt.h"  // esp_ble_tx_power_set / esp_power_level_t (BT stack)
#endif
#include "esp_app_desc.h"  // esp_app_get_description (project/version/build)
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
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

#if CONFIG_NBP_LIVENESS_WDT
#include "liveness_wdt.h"
#endif

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

// Optional clone-side counter source. Set once at boot from main when
// CONFIG_NBP_CLONE is on; never touched otherwise. Plain pointer rather
// than std::atomic: a torn write against a read can't happen on a
// 32-bit aligned pointer for our targets, and the only meaningful read
// is "non-null → invoke".
CloneStatsProvider g_clone_stats_provider = nullptr;
NatThroughputProvider g_nat_throughput_provider = nullptr;

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
constexpr const char *NVS_HOSTNAME_KEY = "hostname";
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
// Per-read/write INFO from NimBLERemoteValueAttribute ('Write complete;
// status=0' / 'Read complete; status=0') floods the log when clone is
// proxying traffic. Capped at WARN regardless of the user-picked
// level — real errors still surface as NIMBLE_LOGE on the same tag.
constexpr const char *NIMBLE_NOISY_TAGS[] = {
    "NimBLERemoteValueAttribute",
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
  // INFO+. Cap scan + per-op tags at WARN regardless of the user-picked
  // level; they only become more verbose via /trace ON's explicit
  // override.
  esp_log_level_t capped = (lvl < ESP_LOG_WARN) ? lvl : ESP_LOG_WARN;
  for (const char *tag : NIMBLE_SCAN_TAGS) esp_log_level_set(tag, capped);
  for (const char *tag : NIMBLE_NOISY_TAGS) esp_log_level_set(tag, capped);
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
// SmartShunts. Runtime-settable via POST /clone?passkey=NNNNNN
// (merged from the previous /passkey endpoint); the boot-time
// replay in apply_log_overrides_from_nvs() loads it before any
// upstream GATT pairing can be triggered.

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
// WiFi power-save listen_interval (DTIM beacons between RX wakeups).
// 0 = WIFI_PS_NONE (no sleep), 1..10 = WIFI_PS_MAX_MODEM with that
// many DTIM beacons between wake-ups. wifi_sta.cpp reads the same NVS
// key on boot via a private mirror (no cross-component dependency).
constexpr const char *NVS_WIFI_LI_KEY = "wifi_li";
constexpr int8_t DEFAULT_WIFI_TX_DBM = 20;
constexpr int8_t DEFAULT_BLE_TX_DBM = 9;
constexpr int DEFAULT_CPU_FREQ_MHZ = 240;
// 0 = WIFI_PS_NONE (power-save OFF) by default — WIFI_PS_MAX_MODEM made
// the device unstable/unreachable and killed serial in steady state.
// Keep in sync with DEFAULT_WIFI_LI in wifi_sta.cpp.
constexpr int DEFAULT_WIFI_LI = 0;

int8_t g_wifi_tx_dbm = DEFAULT_WIFI_TX_DBM;
int8_t g_ble_tx_dbm = DEFAULT_BLE_TX_DBM;
int g_cpu_freq_mhz = DEFAULT_CPU_FREQ_MHZ;
int g_wifi_listen_interval = DEFAULT_WIFI_LI;

// Default OFF: CPU light-sleep (esp_pm + PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP)
// made the device hang for minutes until a watchdog reboot — HTTP,
// api_server and serial all went dark. WiFi PS_NONE (li=0) did NOT prevent
// it; only ls=false did (stability poll: ~3/36 reachable before, 48/48
// after). NVS "cpu_ls" still overrides, so flipping this default only
// affects fresh / NVS-erased devices. Re-enable for thermal savings via
// POST /cpufreq?ls=1 (accepting the hang risk).
bool g_light_sleep = false;

// Whether the CPU is powered down during light sleep (only relevant when
// g_light_sleep is true). Default false = keep the CPU powered and merely
// clock-gate/coast — the PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP path is what hung
// the device, so this lets light sleep be re-enabled for thermal savings
// without the hang. Mapped to esp_sleep_pd_config(ESP_PD_DOMAIN_CPU).
// NVS key "cpu_pd".
bool g_cpu_pd = false;

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
  // Independently gate CPU power-down during light sleep. ESP_PD_OPTION_ON
  // keeps the CPU powered (coast/clock-gate only); AUTO restores the default
  // PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP behaviour. Harmless when light sleep is
  // off (the SoC never sleeps, so the option is never consulted).
#if SOC_PM_SUPPORT_CPU_PD
  esp_sleep_pd_config(ESP_PD_DOMAIN_CPU,
                      g_cpu_pd ? ESP_PD_OPTION_AUTO : ESP_PD_OPTION_ON);
#endif
  return err;
}

#if CONFIG_NBP_BLE
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
#endif  // CONFIG_NBP_BLE

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
  // {"wifi":-20,"ble":-12,"ble_off":false} is ~38 chars — 32 truncated it.
  char buf[48];
  size_t n = build_txpower_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t cpufreq_get(httpd_req_t *req) {
  // {"mhz":160,"ls":false,"pdcpu":false} is 36 chars — 32 truncated it,
  // producing invalid JSON the dashboard couldn't parse (looked like the
  // ls/pdcpu settings weren't persisting).
  char buf[64];
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

esp_err_t wifips_get(httpd_req_t *req) {
  char buf[32];
  size_t n = build_wifips_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t wifips_post(httpd_req_t *req) {
  char query[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  const char *err = handle_wifips_set(query);
  if (err) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}

#if CONFIG_NBP_BLE
esp_err_t advitvl_get(httpd_req_t *req) {
  char buf[32];
  size_t n = build_advitvl_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t advitvl_post(httpd_req_t *req) {
  char query[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  const char *err = handle_advitvl_set(query);
  if (err) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}
#endif  // CONFIG_NBP_BLE

esp_err_t hostname_get(httpd_req_t *req) {
  // 2 * (HOSTNAME_MAX+1) + JSON scaffolding fits comfortably in 96 B.
  char buf[96];
  size_t n = build_hostname_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t appinfo_get(httpd_req_t *req) {
  char buf[256];
  size_t n = build_appinfo_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

#if CONFIG_NBP_LIVENESS_WDT
esp_err_t liveness_get(httpd_req_t *req) {
  liveness_wdt::State s = liveness_wdt::get_state();
  char buf[160];
  char age[16];
  if (s.last_ok_age_s == UINT32_MAX)
    std::strcpy(age, "null");
  else
    snprintf(age, sizeof(age), "%u", static_cast<unsigned>(s.last_ok_age_s));
  int n = snprintf(buf, sizeof(buf),
                   "{\"enabled\":%s,\"interval\":%u,\"threshold\":%u,"
                   "\"failures\":%u,\"last_ok_s\":%s}",
                   s.enabled ? "true" : "false", static_cast<unsigned>(s.interval_s),
                   static_cast<unsigned>(s.threshold), static_cast<unsigned>(s.failures), age);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t liveness_post(httpd_req_t *req) {
  char query[96];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  char val[12];
  if (httpd_query_key_value(query, "test", val, sizeof(val)) == ESP_OK && atoi(val) != 0) {
    liveness_wdt::force_fail_test();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"test\":true}", 23);
  }
  int en = -1, intv = -1, thr = -1;
  if (httpd_query_key_value(query, "enabled", val, sizeof(val)) == ESP_OK) en = atoi(val) ? 1 : 0;
  if (httpd_query_key_value(query, "interval", val, sizeof(val)) == ESP_OK) intv = atoi(val);
  if (httpd_query_key_value(query, "threshold", val, sizeof(val)) == ESP_OK) thr = atoi(val);
  const char *err = liveness_wdt::set_params(en, intv, thr);
  if (err) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}

// One WiFi-quality field across all samples, emitted as a JSON array. Chunked
// so we never need a multi-KB buffer; `null` marks an unknown/lost value.
enum WifiField { WF_RSSI, WF_AP, WF_RTT, WF_STA };
void wifihist_emit(httpd_req_t *req, const char *name, WifiField f,
                   const liveness_wdt::WifiSample *s, size_t n, bool comma_after) {
  char b[768];
  int p = snprintf(b, sizeof(b), "\"%s\":[", name);
  for (size_t i = 0; i < n; ++i) {
    if (p > static_cast<int>(sizeof(b)) - 16) {
      httpd_resp_send_chunk(req, b, p);
      p = 0;
    }
    if (i) b[p++] = ',';
    bool isnull = false;
    int v = 0;
    switch (f) {
      case WF_RSSI: v = s[i].sta_rssi;  isnull = (s[i].sta_rssi == 0); break;
      case WF_AP:   v = s[i].ap_rssi;   isnull = (s[i].ap_rssi == 0); break;
      case WF_RTT:  v = s[i].rtt_ms;    isnull = (s[i].rtt_ms == 0xFFFF); break;
      case WF_STA:  v = s[i].sta_count; break;
    }
    if (isnull) {
      std::memcpy(b + p, "null", 4);
      p += 4;
    } else {
      p += snprintf(b + p, sizeof(b) - p, "%d", v);
    }
  }
  b[p++] = ']';
  if (comma_after) b[p++] = ',';
  httpd_resp_send_chunk(req, b, p);
}

esp_err_t wifihist_get(httpd_req_t *req) {
  // static: keeps ~720 B off the httpd task stack; httpd serializes requests.
  static liveness_wdt::WifiSample buf[120];
  uint32_t interval = 30;
  size_t n = liveness_wdt::get_history(buf, 120, &interval);
  httpd_resp_set_type(req, "application/json");
  char head[64];
  int hn = snprintf(head, sizeof(head), "{\"interval\":%u,\"count\":%u,",
                    static_cast<unsigned>(interval), static_cast<unsigned>(n));
  httpd_resp_send_chunk(req, head, hn);
  wifihist_emit(req, "rssi", WF_RSSI, buf, n, true);
  wifihist_emit(req, "ap", WF_AP, buf, n, true);
  wifihist_emit(req, "rtt", WF_RTT, buf, n, true);
  wifihist_emit(req, "sta", WF_STA, buf, n, false);
  httpd_resp_send_chunk(req, "}", 1);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}
#endif  // CONFIG_NBP_LIVENESS_WDT

esp_err_t hostname_post(httpd_req_t *req) {
  // Query buffer sized for "val=" + max hostname.
  char query[proxy::HOSTNAME_MAX + 8];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  const char *err = handle_hostname_set(query);
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

#if SOC_TEMP_SENSOR_SUPPORTED
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
#else
// No internal temperature sensor on this chip (e.g. classic ESP32);
// /stats.json reports temp_c=0.0.
float read_temp_c() { return 0.0f; }
#endif

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
  // 256 covered the base object; the optional ap_rx/ap_tx 64-bit byte
  // counters (NAT builds) add ~50 chars, so size with headroom.
  char buf[320];
  size_t n = build_stats_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

#if CONFIG_NBP_BLE
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
#endif  // CONFIG_NBP_BLE

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
extern const uint8_t favicon_svg_start[] asm("_binary_favicon_svg_start");
extern const uint8_t favicon_svg_end[]   asm("_binary_favicon_svg_end");

esp_err_t root_get(httpd_req_t *req) {
  const size_t n = static_cast<size_t>(index_html_end - index_html_start);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(
      req, reinterpret_cast<const char *>(index_html_start), n);
}

esp_err_t favicon_get(httpd_req_t *req) {
  const size_t n = static_cast<size_t>(favicon_svg_end - favicon_svg_start);
  httpd_resp_set_type(req, "image/svg+xml");
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
  return httpd_resp_send(
      req, reinterpret_cast<const char *>(favicon_svg_start), n);
}
#endif  // CONFIG_NBP_WIFI

}  // namespace

void record_read() { g_reads.fetch_add(1, std::memory_order_relaxed); }
void record_write() { g_writes.fetch_add(1, std::memory_order_relaxed); }
void record_notify() { g_notifies.fetch_add(1, std::memory_order_relaxed); }
void set_clone_stats_provider(CloneStatsProvider fn) {
  g_clone_stats_provider = fn;
}

void set_nat_throughput_provider(NatThroughputProvider fn) {
  g_nat_throughput_provider = fn;
}

size_t build_stats_json(char *buf, size_t cap) {
  // BLE-sourced counters. When NBP_BLE is off the backend (scanner +
  // GATT pool) isn't compiled, so these read as zero — the dashboard
  // chart just shows flat BLE lines.
#if CONFIG_NBP_BLE
  unsigned in_use = proxy::MAX_CONNECTIONS -
                    ble_backend::connection::free_slots();
  unsigned long adverts = ble_backend::scanner::adv_count();
  unsigned long notify_rx = ble_backend::notify_rx_total();
  unsigned notify_handle = ble_backend::last_notify_handle();
#else
  unsigned in_use = 0;
  unsigned long adverts = 0, notify_rx = 0;
  unsigned notify_handle = 0;
#endif
  // Fold the clone GATT image's activity into the same counters so the
  // dashboard chart reflects both transports. Provider is null until
  // main installs one (under CONFIG_NBP_CLONE).
  CloneCounters cc{};
  if (g_clone_stats_provider) g_clone_stats_provider(&cc);
  in_use += cc.connected_centrals;
  int cpu0 = 0, cpu1 = 0;
  sample_cpu_pct(&cpu0, &cpu1);
  float temp_c = read_temp_c();
  // Compile-time capability flag: the dashboard hides the BLE-only
  // sections (device table, scan duty, adv interval, BLE TX) when false,
  // since their endpoints (/scan, /advitvl, /devices) aren't registered.
#if CONFIG_NBP_BLE
  const char *ble_cap = "true";
#else
  const char *ble_cap = "false";
#endif
  int n = std::snprintf(
      buf, cap,
      "{\"reads\":%lu,\"writes\":%lu,\"notifies\":%lu,\"adverts\":%lu,"
      "\"connections\":%u,\"heap\":%lu,"
      "\"notify_rx\":%lu,\"last_notify_handle\":%u,"
      "\"cpu0\":%d,\"cpu1\":%d,\"temp_c\":%.1f,\"ble\":%s",
      static_cast<unsigned long>(
          g_reads.load(std::memory_order_relaxed) + cc.reads),
      static_cast<unsigned long>(
          g_writes.load(std::memory_order_relaxed) + cc.writes),
      static_cast<unsigned long>(
          g_notifies.load(std::memory_order_relaxed) + cc.notifies),
      adverts,
      in_use,
      static_cast<unsigned long>(esp_get_free_heap_size()),
      notify_rx,
      notify_handle,
      cpu0, cpu1, static_cast<double>(temp_c), ble_cap);
  if (n < 0) return 0;
  // NAT-router SoftAP throughput, only when a provider is installed (i.e.
  // NBP_NAT_ROUTER builds). Cumulative byte counters; the dashboard turns
  // them into a KB/s rate. Omitted entirely on non-router builds so the
  // throughput chart self-hides. (No '}' yet — close after this.)
  if (g_nat_throughput_provider && static_cast<size_t>(n) < cap) {
    NatThroughput nt{};
    g_nat_throughput_provider(&nt);
    int m = std::snprintf(
        buf + n, cap - n, ",\"ap_rx\":%llu,\"ap_tx\":%llu",
        static_cast<unsigned long long>(nt.ap_rx_bytes),
        static_cast<unsigned long long>(nt.ap_tx_bytes));
    if (m > 0) n += m;
  }
  if (static_cast<size_t>(n) < cap) {
    int m = std::snprintf(buf + n, cap - n, "}");
    if (m > 0) n += m;
  }
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
  // ble_off true ⇒ BLE was fully shut down at runtime (NimBLEDevice::deinit);
  // the dashboard renders the BLE-TX dropdown as "off" and reboot re-enables.
  // "ble" still carries the last configured dBm so the prior value is restored
  // in the UI. (No BLE compiled in ⇒ false; the BLE UI is hidden either way.)
#if CONFIG_NBP_BLE
  bool ble_off = ble_backend::powered_off();
#else
  bool ble_off = false;
#endif
  int n = std::snprintf(buf, cap, "{\"wifi\":%d,\"ble\":%d,\"ble_off\":%s}", wifi,
                        static_cast<int>(g_ble_tx_dbm),
                        ble_off ? "true" : "false");
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
#if CONFIG_NBP_BLE
  if (httpd_query_key_value(query, "ble", val, sizeof(val)) == ESP_OK) {
    if (std::strcmp(val, "off") == 0) {
      // Full BLE shutdown — deinit the NimBLE stack. Runtime-only and not
      // persisted: a reboot brings BLE back at the stored dBm, so the device
      // can't be locked out of BLE the way it can't be bricked over WiFi-off.
      if (!ble_backend::powered_off()) {
        if (!ble_backend::power_off()) return "ble off failed";
        ESP_LOGI(TAG, "ble off (runtime full deinit; reboot to re-enable)");
        changed = true;
      }
    } else {
      int dbm = std::atoi(val);
      if (dbm < -12 || dbm > 9) return "ble -12..9 or off";
      if (ble_backend::powered_off()) return "ble off; reboot to re-enable";
      if (dbm != g_ble_tx_dbm) {
        if (apply_ble_tx_dbm(dbm) != ESP_OK) return "ble tx apply failed";
        g_ble_tx_dbm = static_cast<int8_t>(dbm);
        nvs_write_i8(NVS_BLE_TX_KEY, g_ble_tx_dbm);
        ESP_LOGI(TAG, "ble tx -> %d dBm (persisted)",
                 static_cast<int>(g_ble_tx_dbm));
        changed = true;
      }
    }
  }
#endif  // CONFIG_NBP_BLE
  if (!changed) return "no params or no change";
  return nullptr;
}

size_t build_cpufreq_json(char *buf, size_t cap) {
  int n = std::snprintf(buf, cap, "{\"mhz\":%d,\"ls\":%s,\"pdcpu\":%s}",
                        g_cpu_freq_mhz, g_light_sleep ? "true" : "false",
                        g_cpu_pd ? "true" : "false");
  if (n < 0) return 0;
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

// Subset of RFC 1123 hostname grammar: letter/digit/hyphen/underscore/
// dot, length 1..HOSTNAME_MAX, no leading/trailing '-' or '.'. We're
// strict here because the same value drives mDNS (`<name>.local`), the
// netif hostname, and the BLE GAP name — vendor stacks vary in what
// they tolerate, so reject anything that could surprise them.
bool is_valid_hostname(const char *s, size_t len) {
  if (len == 0 || len > proxy::HOSTNAME_MAX) return false;
  if (s[0] == '-' || s[0] == '.') return false;
  if (s[len - 1] == '-' || s[len - 1] == '.') return false;
  for (size_t i = 0; i < len; ++i) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

size_t build_hostname_json(char *buf, size_t cap) {
  // The hostname can in principle contain a backslash via NVS tampering
  // (handle_hostname_set rejects backslashes), but be defensive and
  // escape anyway so the response is always valid JSON.
  int n = std::snprintf(buf, cap, "{\"hostname\":\"%s\",\"default\":\"%s\"}",
                        proxy::hostname(), proxy::DEFAULT_HOSTNAME);
  if (n < 0) return 0;
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

size_t build_appinfo_json(char *buf, size_t cap) {
  // Firmware identity straight from the running image's app descriptor —
  // the same project/version/compile-time the boot log prints. Surfacing
  // it in the browser is the "am I on the right board / is this a stale
  // build?" check without a serial console (see CLAUDE.md multi-board note).
  const esp_app_desc_t *d = esp_app_get_description();
  // First 8 bytes of the ELF SHA256, matching the boot-log identifier —
  // enough to tell two builds apart at a glance.
  char sha[17];
  for (int i = 0; i < 8; ++i)
    std::snprintf(sha + i * 2, 3, "%02x", d->app_elf_sha256[i]);
  int n = std::snprintf(
      buf, cap,
      "{\"project\":\"%s\",\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
      "\"idf\":\"%s\",\"secure\":%lu,\"elf_sha256\":\"%s\"}",
      d->project_name, d->version, d->date, d->time, d->idf_ver,
      static_cast<unsigned long>(d->secure_version), sha);
  if (n < 0) return 0;
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

const char *handle_hostname_set(const char *query) {
  char val[proxy::HOSTNAME_MAX + 1];
  if (httpd_query_key_value(query, "val", val, sizeof(val)) != ESP_OK) {
    return "missing val=";
  }
  size_t len = std::strlen(val);
  if (!is_valid_hostname(val, len)) {
    return "invalid hostname (1..32 chars: A-Za-z0-9._-)";
  }
  // No-op fast path: don't churn NVS if the user resubmits the current
  // value (the UI does this on focus-out as a "save" gesture).
  if (std::strcmp(val, proxy::g_hostname) == 0) return nullptr;
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) return "nvs open failed";
  err = nvs_set_str(h, NVS_HOSTNAME_KEY, val);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err != ESP_OK) return "nvs write failed";
  ESP_LOGI(TAG,
           "hostname set to '%s' (persisted; reboot to apply to mDNS/BLE)",
           val);
  return nullptr;
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
  if (httpd_query_key_value(query, "pdcpu", val, sizeof(val)) == ESP_OK) {
    g_cpu_pd = (std::atoi(val) != 0);
    any = true;
  }
  if (!any) return "missing mhz=, ls= or pdcpu=";
  if (apply_cpu_freq_mhz(mhz, light_sleep) != ESP_OK) {
    return "cpu freq apply failed";
  }
  g_cpu_freq_mhz = mhz;
  nvs_write_i8(NVS_CPU_FREQ_KEY, static_cast<int8_t>(mhz / 10));
  nvs_write_i8("cpu_ls", light_sleep ? 1 : 0);
  nvs_write_i8("cpu_pd", g_cpu_pd ? 1 : 0);
  ESP_LOGI(TAG, "cpu -> %d MHz ls=%d pdcpu=%d (persisted)", mhz,
           light_sleep ? 1 : 0, g_cpu_pd ? 1 : 0);
  return nullptr;
}

#if CONFIG_NBP_BLE
// Peripheral advertising interval (ms). Stored as uint16 under NVS key
// "adv_itvl". 0 means "use NimBLE host default" (~30..60 ms range for
// connectable undirected adv); any other value clamps to the BLE-spec
// bounds 20..10240 ms in ble_backend::set_adv_interval_ms. The sentinel
// 0xFFFF means "advertising off" — the broadcaster is disabled entirely
// (ble_backend::set_advertising_enabled(false)) rather than slowed down.
// The reported rate field is informational — at the controller layer the
// actual rate depends on the random 0..10 ms delay the spec mandates per
// adv event.
constexpr const char *NVS_ADV_ITVL_KEY = "adv_itvl";

// Sentinel stored in the u16 NVS slot to mean "advertising disabled".
// Out of the valid ms range (0 or 20..10240), so it can't collide.
constexpr uint16_t ADV_ITVL_OFF = 0xFFFF;

size_t build_advitvl_json(char *buf, size_t cap) {
  // -1 = advertising off (broadcaster disabled). Otherwise reconstruct ms
  // from the cached 0.625 ms units in ble_backend: 0 units → 0 ms
  // (= "default"); else round to the nearest ms.
  int ms;
  if (!ble_backend::advertising_enabled()) {
    ms = -1;
  } else {
    uint16_t units = ble_backend::adv_interval_units();
    ms = (units == 0) ? 0 : static_cast<int>(
        (static_cast<uint32_t>(units) * 625u + 500u) / 1000u);
  }
  int n = std::snprintf(buf, cap, "{\"ms\":%d}", ms);
  if (n < 0) return 0;
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

const char *handle_advitvl_set(const char *query) {
  char val[8];
  if (httpd_query_key_value(query, "ms", val, sizeof(val)) != ESP_OK) {
    return "missing ms=";
  }
  long parsed = std::strtol(val, nullptr, 10);
  // -1 = off (disable the broadcaster); 0 = NimBLE host default;
  // 20..10240 = explicit interval.
  if (parsed < -1 || parsed > 10240) return "ms must be -1, 0, or 20..10240";
  if (parsed > 0 && parsed < 20) return "ms must be -1, 0, or 20..10240";
  const bool off = (parsed == -1);

  // Persist 0xFFFF for off; otherwise the ms value itself.
  uint16_t stored = off ? ADV_ITVL_OFF : static_cast<uint16_t>(parsed);
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) return "nvs open failed";
  err = nvs_set_u16(h, NVS_ADV_ITVL_KEY, stored);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err != ESP_OK) return "nvs write failed";

  if (off) {
    ble_backend::set_advertising_enabled(false);
    ESP_LOGI(TAG, "advertising -> off (persisted, hot-applied)");
  } else {
    uint16_t ms = static_cast<uint16_t>(parsed);
    // Set the interval first so re-enabling starts with the new params.
    ble_backend::set_adv_interval_ms(ms);
    ble_backend::set_advertising_enabled(true);
    ESP_LOGI(TAG, "adv interval -> %u ms, advertising on (persisted)",
             static_cast<unsigned>(ms));
  }
  return nullptr;
}
#endif  // CONFIG_NBP_BLE

// WiFi power-save listen interval. Stored as int8 0..10 (0 = PS_NONE,
// >0 = PS_MAX_MODEM with that many DTIM beacons between wake-ups).
// Reported as {"li":N} so the dashboard dropdown can show the saved
// value. POST writes NVS and flips PS mode live; the listen_interval
// field itself only takes effect on the next association, so the JS
// pulses the reboot button as an apply hint (same UX as /hostname).
size_t build_wifips_json(char *buf, size_t cap) {
  int n = std::snprintf(buf, cap, "{\"li\":%d}", g_wifi_listen_interval);
  if (n < 0) return 0;
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

const char *handle_wifips_set(const char *query) {
  char val[8];
  if (httpd_query_key_value(query, "li", val, sizeof(val)) != ESP_OK) {
    return "missing li=";
  }
  int li = std::atoi(val);
  if (li < 0 || li > 10) return "li 0..10";
  if (li == g_wifi_listen_interval) return nullptr;
  if (nvs_write_i8(NVS_WIFI_LI_KEY, static_cast<int8_t>(li)) != ESP_OK) {
    return "nvs write failed";
  }
  g_wifi_listen_interval = li;
#if CONFIG_NBP_WIFI
  // PS-mode flip is safe to apply live; listen_interval inside the
  // wifi_config_t needs a reconnect to take effect on the AP — caller
  // should reboot for the full change.
  esp_wifi_set_ps(li > 0 ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE);
#endif
  ESP_LOGI(TAG, "wifi li -> %d (persisted; listen_interval applies on reboot)",
           li);
  return nullptr;
}

#ifdef CONFIG_NBP_SMP
// Persist + apply the static SMP passkey used when the proxy is the
// initiator and the peer (BMS, cloned upstream, …) requests pairing.
// Single source of truth — callers from /clone, future endpoints, or
// boot-time NVS replay all funnel through here.
const char *set_passkey(uint32_t pin) {
  if (pin > 999999) return "passkey must be 0..999999";
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
#if CONFIG_NBP_BTHOME
  // Joined per-row below; snapshotting once outside the loop avoids
  // re-locking the BTHome cache for every device row.
  static ble_backend::bthome::Reading bsnap[32];
  size_t bn = ble_backend::bthome::snapshot(bsnap, 32);
#endif

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
    if (rem() >= 1) *p++ = '"';
#if CONFIG_NBP_DEV_DETAILS
    if (r.appearance != 0) {
      bump(std::snprintf(p, rem(), ",\"app\":%u",
                         static_cast<unsigned>(r.appearance)));
    }
    if (r.connectable) bump(std::snprintf(p, rem(), ",\"conn\":1"));
    if (r.tag) bump(std::snprintf(p, rem(), ",\"tag\":\"%s\"", r.tag));
#endif
#if CONFIG_NBP_BTHOME
    const ble_backend::bthome::Reading *br = nullptr;
    for (size_t j = 0; j < bn; ++j) {
      if (bsnap[j].addr == r.addr) { br = &bsnap[j]; break; }
    }
    if (br) {
      bump(std::snprintf(p, rem(), ",\"bthome\":{"));
      bool first = true;
      auto kv = [&](const char *k, unsigned long v) {
        bump(std::snprintf(p, rem(), "%s\"%s\":%lu", first ? "" : ",", k, v));
        first = false;
      };
      auto kv_i = [&](const char *k, long v) {
        bump(std::snprintf(p, rem(), "%s\"%s\":%ld", first ? "" : ",", k, v));
        first = false;
      };
      if (br->encrypted) kv("enc", 1);
      if (br->have_temperature) kv_i("t", static_cast<long>(br->temperature_c100));
      if (br->have_humidity)    kv("h",   br->humidity_pct100);
      if (br->have_battery)     kv("b",   br->battery_pct);
      if (br->have_voltage)     kv("v",   br->voltage_mv);
      if (br->have_illuminance) kv("l",   static_cast<unsigned long>(br->illuminance_lx100));
      if (br->have_motion)      kv("m",   br->motion);
      if (br->have_button)      kv("btn", br->button_event);
      if (br->have_power)       kv("w",   static_cast<unsigned long>(br->power_w100));
      if (br->have_co2)         kv("co2", br->co2_ppm);
      if (br->have_packet_id)   kv("p",   br->packet_id);
      if (rem() >= 1) *p++ = '}';
    }
#endif
    if (rem() >= 1) *p++ = '}';
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
    for (const char *tag : NIMBLE_NOISY_TAGS) esp_log_level_set(tag, ESP_LOG_DEBUG);
    for (const char *tag : NIMBLE_CORE_TAGS) esp_log_level_set(tag, ESP_LOG_DEBUG);
#if CONFIG_NBP_BLE
    ble_backend::scanner::pause();
#endif
#if CONFIG_NBP_WEB_CONSOLE
    log_ring_reset();
#endif
    ESP_LOGI(TAG, "trace ON: scan paused, core=DEBUG, scan-tags=ERROR");
  } else {
    apply_level(g_current_nimble_level);
#if CONFIG_NBP_BLE
    ble_backend::scanner::resume();
#endif
    ESP_LOGI(TAG, "trace OFF: scan resumed, levels restored to %d",
             static_cast<int>(g_current_nimble_level));
  }
  return on;
}

#if CONFIG_NBP_BLE
size_t build_scan_json(char *buf, size_t cap) {
  uint16_t window = 0, interval = 0;
  ble_backend::scanner::get_duty(&window, &interval);
  int n = std::snprintf(
      buf, cap, "{\"window\":%u,\"interval\":%u,\"active\":%s}",
      static_cast<unsigned>(window), static_cast<unsigned>(interval),
      ble_backend::scanner::get_active() ? "true" : "false");
  if (n < 0) return 0;
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

const char *handle_scan_set(const char *query) {
  char val[8];
  uint16_t cur_win = 0, cur_int = 0;
  ble_backend::scanner::get_duty(&cur_win, &cur_int);
  long window = cur_win, interval = cur_int;
  bool duty_changed = false;
  bool active_changed = false;
  bool active = ble_backend::scanner::get_active();
  if (httpd_query_key_value(query, "window", val, sizeof(val)) == ESP_OK) {
    window = std::strtol(val, nullptr, 10);
    duty_changed = true;
  }
  if (httpd_query_key_value(query, "interval", val, sizeof(val)) == ESP_OK) {
    interval = std::strtol(val, nullptr, 10);
    duty_changed = true;
  }
  if (httpd_query_key_value(query, "active", val, sizeof(val)) == ESP_OK) {
    active = (std::atoi(val) != 0);
    active_changed = true;
  }
  if (!duty_changed && !active_changed) {
    return "missing window= / interval= / active=";
  }
  if (duty_changed) {
    if (window < 20 || window > 10000) return "window 20..10000 ms";
    if (interval < 20 || interval > 10000) return "interval 20..10000 ms";
    if (window > interval) return "window must be <= interval";
  }

  // NVS persist BEFORE applying so a bad apply doesn't leave stale
  // state behind.
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    if (duty_changed) {
      nvs_set_u16(h, "scan_win", static_cast<uint16_t>(window));
      nvs_set_u16(h, "scan_int", static_cast<uint16_t>(interval));
    }
    if (active_changed) {
      nvs_set_i8(h, "scan_act", active ? 1 : 0);
    }
    nvs_commit(h);
    nvs_close(h);
  }
  if (duty_changed) {
    ble_backend::scanner::set_duty(static_cast<uint16_t>(window),
                                   static_cast<uint16_t>(interval));
  }
  if (active_changed) {
    ble_backend::scanner::set_active(active);
  }
  return nullptr;
}

void apply_scan_from_nvs() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  uint16_t window = 0, interval = 0;
  bool have_w = (nvs_get_u16(h, "scan_win", &window) == ESP_OK);
  bool have_i = (nvs_get_u16(h, "scan_int", &interval) == ESP_OK);
  int8_t active_v = -1;
  bool have_a = (nvs_get_i8(h, "scan_act", &active_v) == ESP_OK);
  nvs_close(h);
  if (have_w && have_i && window >= 20 && window <= interval &&
      interval <= 10000) {
    ble_backend::scanner::set_duty(window, interval);
    ESP_LOGI(TAG, "scan duty from NVS: window=%u interval=%u", window,
             interval);
  }
  if (have_a) {
    ble_backend::scanner::set_active(active_v != 0);
  }
}
#endif  // CONFIG_NBP_BLE

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

#if CONFIG_NBP_BLE
void apply_adv_interval_from_nvs() {
  uint16_t stored = 0;
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    if (nvs_get_u16(h, NVS_ADV_ITVL_KEY, &stored) != ESP_OK) stored = 0;
    nvs_close(h);
  }
  if (stored == ADV_ITVL_OFF) {
    // Disable the broadcaster; leave the interval at host default for
    // whenever the user re-enables via POST /advitvl?ms=0.
    ble_backend::set_adv_interval_ms(0);
    ble_backend::set_advertising_enabled(false);
    ESP_LOGI(TAG, "advertising: disabled (persisted off)");
    return;
  }
  ble_backend::set_adv_interval_ms(stored);
  if (stored == 0) {
    ESP_LOGI(TAG, "adv interval: host default (~30..60 ms)");
  } else {
    ESP_LOGI(TAG, "adv interval from NVS: %u ms",
             static_cast<unsigned>(stored));
  }
}
#endif  // CONFIG_NBP_BLE

void apply_hostname_from_nvs() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  char buf[proxy::HOSTNAME_MAX + 1];
  size_t sz = sizeof(buf);
  esp_err_t err = nvs_get_str(h, NVS_HOSTNAME_KEY, buf, &sz);
  nvs_close(h);
  if (err != ESP_OK) return;  // keep static default
  // nvs_get_str writes including NUL; sz includes it.
  size_t len = (sz == 0) ? 0 : sz - 1;
  if (!is_valid_hostname(buf, len)) {
    ESP_LOGW(TAG, "stored hostname '%s' fails validation; using default '%s'",
             buf, proxy::DEFAULT_HOSTNAME);
    return;
  }
  std::memcpy(proxy::g_hostname, buf, len + 1);
  ESP_LOGI(TAG, "hostname from NVS: '%s'", proxy::g_hostname);
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
#if CONFIG_NBP_BLE
  apply_ble_tx_dbm(g_ble_tx_dbm);
#endif
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
  int8_t pd = 0;
  if (nvs_read_i8("cpu_pd", &pd) == ESP_OK) g_cpu_pd = (pd != 0);
  esp_err_t err = apply_cpu_freq_mhz(g_cpu_freq_mhz, g_light_sleep);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_pm_configure(%d MHz ls=%d) failed: %s",
             g_cpu_freq_mhz, g_light_sleep ? 1 : 0, esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "CPU applied: %d MHz, light sleep=%d, pdcpu=%d",
           g_cpu_freq_mhz, g_light_sleep ? 1 : 0, g_cpu_pd ? 1 : 0);
}

void apply_wifi_ps_from_nvs() {
  int8_t v;
  if (nvs_read_i8(NVS_WIFI_LI_KEY, &v) == ESP_OK && v >= 0 && v <= 10) {
    g_wifi_listen_interval = v;
  }
  ESP_LOGI(TAG, "wifi listen_interval (NVS): %d", g_wifi_listen_interval);
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
  httpd_uri_t favicon = {.uri = "/favicon.svg",
                         .method = HTTP_GET,
                         .handler = &favicon_get,
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
  httpd_register_uri_handler(srv, &root);
  httpd_register_uri_handler(srv, &favicon);
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
#if CONFIG_NBP_BLE
  httpd_uri_t advitvl_g = {.uri = "/advitvl",
                           .method = HTTP_GET,
                           .handler = &advitvl_get,
                           .user_ctx = nullptr};
  httpd_uri_t advitvl_p = {.uri = "/advitvl",
                           .method = HTTP_POST,
                           .handler = &advitvl_post,
                           .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &advitvl_g);
  httpd_register_uri_handler(srv, &advitvl_p);
#endif  // CONFIG_NBP_BLE
  httpd_uri_t wifips_g = {.uri = "/wifips",
                          .method = HTTP_GET,
                          .handler = &wifips_get,
                          .user_ctx = nullptr};
  httpd_uri_t wifips_p = {.uri = "/wifips",
                          .method = HTTP_POST,
                          .handler = &wifips_post,
                          .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &wifips_g);
  httpd_register_uri_handler(srv, &wifips_p);
  httpd_uri_t hostname_g = {.uri = "/hostname",
                            .method = HTTP_GET,
                            .handler = &hostname_get,
                            .user_ctx = nullptr};
  httpd_uri_t hostname_p = {.uri = "/hostname",
                            .method = HTTP_POST,
                            .handler = &hostname_post,
                            .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &hostname_g);
  httpd_register_uri_handler(srv, &hostname_p);
  httpd_uri_t appinfo_g = {.uri = "/appinfo",
                           .method = HTTP_GET,
                           .handler = &appinfo_get,
                           .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &appinfo_g);
#if CONFIG_NBP_LIVENESS_WDT
  httpd_uri_t liveness_g = {.uri = "/liveness",
                            .method = HTTP_GET,
                            .handler = &liveness_get,
                            .user_ctx = nullptr};
  httpd_uri_t liveness_p = {.uri = "/liveness",
                            .method = HTTP_POST,
                            .handler = &liveness_post,
                            .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &liveness_g);
  httpd_register_uri_handler(srv, &liveness_p);
  httpd_uri_t wifihist_g = {.uri = "/wifihist",
                            .method = HTTP_GET,
                            .handler = &wifihist_get,
                            .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &wifihist_g);
#endif
#if CONFIG_NBP_BLE
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
#endif  // CONFIG_NBP_BLE
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
