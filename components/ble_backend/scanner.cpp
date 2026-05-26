#include "scanner.h"

#include "address.h"
#include "api_proto.h"
#include "proxy_config.h"
#include "publish.h"

#include "NimBLEDevice.h"
#include "NimBLEScan.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pb_encode.h"

#include <atomic>
#include <cstring>

namespace ble_backend::scanner {

namespace {

constexpr const char *TAG = "ble.scan";
constexpr size_t BATCH = proxy::ADV_BATCH_SIZE;
constexpr size_t FLUSH_TASK_STACK = 4096;
constexpr UBaseType_t FLUSH_TASK_PRIO = 4;

// One batched response under construction. Direct nanopb structs are
// what we hand to pb_encode — no intermediate copy needed.
struct Batch {
  proxyapi_BluetoothLERawAdvertisement records[BATCH];
  size_t count = 0;
};

std::atomic<bool> g_forwarding{false};
std::atomic<uint32_t> g_adv_count{0};
NimBLEScan *g_scan = nullptr;

SemaphoreHandle_t g_mutex = nullptr;
Batch g_pending;
TaskHandle_t g_flush_task = nullptr;

// pb_encode ctx for the flush path. Owns a SNAPSHOT of the batch so we
// can release the scanner mutex before doing socket IO.
struct EncodeCtx {
  proxyapi_BluetoothLERawAdvertisementsResponse msg;
};

size_t encode_response(void *vctx, uint8_t *buf, size_t cap) {
  auto *ctx = static_cast<EncodeCtx *>(vctx);
  pb_ostream_t stream = pb_ostream_from_buffer(buf, cap);
  if (!pb_encode(&stream,
                 proxyapi_BluetoothLERawAdvertisementsResponse_fields,
                 &ctx->msg)) {
    ESP_LOGE(TAG, "encode failed: %s", PB_GET_ERROR(&stream));
    return 0;
  }
  return stream.bytes_written;
}

// Snapshot g_pending into `out` and reset g_pending. Caller must hold g_mutex.
void drain_locked(EncodeCtx *out) {
  out->msg = proxyapi_BluetoothLERawAdvertisementsResponse_init_zero;
  out->msg.advertisements_count = g_pending.count;
  for (size_t i = 0; i < g_pending.count; ++i) {
    out->msg.advertisements[i] = g_pending.records[i];
  }
  g_pending.count = 0;
}

void flush_once() {
  // Take + drain + release, then send. Holding the mutex across socket
  // IO would stall the NimBLE host task on every adv callback.
  EncodeCtx ctx;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  if (g_pending.count == 0) {
    xSemaphoreGive(g_mutex);
    return;
  }
  drain_locked(&ctx);
  xSemaphoreGive(g_mutex);

  if (!publish::has_client()) return;
  publish::send_async(proxyapi::MSG_BLUETOOTH_LE_RAW_ADVERTISEMENTS_RESPONSE,
                      &encode_response, &ctx);
}

void flush_task(void *) {
  const TickType_t period = pdMS_TO_TICKS(proxy::ADV_FLUSH_INTERVAL_MS);
  while (true) {
    // Wake on notify (batch full) OR after period (time-based flush).
    ulTaskNotifyTake(pdTRUE, period);
    flush_once();
  }
}

class AdvCallbacks : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    g_adv_count.fetch_add(1, std::memory_order_relaxed);
    if (!g_forwarding.load(std::memory_order_acquire)) return;
    if (!publish::has_client()) return;

    // Build the record on the stack so we can drop it cheaply if the
    // batch is full.
    proxyapi_BluetoothLERawAdvertisement rec =
        proxyapi_BluetoothLERawAdvertisement_init_zero;
    NimBLEAddress addr = dev->getAddress();
    rec.address = address::swap6(static_cast<uint64_t>(addr));
    rec.rssi = dev->getRSSI();
    rec.address_type = addr.getType();

    // Raw adv payload (legacy adv is ≤31 B; with scan response appended
    // up to 62 B). The cap matches BluetoothLERawAdvertisement.data
    // max_size in api_subset.options.
    auto payload = dev->getPayload();
    size_t plen = payload.size();
    if (plen > sizeof(rec.data.bytes)) plen = sizeof(rec.data.bytes);
    if (plen > 0) std::memcpy(rec.data.bytes, payload.data(), plen);
    rec.data.size = plen;

    bool full = false;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (g_pending.count < BATCH) {
      g_pending.records[g_pending.count++] = rec;
      full = (g_pending.count >= BATCH);
    }
    // else: silently drop. With BATCH=16 and 100ms flush, this only
    // happens when the link is backed up — losing a few ads is acceptable.
    xSemaphoreGive(g_mutex);

    if (full && g_flush_task) {
      xTaskNotifyGive(g_flush_task);
    }
  }
};

AdvCallbacks g_cb;

}  // namespace

void init() {
  g_mutex = xSemaphoreCreateMutex();

  g_scan = NimBLEDevice::getScan();
  g_scan->setScanCallbacks(&g_cb, /*wantDuplicates=*/true);
  g_scan->setActiveScan(false);  // passive matches v1 feature flags
  g_scan->setInterval(proxy::SCAN_INTERVAL_MS);
  g_scan->setWindow(proxy::SCAN_WINDOW_MS);
  g_scan->setMaxResults(0);  // don't cache; we forward live

  xTaskCreate(&flush_task, "ble_adv_flush", FLUSH_TASK_STACK, nullptr,
              FLUSH_TASK_PRIO, &g_flush_task);
}

void start() {
  if (!g_scan) return;
  g_scan->start(0, /*restart=*/true);
  ESP_LOGI(TAG, "scanning");
}

void start_forwarding() {
  g_forwarding.store(true, std::memory_order_release);
  ESP_LOGI(TAG, "forwarding ON");
}

void stop_forwarding() {
  g_forwarding.store(false, std::memory_order_release);
  // Drain any pending so we don't leak a stale batch into the next session.
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_pending.count = 0;
  xSemaphoreGive(g_mutex);
  ESP_LOGI(TAG, "forwarding OFF");
}

uint32_t adv_count() {
  return g_adv_count.load(std::memory_order_relaxed);
}

}  // namespace ble_backend::scanner
