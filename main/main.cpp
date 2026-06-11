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

#if CONFIG_NBP_LIVENESS_WDT
#include "liveness_wdt.h"
#endif

#if CONFIG_NBP_CLONE
#include "clone.h"
#include "clone_config.h"
#include "clone_mirror.h"
#endif

#if CONFIG_NBP_BLE_HTTPD
#include "ble_httpd.h"
#endif

#if CONFIG_NBP_WS_PROXY
#include "ws_proxy.h"
#endif

#if CONFIG_NBP_NAT_ROUTER
#include "nat_router.h"
#endif

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr const char *TAG = "main";

#if CONFIG_NBP_CLONE_BOOT_GUARD
// Boot-safety guard: if the previous boot crashed within BOOT_STABLE_US
// of app_main start, force-disable ble_clone on the next boot. A panic
// loop caused by a malformed cloned GATT DB (or a host stack hit on a
// specific characteristic) would otherwise repeat indefinitely and
// brick the device until reflash. The user can re-enable clone via
// /clone once the underlying issue is fixed.
constexpr const char *NBP_BOOT_NS = "nbp_boot";
constexpr const char *NBP_BOOT_PENDING_KEY = "pending";
constexpr int64_t BOOT_STABLE_US =
    static_cast<int64_t>(CONFIG_NBP_CLONE_BOOT_GUARD_SECS) * 1000 * 1000;

bool boot_was_unstable() {
  esp_reset_reason_t reason = esp_reset_reason();
  bool crash_class = (reason == ESP_RST_PANIC ||
                      reason == ESP_RST_INT_WDT ||
                      reason == ESP_RST_TASK_WDT ||
                      reason == ESP_RST_WDT ||
                      reason == ESP_RST_BROWNOUT);

  nvs_handle_t h;
  if (nvs_open(NBP_BOOT_NS, NVS_READWRITE, &h) != ESP_OK) return false;

  uint8_t pending = 0;
  nvs_get_u8(h, NBP_BOOT_PENDING_KEY, &pending);
  bool bad_boot = crash_class && pending != 0;

  nvs_set_u8(h, NBP_BOOT_PENDING_KEY, 1);
  nvs_commit(h);
  nvs_close(h);

  if (bad_boot) {
    ESP_LOGE(TAG,
             "previous boot crashed within %lld s (reset_reason=%d); "
             "BLE clone will be force-disabled",
             BOOT_STABLE_US / 1000000, static_cast<int>(reason));
  }
  return bad_boot;
}

void schedule_boot_stable_mark() {
  static esp_timer_handle_t timer = nullptr;
  if (timer != nullptr) return;
  esp_timer_create_args_t args = {
      .callback = [](void *) {
        nvs_handle_t h;
        if (nvs_open(NBP_BOOT_NS, NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_u8(h, NBP_BOOT_PENDING_KEY, 0);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "boot stable for %lld s; cleared boot-guard flag",
                 BOOT_STABLE_US / 1000000);
      },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "bootstable",
      .skip_unhandled_events = false,
  };
  esp_timer_create(&args, &timer);
  esp_timer_start_once(timer, BOOT_STABLE_US);
}
#endif  // CONFIG_NBP_CLONE_BOOT_GUARD
}

// Runtime hostname buffer declared in proxy_config.h. Pre-initialised
// to the compile-time default; overwritten at boot by
// api_server::stats::apply_hostname_from_nvs() if the user stored an
// override via /hostname. main is the link root so a single definition
// here is visible to every component.
namespace proxy {
char g_hostname[HOSTNAME_MAX + 1] = "nimble-proxy";
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

#if CONFIG_NBP_CLONE_BOOT_GUARD
  // Check before any clone code runs so config::load() can be overridden
  // below. Always rearms `pending=1` in NVS — cleared by the one-shot
  // at the end of app_main once we've been up CONFIG_NBP_CLONE_BOOT_GUARD_SECS.
  const bool clone_force_disable = boot_was_unstable();
#endif

  // NVS is up — load persisted hostname into proxy::g_hostname before
  // any subsystem reads `proxy::hostname()` (mDNS, esp_netif, NimBLE
  // GAP name, aioesphomeapi DeviceInfo). Changes via /hostname only
  // take effect after the next boot for this reason.
  api_server::stats::apply_hostname_from_nvs();

