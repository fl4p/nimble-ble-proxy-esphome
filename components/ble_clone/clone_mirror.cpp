#include "clone_mirror.h"

#ifdef CONFIG_NBP_CLONE

#include "ble_backend.h"
#include "clone_config.h"
#include "clone_upstream.h"

#include "NimBLEAdvertising.h"
#include "NimBLECharacteristic.h"
#include "host/ble_gap.h"    // ble_gap_adv_active for idempotent re-start
#include "host/ble_gatt.h"   // ble_gatts_find_svc, post-start sanity probe
#include "host/ble_hs_adv.h" // BLE_HS_ADV_TYPE_COMP_UUIDS128 for adv-fit check

#if CONFIG_NBP_BLE_HTTPD
#include "ble_httpd.h"
#endif
#include "NimBLEDescriptor.h"
#include "NimBLEDevice.h"
#include "NimBLERemoteCharacteristic.h"
#include "NimBLERemoteDescriptor.h"
#include "NimBLERemoteService.h"
#include "NimBLEServer.h"
#include "NimBLEService.h"
#include "NimBLEUUID.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <array>
#include <atomic>
#include <cstring>

namespace ble_clone::mirror {

namespace {

constexpr const char *TAG = "clone.mirror";

constexpr size_t MAX_CHARS = CONFIG_NBP_CLONE_MAX_CHARS;
constexpr size_t CACHE_BYTES = CONFIG_NBP_CLONE_VALUE_CACHE_BYTES;

// CCCD UUID = 0x2902. Don't auto-mirror it — NimBLE adds the CCCD itself
// when the characteristic has NOTIFY or INDICATE in its property mask.
// Mirroring it would create a duplicate descriptor and NimBLE rejects
// that. The 0x2901 (User Description) and others we forward as-is.
constexpr uint16_t CCCD_UUID16 = 0x2902;

struct MirrorChar {
  uint16_t upstream_handle = 0;          // upstream attribute handle (BLE)
  uint16_t upstream_props = 0;           // BLE_GATT_CHR_F_* mask
  NimBLECharacteristic *local = nullptr;
  NimBLERemoteCharacteristic *upstream = nullptr;  // null while disconnected
  uint8_t cache_len = 0;
  uint8_t cache[CACHE_BYTES] = {0};
  bool primed = false;
};

std::array<MirrorChar, MAX_CHARS> g_chars{};
size_t g_char_count = 0;

// Mutex guarding the MirrorChar table. Held briefly while updating
// upstream pointers / cache bytes; never across blocking BLE ops.
SemaphoreHandle_t g_mutex = nullptr;

NimBLEServer *g_server = nullptr;
NimBLEAdvertising *g_adv = nullptr;
std::atomic<bool> g_built{false};
std::atomic<bool> g_advertising{false};
std::atomic<uint8_t> g_connected_centrals{0};

std::atomic<uint16_t> g_service_count{0};
std::atomic<uint32_t> g_reads_served{0};
std::atomic<uint32_t> g_writes_proxied{0};
std::atomic<uint32_t> g_notifies_out{0};

// Local GATT server callbacks — bumps the connected-central counter so
// the dashboard can show it.
class ServerCb : public NimBLEServerCallbacks {
 public:
  void onConnect(NimBLEServer *, NimBLEConnInfo &info) override {
    g_connected_centrals.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGI(TAG, "local central connected, total=%u",
             g_connected_centrals.load(std::memory_order_relaxed));
    // Keep advertising so additional centrals can connect (multiplex).
    // NimBLE-CPP's start() logs 'Advertising already active' as a WARN
    // if adv is still running, so pre-check via the host API. Honor the
    // peripheral-advertising master switch (POST /advitvl?ms=-1 = off).
    if (g_adv != nullptr && ble_backend::advertising_enabled() &&
        !ble_gap_adv_active())
      g_adv->start();
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &info, int reason) override {
    uint8_t prev = g_connected_centrals.fetch_sub(1, std::memory_order_relaxed);
    ESP_LOGI(TAG, "local central disconnected reason=%d, remaining=%u",
             reason, prev > 0 ? prev - 1 : 0);
    if (g_adv != nullptr && ble_backend::advertising_enabled() &&
        !ble_gap_adv_active())
      g_adv->start();
  }
};
ServerCb g_server_cb;

// Per-characteristic local callbacks. onRead doesn't modify the cached
// value (NimBLE serves the bytes set by prime_cache/on_upstream_notify
// itself); it only bumps the counter exposed via mirror::stats() so the
// dashboard chart can fold clone reads in. Write captures the value and
// enqueues it to the upstream worker so the host task isn't blocked.
class CharCb : public NimBLECharacteristicCallbacks {
 public:
  void onRead(NimBLECharacteristic *, NimBLEConnInfo &) override {
    g_reads_served.fetch_add(1, std::memory_order_relaxed);
  }
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    // Locate the MirrorChar row for this local characteristic.
    MirrorChar *row = nullptr;
    NimBLERemoteCharacteristic *up = nullptr;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    for (size_t i = 0; i < g_char_count; ++i) {
      if (g_chars[i].local == chr) {
        row = &g_chars[i];
        up = row->upstream;
        break;
      }
    }
    xSemaphoreGive(g_mutex);
    if (row == nullptr) {
      ESP_LOGW(TAG, "onWrite: char not in mirror table");
      return;
    }

