#include "ble_backend.h"

#include "connection.h"
#include "proxy_config.h"
#include "scanner.h"

#include "NimBLEDevice.h"
#include "esp_log.h"

namespace ble_backend {

namespace {

constexpr const char *TAG = "ble";
bool g_started = false;

}  // namespace

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

  ESP_LOGI(TAG, "NimBLE ready (max_conn=%u, scan=%ums/%ums)",
           proxy::MAX_CONNECTIONS, proxy::SCAN_WINDOW_MS,
           proxy::SCAN_INTERVAL_MS);
}

void on_api_client_disconnect() {
  scanner::stop_forwarding();
  connection::disconnect_all();
}

}  // namespace ble_backend
