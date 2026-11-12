// See include/oom_guard.h for why the global allocation functions are
// replaced here.

#include "oom_guard.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstdlib>

#ifndef CONFIG_NBP_OOM_NEW_WAIT_MS
#define CONFIG_NBP_OOM_NEW_WAIT_MS 2000
#endif

namespace oom_guard {

namespace {

constexpr const char *TAG = "oom";
constexpr uint32_t kRetryStepMs = 10;
constexpr uint64_t kLogPeriodUs = 2 * 1000 * 1000;

// Written from the heap's failed-allocation hook, which runs inside
// heap_caps_malloc: possibly from IRAM with the flash cache off, possibly
// on both cores at once. Plain volatile words in DRAM, no locks, no
// atomics helpers that could live in flash. A lost increment when both
// cores fail an allocation in the same instant is acceptable.
DRAM_ATTR volatile uint32_t g_count = 0;
DRAM_ATTR volatile uint32_t g_last_size = 0;
DRAM_ATTR const char *volatile g_last_func = nullptr;
DRAM_ATTR volatile uint32_t g_last_caps = 0;

volatile uint32_t g_new_recovered = 0;
uint32_t g_logged = 0;
esp_timer_handle_t g_log_timer = nullptr;

IRAM_ATTR void on_alloc_failed(size_t size, uint32_t caps,
                               const char *function_name) {
  g_count = g_count + 1;
  g_last_size = static_cast<uint32_t>(size);
  g_last_caps = caps;
  g_last_func = function_name;
}

// Runs on the esp_timer task, so it may log (the hook above may not).
void log_tick(void *) {
  const uint32_t c = g_count;
  if (c == g_logged) return;
  const char *fn = g_last_func;
  ESP_LOGW(TAG,
           "%lu failed heap allocation(s) since last report (total %lu); "
           "last %lu B caps 0x%lx in %s; free %u largest %u min %u",
           static_cast<unsigned long>(c - g_logged),
           static_cast<unsigned long>(c),
           static_cast<unsigned long>(g_last_size),
           static_cast<unsigned long>(g_last_caps), fn ? fn : "?",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)),
           static_cast<unsigned>(
               heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)),
           static_cast<unsigned>(esp_get_minimum_free_heap_size()));
  g_logged = c;
}

bool can_wait() {
  return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
         !xPortInIsrContext() && xPortCanYield();
}

[[noreturn]] void abort_oom(size_t n, uint32_t waited_ms) {
  // Static: no heap left to format into, and the panic is final anyway.
  static char msg[112];
  std::snprintf(msg, sizeof(msg),
                "OOM: operator new(%u) failed after %lu ms wait; free %u "
                "largest %u",
                static_cast<unsigned>(n), static_cast<unsigned long>(waited_ms),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)),
                static_cast<unsigned>(
                    heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
  esp_system_abort(msg);
}

}  // namespace

// Throwing-new slow path. Returns non-null or does not return.
void *new_slow(size_t n) {
  if (CONFIG_NBP_OOM_NEW_WAIT_MS == 0 || !can_wait()) abort_oom(n, 0);
  uint32_t waited = 0;
  while (waited < CONFIG_NBP_OOM_NEW_WAIT_MS) {
    vTaskDelay(pdMS_TO_TICKS(kRetryStepMs) ? pdMS_TO_TICKS(kRetryStepMs) : 1);
    waited += kRetryStepMs;
    // Only retry once a block could fit, so a long wait does not count
    // one failed allocation per step.
    if (heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < n) continue;
    void *p = std::malloc(n);
    if (p != nullptr) {
      g_new_recovered = g_new_recovered + 1;
      return p;
    }
  }
  abort_oom(n, waited);
}

void init() {
  if (heap_caps_register_failed_alloc_callback(&on_alloc_failed) != ESP_OK) {
    ESP_LOGE(TAG, "could not register the failed-allocation hook; "
                  "the oom counter will stay 0");
  }
  if (g_log_timer == nullptr) {
    esp_timer_create_args_t a{};
    a.callback = &log_tick;
    a.name = "oom_log";
    if (esp_timer_create(&a, &g_log_timer) == ESP_OK) {
      esp_timer_start_periodic(g_log_timer, kLogPeriodUs);
    } else {
      ESP_LOGE(TAG, "oom log timer not created; failures counted, not logged");
    }
  }
}

uint32_t count() { return g_count; }
size_t last_size() { return g_last_size; }
const char *last_function() { return g_last_func; }
uint32_t new_recovered() { return g_new_recovered; }

}  // namespace oom_guard

// ---- Replaceable global allocation functions ----
//
// operator delete is left to libstdc++ (it calls free()), which matches
// the malloc() below. Aligned new (align_val_t) is not replaced: nothing
// in the firmware over-aligns, and oom_guard::make() static_asserts it.

void *operator new(std::size_t n) {
  if (n == 0) n = 1;
  void *p = std::malloc(n);
  return p != nullptr ? p : oom_guard::new_slow(n);
}

void *operator new[](std::size_t n) { return ::operator new(n); }

void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
  return std::malloc(n == 0 ? 1 : n);
}

void *operator new[](std::size_t n, const std::nothrow_t &) noexcept {
  return std::malloc(n == 0 ? 1 : n);
}