    auto val = chr->getValue();
    const uint8_t *data = val.data();
    size_t len = val.size();

    // Cache invalidate on write — next notify or refresh will repopulate.
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    row->cache_len = 0;
    row->primed = false;
    xSemaphoreGive(g_mutex);

    if (up == nullptr) {
      ESP_LOGW(TAG, "onWrite handle=%u: upstream disconnected, dropped",
               row->upstream_handle);
      return;
    }

    bool with_response = (row->upstream_props & BLE_GATT_CHR_F_WRITE) != 0;
    if (!upstream::enqueue_write(up, data, len, with_response)) {
      ESP_LOGW(TAG, "onWrite handle=%u: write queue full, dropped",
               row->upstream_handle);
      return;
    }
    g_writes_proxied.fetch_add(1, std::memory_order_relaxed);
  }
};
CharCb g_char_cb;

// Filter upstream properties down to what we can safely expose on the
// local mirror. Strip ENC/AUTHEN/AUTHOR variants — the local image
// doesn't enforce pairing, so a peer that requires auth upstream gets
// surfaced as a plain READ/WRITE locally. Notify the user, but that's
// the simplification we documented.
uint16_t sanitize_props(uint16_t up) {
  uint16_t out = 0;
  if (up & BLE_GATT_CHR_F_BROADCAST) out |= BLE_GATT_CHR_F_BROADCAST;
  if (up & (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
           BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_READ_AUTHOR)) {
    out |= BLE_GATT_CHR_F_READ;
  }
  if (up & (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
           BLE_GATT_CHR_F_WRITE_AUTHEN | BLE_GATT_CHR_F_WRITE_AUTHOR)) {
    out |= BLE_GATT_CHR_F_WRITE;
  }
  if (up & BLE_GATT_CHR_F_WRITE_NO_RSP) out |= BLE_GATT_CHR_F_WRITE_NO_RSP;
  if (up & BLE_GATT_CHR_F_NOTIFY) out |= BLE_GATT_CHR_F_NOTIFY;
  if (up & BLE_GATT_CHR_F_INDICATE) out |= BLE_GATT_CHR_F_INDICATE;
  return out;
}

// Reconstruct the BLE_GATT_CHR_F_* mask from the canX() helpers because
// NimBLE-CPP 2.5 doesn't expose getProperties() on RemoteCharacteristic.
uint16_t upstream_props_of(const NimBLERemoteCharacteristic *chr) {
  uint16_t p = 0;
  if (chr->canBroadcast()) p |= BLE_GATT_CHR_F_BROADCAST;
  if (chr->canRead()) p |= BLE_GATT_CHR_F_READ;
  if (chr->canWriteNoResponse()) p |= BLE_GATT_CHR_F_WRITE_NO_RSP;
  if (chr->canWrite()) p |= BLE_GATT_CHR_F_WRITE;
  if (chr->canNotify()) p |= BLE_GATT_CHR_F_NOTIFY;
  if (chr->canIndicate()) p |= BLE_GATT_CHR_F_INDICATE;
  return p;
}

}  // namespace