  // NVS is up — apply the persisted NimBLE log level before any
  // NimBLE component initialises so it takes effect from the first
  // scan callback.
  api_server::stats::apply_log_overrides_from_nvs();

  // CPU frequency override needs esp_pm and NVS, both available now.
  // Apply before WiFi/BLE init so those subsystems run at the chosen
  // clock from the start.
  api_server::stats::apply_cpu_freq_from_nvs();

#if CONFIG_NBP_WIFI
  // Load persisted WiFi listen_interval into the in-RAM mirror so the
  // dashboard GET reflects the saved value. wifi_sta itself reads the
  // same NVS key independently when stamping wifi_config_t.
  api_server::stats::apply_wifi_ps_from_nvs();
  wifi_sta::start_and_wait_for_ip();
  mdns_announce::start();
  ota::start();
  // Free the single radio for the duration of an OTA upload: drop the SoftAP
  // and pause the BLE scan so coex can't starve the transfer (the
  // SA-Query/heap death-spiral that makes a loaded device's OTA crawl and time
  // out). Restored on failure; a successful OTA reboots. The lambdas are
  // captureless → convert to plain function pointers; the bodies #if out to
  // no-ops in WiFi-only / BLE-only / non-NAT builds.
  ota::set_quiesce_hooks(
      []() {
#if CONFIG_NBP_NAT_ROUTER
        nat_router::pause_for_ota();
#endif
#if CONFIG_NBP_BLE
        ble_backend::quiesce_for_ota();
#endif
      },
      []() {
#if CONFIG_NBP_BLE
        ble_backend::resume_after_ota();
#endif
#if CONFIG_NBP_NAT_ROUTER
        nat_router::resume_after_ota();
#endif
      });
  // Piggyback the stats UI on the OTA httpd so we don't burn an extra
  // LWIP socket budget on a second listener.
  api_server::stats::register_endpoints(ota::handle());
#if CONFIG_NBP_LIVENESS_WDT
  // Self-recovery: reboot if the upstream LAN goes unreachable for a sustained
  // window (a WiFi/coex livelock under heavy forwarded throughput keeps tasks
  // alive — so the Task-WDT never fires — while the IP path is dead).
  liveness_wdt::start();
#endif
#if CONFIG_NBP_BLE
  // Wire the ble_backend → api_server publish hook before either starts
  // accepting traffic. Order between start() calls doesn't matter as
  // long as install() happens before any adv callback fires.
  ble_backend::publish::install(&api_server::send_async,
                                &api_server::has_active_client);
#endif
#endif
#if CONFIG_NBP_BLE
  ble_backend::start();

