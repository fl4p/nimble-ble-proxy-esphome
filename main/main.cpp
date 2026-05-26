// Boot sequence: NVS → WiFi STA → mDNS → API server → NimBLE backend.
// Each subsystem is a separate IDF component; main just wires them together.

#include "ble_backend.h"
#include "proxy_config.h"
#include "stats.h"

#if CONFIG_NBP_WIFI
#include "api_server.h"
#include "mdns_announce.h"
#include "ota.h"
#include "publish.h"
#include "wifi_sta.h"
#endif

#if CONFIG_NBP_CLONE
#include "clone.h"
#include "clone_config.h"
#endif

#if CONFIG_NBP_BLE_HTTPD
#include "ble_httpd.h"
#endif

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

namespace {
constexpr const char *TAG = "main";
}

extern "C" void app_main() {
#if CONFIG_NBP_WEB_CONSOLE
  // First thing: tee esp_log into the in-memory ring so the web console
  // captures NVS / WiFi / mDNS init lines too. UART output continues.
  api_server::stats::install_log_hook();
#endif

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  ESP_LOGI(TAG, "nimble-ble-proxy %s booting", proxy::VERSION);

  // NVS is up — apply the persisted NimBLE log level before any
  // NimBLE component initialises so it takes effect from the first
  // scan callback.
  api_server::stats::apply_log_overrides_from_nvs();

  // CPU frequency override needs esp_pm and NVS, both available now.
  // Apply before WiFi/BLE init so those subsystems run at the chosen
  // clock from the start.
  api_server::stats::apply_cpu_freq_from_nvs();

#if CONFIG_NBP_WIFI
  wifi_sta::start_and_wait_for_ip();
  mdns_announce::start();
  ota::start();
  // Piggyback the stats UI on the OTA httpd so we don't burn an extra
  // LWIP socket budget on a second listener.
  api_server::stats::register_endpoints(ota::handle());
  // Wire the ble_backend → api_server publish hook before either starts
  // accepting traffic. Order between start() calls doesn't matter as
  // long as install() happens before any adv callback fires.
  ble_backend::publish::install(&api_server::send_async,
                                &api_server::has_active_client);
#endif
  ble_backend::start();
#if CONFIG_NBP_WIFI
  api_server::start();
#endif

#if CONFIG_NBP_BLE_HTTPD
  // Register the BLE peripheral GATT service after NimBLE host init
  // (done by ble_backend::start). Must run before any other component
  // calls NimBLEServer::start() that finalises the attribute table —
  // currently the only other peripheral GATT is ble_clone, which has
  // its own one-shot init triggered by upstream discovery.
  ble_httpd::start();
#endif

#if CONFIG_NBP_CLONE
  // Clone supervisor runs alongside the ESPHome-style scanner/proxy.
  // load() reads target MAC from NVS; init() spawns the supervisor task
  // that scans → connects → discovers → builds the local GATT mirror →
  // begins advertising. register_endpoints() exposes /clone for runtime
  // config changes (target MAC, enable/disable) — same pattern as
  // /passkey under CONFIG_NBP_SMP.
  ble_clone::config::load();
  ble_clone::init();
#if CONFIG_NBP_WIFI
  ble_clone::register_endpoints(ota::handle());
#endif
#endif

  // Both radios are up — apply persisted TX power overrides. Done last
  // so any boot-time WiFi traffic (DHCP, mDNS, OTA listener) runs at
  // chip-default power before being potentially throttled down.
  api_server::stats::apply_tx_power_from_nvs();

  // Scanner is running with proxy:: defaults; reapply any persisted
  // window/interval override now (uses scanner::set_duty which is a
  // no-op until init() has been called by ble_backend::start).
  api_server::stats::apply_scan_from_nvs();

  ESP_LOGI(TAG, "boot complete; main task exiting");
}