bool build_from(void *upstream_client_v) {
  if (g_built.load(std::memory_order_acquire)) {
    // GATT DB already registered — supervisor must call rebind_upstream
    // instead. Treat as success so reconnect flows work.
    ESP_LOGI(TAG, "build_from: already built, deferring to rebind");
    return rebind_upstream(upstream_client_v);
  }

  if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();

  auto *client = static_cast<NimBLEClient *>(upstream_client_v);
  if (client == nullptr) {
    ESP_LOGE(TAG, "build_from: null client");
    return false;
  }

  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(&g_server_cb, /*deleteCallbacks=*/false);

  // Walk upstream services → chars → descriptors and build a matching
  // local tree. The local UUIDs are copied verbatim so vendor apps that
  // hard-code UUIDs Just Work.
  const auto &services = client->getServices(/*refresh=*/false);
  ESP_LOGI(TAG, "building from %u upstream services",
           static_cast<unsigned>(services.size()));

  size_t total_chars = 0;
  size_t total_descs = 0;

  for (auto *svc : services) {
    NimBLEUUID svc_uuid = svc->getUUID();
    // Skip GAP (0x1800) and GATT (0x1801): NimBLE registers these
    // itself and rejects duplicates.
    if (svc_uuid.bitSize() == 16) {
      const uint8_t *v = svc_uuid.getValue();
      uint16_t u16 = static_cast<uint16_t>(v[0]) |
                     (static_cast<uint16_t>(v[1]) << 8);
      if (u16 == 0x1800 || u16 == 0x1801) {
        ESP_LOGI(TAG, "  skip standard service 0x%04x", u16);
        continue;
      }
    }
    NimBLEService *local_svc = g_server->createService(svc_uuid);
    if (local_svc == nullptr) {
      ESP_LOGW(TAG, "  createService failed for %s", svc_uuid.toString().c_str());
      continue;
    }
    g_service_count.fetch_add(1, std::memory_order_relaxed);

    const auto &chars = svc->getCharacteristics(/*refresh=*/false);
    for (auto *chr : chars) {
      if (g_char_count >= MAX_CHARS) {
        ESP_LOGW(TAG, "  hit MAX_CHARS=%u, truncating",
                 static_cast<unsigned>(MAX_CHARS));
        break;
      }
      uint16_t up_props = upstream_props_of(chr);
      uint16_t local_props = sanitize_props(up_props);
      NimBLEUUID chr_uuid = chr->getUUID();

      NimBLECharacteristic *local_chr =
          local_svc->createCharacteristic(chr_uuid, local_props);
      local_chr->setCallbacks(&g_char_cb);

      auto &row = g_chars[g_char_count++];
      row.upstream_handle = chr->getHandle();
      row.upstream_props = up_props;
      row.local = local_chr;
      row.upstream = chr;
      row.cache_len = 0;
      row.primed = false;

      // Don't mirror descriptors. The reference micropython clone.py
      // skips them too. Empirically, mirroring the 0x2901 User
      // Description descriptor without a value (we don't read its
      // upstream content) makes macOS CoreBluetooth fail the entire
      // service's characteristic discovery with "Unknown error". The
      // CCCD that NOTIFY/INDICATE chars need is auto-added by NimBLE.

      ESP_LOGD(TAG, "  char %s handle=%u props=0x%04x",
               chr_uuid.toString().c_str(), chr->getHandle(), up_props);
      ++total_chars;
    }
  }

  ESP_LOGI(TAG, "structure captured: %u chars, %u descriptors (server not "
                "started yet — needs idle host)",
           static_cast<unsigned>(total_chars),
           static_cast<unsigned>(total_descs));
  return true;
}

