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

#if CONFIG_NBP_DEVICES_PANEL
// Live device table. Same mutex as the forwarding batch — critical
// sections are tiny (linear scan + memcpy) so the contention added on
// top of the adv-forward path is negligible.
constexpr size_t DEV_CAP = 64;
DeviceRow g_devices[DEV_CAP];
size_t g_device_count = 0;

// Returns index in g_devices for `addr`, allocating or evicting LRU as
// needed. Caller must hold g_mutex.
size_t find_or_alloc_device(uint64_t addr) {
  for (size_t i = 0; i < g_device_count; ++i) {
    if (g_devices[i].addr == addr) return i;
  }
  if (g_device_count < DEV_CAP) {
    size_t idx = g_device_count++;
    g_devices[idx] = DeviceRow{};
    g_devices[idx].addr = addr;
    return idx;
  }
  // Full — evict the row with the oldest sighting.
  size_t oldest = 0;
  for (size_t i = 1; i < DEV_CAP; ++i) {
    if (g_devices[i].last_ms < g_devices[oldest].last_ms) oldest = i;
  }
  g_devices[oldest] = DeviceRow{};
  g_devices[oldest].addr = addr;
  return oldest;
}

void record_device(const NimBLEAdvertisedDevice *dev, uint64_t addr,
                   uint8_t addr_type) {
  // Parse outside the mutex — getName() walks the AD list.
  std::string nm = dev->getName();
  int8_t rssi = static_cast<int8_t>(dev->getRSSI());
  uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

  xSemaphoreTake(g_mutex, portMAX_DELAY);
  auto &row = g_devices[find_or_alloc_device(addr)];
  row.addr_type = addr_type;
  row.rssi = rssi;
  row.adv_count++;
  row.last_ms = now_ms;
  // Persist last non-empty name — many devices put it only in the scan
  // response, so plain adv packets don't overwrite a known name.
  if (!nm.empty()) {
    size_t k = nm.size();
    if (k >= sizeof(row.name)) k = sizeof(row.name) - 1;
    std::memcpy(row.name, nm.data(), k);
    row.name[k] = 0;
  }
  xSemaphoreGive(g_mutex);
}
#endif  // CONFIG_NBP_DEVICES_PANEL

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

    // NimBLEAddress::operator uint64_t() memcpys NimBLE's 6 LE bytes
    // into a uint64 on an LE host. Result: bits 40-47 hold the MAC's
    // MSB, which is exactly the layout aioesphomeapi formats back into
    // MSB-first hex ("20A111022345" → "20:A1:11:02:23:45"). No swap.
    NimBLEAddress addr = dev->getAddress();
    uint64_t addr_u64 = static_cast<uint64_t>(addr);
#if CONFIG_NBP_DEVICES_PANEL
    record_device(dev, addr_u64, addr.getType());
#endif

    if (!g_forwarding.load(std::memory_order_acquire)) return;
    if (!publish::has_client()) return;

    // Build the record on the stack so we can drop it cheaply if the
    // batch is full.
    proxyapi_BluetoothLERawAdvertisement rec =
        proxyapi_BluetoothLERawAdvertisement_init_zero;
    rec.address = addr_u64;
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

  // NimBLE log level is applied by api_server::stats from NVS
  // (apply_log_overrides_from_nvs) before this runs. NimBLE-Cpp's
  // scanner is the noisiest source — "New advertiser: <mac>" at INFO
  // on every advert with wantDuplicates=true — so the persisted level
  // gates that flood.

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
  if (!g_scan) {
    ESP_LOGE(TAG, "g_scan is null — NimBLEDevice::init() must precede start()");
    return;
  }
  bool ok = g_scan->start(0, /*isContinue=*/true);
  if (!ok) {
    ESP_LOGE(TAG, "NimBLEScan::start() returned false — scan not active");
    return;
  }
  ESP_LOGI(TAG, "scanning (interval=%ums window=%ums passive)",
           proxy::SCAN_INTERVAL_MS, proxy::SCAN_WINDOW_MS);
}

void resume() {
  // NimBLE-Cpp's start() is a no-op if scan is already running, so this
  // is cheap to call defensively after every connect-procedure end.
  if (!g_scan) return;
  if (g_scan->isScanning()) return;
  if (!g_scan->start(0, /*isContinue=*/true)) {
    ESP_LOGW(TAG, "scan resume failed");
    return;
  }
  ESP_LOGI(TAG, "scan resumed");
}

void pause() {
  if (!g_scan) return;
  if (!g_scan->isScanning()) return;
  g_scan->stop();
  ESP_LOGI(TAG, "scan paused (trace)");
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

#if CONFIG_NBP_DEVICES_PANEL
size_t snapshot_devices(DeviceRow *out, size_t cap) {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  size_t n = g_device_count < cap ? g_device_count : cap;
  std::memcpy(out, g_devices, n * sizeof(DeviceRow));
  xSemaphoreGive(g_mutex);
  return n;
}
#endif

}  // namespace ble_backend::scanner
