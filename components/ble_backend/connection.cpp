#include "connection.h"

#include "address.h"
#include "proxy_config.h"
#include "scanner.h"

#include "NimBLEDevice.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <array>
#include <atomic>
#include <cstring>

namespace ble_backend::connection {

namespace {

constexpr const char *TAG = "ble.conn";

enum class State : uint8_t {
  Free = 0,
  Connecting,
  Connected,
  Disconnecting,
};

struct Slot {
  State state = State::Free;
  uint64_t address = 0;
  uint8_t address_type = 0;
  NimBLEClient *client = nullptr;
  ConnectCallback cb = nullptr;
};

std::array<Slot, proxy::MAX_CONNECTIONS> g_slots;
SemaphoreHandle_t g_mutex = nullptr;
FreeChangeCallback g_free_cb = nullptr;

#ifdef CONFIG_NBP_SMP
// Static passkey used when a peer requests pairing with KEYBOARD_ONLY
// I/O (we type, they display). Atomically updatable at runtime via
// connection::set_passkey() — funnelled through
// api_server::stats::set_passkey() which is now the only public
// surface (POST /clone?passkey=NNNNNN). Default 123456 matches most
// Victron SmartShunts and many ESP32-based peripherals; fallback
// "000000" is the other common Victron PIN.
std::atomic<uint32_t> g_passkey{123456};
#endif

Slot *find_by_addr_locked(uint64_t address) {
  for (auto &s : g_slots) {
    if (s.state != State::Free && s.address == address) return &s;
  }
  return nullptr;
}

Slot *find_by_client_locked(const NimBLEClient *c) {
  for (auto &s : g_slots) {
    if (s.client == c) return &s;
  }
  return nullptr;
}

Slot *alloc_locked() {
  for (auto &s : g_slots) {
    if (s.state == State::Free) return &s;
  }
  return nullptr;
}

void notify_free_change() {
  if (g_free_cb) g_free_cb();
}

// NimBLEClientCallbacks: shared callback object; we map back to the
// slot via NimBLEClient*. Connect is now async (asyncConnect=true in
// connect()), so onConnect / onConnectFail drive the success/failure
// path instead of the connect() return value.
//
// All callbacks fire from the NimBLE host task. We touch slot state
// under the mutex and snapshot the cb/address before releasing so we
// never call api_server::send_async while holding the mutex.
class ClientCb : public NimBLEClientCallbacks {
 public:
#ifdef CONFIG_NBP_SMP
  // Peer requested a passkey (because it has DisplayOnly I/O cap and
  // we declared KEYBOARD_ONLY). Inject the stored static passkey via
  // NimBLEDevice — runs in NimBLE host task.
  void onPassKeyEntry(NimBLEConnInfo &connInfo) override {
    uint32_t pin = g_passkey.load(std::memory_order_relaxed);
    ESP_LOGI(TAG, "onPassKeyEntry -> injecting passkey %06lu",
             static_cast<unsigned long>(pin));
    NimBLEDevice::injectPassKey(connInfo, pin);
  }

  void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
    ESP_LOGI(TAG,
             "onAuthenticationComplete encrypted=%d authenticated=%d "
             "bonded=%d key_size=%u",
             connInfo.isEncrypted(), connInfo.isAuthenticated(),
             connInfo.isBonded(), connInfo.getSecKeySize());
  }
#endif  // CONFIG_NBP_SMP

  void onConnect(NimBLEClient *c) override {
    ConnectCallback cb_snapshot = nullptr;
    uint64_t addr_snapshot = 0;
    uint16_t mtu = c->getMTU();
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    Slot *s = find_by_client_locked(c);
    if (s != nullptr && s->state == State::Connecting) {
      s->state = State::Connected;
      cb_snapshot = s->cb;
      addr_snapshot = s->address;
    }
    xSemaphoreGive(g_mutex);
    ESP_LOGI(TAG, "onConnect %012llx mtu=%u",
             static_cast<unsigned long long>(addr_snapshot), mtu);
    if (cb_snapshot) {
      ConnectionResult r{true, mtu, 0};
      cb_snapshot(addr_snapshot, r);
    }
    notify_free_change();
    // NimBLE suspends the scan while doing the GAP connect procedure;
    // resume it so advert forwarding stays alive for other devices.
    scanner::resume();
  }

  void onConnectFail(NimBLEClient *c, int reason) override {
    ConnectCallback cb_snapshot = nullptr;
    uint64_t addr_snapshot = 0;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    Slot *s = find_by_client_locked(c);
    if (s != nullptr) {
      cb_snapshot = s->cb;
      addr_snapshot = s->address;
      s->state = State::Free;
      s->address = 0;
      s->address_type = 0;
      s->cb = nullptr;
    }
    xSemaphoreGive(g_mutex);
    ESP_LOGW(TAG, "onConnectFail %012llx reason=%d",
             static_cast<unsigned long long>(addr_snapshot), reason);
    if (cb_snapshot) {
      ConnectionResult r{false, 0, reason};
      cb_snapshot(addr_snapshot, r);
    }
    notify_free_change();
    scanner::resume();
  }