bool finalize_server() {
  if (g_server == nullptr) {
    ESP_LOGE(TAG, "finalize_server before build_from");
    return false;
  }
  if (g_built.load(std::memory_order_acquire)) return true;

  // Single server->start() for the session, with both ble_httpd's
  // dashboard service AND our cloned services in m_svcVec. Going
  // through ble_httpd::activate() ensures we don't fight with an
  // earlier start() in ble_httpd's own startup path (which would
  // lock the GATT DB and cause cloned chars to be added but never
  // registered).
#if CONFIG_NBP_BLE_HTTPD
  ble_httpd::activate();
#else
  if (!g_server->start()) {
    ESP_LOGE(TAG, "NimBLEServer::start() failed");
    return false;
  }
#endif
  g_built.store(true, std::memory_order_release);

  // Direct probe of NimBLE host's view: for each unique service in our
  // captured g_chars, ask the host whether it knows about it.
  // ble_gatts_find_svc returns BLE_HS_ENOENT (5) when the host doesn't.
  // Also probe the ble_httpd dashboard service if compiled in. Useful
  // because gattRegisterCallback may have silently skipped a service.
  {
    NimBLEService *seen[16] = {};
    size_t seen_n = 0;
    for (size_t i = 0; i < g_char_count; ++i) {
      auto *s = g_chars[i].local->getService();
      bool dup = false;
      for (size_t j = 0; j < seen_n; ++j) if (seen[j] == s) { dup = true; break; }
      if (dup || s == nullptr) continue;
      seen[seen_n++] = s;
      uint16_t h = 0;
      int rc = ble_gatts_find_svc(s->getUUID().getBase(), &h);
      ESP_LOGI(TAG, "  host_svc(clone) %s handle=%u rc=%d",
               s->getUUID().toString().c_str(), h, rc);
    }
#if CONFIG_NBP_BLE_HTTPD
    NimBLEUUID dash("6e627062-7072-7879-0001-000000000000");
    uint16_t h = 0;
    int rc = ble_gatts_find_svc(dash.getBase(), &h);
    ESP_LOGI(TAG, "  host_svc(dashboard) %s handle=%u rc=%d",
             dash.toString().c_str(), h, rc);
#endif
  }

  g_adv = NimBLEDevice::getAdvertising();
  // NOTE: service UUIDs + scan-response enable are deferred to
  // start_advertising() so the name lands in the *main* AD payload.
  // NimBLE-CPP's setName() routes into scan response when
  // m_scanResp=true was set first, and passive scanners (default on
  // macOS / iOS) never request scan responses — they'd then see only
  // flags + UUIDs and report a cached / GAP-default name instead.
  ESP_LOGI(TAG, "GATT DB registered (services=%u chars=%u)",
           g_service_count.load(std::memory_order_relaxed),
           static_cast<unsigned>(g_char_count));
  // Dump the assigned local handles + properties so we can correlate
  // with the upstream layout and with CoreBluetooth's "service N" errors.
  for (size_t i = 0; i < g_char_count; ++i) {
    auto *svc = g_chars[i].local->getService();
    ESP_LOGI(TAG, "  local[%u] svc=%s chr=%s val_handle=%u props=0x%04x",
             static_cast<unsigned>(i),
             svc ? svc->getUUID().toString().c_str() : "?",
             g_chars[i].local->getUUID().toString().c_str(),
             g_chars[i].local->getHandle(),
             g_chars[i].upstream_props);
  }
  return true;
}

