#include "ble_backend.h"

#include "connection.h"
#include "proxy_config.h"
#include "scanner.h"

#include "NimBLEDevice.h"
#include "esp_log.h"
#include "host/ble_gap.h"

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_NBP_BLE_AUTO_OFF
#include "NimBLEServer.h"
#endif

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
// runtime; this is user intent and the only half persisted to NVS.
std::atomic<bool> g_adv_enabled{true};

// Live auto-suspend gate, driven by the power supervisor (independent of
// the user switch above). Advertising actually runs only when the user
// enabled it AND it is not auto-suspended. Not persisted.
std::atomic<bool> g_adv_auto_suspend{false};

// Combined "may we advertise right now?" — what every adv start path and
// advertising_enabled() consult.
bool adv_allowed() {
  return g_adv_enabled.load(std::memory_order_relaxed) &&
         !g_adv_auto_suspend.load(std::memory_order_relaxed);
}

// Reconcile the radio to a freshly-computed allowed state. Stop adv when
// it just became disallowed; (re)start it when it just became allowed.
// Idempotent: a no-op when the allowed state didn't actually change or
// the radio already matches. Safe to call before NimBLE init (adv null).
void apply_adv_state(bool now_allowed, bool was_allowed) {
  auto *adv = NimBLEDevice::getAdvertising();
  if (adv == nullptr) return;
  if (!now_allowed) {
    if (adv->isAdvertising()) adv->stop();
  } else if (!was_allowed) {
    // NimBLE keeps the last advertising payload across stop(), so start()
    // resumes the cloned/dashboard adv without a central reconnect.
    if (!adv->isAdvertising()) adv->start();
  }
}

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
  // master switch (and the auto-suspend gate) so an interval change can't
  // resurrect advertising that the user disabled via POST /advitvl?ms=-1
  // or that the power supervisor suspended.
  if (adv->isAdvertising()) {
    adv->stop();
    if (adv_allowed()) adv->start();
  }
}

bool advertising_enabled() {
  // Reports the *combined* state (user switch AND not auto-suspended), so
  // existing callers that gate adv start paths on this honor both.
  return adv_allowed();
}

void set_advertising_enabled(bool on) {
  // Pre-init (NimBLEDevice::getAdvertising() is null until start()): the
  // flag is stored and the gated start paths honor it once the host is up.
  bool was_allowed = adv_allowed();
  g_adv_enabled.store(on, std::memory_order_relaxed);
  apply_adv_state(adv_allowed(), was_allowed);
}

