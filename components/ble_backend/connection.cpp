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
  // API client that opened this link. Set in connect(), cleared to
  // kNoOwner by release_owner() when that client leaves. Every cb
  // invocation carries it so the bridge reports only to the owner.
  OwnerId owner = kNoOwner;
  // Teardown requested while the slot was still Connecting. NimBLE may
  // already have the link up (connect-established timer pending) when
  // we cancel, so onConnect checks this and terminates instead of
  // reporting success.
  bool abort = false;
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

void clear_locked(Slot *s) {
  s->state = State::Free;
  s->address = 0;
  s->address_type = 0;
  s->cb = nullptr;
  s->owner = kNoOwner;
  s->abort = false;
}

// What the caller must do (after dropping g_mutex) to tear a slot down.
enum class Teardown : uint8_t { None, Terminate, Cancel };

// Mark `s` for teardown under g_mutex. NimBLE calls are issued by the
// caller after the mutex is released, as everywhere else in this file,
// so a host-task callback blocked on g_mutex can never deadlock us.
Teardown begin_teardown_locked(Slot *s) {
  switch (s->state) {
    case State::Connected:
      s->state = State::Disconnecting;
      return Teardown::Terminate;
    case State::Connecting:
      // Stay Connecting: onConnectFail / onDisconnect frees the slot.
      // If NimBLE already holds the link (established timer pending,
      // onConnect not yet delivered) a GAP cancel is a no-op, so
      // terminate instead; otherwise cancel the pending GAP connect
      // (only one can be outstanding, so it is ours).
      s->abort = true;
      return s->client->isConnected() ? Teardown::Terminate : Teardown::Cancel;
    case State::Disconnecting:
    case State::Free:
      break;
  }
  return Teardown::None;
}

