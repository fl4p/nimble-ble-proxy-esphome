#include "connection.h"

#include "address.h"
#include "proxy_config.h"
#include "scanner.h"

#include "NimBLEDevice.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
#ifdef CONFIG_NBP_SMP
  // Bumped on every allocation. A queued bonding job carries the value
  // it was created with, so a job that finishes after the slot has been
  // recycled (peer dropped mid-SMP, HA reconnected to someone else) is
  // recognised as stale instead of reporting against the new peer.
  uint32_t gen = 0;
  bool bond_busy = false;           // a bonding job is queued/running
  bool bond_holds_connect = false;  // ConnectCallback deferred until it ends
#endif
};

std::array<Slot, proxy::MAX_CONNECTIONS> g_slots;
SemaphoreHandle_t g_mutex = nullptr;
FreeChangeCallback g_free_cb = nullptr;

#ifdef CONFIG_NBP_SMP
// Static passkey used when a peer requests pairing with KEYBOARD_ONLY
// I/O (we type, they display). Atomically updatable at runtime via
// connection::set_passkey() — funnelled through
// api_server::stats::set_passkey() which is the only public surface
// (POST /bond?passkey=NNNNNN, or the /clone form). Default 123456 matches most
// Victron SmartShunts and many ESP32-based peripherals; fallback
// "000000" is the other common Victron PIN.
std::atomic<uint32_t> g_passkey{123456};

// ---- auto-bond list ----
//
// Addresses the proxy pairs with as soon as the GAP connect completes,
// before the ConnectionResponse goes out to Home Assistant. Needed for
// peers that refuse *service discovery* on an unauthenticated link and
// simply drop the connection — the ESPHome protocol's PAIR request
// arrives far too late for those, because HA only sends it after a
// successful connect+discover.
//
// Guarded by g_mutex like the slots. Kept at AUTO_BOND_MAX entries: a
// longer list could not be honoured anyway, the bond store holds
// CONFIG_BT_NIMBLE_MAX_BONDS peers.
static_assert(AUTO_BOND_MAX <= CONFIG_BT_NIMBLE_MAX_BONDS,
              "AUTO_BOND_MAX exceeds the NimBLE bond store — raise "
              "CONFIG_BT_NIMBLE_MAX_BONDS in sdkconfig.defaults");
std::array<uint64_t, AUTO_BOND_MAX> g_auto_bond{};
uint8_t g_auto_bond_count = 0;

bool auto_bond_listed_locked(uint64_t address) {
  for (uint8_t i = 0; i < g_auto_bond_count; ++i) {
    if (g_auto_bond[i] == address) return true;
  }
  return false;
}
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
    if (s.state == State::Free) {
#ifdef CONFIG_NBP_SMP
      ++s.gen;
#endif
      return &s;
    }
  }
  return nullptr;
}

void notify_free_change() {
  if (g_free_cb) g_free_cb();
}

#ifdef CONFIG_NBP_SMP
// ---- bonding worker ----
//
// NimBLEClient::secureConnection() blocks the calling task until the
// controller reports BLE_GAP_EVENT_ENC_CHANGE or the link drops — up to
// the 30 s SMP timeout. That must not happen on the api_server client
// task (HA's pings would stall and the connection would be dropped) nor
// on the NimBLE host task (which is what would deliver the event), so
// pairing runs on a dedicated worker.
//
// The worker is created lazily on the first bonding request: builds that
// never bond pay nothing, which matters on this no-PSRAM part.

enum class BondKind : uint8_t {
  AutoBond,  // deferred connect result; delivered via Slot::cb
  Explicit,  // HA asked for it; delivered via PairCallback
};

struct BondJob {
  uint8_t slot;
  uint32_t gen;
  uint64_t address;
  uint16_t mtu;  // AutoBond: MTU to report with the deferred connect result
  BondKind kind;
  PairCallback cb;  // Explicit only
};

// The queue is created in init() (a handful of bytes); the worker task
// itself is spawned on first use so a device that never bonds doesn't
// carry its stack. g_bond_start is a dedicated mutex — taking g_mutex
// around xTaskCreate would hold it across an allocation that the NimBLE
// host task may be waiting on.
QueueHandle_t g_bond_q = nullptr;
SemaphoreHandle_t g_bond_start = nullptr;
TaskHandle_t g_bond_task = nullptr;

void bond_task(void *);

// Caller must NOT hold g_mutex (task creation can block).
bool bond_enqueue(const BondJob &job) {
  if (g_bond_q == nullptr || g_bond_start == nullptr) return false;
  if (g_bond_task == nullptr) {
    xSemaphoreTake(g_bond_start, portMAX_DELAY);
    if (g_bond_task == nullptr &&
        xTaskCreate(&bond_task, "ble_bond", 3072, nullptr, 5, &g_bond_task) !=
            pdPASS) {
      g_bond_task = nullptr;
      xSemaphoreGive(g_bond_start);
      ESP_LOGE(TAG, "bond task create failed");
      return false;
    }
    xSemaphoreGive(g_bond_start);
  }
  return xQueueSend(g_bond_q, &job, 0) == pdTRUE;
}