bool rebind_upstream(void *upstream_client_v) {
  if (!g_built.load(std::memory_order_acquire)) return false;
  auto *client = static_cast<NimBLEClient *>(upstream_client_v);
  if (client == nullptr) return false;

  // Walk again and re-attach each MirrorChar to its (possibly new)
  // upstream NimBLERemoteCharacteristic*. Match by UUID — handles can
  // shift across discoverAttributes() refreshes.
  size_t rebound = 0;
  size_t missing = 0;
  const auto &services = client->getServices(/*refresh=*/false);
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (size_t i = 0; i < g_char_count; ++i) {
    auto &row = g_chars[i];
    NimBLEUUID svc_uuid = row.local->getService()->getUUID();
    NimBLEUUID chr_uuid = row.local->getUUID();
    row.upstream = nullptr;
    for (auto *svc : services) {
      if (!(svc->getUUID() == svc_uuid)) continue;
      for (auto *chr : svc->getCharacteristics(/*refresh=*/false)) {
        if (chr->getUUID() == chr_uuid) {
          row.upstream = chr;
          row.upstream_handle = chr->getHandle();
          ++rebound;
          goto next_row;
        }
      }
    }
    ++missing;
  next_row:;
  }
  xSemaphoreGive(g_mutex);

  ESP_LOGI(TAG, "rebind: %u/%u upstream chars matched, %u missing",
           static_cast<unsigned>(rebound),
           static_cast<unsigned>(g_char_count),
           static_cast<unsigned>(missing));
  return rebound > 0;
}

void disconnect_upstream() {
  if (!g_built.load(std::memory_order_acquire)) return;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (size_t i = 0; i < g_char_count; ++i) {
    g_chars[i].upstream = nullptr;
    g_chars[i].primed = false;
  }
  xSemaphoreGive(g_mutex);
}

void prime_cache(uint16_t upstream_handle, const uint8_t *data, size_t len) {
  if (g_mutex == nullptr) return;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (size_t i = 0; i < g_char_count; ++i) {
    if (g_chars[i].upstream_handle == upstream_handle) {
      size_t n = len < CACHE_BYTES ? len : CACHE_BYTES;
      std::memcpy(g_chars[i].cache, data, n);
      g_chars[i].cache_len = n;
      g_chars[i].primed = true;
      // Also push into the local NimBLE characteristic so reads from a
      // central return the value without going through any callback.
      if (g_chars[i].local != nullptr) {
        g_chars[i].local->setValue(data, n);
      }
      break;
    }
  }
  xSemaphoreGive(g_mutex);
}

void for_each_subscribable(
    void (*cb)(void *ctx, void *upstream_chr, uint16_t handle, bool indicate),
    void *ctx) {
  if (g_mutex == nullptr) return;
  // Snapshot under the mutex; iterate AFTER releasing. The callback
  // does a synchronous BLE subscribe that may take 100s of ms — holding
  // the mutex across it would block the NimBLE host task (which fields
  // upstream notifies via on_upstream_notify, also taking this mutex).
  struct Entry {
    void *chr;
    uint16_t handle;
    bool indicate;
  };
  std::array<Entry, MAX_CHARS> snap{};
  size_t n_snap = 0;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (size_t i = 0; i < g_char_count; ++i) {
    auto &row = g_chars[i];
    if (row.upstream == nullptr) continue;
    bool n = (row.upstream_props & BLE_GATT_CHR_F_NOTIFY) != 0;
    bool ind = (row.upstream_props & BLE_GATT_CHR_F_INDICATE) != 0;
    if (!n && !ind) continue;
    snap[n_snap++] =
        Entry{row.upstream, row.upstream_handle, /*indicate=*/(!n && ind)};
  }
  xSemaphoreGive(g_mutex);

  for (size_t i = 0; i < n_snap; ++i) {
    cb(ctx, snap[i].chr, snap[i].handle, snap[i].indicate);
  }
}