void run_teardown(NimBLEClient *client, Teardown t) {
  if (client == nullptr) return;
  if (t == Teardown::Terminate) {
    client->disconnect();  // onDisconnect fires from the host task
  } else if (t == Teardown::Cancel) {
    client->cancelConnect();  // onConnectFail fires from the host task
  }
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
    OwnerId owner_snapshot = kNoOwner;
    bool orphan = false;
    uint16_t mtu = c->getMTU();
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    Slot *s = find_by_client_locked(c);
    if (s != nullptr && s->state == State::Connecting) {
      addr_snapshot = s->address;
      if (s->abort || s->owner == kNoOwner) {
        // Its client disconnected (or asked to disconnect) while the
        // connect was in flight and the GAP cancel lost the race. Nobody
        // wants this link: drop it rather than report it or leave it up
        // holding the peer (which stops advertising while linked).
        s->state = State::Disconnecting;
        orphan = true;
      } else {
        s->state = State::Connected;
        cb_snapshot = s->cb;
        owner_snapshot = s->owner;
      }
    }
    xSemaphoreGive(g_mutex);
    if (orphan) {
      ESP_LOGW(TAG, "onConnect %012llx: owner gone, terminating",
               static_cast<unsigned long long>(addr_snapshot));
      c->disconnect();  // onDisconnect frees the slot
      scanner::resume();
      return;
    }
    ESP_LOGI(TAG, "onConnect %012llx mtu=%u owner=%lu",
             static_cast<unsigned long long>(addr_snapshot), mtu,
             static_cast<unsigned long>(owner_snapshot));
    if (cb_snapshot) {
      ConnectionResult r{true, mtu, 0};
      cb_snapshot(owner_snapshot, addr_snapshot, r);
    }
    notify_free_change();
    // NimBLE suspends the scan while doing the GAP connect procedure;
    // resume it so advert forwarding stays alive for other devices.
    scanner::resume();
  }

  void onConnectFail(NimBLEClient *c, int reason) override {
    ConnectCallback cb_snapshot = nullptr;
    uint64_t addr_snapshot = 0;
    OwnerId owner_snapshot = kNoOwner;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    Slot *s = find_by_client_locked(c);
    if (s != nullptr && s->state != State::Free) {
      cb_snapshot = s->cb;
      addr_snapshot = s->address;
      owner_snapshot = s->owner;
      clear_locked(s);
    }
    xSemaphoreGive(g_mutex);
    ESP_LOGW(TAG, "onConnectFail %012llx reason=%d",
             static_cast<unsigned long long>(addr_snapshot), reason);
    if (cb_snapshot) {
      ConnectionResult r{false, 0, reason};
      cb_snapshot(owner_snapshot, addr_snapshot, r);
    }
    notify_free_change();
    scanner::resume();
  }

  void onDisconnect(NimBLEClient *c, int reason) override {
    ConnectCallback cb_snapshot = nullptr;
    uint64_t addr_snapshot = 0;
    OwnerId owner_snapshot = kNoOwner;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    Slot *s = find_by_client_locked(c);
    if (s != nullptr && s->state == State::Free) s = nullptr;
    if (s != nullptr) {
      cb_snapshot = s->cb;
      addr_snapshot = s->address;
      owner_snapshot = s->owner;
      // Drop slot back to free; keep the NimBLEClient* for reuse on
      // the next connect (NimBLEDevice owns it).
      clear_locked(s);
    }
    xSemaphoreGive(g_mutex);

    if (s != nullptr) {
      ESP_LOGI(TAG, "onDisconnect %012llx reason=%d owner=%lu",
               static_cast<unsigned long long>(addr_snapshot), reason,
               static_cast<unsigned long>(owner_snapshot));
      if (cb_snapshot) {
        ConnectionResult r{false, 0, reason};
        cb_snapshot(owner_snapshot, addr_snapshot, r);
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

// Ownership rule for CONNECT: a slot belongs to exactly one API client.
// A CONNECT for an address that any client already holds — another live
// client, or the requester itself — is refused (bt_handlers answers the
// requester alone with error -100). We deliberately do NOT let client B
// take over A's link: A may be mid-transaction (BMS command/response,
// notify stream), a takeover would silently kill it, and the two clients
// would then fight over the peer. The case that used to need a takeover —
// links left behind by a client that restarted while another client kept
// the proxy busy — no longer exists, because release_owner() drops a
// client's links the moment its API connection closes.
bool connect(uint64_t address, uint8_t address_type, OwnerId owner,
             ConnectCallback cb) {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  if (Slot *held = find_by_addr_locked(address); held != nullptr) {
    OwnerId holder = held->owner;
    xSemaphoreGive(g_mutex);
    ESP_LOGW(TAG, "%012llx already connected/connecting (owner=%lu, req=%lu)",
             static_cast<unsigned long long>(address),
             static_cast<unsigned long>(holder),
             static_cast<unsigned long>(owner));
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
  s->owner = owner;
  s->abort = false;
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

  ESP_LOGI(TAG, "connect %012llx type=%u owner=%lu (async)",
           static_cast<unsigned long long>(address), address_type,
           static_cast<unsigned long>(owner));

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
  //
  // No release_owner(owner) can interleave between the unlock above and
  // this call: connect() runs on the owner's own api_client task, and
  // that task only releases its links after its dispatch loop exits.
  bool issued = client->connect(nimble_addr, /*deleteAttibutes=*/true,
                                /*asyncConnect=*/true, /*exchangeMTU=*/true);
  if (!issued) {
    // Couldn't even issue the connect (controller busy, bad addr, etc).
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    ConnectCallback cb_snapshot = nullptr;
    OwnerId owner_snapshot = kNoOwner;
    if (s->state == State::Connecting && s->address == address) {
      cb_snapshot = s->cb;
      owner_snapshot = s->owner;
      clear_locked(s);
    }
    xSemaphoreGive(g_mutex);
    notify_free_change();
    if (cb_snapshot) {
      ConnectionResult r{false, 0, /*errno-ish=*/-1};
      cb_snapshot(owner_snapshot, address, r);
    }
  }
  return true;
}

DisconnectStatus disconnect(uint64_t address, OwnerId owner) {
  NimBLEClient *client = nullptr;
  Teardown t = Teardown::None;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  Slot *s = find_by_addr_locked(address);
  if (s == nullptr) {
    xSemaphoreGive(g_mutex);
    return DisconnectStatus::NotFound;
  }
  if (s->owner != owner) {
    OwnerId holder = s->owner;
    xSemaphoreGive(g_mutex);
    ESP_LOGW(TAG, "disconnect %012llx refused: owner=%lu, req=%lu",
             static_cast<unsigned long long>(address),
             static_cast<unsigned long>(holder),
             static_cast<unsigned long>(owner));
    return DisconnectStatus::NotOwner;
  }
  // Connected → terminate; still Connecting → cancel (or terminate if
  // NimBLE already has the link). Already Disconnecting → nothing to do,
  // the pending onDisconnect reports to the owner.
  t = begin_teardown_locked(s);
  client = s->client;
  xSemaphoreGive(g_mutex);
  run_teardown(client, t);
  return DisconnectStatus::Started;
}

void release_owner(OwnerId owner) {
  if (owner == kNoOwner) return;
  NimBLEClient *clients[proxy::MAX_CONNECTIONS];
  Teardown actions[proxy::MAX_CONNECTIONS];
  uint8_t n = 0;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (auto &s : g_slots) {
    if (s.state == State::Free || s.owner != owner) continue;
    // Ownerless from here on: whatever callback fires next for this slot
    // (onDisconnect / onConnectFail / a late onConnect) reports to no one.
    s.owner = kNoOwner;
    Teardown t = begin_teardown_locked(&s);
    ESP_LOGI(TAG, "release %012llx (owner=%lu gone, state=%u)",
             static_cast<unsigned long long>(s.address),
             static_cast<unsigned long>(owner),
             static_cast<unsigned>(s.state));
    if (t != Teardown::None) {
      clients[n] = s.client;
      actions[n] = t;
      ++n;
    }
  }
  xSemaphoreGive(g_mutex);
  // Issued outside g_mutex. The freed slots and the ConnectionsFree update
  // to the remaining clients follow from the host-task callbacks.
  for (uint8_t i = 0; i < n; ++i) run_teardown(clients[i], actions[i]);
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
  NimBLEClient *clients[proxy::MAX_CONNECTIONS];
  Teardown actions[proxy::MAX_CONNECTIONS];
  uint8_t n = 0;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (auto &s : g_slots) {
    if (s.state == State::Free) continue;
    s.owner = kNoOwner;
    Teardown t = begin_teardown_locked(&s);
    if (t != Teardown::None) {
      clients[n] = s.client;
      actions[n] = t;
      ++n;
    }
  }
  xSemaphoreGive(g_mutex);
  for (uint8_t i = 0; i < n; ++i) run_teardown(clients[i], actions[i]);
}

void *client_for(uint64_t address, OwnerId owner) {
  void *p = nullptr;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  Slot *s = find_by_addr_locked(address);
  if (s != nullptr && s->state == State::Connected && s->owner == owner &&
      owner != kNoOwner) {
    p = s->client;
  }
  xSemaphoreGive(g_mutex);
  return p;
}

}  // namespace ble_backend::connection
