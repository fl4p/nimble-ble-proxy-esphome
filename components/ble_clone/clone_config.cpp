#include "clone_config.h"

#ifdef CONFIG_NBP_CLONE

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include <cstring>

namespace ble_clone::config {

namespace {

constexpr const char *TAG = "clone.cfg";

constexpr const char *NVS_NS = "nbp_clone";
constexpr const char *NVS_ADDR = "addr";
constexpr const char *NVS_TYPE = "type";
constexpr const char *NVS_ENABLED = "enabled";
constexpr const char *NVS_NAME_SUFFIX = "name_suffix";

constexpr const char *DEFAULT_NAME_SUFFIX = "_cloned";

SemaphoreHandle_t g_mutex = nullptr;
Target g_snapshot{};
bool g_loaded = false;

void ensure_mutex() {
  if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();
}

}  // namespace

void load() {
  ensure_mutex();

  Target t{};
  t.address = 0;
  t.address_type = 0;
  t.enabled = false;
  std::strncpy(t.name_suffix, DEFAULT_NAME_SUFFIX, sizeof(t.name_suffix) - 1);

  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
  if (err == ESP_OK) {
    uint64_t addr = 0;
    if (nvs_get_u64(h, NVS_ADDR, &addr) == ESP_OK) t.address = addr;

    uint8_t type = 0;
    if (nvs_get_u8(h, NVS_TYPE, &type) == ESP_OK) t.address_type = type;

    uint8_t en = 0;
    if (nvs_get_u8(h, NVS_ENABLED, &en) == ESP_OK) t.enabled = (en != 0);

    size_t sz = sizeof(t.name_suffix);
    char tmp[sizeof(t.name_suffix)] = {0};
    if (nvs_get_str(h, NVS_NAME_SUFFIX, tmp, &sz) == ESP_OK) {
      std::strncpy(t.name_suffix, tmp, sizeof(t.name_suffix) - 1);
      t.name_suffix[sizeof(t.name_suffix) - 1] = 0;
    }
    nvs_close(h);
  } else if (err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "nvs_open ro: %s", esp_err_to_name(err));
  }

  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_snapshot = t;
  g_loaded = true;
  xSemaphoreGive(g_mutex);

  ESP_LOGI(TAG, "loaded addr=%012llx type=%u enabled=%d suffix='%s'",
           static_cast<unsigned long long>(t.address), t.address_type,
           t.enabled, t.name_suffix);
}

Target snapshot() {
  ensure_mutex();
  Target out{};
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  out = g_snapshot;
  xSemaphoreGive(g_mutex);
  return out;
}

bool set(const Target &t) {
  ensure_mutex();

  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "nvs_open rw: %s", esp_err_to_name(err));
    return false;
  }
  err = nvs_set_u64(h, NVS_ADDR, t.address);
  if (err == ESP_OK) err = nvs_set_u8(h, NVS_TYPE, t.address_type);
  if (err == ESP_OK) err = nvs_set_u8(h, NVS_ENABLED, t.enabled ? 1 : 0);
  if (err == ESP_OK) err = nvs_set_str(h, NVS_NAME_SUFFIX, t.name_suffix);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "nvs write: %s", esp_err_to_name(err));
    return false;
  }

  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_snapshot = t;
  xSemaphoreGive(g_mutex);
  ESP_LOGI(TAG, "set addr=%012llx type=%u enabled=%d suffix='%s'",
           static_cast<unsigned long long>(t.address), t.address_type,
           t.enabled, t.name_suffix);
  return true;
}

}  // namespace ble_clone::config

#endif  // CONFIG_NBP_CLONE
