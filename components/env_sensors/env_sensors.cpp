#include "env_sensors.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/i2c_master.h"
#include "env_core.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_confirm.h"
#include "nbp_gpio.h"
#include "nvs.h"
#include "onewire_bus.h"
#include "onewire_device.h"

namespace env_sensors {
namespace {

constexpr const char *TAG = "env";
constexpr const char *NVS_NS = "envsens";
constexpr int kDsMax = CONFIG_NBP_ENV_DS_MAX;
constexpr uint32_t kStack = 3072;
constexpr UBaseType_t kPrio = 1;
constexpr int kI2cTimeoutMs = 50;
constexpr int kShtMeasMs = 16;  // 0x2400 high repeatability, max 15.5 ms
constexpr int kDsConvMs = 750;  // 12 bit
constexpr size_t kJsonCap = env::kJsonBase + 2 * env::kJsonSht + kDsMax * env::kJsonDs;

// Pins in use (-1 = bus off) and the raw NVS values (-2 = no override).
int g_sda = CONFIG_NBP_ENV_SDA_GPIO, g_scl = CONFIG_NBP_ENV_SCL_GPIO,
    g_ow = CONFIG_NBP_ENV_OW_GPIO;
int g_nvs[3] = {-2, -2, -2};
std::atomic<int> g_period{CONFIG_NBP_ENV_PERIOD_S};
bool g_loaded = false, g_pins_ok = false;

TaskHandle_t g_task = nullptr;
i2c_master_bus_handle_t g_i2c = nullptr;
i2c_master_dev_handle_t g_sht_dev[2] = {};
onewire_bus_handle_t g_ow_bus = nullptr;

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
env::Reading g_sht[2] = {}, g_ds[kDsMax] = {};
env::Counters g_cnt = {};

constexpr const char *kKeys[3] = {"sda", "scl", "ow"};
constexpr int kKconfig[3] = {CONFIG_NBP_ENV_SDA_GPIO, CONFIG_NBP_ENV_SCL_GPIO,
                             CONFIG_NBP_ENV_OW_GPIO};

uint64_t own_mask() {
  return g_pins_ok ? nbp_gpio::bit(g_sda) | nbp_gpio::bit(g_scl) | nbp_gpio::bit(g_ow) : 0;
}

bool pins_ok(int sda, int scl, int ow) {
  uint64_t own = own_mask();
  auto ok = [&](int p) { return p == -1 || nbp_gpio::allowed(p, own); };
  if (!ok(sda) || !ok(scl) || !ok(ow) || (sda < 0) != (scl < 0)) return false;
  return sda < 0 || (sda != scl && sda != ow && scl != ow);
}

void load_once() {
  if (g_loaded) return;
  g_loaded = true;
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    int8_t i8;
    uint16_t u16;
    for (int i = 0; i < 3; i++)
      if (nvs_get_i8(h, kKeys[i], &i8) == ESP_OK) g_nvs[i] = i8;
    if (nvs_get_u16(h, "period", &u16) == ESP_OK && u16 >= 5 && u16 <= 3600)
      g_period = u16;
    nvs_close(h);
  }
  int p[3];
  for (int i = 0; i < 3; i++) p[i] = g_nvs[i] == -2 ? kKconfig[i] : g_nvs[i];
  if (pins_ok(p[0], p[1], p[2])) {
    g_sda = p[0], g_scl = p[1], g_ow = p[2];
  } else {
    ESP_LOGW(TAG, "NVS pins sda=%d scl=%d ow=%d invalid, using Kconfig", p[0], p[1], p[2]);
  }
  g_pins_ok = pins_ok(g_sda, g_scl, g_ow);
  if (g_pins_ok)
    nbp_gpio::claim(own_mask());
  else
    ESP_LOGE(TAG, "pins sda=%d scl=%d ow=%d not allowed, sensors off", g_sda, g_scl, g_ow);
}

// Under g_mux.
void note(env::Reading &r, uint8_t e, float t, float rh, int64_t now) {
  r.err = e;
  if (e == env::kOk) {
    r.t = t;
    r.rh = rh;
    r.at_us = now;
    r.fails = 0;
  } else {
    r.fails++;
  }
}

void sample_sht() {
  for (int k = 0; k < 2; k++) {
    uint8_t addr = 0x44 + k;
    esp_err_t e = i2c_master_probe(g_i2c, addr, kI2cTimeoutMs);  // silent on NACK
    env::Err r;
    float t = 0, rh = 0;
    if (e == ESP_ERR_NOT_FOUND) {
      r = env::kAbsent;
    } else if (e != ESP_OK) {
      r = env::kI2c;
    } else {
      static const uint8_t cmd[2] = {0x24, 0x00};
      uint8_t b[6];
      e = i2c_master_transmit(g_sht_dev[k], cmd, 2, kI2cTimeoutMs);
      if (e == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(kShtMeasMs) + 2);
        e = i2c_master_receive(g_sht_dev[k], b, 6, kI2cTimeoutMs);
      }
      r = e != ESP_OK ? env::kI2c : env::sht_parse(b, &t, &rh);
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&g_mux);
    env::Reading &s = g_sht[k];
    // An address never seen is not reported; one that vanished is.
    if (s.id || r != env::kAbsent) {
      s.id = addr;
      note(s, r, t, rh, now);
      if (r == env::kCrc) g_cnt.sht_crc++;
      else if (r != env::kOk) g_cnt.sht_i2c++;
    }
    portEXIT_CRITICAL(&g_mux);
  }
}