void set_advertising_auto_suspend(bool suspend) {
  bool was_allowed = adv_allowed();
  g_adv_auto_suspend.store(suspend, std::memory_order_relaxed);
  apply_adv_state(adv_allowed(), was_allowed);
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

// Runtime full BLE shutdown — the "off" choice on the dashboard's BLE TX
// dropdown. Unlike the auto-off supervisor (which only quiesces the radio,
// keeping the host/controller and the ble_httpd/clone GATT DB alive), this
// tears the whole NimBLE stack down via NimBLEDevice::deinit(true): host +
// controller deinit, ~heap reclaimed, no BLE airtime at all. Consequences,
// by design (see CLAUDE.md "BLE serves two independent roles"): the ble_httpd
// recovery dashboard and any clone mirror are gone until reboot, and re-init
// mid-session is deliberately not supported — like WiFi-off, this is
// runtime-only and reboot brings BLE back at its configured settings.
std::atomic<bool> g_powered_off{false};

bool powered_off() { return g_powered_off.load(std::memory_order_relaxed); }

bool power_off() {
  if (g_powered_off.exchange(true)) return true;  // already off — idempotent
#if CONFIG_NBP_BLE_AUTO_OFF
  // The auto-off supervisor task touches NimBLE singletons (getServer,
  // scanner resume/pause). It checks g_powered_off at the top of each loop
  // and then idles, but it may be mid-iteration right now. Wait past one full
  // supervisor tick so any in-flight iteration finishes and the next one sees
  // the flag — then no other task races our deinit. (2 s tick; rare action.)
  vTaskDelay(pdMS_TO_TICKS(2100));
#endif
  scanner::stop_forwarding();
  // deinit(true): stop scan, disconnect all links, delete the server/scan/
  // advertising objects, and disable+deinit the BT controller.
  NimBLEDevice::deinit(true);
  ESP_LOGI(TAG, "BLE powered off (full deinit; reboot to re-enable)");
  return true;
}

// Reversible radio quiesce for the duration of an OTA download (contrast
// power_off, which fully deinits). Pause the central scan and suspend our
// advertising so coex can't starve the firmware upload; restore on failure.
// While quiesced the auto-off supervisor stands down — without this it would
// re-resume the scan within a tick (an API client is connected during OTA, so
// "central needed" is true). On OTA success the device reboots.
std::atomic<bool> g_ota_quiesce{false};

bool ota_quiesced() { return g_ota_quiesce.load(std::memory_order_relaxed); }

void quiesce_for_ota() {
  g_ota_quiesce.store(true, std::memory_order_relaxed);
  scanner::pause();
  set_advertising_auto_suspend(true);
  ESP_LOGI(TAG, "OTA: scan paused + advertising suspended to free the radio");
}

void resume_after_ota() {
  g_ota_quiesce.store(false, std::memory_order_relaxed);
  scanner::resume();
  set_advertising_auto_suspend(false);  // supervisor (if any) re-evaluates
  ESP_LOGI(TAG, "OTA failed: BLE radio restored");
}

#if CONFIG_NBP_BLE_AUTO_OFF
namespace power {

namespace {

constexpr const char *PTAG = "ble.pwr";
constexpr TickType_t TICK = pdMS_TO_TICKS(2000);
// Idle ticks (of TICK each) a role must stay unneeded before we quiesce
// it. At least 1 so a 0-second config still debounces one tick.
constexpr int IDLE_TICKS =
    (CONFIG_NBP_BLE_AUTO_OFF_IDLE_SECS * 1000 / 2000) > 0
        ? (CONFIG_NBP_BLE_AUTO_OFF_IDLE_SECS * 1000 / 2000)
        : 1;

Hooks g_hooks{};
std::atomic<bool> g_running{false};

// Null hook ⇒ assume the role is needed (never quiesce on missing info).
bool call_or(bool (*fn)(), bool dflt) { return fn ? fn() : dflt; }

// A central is connected to *us* (peripheral role) — ble_httpd dashboard
// or a clone-mirror consumer. NimBLEServer is null until something
// creates it (ble_httpd::start / clone), so no server ⇒ no centrals.
bool peripheral_central_connected() {
  auto *srv = NimBLEDevice::getServer();
  return srv != nullptr && srv->getConnectedCount() > 0;
}

void supervisor_task(void *) {
  int central_idle = 0;
  int periph_idle = 0;
  bool scan_quiesced = false;
  bool adv_suspended = false;

  for (;;) {
    vTaskDelay(TICK);

    // Stand down while BLE is powered off (deinit — singletons gone, reboot to
    // revive) or quiesced for an OTA (we hold the scan paused on purpose; don't
    // let "central needed" re-resume it mid-upload).
    if (powered_off() || ota_quiesced()) continue;

    const bool clone = call_or(g_hooks.clone_active, false);
    const bool api = call_or(g_hooks.api_client_connected, true);
    const bool wifi = call_or(g_hooks.wifi_connected, false);
    const bool gatt_links =
        connection::free_slots() < proxy::MAX_CONNECTIONS;

    // Central scan (the dominant draw): needed by anyone consuming
    // advertisements or holding/​seeking a GATT link.
    const bool central_needed = api || gatt_links || clone;
    // Peripheral advertising: needed whenever WiFi is down (BLE is then
    // the only path to the dashboard), or a central is attached, or clone
    // is broadcasting its mirror.
    const bool peripheral_needed =
        !wifi || peripheral_central_connected() || clone;

    if (central_needed) {
      central_idle = 0;
      if (scan_quiesced) {
        scanner::resume();
        scan_quiesced = false;
        ESP_LOGI(PTAG, "central needed → scan resumed");
      }
    } else if (!scan_quiesced && ++central_idle >= IDLE_TICKS) {
      scanner::pause();
      scan_quiesced = true;
      ESP_LOGI(PTAG, "central idle %ds → scan paused",
               CONFIG_NBP_BLE_AUTO_OFF_IDLE_SECS);
    }

    if (peripheral_needed) {
      periph_idle = 0;
      if (adv_suspended) {
        set_advertising_auto_suspend(false);
        adv_suspended = false;
        ESP_LOGI(PTAG, "peripheral needed → advertising restored");
      }
    } else if (!adv_suspended && ++periph_idle >= IDLE_TICKS) {
      set_advertising_auto_suspend(true);
      adv_suspended = true;
      ESP_LOGI(PTAG, "peripheral idle %ds (WiFi up) → advertising suspended",
               CONFIG_NBP_BLE_AUTO_OFF_IDLE_SECS);
    }
  }
}

}  // namespace

void init(const Hooks &hooks) {
  if (g_running.exchange(true)) return;
  g_hooks = hooks;
  // 3 KB stack: the task only reads atomics + calls NimBLE getters and
  // scanner start/stop. Priority 4 — below the NimBLE host task, above idle.
  xTaskCreate(&supervisor_task, "ble_pwr", 3072, nullptr, 4, nullptr);
  ESP_LOGI(PTAG, "BLE auto-off supervisor started (idle grace %ds)",
           CONFIG_NBP_BLE_AUTO_OFF_IDLE_SECS);
}

}  // namespace power
#endif  // CONFIG_NBP_BLE_AUTO_OFF

}  // namespace ble_backend