  void onDisconnect(NimBLEClient *c, int reason) override {
    ConnectCallback cb_snapshot = nullptr;
    uint64_t addr_snapshot = 0;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    Slot *s = find_by_client_locked(c);
    if (s != nullptr) {
      cb_snapshot = s->cb;
      addr_snapshot = s->address;
      // Drop slot back to free; keep the NimBLEClient* for reuse on
      // the next connect (NimBLEDevice owns it).
      s->state = State::Free;
      s->address = 0;
      s->address_type = 0;
      s->cb = nullptr;
    }
    xSemaphoreGive(g_mutex);

    if (s != nullptr) {
      ESP_LOGI(TAG, "onDisconnect %012llx reason=%d",
               static_cast<unsigned long long>(addr_snapshot), reason);
      if (cb_snapshot) {
        ConnectionResult r{false, 0, reason};
        cb_snapshot(addr_snapshot, r);
      }
      notify_free_change();
      scanner::resume();
    }
  }
};

ClientCb g_client_cb;

}  // namespace

void init() {
  g_mutex = xSemaphoreCreateMutex();
}

#ifdef CONFIG_NBP_SMP
void set_passkey(uint32_t pin) {
  g_passkey.store(pin, std::memory_order_relaxed);
}

uint32_t get_passkey() {
  return g_passkey.load(std::memory_order_relaxed);
}
#endif

void register_free_change_cb(FreeChangeCallback cb) {
  g_free_cb = cb;
}

bool connect(uint64_t address, uint8_t address_type, ConnectCallback cb) {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  if (find_by_addr_locked(address) != nullptr) {
    xSemaphoreGive(g_mutex);
    ESP_LOGW(TAG, "%012llx already connected/connecting",
             static_cast<unsigned long long>(address));
    return false;
  }
  Slot *s = alloc_locked();
  if (s == nullptr) {
    xSemaphoreGive(g_mutex);
    ESP_LOGW(TAG, "no free slot");
    return false;
  }
  s->state = State::Connecting;
  s->address = address;
  s->address_type = address_type;
  s->cb = cb;
  if (s->client == nullptr) {
    s->client = NimBLEDevice::createClient();
    s->client->setClientCallbacks(&g_client_cb, /*deleteCallbacks=*/false);
    // NimBLE-Cpp setConnectTimeout takes milliseconds (default 30000).
    s->client->setConnectTimeout(proxy::CONNECT_TIMEOUT_MS);
  }
  NimBLEClient *client = s->client;
  xSemaphoreGive(g_mutex);
  notify_free_change();

  // NimBLEAddress(uint64_t, type) takes MSB-first hex, so 0x20a111022345
  // becomes MAC 20:A1:11:02:23:45 — which matches what aioesphomeapi sends.
  // (Avoid the (uint8_t[6], type) constructor: it internally reverse_copies,
  // so passing LE wire-order bytes ends up wrong on-air.)
  NimBLEAddress nimble_addr(address, address_type);

  ESP_LOGI(TAG, "connect %012llx type=%u (async)",
           static_cast<unsigned long long>(address), address_type);

  // Stop our continuous scan first. NimBLE's ble_gap_connect implicitly
  // pre-empts the radio but ESP32-S3 + NimBLE host appears to miss
  // connectable adverts during the connect-scan when our user scan was
  // running; cancelling it explicitly is what micropython's aioble does
  // and what makes the connect actually catch the peer.
  auto *scan = NimBLEDevice::getScan();
  if (scan && scan->isScanning()) {
    scan->stop();
  }

  // Async connect — returns immediately. ClientCb::onConnect /
  // onConnectFail will fire later from the NimBLE host task and route
  // the result through the registered ConnectCallback.
  bool issued = client->connect(nimble_addr, /*deleteAttibutes=*/true,
                                /*asyncConnect=*/true, /*exchangeMTU=*/true);
  if (!issued) {
    // Couldn't even issue the connect (controller busy, bad addr, etc).
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    ConnectCallback cb_snapshot = s->cb;
    s->state = State::Free;
    s->address = 0;
    s->address_type = 0;
    s->cb = nullptr;
    xSemaphoreGive(g_mutex);
    notify_free_change();
    if (cb_snapshot) {
      ConnectionResult r{false, 0, /*errno-ish=*/-1};
      cb_snapshot(address, r);
    }
  }
  return true;
}

void disconnect(uint64_t address) {
  NimBLEClient *client = nullptr;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  Slot *s = find_by_addr_locked(address);
  if (s != nullptr && s->state == State::Connected) {
    s->state = State::Disconnecting;
    client = s->client;
  }
  xSemaphoreGive(g_mutex);
  if (client != nullptr) {
    client->disconnect();  // onDisconnect callback fires from host task
  }
}

uint8_t free_slots() {
  uint8_t n = 0;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (auto &s : g_slots) {
    if (s.state == State::Free) ++n;
  }
  xSemaphoreGive(g_mutex);
  return n;
}

uint8_t in_use_addresses(uint64_t *out, uint8_t cap) {
  uint8_t n = 0;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (auto &s : g_slots) {
    if (s.state != State::Free && n < cap) {
      out[n++] = s.address;
    }
  }
  xSemaphoreGive(g_mutex);
  return n;
}

void disconnect_all() {
  for (auto &s : g_slots) {
    if (s.state != State::Free) disconnect(s.address);
  }
}

void *client_for(uint64_t address) {
  void *p = nullptr;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  Slot *s = find_by_addr_locked(address);
  if (s != nullptr && s->state == State::Connected) p = s->client;
  xSemaphoreGive(g_mutex);
  return p;
}

}  // namespace ble_backend::connection
