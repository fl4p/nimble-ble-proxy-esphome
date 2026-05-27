#include "clone_upstream.h"

#ifdef CONFIG_NBP_CLONE

#include "clone_config.h"
#include "clone_mirror.h"

#include "NimBLEAddress.h"
#include "NimBLEAdvertisedDevice.h"
#include "NimBLEClient.h"
#include "NimBLEDevice.h"
#include "NimBLERemoteCharacteristic.h"
#include "NimBLERemoteService.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#include "scanner.h"  // pause/resume around upstream connect+finalize
#ifdef CONFIG_NBP_SMP
#include "ble_backend.h"
#include "connection.h"  // for connection::get_passkey()
#endif

#include "host/ble_gap.h"  // ble_gap_*_active for idle-wait poll
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <atomic>
#include <cstring>

namespace ble_clone::upstream {

namespace {

constexpr const char *TAG = "clone.up";

constexpr size_t WRITE_QUEUE_DEPTH = 8;
constexpr size_t WRITE_DATA_BYTES = CONFIG_NBP_CLONE_VALUE_CACHE_BYTES;
// 12K — discoverAttributes recurses through services/chars/descriptors
// and NimBLE-CPP's vector + std::string ops on the call stack. The 6K
// default crashed on fugu-fry mid-discovery.
constexpr uint32_t SUPERVISOR_STACK = 12288;
constexpr UBaseType_t SUPERVISOR_PRIO = 4;
constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t INITIAL_BACKOFF_MS = 1000;
constexpr uint32_t MAX_BACKOFF_MS = 30000;

struct WriteRequest {
  void *upstream_chr;  // NimBLERemoteCharacteristic*
  uint16_t len;
  bool with_response;
  uint8_t data[WRITE_DATA_BYTES];
};

QueueHandle_t g_write_queue = nullptr;
TaskHandle_t g_supervisor_task = nullptr;
NimBLEClient *g_client = nullptr;

std::atomic<State> g_state{State::Idle};
std::atomic<uint64_t> g_address{0};
std::atomic<uint16_t> g_mtu{0};
std::atomic<uint32_t> g_reconnects{0};
std::atomic<uint32_t> g_last_disconnect_reason{0};
std::atomic<uint32_t> g_notifies_seen{0};
std::atomic<uint32_t> g_writes_drained{0};
std::atomic<uint32_t> g_writes_dropped{0};

bool g_mirror_built = false;

// Per-notify dispatch — passes the upstream char's value handle to the
// mirror, which fans out to all subscribed local centrals.
void on_upstream_notify(NimBLERemoteCharacteristic *chr, uint8_t *data,
                        size_t len, bool /*isNotify*/) {
  g_notifies_seen.fetch_add(1, std::memory_order_relaxed);
  if (chr == nullptr || data == nullptr) return;
  mirror::on_upstream_notify(chr->getHandle(), data, len);
}

// Async connect resolution: the supervisor issues the connect, then
// blocks on g_connect_sem until onConnect/onConnectFail fires (or timeout).
// Result lands in g_connect_ok.
SemaphoreHandle_t g_connect_sem = nullptr;
std::atomic<bool> g_connect_ok{false};

// Same pattern for disconnect: the supervisor needs to know when the
// link is fully torn down at the host level (not just at the user-API
// level) so ble_gatts_mutable flips to true before we call
// finalize_server. Signalled from onDisconnect.
SemaphoreHandle_t g_disconnect_sem = nullptr;

class ClientCb : public NimBLEClientCallbacks {
 public:
#ifdef CONFIG_NBP_SMP
  // Victron SmartShunt et al require an encrypted link before they'll
  // serve GATT characteristic discovery. They send a pairing request
  // with DisplayOnly IO; we respond with the stored passkey (default
  // 123456, runtime-mutable via POST /clone?passkey=NNNNNN). Same flow
  // as ble_backend::connection::ClientCb.
  void onPassKeyEntry(NimBLEConnInfo &connInfo) override {
    uint32_t pin = ble_backend::connection::get_passkey();
    ESP_LOGI(TAG, "onPassKeyEntry -> injecting passkey %06lu",
             static_cast<unsigned long>(pin));
    NimBLEDevice::injectPassKey(connInfo, pin);
  }
  void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
    ESP_LOGI(TAG,
             "onAuthenticationComplete enc=%d auth=%d bonded=%d key_size=%u",
             connInfo.isEncrypted(), connInfo.isAuthenticated(),
             connInfo.isBonded(), connInfo.getSecKeySize());
  }
#endif  // CONFIG_NBP_SMP

