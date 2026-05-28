#include "scanner.h"

#include "address.h"
#include "api_proto.h"
#include "bthome.h"
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
#include <cstdint>
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

// Live scan duty cycle. Initialised to proxy:: defaults, mutated at
// runtime via set_duty(). Read with relaxed ordering — both writers
// (httpd worker / BLE dispatch task) and readers (UI poll) tolerate
// being a tick behind.
std::atomic<uint16_t> g_window_ms{proxy::SCAN_WINDOW_MS};
std::atomic<uint16_t> g_interval_ms{proxy::SCAN_INTERVAL_MS};
std::atomic<bool> g_active_scan{true};

SemaphoreHandle_t g_mutex = nullptr;
Batch g_pending;
TaskHandle_t g_flush_task = nullptr;

#if CONFIG_NBP_DEVICES_PANEL
// Live device table. Same mutex as the forwarding batch — critical
// sections are tiny (linear scan + memcpy) so the contention added on
// top of the adv-forward path is negligible.
constexpr size_t DEV_CAP = 64;
// MACs rotate (random/resolvable-private addresses change every ~15 min),
// so without expiry the table fills with ghosts of devices long gone.
// Drop a row once it hasn't been seen for this long.
constexpr uint32_t DEVICE_STALE_MS = 30u * 60u * 1000u;  // 30 min
// How often flush_task sweeps the table for stale rows.
constexpr uint32_t DEVICE_PRUNE_INTERVAL_MS = 60u * 1000u;  // 1 min
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

// Drop rows not seen within DEVICE_STALE_MS. Caller must NOT hold g_mutex.
// uint32 millis wrap at ~49.7 days; the unsigned (now - last_ms) difference
// stays correct for any gap shorter than that, so the 30-min threshold is
// wrap-safe. Compacts in place by moving the last live row into each hole.
void prune_stale_devices() {
  const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  size_t i = 0;
  while (i < g_device_count) {
    if (now_ms - g_devices[i].last_ms > DEVICE_STALE_MS) {
      g_devices[i] = g_devices[--g_device_count];  // re-test the moved row
    } else {
      ++i;
    }
  }
  xSemaphoreGive(g_mutex);
}