void for_each_readable(
    void (*cb)(void *ctx, void *upstream_chr, uint16_t handle), void *ctx) {
  if (g_mutex == nullptr) return;
  struct Entry {
    void *chr;
    uint16_t handle;
  };
  std::array<Entry, MAX_CHARS> snap{};
  size_t n_snap = 0;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (size_t i = 0; i < g_char_count; ++i) {
    auto &row = g_chars[i];
    if (row.upstream == nullptr) continue;
    if (!(row.upstream_props & BLE_GATT_CHR_F_READ)) continue;
    snap[n_snap++] = Entry{row.upstream, row.upstream_handle};
  }
  xSemaphoreGive(g_mutex);

  for (size_t i = 0; i < n_snap; ++i) {
    cb(ctx, snap[i].chr, snap[i].handle);
  }
}

void start_advertising(const char *base_name) {
  if (!g_built.load(std::memory_order_acquire)) return;
  if (g_adv == nullptr) g_adv = NimBLEDevice::getAdvertising();

  config::Target t = config::snapshot();
  // Adv name = base + suffix, truncated so flags + name AD fit in the
  // 31-byte legacy adv payload: flags(3B) + name AD header(2B) = 5B, so
  // name ≤ 26 chars. NimBLE's setName logs 'Data length exceeded' and
  // drops the name entirely if we go over.
  char name[32] = {0};
  std::snprintf(name, sizeof(name), "%s%s", base_name ? base_name : "clone",
                t.name_suffix);
  name[26] = 0;

  // NimBLEDevice was init'd with proxy::hostname() — override the adv
  // name so vendor apps find the cloned device by the expected name.
  // (resetGATT()/ble_svc_gap_init() in finalize_server resets the GAP
  // 0x2A00 char back to the Kconfig default, so this call has to come
  // AFTER the server is registered.)
  NimBLEDevice::setDeviceName(name);

  // Clean slate so we don't inherit stale fields (e.g. an earlier
  // start_advertising attempt before name was known).
  g_adv->reset();

  // reset() wipes the advertising-interval params back to "use host
  // default" (NimBLEAdvertising::reset() reassigns to a default-
  // constructed object). Re-apply the user override here so the value
  // configured via POST /advitvl survives every clone reconnect cycle.
  uint16_t adv_units = ble_backend::adv_interval_units();
  if (adv_units != 0) {
    g_adv->setMinInterval(adv_units);
    g_adv->setMaxInterval(adv_units);
  }

  // Name in main payload (so passive scanners see it).
  g_adv->setName(name);
  g_adv->enableScanResponse(true);

  // NimBLEAdvertising::addServiceUUID tries the main payload first and
  // logs 'data length exceeded' when the UUID doesn't fit there, even
  // when it then successfully falls back to scan response. To keep the
  // log clean we build the scan-response payload directly on a local
  // NimBLEAdvertisementData (which has no fallback path) and decide per
  // UUID whether to add it to main or scan based on an exact fit check.
  // A UUID of B bytes costs B+2 bytes as a new AD record, or B bytes
  // coalesced into an existing same-type record.
  auto ad_type_for = [](uint8_t bytes) {
    return bytes == 2  ? BLE_HS_ADV_TYPE_COMP_UUIDS16
         : bytes == 4  ? BLE_HS_ADV_TYPE_COMP_UUIDS32
                       : BLE_HS_ADV_TYPE_COMP_UUIDS128;
  };
  auto fits = [&](const NimBLEAdvertisementData &d, uint8_t bytes) {
    int type = ad_type_for(bytes);
    size_t need = (d.getDataLocation(type) != -1) ? bytes : bytes + 2;
    return d.getPayload().size() + need <= 31;
  };

  NimBLEAdvertisementData scan_rsp;

  // Cloned upstream UUIDs first — vendor apps that filter by them need
  // them advertised. Dedup by NimBLEService pointer (every char of the
  // same service returns the same pointer from getService()).
  NimBLEService *seen[16] = {};
  size_t seen_n = 0;
  for (size_t i = 0; i < g_char_count; ++i) {
    auto *svc = g_chars[i].local->getService();
    if (svc == nullptr) continue;
    bool dup = false;
    for (size_t j = 0; j < seen_n; ++j) {
      if (seen[j] == svc) { dup = true; break; }
    }
    if (dup) continue;
    if (seen_n < sizeof(seen) / sizeof(seen[0])) seen[seen_n++] = svc;
    auto uuid = svc->getUUID();
    uint8_t bytes = uuid.bitSize() / 8;
    if (bytes != 2 && bytes != 4 && bytes != 16) continue;
    // Prefer main only when it has room; otherwise go directly to scan
    // response (avoiding NimBLE's main-first error log).
    if (fits(g_adv->getAdvertisementData(), bytes)) {
      g_adv->addServiceUUID(uuid);
    } else if (fits(scan_rsp, bytes)) {
      scan_rsp.addServiceUUID(uuid);
    } else {
      ESP_LOGI(TAG, "adv budget full, dropping cloned UUID");
    }
  }

#if CONFIG_NBP_BLE_HTTPD
  // Dashboard UUID is best-effort. Try main first, then scan, else skip.
  NimBLEUUID dash("6e627062-7072-7879-0001-000000000000");
  if (fits(g_adv->getAdvertisementData(), 16)) {
    g_adv->addServiceUUID(dash);
  } else if (fits(scan_rsp, 16)) {
    scan_rsp.addServiceUUID(dash);
  } else {
    ESP_LOGI(TAG,
             "skipping dashboard UUID in adv: budget full "
             "(picker can still filter by name)");
  }
#endif

  if (scan_rsp.getPayload().size() > 0) {
    g_adv->setScanResponseData(scan_rsp);
  }

  // Payload is fully prepared above; only begin broadcasting if the
  // peripheral-advertising master switch is on. When off, re-enabling via
  // POST /advitvl?ms=0 resumes this same payload without a clone reconnect.
  if (!ble_backend::advertising_enabled()) {
    ESP_LOGI(TAG,
             "advertising disabled (POST /advitvl?ms=0 to enable); "
             "payload prepared for '%s'",
             name);
    return;
  }

  if (g_adv->start()) {
    g_advertising.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "advertising as '%s'", name);
  } else {
    ESP_LOGW(TAG, "advertising start failed");
  }
}

