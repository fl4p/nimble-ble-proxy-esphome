#include "ble_backend.h"

#include "connection.h"
#include "proxy_config.h"
#include "scanner.h"

#include "NimBLEDevice.h"
#include "esp_log.h"
#include "host/ble_gap.h"

#include <atomic>

namespace ble_backend {

namespace {

constexpr const char *TAG = "ble";
bool g_started = false;

// Diagnostic: count BLE_GAP_EVENT_NOTIFY_RX events at the NimBLE host
// level, independent of NimBLE-Arduino's dispatch path. Lets us answer
// "is the peer sending notifies at all?" via /stats.json.
std::atomic<uint32_t> g_notify_rx_total{0};
std::atomic<uint16_t> g_last_notify_handle{0};

// Peripheral adv interval (0.625 ms units). 0 = use NimBLE host default
// (~30..60 ms range). Set via stats::apply_adv_interval_from_nvs at
// boot and via POST /advitvl at runtime.
std::atomic<uint16_t> g_adv_interval_units{0};

// Master enable for the device's own peripheral advertising. Default on.
// Flipped via the /advitvl config surface (ms=-1 = off) at boot and
// runtime; every adv start path checks advertising_enabled() first.
std::atomic<bool> g_adv_enabled{true};

struct ble_gap_event_listener g_evt_listener;

int notify_listener_cb(struct ble_gap_event *event, void *arg) {
  if (event->type == BLE_GAP_EVENT_NOTIFY_RX) {
    g_notify_rx_total.fetch_add(1, std::memory_order_relaxed);
    g_last_notify_handle.store(event->notify_rx.attr_handle,
                               std::memory_order_relaxed);
    // Downgraded to DEBUG once notify-RX was confirmed working end-to-end
    // (BMS bring-up, 2026-05-26). The counters above remain the ongoing
    // diagnostic surface — visible via /stats.json as notify_rx and
    // last_notify_handle. Bump to INFO temporarily by setting the "ble"
    // tag level if you need a per-packet log again.
    ESP_LOGD(TAG, "notify_rx conn=%u handle=%u len=%u indication=%u",
             event->notify_rx.conn_handle,
             event->notify_rx.attr_handle,
             event->notify_rx.om ? event->notify_rx.om->om_len : 0,
             event->notify_rx.indication);
  }
  // Listener is observe-only; return 0 to keep dispatching to other handlers.
  return 0;
}

}  // namespace

uint32_t notify_rx_total() {
  return g_notify_rx_total.load(std::memory_order_relaxed);
}

uint16_t last_notify_handle() {
  return g_last_notify_handle.load(std::memory_order_relaxed);
}

uint16_t adv_interval_units() {
  return g_adv_interval_units.load(std::memory_order_relaxed);
}

void set_adv_interval_ms(uint16_t ms) {
  // 0 = host default. Anything else gets clamped to BLE-spec bounds so
  // we never feed an HCI value the controller would reject (which would
  // leave us silently not advertising).
  uint16_t units = 0;
  if (ms != 0) {
    if (ms < 20) ms = 20;
    if (ms > 10240) ms = 10240;
    units = static_cast<uint16_t>(static_cast<uint32_t>(ms) * 1000u / 625u);
  }
  g_adv_interval_units.store(units, std::memory_order_relaxed);

  // Apply to the singleton. NimBLEDevice::getAdvertising() returns null
  // if init() hasn't run yet — early-boot apply lands here harmlessly
  // and ble_httpd::activate / clone start_advertising picks the value
  // up via adv_interval_units().
  auto *adv = NimBLEDevice::getAdvertising();
  if (adv == nullptr) return;
  if (units != 0) {
    adv->setMinInterval(units);
    adv->setMaxInterval(units);
  }
  // Hot-restart: if we're currently advertising, stop+start so the new
  // interval lands in the next HCI window. If not advertising, the
  // configured params will be used on the next start() call. Honor the
  // master switch so an interval change can't resurrect advertising that
  // the user disabled via POST /advitvl?ms=-1.
  if (adv->isAdvertising()) {
    adv->stop();
    if (g_adv_enabled.load(std::memory_order_relaxed)) adv->start();
  }
}

bool advertising_enabled() {
  return g_adv_enabled.load(std::memory_order_relaxed);
}

void set_advertising_enabled(bool on) {
  bool was = g_adv_enabled.exchange(on, std::memory_order_relaxed);

  // Pre-init (NimBLEDevice::getAdvertising() is null until start()): the
  // flag is stored and the gated start paths honor it once the host is up.
  auto *adv = NimBLEDevice::getAdvertising();
  if (adv == nullptr) return;

  if (!on) {
    // Kill switch: drop any in-flight advertising immediately.
    if (adv->isAdvertising()) adv->stop();
  } else if (!was) {
    // Re-enable after a disable. NimBLE keeps the last advertising payload
    // across stop(), so start() resumes the cloned/dashboard adv without
    // waiting for the next central reconnect to re-trigger it.
    if (!adv->isAdvertising()) adv->start();
  }
}

void start() {
  if (g_started) return;
  g_started = true;

  NimBLEDevice::init(proxy::hostname());
  // Slightly larger MTU than the 23-byte default so notification payloads
  // aren't capped at 20 B. Peers may still negotiate down.
  NimBLEDevice::setMTU(247);

#ifdef CONFIG_NBP_SMP
  // SMP defaults for peripherals that require pairing (Victron
  // SmartShunt, some BMS variants). Bond, MITM auth, Secure
  // Connections preferred. IO_CAP=KEYBOARD_ONLY tells the peer we'll
  // type the passkey it displays. The passkey itself is injected by
  // ClientCb::onPassKeyEntry. CONFIG_BT_NIMBLE_NVS_PERSIST=y stores
  // the resulting bond in NVS so subsequent connects skip pairing.
  NimBLEDevice::setSecurityAuth(/*bond=*/true,
                                /*mitm=*/true,
                                /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
#endif  // CONFIG_NBP_SMP

  scanner::init();
  connection::init();

  scanner::start();

  // Diagnostic listener for NOTIFY_RX events at the bare-metal NimBLE
  // host level. Multiple GAP listeners coexist; this runs in parallel
  // with NimBLE-Arduino's own dispatcher.
  int rc = ble_gap_event_listener_register(&g_evt_listener,
                                           &notify_listener_cb, nullptr);
  if (rc != 0) {
    ESP_LOGW(TAG, "ble_gap_event_listener_register rc=%d", rc);
  }

  ESP_LOGI(TAG, "NimBLE ready (max_conn=%u, scan=%ums/%ums)",
           proxy::MAX_CONNECTIONS, proxy::SCAN_WINDOW_MS,
           proxy::SCAN_INTERVAL_MS);
}

void on_api_client_disconnect() {
  scanner::stop_forwarding();
  connection::disconnect_all();
}

}  // namespace ble_backend