  // NimBLE host is up, NimBLEDevice::getAdvertising() returns a valid
  // singleton. Apply the persisted advertising interval *before* any
  // adv->start() call below so the configured rate lands in the first
  // HCI window. clone_mirror::start_advertising re-reads the value
  // after its g_adv->reset() call.
  // Also applies the persisted "advertising off" state (NVS adv_itvl
  // sentinel 0xFFFF) — done here, after ble_backend::start so the adv
  // singleton exists and before clone's supervisor task could call
  // start_advertising, so a stored "off" gates the very first adv start.
  api_server::stats::apply_adv_interval_from_nvs();
#endif  // CONFIG_NBP_BLE

#if CONFIG_NBP_WIFI
  api_server::start();
#if CONFIG_NBP_WS_PROXY
  // Browser bridge: tunnel the aioesphomeapi TCP protocol over a WebSocket
  // on the shared dashboard httpd. Registered after api_server::start() so
  // the loopback target is already listening, though the connection itself
  // is opened lazily per browser. api_server stays untouched — each WS
  // client appears to it as an ordinary TCP client.
  ws_proxy::register_endpoint(ota::handle());
#endif
#if CONFIG_NBP_NAT_ROUTER
  // STA is up (start_and_wait_for_ip returned above). Bring up the SoftAP
  // in APSTA mode and NAT its clients out through the STA uplink, then
  // expose /nat + /portmap on the shared dashboard httpd. start() must
  // run before register_endpoints so the GET reflects the live AP state.
  nat_router::start();
  nat_router::register_endpoints(ota::handle());
#if CONFIG_NBP_NAT_THROUGHPUT
  // Bridge SoftAP byte counters into /stats.json so the dashboard renders
  // the repeater-throughput chart. Same edge-component pattern as the clone
  // provider above: api_server stays free of any nat_router dependency.
  api_server::stats::set_nat_throughput_provider(
      [](api_server::stats::NatThroughput *out) {
        nat_router::get_ap_throughput(&out->ap_rx_bytes, &out->ap_tx_bytes);
      });
#endif
#endif
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
  // that scans → connects → discovers → builds the local GATT mirror,
  // then calls ble_httpd::activate() to register all services in one
  // shot. register_endpoints() exposes /clone for runtime config
  // changes (target MAC, enable/disable, and SMP passkey under
  // CONFIG_NBP_SMP — the previous standalone /passkey endpoint was
  // folded in here).
  ble_clone::config::load();
#if CONFIG_NBP_CLONE_BOOT_GUARD
  if (clone_force_disable) {
    auto t = ble_clone::config::snapshot();
    if (t.enabled) {
      t.enabled = false;
      ble_clone::config::set(t);
      ESP_LOGE(TAG, "BLE clone force-disabled due to unstable previous boot");
    }
  }
#endif
  ble_clone::init();
#if CONFIG_NBP_WIFI
  ble_clone::register_endpoints(ota::handle());
  // Bridge clone counters into /stats.json so the dashboard chart sees
  // both transports. Kept in main/ so ble_clone has no link-time
  // dependency on api_server (and vice versa) — the two components stay
  // independent and main is the only edge that knows both.
  api_server::stats::set_clone_stats_provider(
      [](api_server::stats::CloneCounters *out) {
        auto s = ble_clone::mirror::stats();
        out->reads = s.reads_served_from_cache + s.reads_proxied;
        out->writes = s.writes_proxied;
        out->notifies = s.notifies_out;
        out->connected_centrals = s.connected_centrals;
      });
#endif
#endif

#if CONFIG_NBP_BLE_HTTPD
  // Fallback: if clone is disabled, or if the supervisor never
  // finishes building (upstream unreachable), still activate ble_httpd
  // so the dashboard is reachable. activate() is idempotent — when
  // clone's finalize_server runs successfully, that becomes a no-op.
  // The cost is the dashboard going live without cloned services
  // present until clone catches up; subsequent clone connects work
  // fine because activate() registered both ble_httpd and any clone
  // services already in m_svcVec at that moment.
#if !CONFIG_NBP_CLONE
  ble_httpd::activate();
#endif
#endif

  // Both radios are up — apply persisted TX power overrides. Done last
  // so any boot-time WiFi traffic (DHCP, mDNS, OTA listener) runs at
  // chip-default power before being potentially throttled down.
  api_server::stats::apply_tx_power_from_nvs();

#if CONFIG_NBP_BLE
  // Scanner is running with proxy:: defaults; reapply any persisted
  // window/interval override now (uses scanner::set_duty which is a
  // no-op until init() has been called by ble_backend::start).
  api_server::stats::apply_scan_from_nvs();
#endif

#if CONFIG_NBP_BLE_AUTO_OFF
  // Radio auto-quiesce. Wired last so every subsystem the supervisor
  // queries is already up. Predicates are injected here — main is the
  // only edge that knows api_server + wifi_sta + ble_clone, keeping
  // ble_backend free of link-time deps on them (publish::install pattern).
  {
    ble_backend::power::Hooks pwr_hooks{};
#if CONFIG_NBP_WIFI
    pwr_hooks.api_client_connected = &api_server::has_active_client;
    pwr_hooks.wifi_connected = &wifi_sta::is_connected;
#else
    // No WiFi build: there is never an API client, and BLE is the only
    // dashboard transport — so central may quiesce freely, but the
    // peripheral (advertising) must always stay up (wifi_connected=false).
    pwr_hooks.api_client_connected = [] { return false; };
    pwr_hooks.wifi_connected = [] { return false; };
#endif
#if CONFIG_NBP_CLONE
    pwr_hooks.clone_active = [] { return ble_clone::config::snapshot().enabled; };
#endif
    ble_backend::power::init(pwr_hooks);
  }
#endif

#if CONFIG_NBP_CLONE_BOOT_GUARD
  // Arm the "boot stable" mark. If we survive CONFIG_NBP_CLONE_BOOT_GUARD_SECS
  // without a crash-class reset, the pending flag is cleared in NVS and
  // clone remains enabled across the next reboot. A panic before then
  // leaves the flag set; the next boot reads it and force-disables clone.
  schedule_boot_stable_mark();
#endif

  ESP_LOGI(TAG, "boot complete; main task exiting");
}