env::Err ds_read(uint64_t rom, float *t) {
  uint8_t cmd[10] = {0x55};  // match ROM, ROM, read scratchpad
  std::memcpy(cmd + 1, &rom, 8);
  cmd[9] = 0xBE;
  uint8_t sp[9];
  esp_err_t e = onewire_bus_reset(g_ow_bus);
  if (e == ESP_ERR_NOT_FOUND) return env::kAbsent;
  if (e == ESP_OK) e = onewire_bus_write_bytes(g_ow_bus, cmd, sizeof(cmd));
  if (e == ESP_OK) e = onewire_bus_read_bytes(g_ow_bus, sp, sizeof(sp));
  return e != ESP_OK ? env::kBus : env::ds_parse(sp, t);
}

struct DsResult {
  uint64_t rom;
  float t;
  env::Err err;
};

void sample_ds() {
  DsResult res[kDsMax];
  int n = 0, extra = 0;
  esp_err_t e = onewire_bus_reset(g_ow_bus);  // no presence: skip the search
  bool bus_err = e != ESP_OK && e != ESP_ERR_NOT_FOUND;
  if (e == ESP_OK) {
    onewire_device_iter_handle_t it = nullptr;
    if (onewire_new_device_iter(g_ow_bus, &it) == ESP_OK) {
      onewire_device_t d;
      for (int guard = 0; guard < 2 * kDsMax + 8; guard++) {
        e = onewire_device_iter_get_next(it, &d);
        if (e == ESP_ERR_NOT_FOUND) break;
        if (e == ESP_ERR_INVALID_CRC) continue;  // the search state advanced
        if (e != ESP_OK) {
          bus_err = true;
          break;
        }
        uint8_t fam = d.address & 0xFF;
        if (fam != 0x28 && fam != 0x22) continue;  // DS18B20, DS1822
        if (n < kDsMax) res[n++] = {d.address, 0, env::kOk};
        else extra++;
      }
      onewire_del_device_iter(it);
    }
    static const uint8_t conv[2] = {0xCC, 0x44};  // skip ROM, convert T
    if (n && onewire_bus_reset(g_ow_bus) == ESP_OK &&
        onewire_bus_write_bytes(g_ow_bus, conv, 2) == ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(kDsConvMs) + 1);
      for (int i = 0; i < n; i++) res[i].err = ds_read(res[i].rom, &res[i].t);
    } else {
      for (int i = 0; i < n; i++) res[i].err = env::kBus;
    }
  }
  int64_t now = esp_timer_get_time();
  bool seen[kDsMax] = {};
  portENTER_CRITICAL(&g_mux);
  // Known ROMs first, then new ones into free slots, then into slots whose
  // ROM was not found this cycle.
  for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < n; i++) {
      if (res[i].rom == 0) continue;
      int j = 0;
      while (j < kDsMax && !(pass == 0   ? g_ds[j].id == res[i].rom
                             : pass == 1 ? g_ds[j].id == 0
                                         : !seen[j]))
        j++;
      if (j == kDsMax) continue;
      if (pass) g_ds[j] = {res[i].rom, 0, 0, 0, 0, 0};
      seen[j] = true;
      note(g_ds[j], res[i].err, res[i].t, 0, now);
      if (res[i].err == env::kCrc) g_cnt.ds_crc++;
      else if (res[i].err == env::kPor) g_cnt.ds_por++;
      else if (res[i].err == env::kAbsent) g_cnt.ds_miss++;
      else if (res[i].err == env::kBus) bus_err = true;
      res[i].rom = 0;
    }
  }
  for (int i = 0; i < n; i++) extra += res[i].rom != 0;
  if (extra) g_cnt.ds_full++;
  for (int j = 0; j < kDsMax; j++) {
    if (!g_ds[j].id || seen[j]) continue;
    note(g_ds[j], bus_err ? env::kBus : env::kAbsent, 0, 0, now);
    if (!bus_err) g_cnt.ds_miss++;
  }
  if (bus_err) g_cnt.ds_bus++;
  portEXIT_CRITICAL(&g_mux);
}