// Clear the slot's bonding flags if the job still matches, and hand back
// the deferred ConnectCallback when this job owns it.
ConnectCallback bond_finish(const BondJob &job, uint64_t *addr_out) {
  ConnectCallback cb = nullptr;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  Slot &s = g_slots[job.slot];
  if (s.gen == job.gen) {
    s.bond_busy = false;
    if (s.bond_holds_connect) {
      s.bond_holds_connect = false;
      if (s.state == State::Connected) {
        cb = s.cb;
        *addr_out = s.address;
      }
    }
  }
  xSemaphoreGive(g_mutex);
  return cb;
}

void bond_task(void *) {
  BondJob job;
  while (xQueueReceive(g_bond_q, &job, portMAX_DELAY) == pdTRUE) {
    NimBLEClient *client = nullptr;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    Slot &s = g_slots[job.slot];
    if (s.gen == job.gen && s.state == State::Connected) client = s.client;
    xSemaphoreGive(g_mutex);

    bool ok = false;
    int32_t err = -1;  // sentinel: link went away before SMP could start
    if (client != nullptr) {
      // Released before the call: secureConnection() blocks, and the
      // NimBLE host task takes g_mutex from its own callbacks.
      ok = client->secureConnection();
      err = ok ? 0 : client->getLastError();
      ESP_LOGI(TAG, "bond %012llx -> %s (err=%ld)",
               static_cast<unsigned long long>(job.address),
               ok ? "paired" : "failed", static_cast<long>(err));
    }

    uint64_t addr = 0;
    ConnectCallback connect_cb = bond_finish(job, &addr);
    if (job.kind == BondKind::AutoBond) {
      if (connect_cb != nullptr) {
        // Report the link either way: a failed bond leaves a usable but
        // unauthenticated connection, which is exactly what HA would
        // have got without the auto-bond list. Discovery will then fail
        // the same way it does today and HA retries on its own terms.
        if (!ok) {
          ESP_LOGW(TAG, "auto-bond %012llx failed (err=%ld); reporting the "
                        "link to HA unauthenticated",
                   static_cast<unsigned long long>(addr),
                   static_cast<long>(err));
        }
        ConnectionResult r{true, job.mtu, 0};
        connect_cb(addr, r);
      }
      // connect_cb == nullptr: the peer dropped while we were pairing and
      // onDisconnect already reported the failure. Nothing to send.
    } else if (job.cb != nullptr) {
      PairResult pr{ok, err};
      job.cb(job.address, pr);
    }
  }
}
#endif  // CONFIG_NBP_SMP

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
#ifdef CONFIG_NBP_SMP
    // Read the link's security state BEFORE taking g_mutex: getConnInfo
    // goes into the host (ble_hs lock), and the reverse order — our
    // mutex then the host lock — would invert against the host task,
    // which reaches this callback holding its own locks.
    bool encrypted = c->getConnInfo().isEncrypted();
    BondJob job{};
    bool defer = false;
#endif
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    Slot *s = find_by_client_locked(c);
    if (s != nullptr && s->state == State::Connecting) {
      s->state = State::Connected;
#ifdef CONFIG_NBP_SMP
      if (!encrypted && auto_bond_listed_locked(s->address)) {
        s->bond_busy = true;
        s->bond_holds_connect = true;
        job.slot = static_cast<uint8_t>(s - g_slots.data());
        job.gen = s->gen;
        job.address = s->address;
        job.mtu = mtu;
        job.kind = BondKind::AutoBond;
        job.cb = nullptr;
        defer = true;
      }
#endif
      addr_snapshot = s->address;
#ifdef CONFIG_NBP_SMP
      if (!defer)
#endif
      {
        cb_snapshot = s->cb;
      }
    }
    xSemaphoreGive(g_mutex);
    ESP_LOGI(TAG, "onConnect %012llx mtu=%u",
             static_cast<unsigned long long>(addr_snapshot), mtu);
#ifdef CONFIG_NBP_SMP
    if (defer) {
      ESP_LOGI(TAG, "auto-bond %012llx: pairing before reporting the link",
               static_cast<unsigned long long>(job.address));
      if (!bond_enqueue(job)) {
        // Couldn't start the worker — fall back to today's behaviour
        // and report the (unauthenticated) link straight away.
        ESP_LOGW(TAG, "auto-bond %012llx: enqueue failed",
                 static_cast<unsigned long long>(job.address));
        uint64_t addr = 0;
        cb_snapshot = bond_finish(job, &addr);
        if (cb_snapshot != nullptr) addr_snapshot = addr;
      }
    }