#if CONFIG_NBP_DEV_DETAILS
// Classify a device by its manufacturer data + advertised 16-bit
// service UUIDs. Returns a static string literal (safe to store as
// pointer) or nullptr when nothing recognized. Manufacturer ID wins
// over service-UUID when both match because the company ID is usually
// the more specific signal (e.g. "Ruuvi" beats "EnvSense").
const char *classify_device(const NimBLEAdvertisedDevice *dev) {
  std::string mfg = dev->getManufacturerData();
  if (mfg.size() >= 2) {
    auto b0 = static_cast<uint8_t>(mfg[0]);
    auto b1 = static_cast<uint8_t>(mfg[1]);
    uint16_t cid = uint16_t(b0) | (uint16_t(b1) << 8);
    if (mfg.size() >= 4 &&
        static_cast<uint8_t>(mfg[2]) == 0xBE &&
        static_cast<uint8_t>(mfg[3]) == 0xAC) {
      return "AltBeacon";
    }
    if (cid == 0x004C && mfg.size() >= 3) {
      // Apple Continuity — first byte after CID is the subtype. iBeacon
      // (0x02) carries its own length byte (0x15); other subtypes are
      // generic Continuity TLVs documented from reverse-engineering.
      uint8_t sub = static_cast<uint8_t>(mfg[2]);
      if (sub == 0x02 && mfg.size() >= 25 &&
          static_cast<uint8_t>(mfg[3]) == 0x15) {
        // iBeacon UUID is bytes 4..19 in big-endian order (the iBeacon
        // spec transmits the proximity UUID MSB-first, matching how it
        // would be printed). Tag known vendor defaults.
        static const uint8_t ESTIMOTE[16] = {
            0xB9, 0x40, 0x7F, 0x30, 0xF5, 0xF8, 0x46, 0x6E,
            0xAF, 0xF9, 0x25, 0x55, 0x6B, 0x57, 0xFE, 0x6D};
        static const uint8_t KONTAKT[16] = {
            0xF7, 0x82, 0x6D, 0xA6, 0x4F, 0xA2, 0x4E, 0x98,
            0x80, 0x24, 0xBC, 0x5B, 0x71, 0xE0, 0x89, 0x3E};
        static const uint8_t RADIUS[16] = {
            0x2F, 0x23, 0x44, 0x54, 0xCF, 0x6D, 0x4A, 0x0F,
            0xAD, 0xF2, 0xF4, 0x91, 0x1B, 0xA9, 0xFF, 0xA6};
        static const uint8_t AIRLOCATE[16] = {
            0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2,
            0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0};
        // Tesla vehicles (Model 3/Y/S/X) broadcast iBeacon frames whose
        // Proximity UUID is the phone-key service UUID. The same UUID
        // also appears in the 128-bit service-UUID branch below for
        // cases where the car advertises the service directly.
        static const uint8_t TESLA[16] = {
            0x74, 0x27, 0x8B, 0xDA, 0xB6, 0x44, 0x45, 0x20,
            0x8F, 0x0C, 0x72, 0x0E, 0xAF, 0x05, 0x99, 0x35};
        const auto *u = reinterpret_cast<const uint8_t *>(mfg.data() + 4);
        if (std::memcmp(u, TESLA, 16) == 0) return "iBeacon·Tesla";
        if (std::memcmp(u, ESTIMOTE, 16) == 0) return "iBeacon·Estimote";
        if (std::memcmp(u, KONTAKT, 16) == 0) return "iBeacon·Kontakt";
        if (std::memcmp(u, RADIUS, 16) == 0) return "iBeacon·RadBeacon";
        if (std::memcmp(u, AIRLOCATE, 16) == 0) return "iBeacon·AirLocate";
        return "iBeacon";
      }
      if (sub == 0x02 && mfg.size() >= 4 &&
          static_cast<uint8_t>(mfg[3]) == 0x15) return "iBeacon";
      switch (sub) {
        case 0x05: return "AirDrop";
        case 0x07: return "AirPods";        // Proximity Pairing (AirPods, Beats, …)
        case 0x09: return "AirPlay";        // any device offering/discovering AirPlay
        case 0x0C: return "Handoff";
        case 0x0D: return "WiFiSettings";
        case 0x0E: return "Hotspot";
        case 0x0F: return "NearbyAction";   // Wi-Fi join prompt, setup, etc.
        case 0x10: return "NearbyInfo";     // presence + activity state (every iPhone)
        case 0x12: return "FindMy";
        case 0x16: return "HomeKit";
        // 0x0A / 0x0B are contested in community sources — fall through.
        default: break;
      }
      return "Apple";
    }
    switch (cid) {
      case 0x0006: return "Microsoft";
      case 0x0075: return "Samsung";
      case 0x00E0: return "Google";
      case 0x0087: return "Garmin";
      case 0x009E: return "Bose";
      case 0x012D: return "Sony";
      case 0x05A7: return "Sonos";
      case 0x004D: return "Sennheiser";
      case 0x015E: return "Tile";
      case 0x0499: return "Ruuvi";
      case 0x02E1: return "Victron";
      case 0x038F: return "Xiaomi";
      case 0x0157: return "Anker";
      case 0x0059: return "Nordic";
      case 0x02E5: return "Espressif";
      case 0x00C4: return "LG";
      default: break;
    }
  }
  // Fall through to service UUID inspection for devices that don't
  // emit manufacturer-data (BTHome sensors, Eddystone, ESS-only
  // peripherals, etc).
  uint8_t n = dev->getServiceUUIDCount();
  // 128-bit vendor-specific UUIDs — built once at first call. Most
  // proprietary protocols (Tesla phone-key, Nordic UART, TI SensorTag,
  // Bose Connect, Pebble) live in this space rather than SIG-allocated
  // 16-bit UUIDs. Equality test relies on NimBLEUUID::operator==.
  static const NimBLEUUID TESLA_KEY("74278BDA-B644-4520-8F0C-720EAF059935");
  static const NimBLEUUID NORDIC_UART("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
  static const NimBLEUUID TI_SENSORTAG("F000AA00-0451-4000-B000-000000000000");
  for (uint8_t i = 0; i < n; ++i) {
    NimBLEUUID u = dev->getServiceUUID(i);
    uint8_t bs = u.bitSize();
    if (bs == 16) {
      const uint8_t *v = u.getValue();
      uint16_t u16 = uint16_t(v[0]) | (uint16_t(v[1]) << 8);
      switch (u16) {
        case 0xFCD2: return "BTHome";
        case 0xFE9F: case 0xFE2C: return "FastPair";
        case 0xFEAA: return "Eddystone";
        case 0xFD6F: return "ENS";
        case 0xFE95: return "MiBeacon";
        case 0xFDA0: return "SwitchBot";
        case 0x180F: return "Battery";
        case 0x181A: return "EnvSense";
        case 0x1812: return "HID";
        case 0x180D: return "HeartRate";
        case 0x1816: return "Cycling";
        case 0x180A: return "DevInfo";
        default: break;
      }
    } else if (bs == 128) {
      if (u == TESLA_KEY) return "Tesla";
      if (u == NORDIC_UART) return "NordicUART";
      if (u == TI_SENSORTAG) return "TI SensorTag";
    }
  }
  return nullptr;
}
#endif  // CONFIG_NBP_DEV_DETAILS

void record_device(const NimBLEAdvertisedDevice *dev, uint64_t addr,
                   uint8_t addr_type) {
  // Parse outside the mutex — getName() walks the AD list.
  std::string nm = dev->getName();
  int8_t rssi = static_cast<int8_t>(dev->getRSSI());
#if CONFIG_NBP_DEV_DETAILS
  uint16_t app = dev->haveAppearance() ? dev->getAppearance() : 0;
  bool conn = dev->isConnectable();
  const char *tag = classify_device(dev);
#endif
  uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

  xSemaphoreTake(g_mutex, portMAX_DELAY);
  auto &row = g_devices[find_or_alloc_device(addr)];
  row.addr_type = addr_type;
  row.rssi = rssi;
  row.adv_count++;
  row.last_ms = now_ms;
#if CONFIG_NBP_DEV_DETAILS
  // Appearance comes typically from the scan response; once seen, a
  // later frame without it shouldn't wipe what we have. Connectable
  // and tag *can* shift between adv modes in principle but in practice
  // are stable — we record the most recent observation either way.
  if (app != 0) row.appearance = app;
  row.connectable = conn;
  if (tag) row.tag = tag;
#endif
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
#if CONFIG_NBP_DEVICES_PANEL
  TickType_t last_prune = xTaskGetTickCount();
  const TickType_t prune_period = pdMS_TO_TICKS(DEVICE_PRUNE_INTERVAL_MS);
#endif
  while (true) {
    // Wake on notify (batch full) OR after period (time-based flush).
    ulTaskNotifyTake(pdTRUE, period);
    flush_once();
#if CONFIG_NBP_DEVICES_PANEL
    // Sweep stale devices roughly once a minute (the flush wakes far more
    // often, so this just rate-limits the table scan onto the same task).
    TickType_t now = xTaskGetTickCount();
    if (now - last_prune >= prune_period) {
      prune_stale_devices();
      last_prune = now;
    }
#endif
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

#if CONFIG_NBP_BTHOME
    // BTHome v2: 16-bit service UUID 0xFCD2 in service-data AD field.
    // NimBLE returns an empty string when the field is absent.
    std::string svc = dev->getServiceData(NimBLEUUID(uint16_t{0xFCD2}));
    if (!svc.empty()) {
      ble_backend::bthome::ingest(
          addr_u64, reinterpret_cast<const uint8_t *>(svc.data()), svc.size());
    }
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
  // Default active (pulls scan responses for full names); runtime-mutable
  // via set_active() — atomic seeds the NimBLE-side flag below.
  g_scan->setActiveScan(g_active_scan.load(std::memory_order_relaxed));
  g_scan->setInterval(g_interval_ms.load(std::memory_order_relaxed));
  g_scan->setWindow(g_window_ms.load(std::memory_order_relaxed));
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
           g_interval_ms.load(std::memory_order_relaxed),
           g_window_ms.load(std::memory_order_relaxed));
}

void set_duty(uint16_t window_ms, uint16_t interval_ms) {
  if (!g_scan) return;
  // BLE scanner-window must be <= interval; caller already validates.
  g_scan->setInterval(interval_ms);
  g_scan->setWindow(window_ms);
  g_window_ms.store(window_ms, std::memory_order_relaxed);
  g_interval_ms.store(interval_ms, std::memory_order_relaxed);
  // setInterval/setWindow change the on-the-air timings on the next
  // scan epoch, but a stop+restart makes the change visible to peer
  // RSSI/age stats immediately and keeps the log line in sync.
  if (g_scan->isScanning()) {
    g_scan->stop();
    g_scan->start(0, /*isContinue=*/true);
  }
  ESP_LOGI(TAG, "scan duty -> window=%ums interval=%ums (%u%%)",
           window_ms, interval_ms,
           interval_ms > 0 ? (window_ms * 100u) / interval_ms : 0u);
}

void get_duty(uint16_t *window_ms, uint16_t *interval_ms) {
  if (window_ms)   *window_ms   = g_window_ms.load(std::memory_order_relaxed);
  if (interval_ms) *interval_ms = g_interval_ms.load(std::memory_order_relaxed);
}

void set_active(bool on) {
  if (!g_scan) return;
  g_active_scan.store(on, std::memory_order_relaxed);
  g_scan->setActiveScan(on);
  // setActiveScan affects the next scan epoch; stop+restart so the new
  // mode applies immediately, mirroring set_duty().
  if (g_scan->isScanning()) {
    g_scan->stop();
    g_scan->start(0, /*isContinue=*/true);
  }
  ESP_LOGI(TAG, "scan mode -> %s", on ? "active" : "passive");
}

bool get_active() {
  return g_active_scan.load(std::memory_order_relaxed);
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