  void onConnect(NimBLEClient *c) override {
    ESP_LOGI(TAG, "onConnect mtu=%u", c->getMTU());
    g_connect_ok.store(true, std::memory_order_relaxed);
    if (g_connect_sem) xSemaphoreGive(g_connect_sem);
  }
  void onConnectFail(NimBLEClient *, int reason) override {
    ESP_LOGW(TAG, "onConnectFail reason=%d", reason);
    g_connect_ok.store(false, std::memory_order_relaxed);
    g_last_disconnect_reason.store(static_cast<uint32_t>(reason),
                                   std::memory_order_relaxed);
    if (g_connect_sem) xSemaphoreGive(g_connect_sem);
  }
  void onDisconnect(NimBLEClient *, int reason) override {
    g_last_disconnect_reason.store(static_cast<uint32_t>(reason),
                                   std::memory_order_relaxed);
    ESP_LOGI(TAG, "onDisconnect reason=%d", reason);
    mirror::disconnect_upstream();
    g_state.store(State::Reconnecting, std::memory_order_relaxed);
    if (g_disconnect_sem) xSemaphoreGive(g_disconnect_sem);
  }
};
ClientCb g_client_cb;

void prime_one_read(void *ctx, void *upstream_chr_v, uint16_t handle) {
  (void)ctx;
  auto *chr = static_cast<NimBLERemoteCharacteristic *>(upstream_chr_v);
  if (chr == nullptr) return;
  NimBLEAttValue v = chr->readValue();
  if (v.size() == 0) return;
  mirror::prime_cache(handle, v.data(), v.size());
}

void subscribe_one(void *ctx, void *upstream_chr_v, uint16_t handle,
                   bool indicate) {
  (void)ctx;
  (void)handle;
  auto *chr = static_cast<NimBLERemoteCharacteristic *>(upstream_chr_v);
  if (chr == nullptr) return;
  // NimBLE's per-char subscribe takes (notifications=true|false), where
  // true means NOTIFY and false means INDICATE. The wrapper unifies
  // both into a CCCD write under the hood.
  bool ok = chr->subscribe(/*notifications=*/!indicate,
                           &on_upstream_notify,
                           /*response=*/true);
  if (!ok) {
    ESP_LOGW(TAG, "subscribe failed for handle=%u indicate=%d",
             chr->getHandle(), indicate);
  }
}

// Try once to connect+discover+(build or rebind). Returns true if
// upstream is Ready, false on any failure (caller backs off).
bool try_session(uint64_t address, uint8_t address_type) {
  if (g_client == nullptr) {
    g_client = NimBLEDevice::createClient();
    if (g_client == nullptr) {
      ESP_LOGE(TAG, "createClient returned null (max clients reached?)");
      return false;
    }
    g_client->setClientCallbacks(&g_client_cb, /*deleteCallbacks=*/false);
    g_client->setConnectTimeout(CONNECT_TIMEOUT_MS);
  }

  g_state.store(State::Connecting, std::memory_order_relaxed);
  NimBLEAddress addr(address, address_type);
  ESP_LOGI(TAG, "connecting %012llx type=%u",
           static_cast<unsigned long long>(address), address_type);

  // Async connect — sync mode in NimBLE-CPP has no timeout for the GAP
  // procedure and blocks forever when the peer is unresponsive (verified
  // against fugu-fry post-reconnect-cycle). Issue the connect, then wait
  // on the ClientCb semaphore with our own bounded timeout.
  if (g_connect_sem == nullptr) g_connect_sem = xSemaphoreCreateBinary();
  // Drain any stale signal from a previous attempt.
  xSemaphoreTake(g_connect_sem, 0);
  g_connect_ok.store(false, std::memory_order_relaxed);
  // Bonded peers immediately push notifications on reconnect (because
  // their CCCD subscription state survives across sessions). Those
  // notifies can land before discoverAttributes finishes rebuilding the
  // client-side char cache, and NimBLE-CPP logs each one as
  // 'NimBLEClient: unknown handle: N'. Suppress that NimBLE log only
  // around the connect→discover window — restored before we proceed.
  esp_log_level_set("NimBLEClient", ESP_LOG_ERROR);
  if (!g_client->connect(addr, /*deleteAttibutes=*/true,
                         /*asyncConnect=*/true,
                         /*exchangeMTU=*/true)) {
    esp_log_level_set("NimBLEClient", ESP_LOG_WARN);
    ESP_LOGW(TAG, "connect (async) returned false — controller busy?");
    return false;
  }
  if (xSemaphoreTake(g_connect_sem, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) !=
      pdTRUE) {
    esp_log_level_set("NimBLEClient", ESP_LOG_WARN);
    ESP_LOGW(TAG, "connect timeout after %ums", CONNECT_TIMEOUT_MS);
    g_client->disconnect();
    return false;
  }
  if (!g_connect_ok.load(std::memory_order_relaxed)) {
    esp_log_level_set("NimBLEClient", ESP_LOG_WARN);
    ESP_LOGW(TAG, "connect failed (onConnectFail)");
    return false;
  }
  g_mtu.store(g_client->getMTU(), std::memory_order_relaxed);
  ESP_LOGI(TAG, "connected mtu=%u", g_mtu.load(std::memory_order_relaxed));
  // NimBLE auto-stops the scan when a central initiates a connect.
  // Resume it so the dashboard's device table and the api_server's
  // raw-adv forwarding keep working while clone holds an upstream link.
  ble_backend::scanner::resume();

  g_state.store(State::Discovering, std::memory_order_relaxed);
  ESP_LOGI(TAG, "starting discoverAttributes; heap=%u",
           static_cast<unsigned>(esp_get_free_heap_size()));
  if (!g_client->discoverAttributes()) {
    esp_log_level_set("NimBLEClient", ESP_LOG_WARN);
    ESP_LOGW(TAG, "discoverAttributes failed");
    g_client->disconnect();
    return false;
  }
  esp_log_level_set("NimBLEClient", ESP_LOG_WARN);
  ESP_LOGI(TAG, "discoverAttributes ok; heap=%u",
           static_cast<unsigned>(esp_get_free_heap_size()));

  if (!g_mirror_built) {
    // First-time: extract the GATT structure (UUIDs / handles / props /
    // descriptor UUIDs) from the discovered upstream attribute table.
    // build_from() captures everything it needs into MirrorChar rows
    // BEFORE we register the local GATT server — because
    // ble_gatts_add_svcs (invoked by NimBLEServer::start) returns
    // BLE_HS_EBUSY while any BLE connection is up. Verified by the
    // SYSINIT_PANIC_ASSERT at ble_svc_gap.c:395 during Victron bring-up.
    // So: extract, drop the upstream link, register the server, reconnect.
    if (!mirror::build_from(g_client)) {
      ESP_LOGW(TAG, "mirror build_from failed");
      g_client->disconnect();
      return false;
    }
    ESP_LOGI(TAG, "structure captured; disconnecting upstream so the host "
                  "is mutable for ble_gatts_start");
    if (g_disconnect_sem == nullptr) {
      g_disconnect_sem = xSemaphoreCreateBinary();
    }
    xSemaphoreTake(g_disconnect_sem, 0);  // drain stale signal
    // Stop the scanner BEFORE the disconnect. ble_gatts_mutable() (the
    // gate inside ble_gatts_reset / ble_gatts_add_svcs) returns false
    // when ANY GAP procedure is active — adv, scan, or connect. With
    // active scan running through the disconnect window NimBLE's
    // master FSM can still be in BLE_GAP_OP_M_DISC when we call
    // server->start, and resetGATT silently fails (rc discarded by
    // NimBLE-CPP), causing the next ble_svc_gap_init to panic at
    // ble_svc_gap.c:395. Cross-ref clone.md §13.1.
    ble_backend::scanner::pause();
    mirror::disconnect_upstream();        // null out the stale RemoteChar* refs
    g_client->disconnect();
    // Wait for the actual onDisconnect callback — that's when the host
    // sees the link as gone and ble_gatts_mutable() returns true.
    // isConnected() flips earlier (when the API-level disconnect is
    // accepted) but BLE_GATT_OP / ble_att_svr_reset hasn't completed yet.
    if (xSemaphoreTake(g_disconnect_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
      ESP_LOGW(TAG, "disconnect didn't complete within 3s");
      ble_backend::scanner::resume();
      return false;
    }
    // Poll the host until every GAP role reports idle. ble_gatts_mutable
    // (the gate inside ble_gatts_reset / ble_gatts_add_svcs) also checks
    // that there are no active conns; the active-conn list is internal
    // to NimBLE so we don't probe it directly, but the GAP-procedure
    // bits clear once the disconnect event has been processed by the
    // host. ~200 ms cap is generous: the actual transition is microseconds.
    {
      const TickType_t deadline =
          xTaskGetTickCount() + pdMS_TO_TICKS(200);
      while (xTaskGetTickCount() < deadline) {
        if (!ble_gap_adv_active() && !ble_gap_disc_active() &&
            !ble_gap_conn_active()) {
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      ESP_LOGI(TAG, "host idle: adv=%d disc=%d conn=%d",
               ble_gap_adv_active(), ble_gap_disc_active(),
               ble_gap_conn_active());
    }

    if (!mirror::finalize_server()) {
      ESP_LOGW(TAG, "mirror finalize_server (ble_gatts_start) failed");
      ble_backend::scanner::resume();
      return false;
    }
    g_mirror_built = true;

    // Reconnect for the steady-state session: re-discover, rebind, prime,
    // subscribe. Same 'unknown handle' suppression as the initial
    // connect — bonded notifies race the cache rebuild here too.
    ESP_LOGI(TAG, "reconnecting upstream for steady-state session");
    xSemaphoreTake(g_connect_sem, 0);
    g_connect_ok.store(false, std::memory_order_relaxed);
    esp_log_level_set("NimBLEClient", ESP_LOG_ERROR);
    if (!g_client->connect(addr, /*deleteAttibutes=*/true,
                           /*asyncConnect=*/true,
                           /*exchangeMTU=*/true)) {
      esp_log_level_set("NimBLEClient", ESP_LOG_WARN);
      ESP_LOGW(TAG, "reconnect (async) returned false");
      return false;
    }
    if (xSemaphoreTake(g_connect_sem, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) !=
            pdTRUE ||
        !g_connect_ok.load(std::memory_order_relaxed)) {
      esp_log_level_set("NimBLEClient", ESP_LOG_WARN);
      ESP_LOGW(TAG, "reconnect timeout/fail");
      return false;
    }
    g_mtu.store(g_client->getMTU(), std::memory_order_relaxed);
    ESP_LOGI(TAG, "reconnected mtu=%u",
             g_mtu.load(std::memory_order_relaxed));
    ble_backend::scanner::resume();
    if (!g_client->discoverAttributes()) {
      esp_log_level_set("NimBLEClient", ESP_LOG_WARN);
      ESP_LOGW(TAG, "post-rebuild discoverAttributes failed");
      g_client->disconnect();
      return false;
    }
    esp_log_level_set("NimBLEClient", ESP_LOG_WARN);
  }

  if (!mirror::rebind_upstream(g_client)) {
    ESP_LOGW(TAG, "mirror rebind_upstream failed");
    g_client->disconnect();
    return false;
  }

  // Prime read caches for all readable chars so the first local read
  // returns real bytes instead of an empty value.
  mirror::for_each_readable(&prime_one_read, nullptr);
  // Subscribe to every NOTIFY/INDICATE upstream char so notifies start
  // flowing immediately — same approach as the micropython clone.py.
  mirror::for_each_subscribable(&subscribe_one, nullptr);

  // Idempotent — NimBLEAdvertising::start() is a no-op if already
  // advertising, so calling on every successful session works for both
  // first-time bring-up and reconnect.
  if (!mirror::stats().advertising) {
    std::string up_name;
    auto *gap = g_client->getService(NimBLEUUID(static_cast<uint16_t>(0x1800)));
    if (gap != nullptr) {
      auto *name_chr =
          gap->getCharacteristic(NimBLEUUID(static_cast<uint16_t>(0x2a00)));
      if (name_chr != nullptr) {
        NimBLEAttValue v = name_chr->readValue();
        if (v.size() > 0) {
          up_name.assign(reinterpret_cast<const char *>(v.data()), v.size());
        }
      }
    }
    if (up_name.empty()) up_name = "clone";
    mirror::start_advertising(up_name.c_str());
  } else {
    g_reconnects.fetch_add(1, std::memory_order_relaxed);
  }

  g_state.store(State::Ready, std::memory_order_relaxed);
  return true;
}

void drain_writes_until(TickType_t until) {
  WriteRequest req;
  while (xTaskGetTickCount() < until) {
    TickType_t remaining = until - xTaskGetTickCount();
    if (xQueueReceive(g_write_queue, &req, remaining) != pdTRUE) break;
    auto *chr = static_cast<NimBLERemoteCharacteristic *>(req.upstream_chr);
    if (chr == nullptr) continue;
    bool ok = chr->writeValue(req.data, req.len, req.with_response);
    if (ok) {
      g_writes_drained.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_writes_dropped.fetch_add(1, std::memory_order_relaxed);
      ESP_LOGW(TAG, "upstream writeValue failed handle=%u len=%u",
               chr->getHandle(), req.len);
    }
  }
}

void supervisor_task(void *) {
  ESP_LOGI(TAG, "supervisor started");
  uint32_t backoff_ms = INITIAL_BACKOFF_MS;

  while (true) {
    config::Target target = config::snapshot();

    if (!target.enabled || target.address == 0) {
      g_state.store(State::Disabled, std::memory_order_relaxed);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    g_address.store(target.address, std::memory_order_relaxed);

    bool was_ready = (g_state.load(std::memory_order_relaxed) == State::Ready);
    if (!was_ready) {
      g_state.store(State::Scanning, std::memory_order_relaxed);
      bool ok = try_session(target.address, target.address_type);
      if (!ok) {
        ESP_LOGW(TAG, "session attempt failed; backoff %ums",
                 static_cast<unsigned>(backoff_ms));
        // Drain any queued writes during backoff to prevent stale ops
        // piling up while the link is down (they'll fail at write time
        // since chr->writeValue checks connected state).
        TickType_t until =
            xTaskGetTickCount() + pdMS_TO_TICKS(backoff_ms);
        drain_writes_until(until);
        backoff_ms = backoff_ms < MAX_BACKOFF_MS ? backoff_ms * 2 : MAX_BACKOFF_MS;
        continue;
      }
      backoff_ms = INITIAL_BACKOFF_MS;
    }

    // Ready: pump the write queue. On disconnect, ClientCb flips
    // g_state to Reconnecting and we loop back to try_session.
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    drain_writes_until(until);
  }
}

}  // namespace

void init() {
  if (g_write_queue == nullptr) {
    g_write_queue = xQueueCreate(WRITE_QUEUE_DEPTH, sizeof(WriteRequest));
  }
}

void start() {
  if (g_supervisor_task != nullptr) return;
  if (g_write_queue == nullptr) init();
  xTaskCreate(&supervisor_task, "clone_sup", SUPERVISOR_STACK, nullptr,
              SUPERVISOR_PRIO, &g_supervisor_task);
}

Status status() {
  Status s{};
  s.state = g_state.load(std::memory_order_relaxed);
  s.address = g_address.load(std::memory_order_relaxed);
  s.mtu = g_mtu.load(std::memory_order_relaxed);
  s.reconnects = g_reconnects.load(std::memory_order_relaxed);
  s.last_disconnect_reason =
      g_last_disconnect_reason.load(std::memory_order_relaxed);
  s.notifies_seen = g_notifies_seen.load(std::memory_order_relaxed);
  s.writes_drained = g_writes_drained.load(std::memory_order_relaxed);
  s.writes_dropped = g_writes_dropped.load(std::memory_order_relaxed);
  return s;
}

bool enqueue_write(void *upstream_chr, const uint8_t *data, size_t len,
                   bool with_response) {
  if (g_write_queue == nullptr) return false;
  if (upstream_chr == nullptr || data == nullptr) return false;
  if (len == 0 || len > WRITE_DATA_BYTES) {
    ESP_LOGW(TAG, "enqueue_write: len=%u out of range [1..%u]",
             static_cast<unsigned>(len),
             static_cast<unsigned>(WRITE_DATA_BYTES));
    return false;
  }
  WriteRequest req{};
  req.upstream_chr = upstream_chr;
  req.len = static_cast<uint16_t>(len);
  req.with_response = with_response;
  std::memcpy(req.data, data, len);
  if (xQueueSend(g_write_queue, &req, 0) != pdTRUE) {
    g_writes_dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

}  // namespace ble_clone::upstream

#endif  // CONFIG_NBP_CLONE