void sample_task(void *) {
  for (;;) {
    int64_t t0 = esp_timer_get_time();
    if (g_i2c) sample_sht();
    if (g_ow_bus) sample_ds();
    portENTER_CRITICAL(&g_mux);
    g_cnt.cycles++;
    portEXIT_CRITICAL(&g_mux);
    int64_t left_ms = g_period.load() * 1000LL - (esp_timer_get_time() - t0) / 1000;
    vTaskDelay(pdMS_TO_TICKS(left_ms > 1000 ? left_ms : 1000));
  }
}

esp_err_t sensors_get(httpd_req_t *req) {
  env::Reading s[2], d[kDsMax];
  env::Counters c;
  portENTER_CRITICAL(&g_mux);
  std::memcpy(s, g_sht, sizeof(s));
  std::memcpy(d, g_ds, sizeof(d));
  c = g_cnt;
  portEXIT_CRITICAL(&g_mux);
  env::Cfg cfg = {g_sda, g_scl, g_ow, g_period.load(), g_task != nullptr,
                  g_task ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(g_task)) : 0u};
  char buf[kJsonCap];
  int n = env::render(buf, sizeof(buf), s, 2, d, kDsMax, c, cfg, esp_timer_get_time());
  if (n < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

// POST /sensors?period=30&sda=&scl=&ow=   period applies at once; pins
// (-1 = bus off, kconfig = drop the override) after a reboot.
esp_err_t sensors_post(httpd_req_t *req) {
  if (!nbp_http::confirmed(req)) return nbp_http::send_unconfirmed(req);
  char q[96], v[12];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  long period = -1;
  int pins[3] = {g_nvs[0], g_nvs[1], g_nvs[2]};
  bool set_pins = false;
  if (httpd_query_key_value(q, "period", v, sizeof(v)) == ESP_OK) {
    period = strtol(v, nullptr, 10);
    if (period < 5 || period > 3600)
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "period 5..3600");
  }
  for (int i = 0; i < 3; i++) {
    if (httpd_query_key_value(q, kKeys[i], v, sizeof(v)) != ESP_OK) continue;
    set_pins = true;
    if (std::strcmp(v, "kconfig") == 0) {
      pins[i] = -2;
      continue;
    }
    char *end;
    long p = strtol(v, &end, 10);
    if (end == v || *end || p < -1 || p > 48)
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pin: -1..48 or kconfig");
    pins[i] = static_cast<int>(p);
  }
  if (set_pins) {
    int p[3];
    for (int i = 0; i < 3; i++) p[i] = pins[i] == -2 ? kKconfig[i] : pins[i];
    if (!pins_ok(p[0], p[1], p[2]))
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pins not allowed");
  }
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
  esp_err_t e = ESP_OK;
  if (period > 0) e = nvs_set_u16(h, "period", static_cast<uint16_t>(period));
  for (int i = 0; set_pins && e == ESP_OK && i < 3; i++) {
    if (pins[i] != -2) {
      e = nvs_set_i8(h, kKeys[i], static_cast<int8_t>(pins[i]));
    } else if ((e = nvs_erase_key(h, kKeys[i])) == ESP_ERR_NVS_NOT_FOUND) {
      e = ESP_OK;
    }
  }
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  if (e != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write");
  if (period > 0) g_period = static_cast<int>(period);
  if (set_pins) std::memcpy(g_nvs, pins, sizeof(pins));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, set_pins ? "{\"ok\":true,\"reboot\":true}"
                                          : "{\"ok\":true}");
}