void stop_advertising() {
  if (g_adv == nullptr) return;
  g_adv->stop();
  g_advertising.store(false, std::memory_order_release);
  ESP_LOGI(TAG, "advertising stopped");
}

void on_upstream_notify(uint16_t upstream_value_handle, const uint8_t *data,
                        size_t len) {
  if (!g_built.load(std::memory_order_acquire)) return;
  if (g_mutex == nullptr) return;

  NimBLECharacteristic *local = nullptr;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (size_t i = 0; i < g_char_count; ++i) {
    if (g_chars[i].upstream_handle == upstream_value_handle) {
      size_t n = len < CACHE_BYTES ? len : CACHE_BYTES;
      std::memcpy(g_chars[i].cache, data, n);
      g_chars[i].cache_len = n;
      g_chars[i].primed = true;
      local = g_chars[i].local;
      break;
    }
  }
  xSemaphoreGive(g_mutex);

  if (local != nullptr) {
    local->setValue(data, len);
    local->notify(data, len);
    g_notifies_out.fetch_add(1, std::memory_order_relaxed);
  }
}

Stats stats() {
  Stats s{};
  s.services = g_service_count.load(std::memory_order_relaxed);
  s.characteristics = static_cast<uint16_t>(g_char_count);
  s.reads_served_from_cache = g_reads_served.load(std::memory_order_relaxed);
  s.writes_proxied = g_writes_proxied.load(std::memory_order_relaxed);
  s.notifies_out = g_notifies_out.load(std::memory_order_relaxed);
  s.connected_centrals = g_connected_centrals.load(std::memory_order_relaxed);
  s.advertising = g_advertising.load(std::memory_order_relaxed);
  return s;
}

}  // namespace ble_clone::mirror

#endif  // CONFIG_NBP_CLONE
