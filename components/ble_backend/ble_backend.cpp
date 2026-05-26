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

struct ble_gap_event_listener g_evt_listener;

int notify_listener_cb(struct ble_gap_event *event, void *arg) {
  if (event->type == BLE_GAP_EVENT_NOTIFY_RX) {
    g_notify_rx_total.fetch_add(1, std::memory_order_relaxed);
    g_last_notify_handle.store(event->notify_rx.attr_handle,
                               std::memory_order_relaxed);
    ESP_LOGI(TAG, "notify_rx conn=%u handle=%u len=%u indication=%u",
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

void start() {
  if (g_started) return;
  g_started = true;

  NimBLEDevice::init(proxy::HOSTNAME);
  // Slightly larger MTU than the 23-byte default so notification payloads
  // aren't capped at 20 B. Peers may still negotiate down.
  NimBLEDevice::setMTU(247);

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