void open_i2c() {
  i2c_master_bus_config_t bc = {};
  bc.i2c_port = -1;
  bc.sda_io_num = static_cast<gpio_num_t>(g_sda);
  bc.scl_io_num = static_cast<gpio_num_t>(g_scl);
  bc.clk_source = I2C_CLK_SRC_DEFAULT;
  bc.glitch_ignore_cnt = 7;
  bc.flags.enable_internal_pullup = true;
  esp_err_t e = i2c_new_master_bus(&bc, &g_i2c);
  for (int k = 0; e == ESP_OK && k < 2; k++) {
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = 0x44 + k;
    dc.scl_speed_hz = 100000;
    e = i2c_master_bus_add_device(g_i2c, &dc, &g_sht_dev[k]);
  }
  if (e == ESP_OK) return;
  ESP_LOGE(TAG, "I2C sda=%d scl=%d: %s", g_sda, g_scl, esp_err_to_name(e));
  for (auto &d : g_sht_dev) {
    if (d) i2c_master_bus_rm_device(d);
    d = nullptr;
  }
  if (g_i2c) i2c_del_master_bus(g_i2c);
  g_i2c = nullptr;
}

void open_ow() {
  onewire_bus_config_t bc = {};
  bc.bus_gpio_num = g_ow;
  bc.flags.en_pull_up = 1;
  onewire_bus_rmt_config_t rc = {};
  rc.max_rx_bytes = 9;  // scratchpad
  esp_err_t e = onewire_new_bus_rmt(&bc, &rc, &g_ow_bus);
  if (e == ESP_OK) return;
  ESP_LOGE(TAG, "1-Wire gpio=%d: %s", g_ow, esp_err_to_name(e));
  g_ow_bus = nullptr;
}

}  // namespace

void early_init() { load_once(); }

void start() {
  if (g_task != nullptr) return;
  load_once();
  if (!g_pins_ok) return;
  if (g_sda >= 0 && !g_i2c) open_i2c();
  if (g_ow >= 0 && !g_ow_bus) open_ow();
  if (!g_i2c && !g_ow_bus) return;
  if (xTaskCreate(&sample_task, "env", kStack, nullptr, kPrio, &g_task) != pdPASS) {
    ESP_LOGE(TAG, "xTaskCreate failed");
    g_task = nullptr;
    return;
  }
  ESP_LOGI(TAG, "sda=%d scl=%d ow=%d every %d s", g_i2c ? g_sda : -1,
           g_i2c ? g_scl : -1, g_ow_bus ? g_ow : -1, g_period.load());
}

void register_endpoints(void *httpd_handle) {
  if (httpd_handle == nullptr) return;
  auto srv = static_cast<httpd_handle_t>(httpd_handle);
  httpd_uri_t u = {};
  u.uri = "/sensors";
  u.method = HTTP_GET;
  u.handler = &sensors_get;
  httpd_register_uri_handler(srv, &u);
  u.method = HTTP_POST;
  u.handler = &sensors_post;
  httpd_register_uri_handler(srv, &u);
}

int append_stats_json(char *buf, size_t cap) {
  portENTER_CRITICAL(&g_mux);
  env::Counters c = g_cnt;
  portEXIT_CRITICAL(&g_mux);
  return std::snprintf(
      buf, cap, ",\"env_cycles\":%lu,\"env_sht_err\":%lu,\"env_ds_err\":%lu,\"env_stack_free\":%u",
      static_cast<unsigned long>(c.cycles),
      static_cast<unsigned long>(c.sht_crc + c.sht_i2c),
      static_cast<unsigned long>(c.ds_crc + c.ds_miss + c.ds_bus + c.ds_por + c.ds_full),
      g_task ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(g_task)) : 0u);
}

}  // namespace env_sensors