#endif
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
#ifdef CONFIG_NBP_SMP
      // A bonding job may still be blocked in secureConnection(); it
      // detects the freed slot and drops its result. Reset the flags so
      // the next tenant of this slot starts clean.
      s->bond_busy = false;
      s->bond_holds_connect = false;
#endif
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
#ifdef CONFIG_NBP_SMP
      // If the peer dropped us mid-SMP, cb_snapshot above IS the
      // deferred connect result — HA gets one connected=false and the
      // bonding job stays silent (bond_finish sees state != Connected).
      s->bond_busy = false;
      s->bond_holds_connect = false;
#endif
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
#ifdef CONFIG_NBP_SMP
  g_bond_q = xQueueCreate(proxy::MAX_CONNECTIONS, sizeof(BondJob));
  g_bond_start = xSemaphoreCreateMutex();
#endif
}

#ifdef CONFIG_NBP_SMP
void set_passkey(uint32_t pin) {
  g_passkey.store(pin, std::memory_order_relaxed);
}

uint32_t get_passkey() {
  return g_passkey.load(std::memory_order_relaxed);
}

bool pair(uint64_t address, PairCallback cb) {
  BondJob job{};
  NimBLEClient *client = nullptr;

  xSemaphoreTake(g_mutex, portMAX_DELAY);
  Slot *s = find_by_addr_locked(address);
  if (s == nullptr || s->state != State::Connected || s->bond_busy) {
    xSemaphoreGive(g_mutex);
    return false;
  }
  client = s->client;
  job.slot = static_cast<uint8_t>(s - g_slots.data());
  job.gen = s->gen;
  job.address = address;
  job.kind = BondKind::Explicit;
  job.cb = cb;
  xSemaphoreGive(g_mutex);

  // Outside the mutex — see the ordering note in onConnect.
  if (client->getConnInfo().isEncrypted()) {
    if (cb != nullptr) {
      PairResult r{true, 0};
      cb(address, r);
    }
    return true;
  }

  // Re-check under the lock and claim the slot: the link could have gone
  // away (or an auto-bond could have claimed it) while we were asking
  // the host about encryption.
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  Slot &slot = g_slots[job.slot];
  if (slot.gen != job.gen || slot.state != State::Connected ||
      slot.bond_busy) {
    xSemaphoreGive(g_mutex);
    return false;
  }
  slot.bond_busy = true;
  xSemaphoreGive(g_mutex);

  if (!bond_enqueue(job)) {
    uint64_t discard = 0;
    bond_finish(job, &discard);  // clears bond_busy
    return false;
  }
  return true;
}

namespace {
// NimBLEDevice::isBonded()/deleteBond() match on the full NimBLEAddress
// including its type, but HA only ever hands us the 48-bit value (the
// address type it sends is the one from the advertisement, which is not
// necessarily the peer's *identity* address type in the bond store).
// Enumerate the store and compare the 48 bits instead.
bool find_bond(uint64_t address, NimBLEAddress *out) {
  int n = NimBLEDevice::getNumBonds();
  for (int i = 0; i < n; ++i) {
    NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
    if (static_cast<uint64_t>(a) == address) {
      if (out != nullptr) *out = a;
      return true;
    }
  }
  return false;
}
}  // namespace

bool unpair(uint64_t address) {
  NimBLEAddress a{};
  if (!find_bond(address, &a)) return false;
  bool ok = NimBLEDevice::deleteBond(a);
  ESP_LOGI(TAG, "unpair %012llx -> %s",
           static_cast<unsigned long long>(address), ok ? "ok" : "failed");
  return ok;
}

uint8_t bonded_addresses(uint64_t *out, uint8_t cap) {
  uint8_t n = 0;
  int total = NimBLEDevice::getNumBonds();
  for (int i = 0; i < total && n < cap; ++i) {
    out[n++] = static_cast<uint64_t>(NimBLEDevice::getBondedAddress(i));
  }
  return n;
}

void set_auto_bond_list(const uint64_t *addrs, uint8_t n) {
  if (n > AUTO_BOND_MAX) n = AUTO_BOND_MAX;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (uint8_t i = 0; i < n; ++i) g_auto_bond[i] = addrs[i];
  g_auto_bond_count = n;
  xSemaphoreGive(g_mutex);
}

uint8_t get_auto_bond_list(uint64_t *out, uint8_t cap) {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  uint8_t n = g_auto_bond_count;
  if (n > cap) n = cap;
  for (uint8_t i = 0; i < n; ++i) out[i] = g_auto_bond[i];
  xSemaphoreGive(g_mutex);
  return n;
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
