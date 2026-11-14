#include "connection.h"

#include "address.h"
#include "ble_backend.h"
#include "proxy_config.h"
#include "scanner.h"

#include "NimBLEDevice.h"
#include "esp_bt.h"  // esp_ble_tx_power_get[_enhanced] for /links
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
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

// Wait for a reused NimBLEClient to finish its previous teardown before
// connecting again (see connect()). 100 × 10 ms = 1 s worst case.
constexpr int CLIENT_SETTLE_POLLS = 100;
constexpr int CLIENT_SETTLE_POLL_MS = 10;

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
  uint16_t mtu = 0;  // negotiated MTU, set in onConnect
  int64_t connected_us = 0;  // esp_timer_get_time() at onConnect (/links)
  // Teardown requested while the slot was still Connecting. NimBLE may
  // already have the link up (connect-established timer pending) when
  // we cancel, so onConnect checks this and terminates instead of
  // reporting success.
  bool abort = false;
#ifdef CONFIG_NBP_SMP
  // Bumped on every allocation. A queued bonding job carries the value
  // it was created with, so a job that finishes after the slot has been
  // recycled (peer dropped mid-SMP, HA reconnected to someone else) is
  // recognised as stale instead of reporting against the new peer.
  // Neither gen nor bond_busy is reset by clear_locked().
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
// api_server::stats::set_passkey(), reached via POST /bond?passkey=,
// POST /clone?passkey= in clone builds or POST /passkey?val= without
// clone. Default 123456 matches most Victron SmartShunts and many
// ESP32-based peripherals; fallback "000000" is the other common
// Victron PIN.
std::atomic<uint32_t> g_passkey{123456};

// ---- auto-bond list ----
//
// Addresses the proxy pairs with as soon as the GAP connect completes,
// before the ConnectionResponse goes out to the owning API client.
// Needed for peers that refuse *service discovery* on an unauthenticated
// link and simply drop the connection — the ESPHome protocol's PAIR
// request arrives far too late for those, because HA only sends it after
// a successful connect+discover.
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

#ifdef CONFIG_NBP_SMP
// A slot whose bonding job is still in flight is NOT available, even once
// the link is gone: the worker is holding this slot's NimBLEClient* and is
// about to call (or is inside) secureConnection() on it. Handing the slot
// — and therefore that same client object, which slots keep and reuse — to
// a new peer would run SMP against the wrong device. bond_finish() clears
// the flag once the worker is done with the pointer, which a disconnect
// makes happen promptly (it releases secureConnection's wait).
bool slot_available_locked(const Slot &s) {
  return s.state == State::Free && !s.bond_busy;
}
#else
bool slot_available_locked(const Slot &s) { return s.state == State::Free; }
#endif

Slot *alloc_locked() {
  for (auto &s : g_slots) {
    if (slot_available_locked(s)) {
#ifdef CONFIG_NBP_SMP
      ++s.gen;
#endif
      return &s;
    }
  }
  return nullptr;
}

void clear_locked(Slot *s) {
  s->state = State::Free;
  s->address = 0;
  s->address_type = 0;
  s->cb = nullptr;
  s->owner = kNoOwner;
  s->mtu = 0;
  s->connected_us = 0;
  s->abort = false;
#ifdef CONFIG_NBP_SMP
  // A bonding job may still be blocked in secureConnection(); it detects
  // the freed slot and drops its result. If it was withholding the
  // connect result, the caller's cb snapshot IS that result (one
  // connected=false; the job stays silent because bond_finish() sees
  // state != Connected). bond_busy is deliberately NOT cleared here — it
  // pins the slot until the worker is finished with this client pointer
  // (see slot_available_locked); only bond_finish() clears it.
  s->bond_holds_connect = false;
#endif
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
      // Already being torn down (DISCONNECT then release_owner, or the
      // reverse): do nothing. After a first Terminate NimBLE is
      // DISCONNECTING, so isConnected() is false and we would pick Cancel —
      // and ble_gap_conn_cancel() cancels whatever GAP connect is pending,
      // which may be ANOTHER client's. A first Cancel needs no repeat
      // either: its onConnectFail (or a late onConnect, which sees abort)
      // finishes the job.
      if (s->abort) return Teardown::None;
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
// The worker is created on the first bonding request and deletes itself
// after BOND_IDLE_EXIT_MS without a job, so its stack is heap only while
// bonding is actually happening (no PSRAM on the S3; see CLAUDE.md "Heap
// budget"). A failed xTaskCreate is not fatal: the caller falls back to
// reporting the link unauthenticated / answering PAIR with an error.
constexpr uint32_t BOND_TASK_STACK = 3072;
constexpr uint32_t BOND_IDLE_EXIT_MS = 30000;

enum class BondKind : uint8_t {
  AutoBond,  // deferred connect result; delivered via Slot::cb
  Explicit,  // a client asked for it; delivered via PairCallback
};

struct BondJob {
  uint8_t slot;
  uint32_t gen;
  uint64_t address;
  OwnerId owner;  // Explicit: the requester (the link's owner)
  uint16_t mtu;   // AutoBond: MTU to report with the deferred connect result
  BondKind kind;
  PairCallback cb;  // Explicit only
};

// The queue is created in init() (a handful of bytes). g_bond_start
// serialises task creation, enqueueing and the worker's idle exit, so a
// job can never be queued just as the worker decides to quit. It is a
// dedicated mutex — taking g_mutex around xTaskCreate would hold it across
// an allocation that the NimBLE host task may be waiting on.
QueueHandle_t g_bond_q = nullptr;
SemaphoreHandle_t g_bond_start = nullptr;
TaskHandle_t g_bond_task = nullptr;

void bond_task(void *);

// Caller must NOT hold g_mutex (task creation allocates).
bool bond_enqueue(const BondJob &job) {
  if (g_bond_q == nullptr || g_bond_start == nullptr) return false;
  xSemaphoreTake(g_bond_start, portMAX_DELAY);
  if (g_bond_task == nullptr &&
      xTaskCreate(&bond_task, "ble_bond", BOND_TASK_STACK, nullptr, 5,
                  &g_bond_task) != pdPASS) {
    g_bond_task = nullptr;
    xSemaphoreGive(g_bond_start);
    ESP_LOGE(TAG, "bond task create failed (heap)");
    return false;
  }
  bool ok = xQueueSend(g_bond_q, &job, 0) == pdTRUE;
  xSemaphoreGive(g_bond_start);
  return ok;
}

// Is the link this job was started for still up? Used after sending a
// deferred connect result to detect the case where the peer dropped
// between the decision and the send.
bool job_link_live(const BondJob &job) {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  const Slot &s = g_slots[job.slot];
  bool live = (s.gen == job.gen && s.state == State::Connected);
  xSemaphoreGive(g_mutex);
  return live;
}

// Clear the slot's bonding flags if the job still matches, and hand back
// the deferred ConnectCallback (with its owner) when this job owns it.
ConnectCallback bond_finish(const BondJob &job, uint64_t *addr_out,
                            OwnerId *owner_out) {
  ConnectCallback cb = nullptr;
  bool freed = false;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  Slot &s = g_slots[job.slot];
  if (s.gen == job.gen) {
    s.bond_busy = false;
    // The link went away while we were pairing: the slot only becomes
    // allocatable now, so announce it (free_slots() counted it as taken).
    freed = (s.state == State::Free);
    if (s.bond_holds_connect) {
      s.bond_holds_connect = false;
      if (s.state == State::Connected) {
        cb = s.cb;
        *addr_out = s.address;
        *owner_out = s.owner;
      }
    }
  }
  xSemaphoreGive(g_mutex);
  if (freed) notify_free_change();
  return cb;
}

void bond_run(const BondJob &job) {
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
  OwnerId owner = kNoOwner;
  ConnectCallback connect_cb = bond_finish(job, &addr, &owner);
  if (job.kind == BondKind::AutoBond) {
    if (connect_cb != nullptr) {
      // Report the link either way: a failed bond leaves a usable but
      // unauthenticated connection, which is exactly what the client
      // would have got without the auto-bond list. Discovery will then
      // fail the same way it does today and HA retries on its own terms.
      if (!ok) {
        ESP_LOGW(TAG, "auto-bond %012llx failed (err=%ld); reporting the "
                      "link unauthenticated",
                 static_cast<unsigned long long>(addr),
                 static_cast<long>(err));
      }
      ConnectionResult r{true, job.mtu, 0};
      connect_cb(owner, addr, r);
      // bond_finish() checked the link was up, but the peer can drop in
      // the gap between that check and the send above — and onDisconnect
      // would then have pushed its connected=false FIRST, leaving the
      // owner holding a link that no longer exists. Re-check and correct.
      // A duplicate connected=false is harmless; a phantom connection
      // is not.
      if (!job_link_live(job)) {
        ESP_LOGW(TAG, "auto-bond %012llx: link dropped while reporting it; "
                      "sending a corrective disconnect",
                 static_cast<unsigned long long>(addr));
        ConnectionResult down{false, 0, 0};
        connect_cb(owner, addr, down);
      }
    }
    // connect_cb == nullptr: the peer dropped (or its owner left) while
    // we were pairing and the teardown callback already reported it.
  } else if (job.cb != nullptr) {
    PairResult pr{ok, err};
    job.cb(job.owner, job.address, pr);
  }
}

void bond_task(void *) {
  BondJob job;
  for (;;) {
    if (xQueueReceive(g_bond_q, &job, pdMS_TO_TICKS(BOND_IDLE_EXIT_MS)) ==
        pdTRUE) {
      bond_run(job);
      continue;
    }
    // Idle: give the stack back. Under g_bond_start no job can be queued
    // between the emptiness check and clearing the handle, and the next
    // bond_enqueue() starts a fresh worker.
    xSemaphoreTake(g_bond_start, portMAX_DELAY);
    if (uxQueueMessagesWaiting(g_bond_q) == 0) {
      g_bond_task = nullptr;
      xSemaphoreGive(g_bond_start);
      vTaskDelete(nullptr);
    }
    xSemaphoreGive(g_bond_start);
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
    OwnerId owner_snapshot = kNoOwner;
    bool orphan = false;
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
        s->mtu = mtu;
        s->connected_us = esp_timer_get_time();
        owner_snapshot = s->owner;
#ifdef CONFIG_NBP_SMP
        if (!encrypted && auto_bond_listed_locked(s->address)) {
          // Withhold the connect result until SMP has resolved, so the
          // owner's service discovery runs over an encrypted link.
          s->bond_busy = true;
          s->bond_holds_connect = true;
          job.slot = static_cast<uint8_t>(s - g_slots.data());
          job.gen = s->gen;
          job.address = s->address;
          job.owner = s->owner;
          job.mtu = mtu;
          job.kind = BondKind::AutoBond;
          job.cb = nullptr;
          defer = true;
        } else
#endif
        {
          cb_snapshot = s->cb;
        }
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
#ifdef CONFIG_NBP_SMP
    if (defer) {
      ESP_LOGI(TAG, "auto-bond %012llx: pairing before reporting the link",
               static_cast<unsigned long long>(job.address));
      if (!bond_enqueue(job)) {
        // Couldn't start the worker — fall back to reporting the
        // (unauthenticated) link straight away.
        ESP_LOGW(TAG, "auto-bond %012llx: enqueue failed",
                 static_cast<unsigned long long>(job.address));
        uint64_t addr = 0;
        OwnerId owner = kNoOwner;
        cb_snapshot = bond_finish(job, &addr, &owner);
        if (cb_snapshot != nullptr) {
          addr_snapshot = addr;
          owner_snapshot = owner;
        }
      }
    }
#endif
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

// g_mutex is created by ble_backend::start() and never deleted. Every public
// entry point below checks it first: the dashboard httpd and the api_server
// can call in before start() (GET /stats.json in the boot window once tripped
// configASSERT(pxQueue) in xQueueSemaphoreTake, crash 2026-11-12T1519). With
// no mutex there are no links, so each answers "nothing here".
void init() {
  if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();
#ifdef CONFIG_NBP_SMP
  if (g_bond_q == nullptr) {
    g_bond_q = xQueueCreate(proxy::MAX_CONNECTIONS, sizeof(BondJob));
  }
  if (g_bond_start == nullptr) g_bond_start = xSemaphoreCreateMutex();
#endif
}

#ifdef CONFIG_NBP_SMP
void set_passkey(uint32_t pin) {
  g_passkey.store(pin, std::memory_order_relaxed);
}

uint32_t get_passkey() {
  return g_passkey.load(std::memory_order_relaxed);
}

bool pair(uint64_t address, OwnerId owner, PairCallback cb) {
  BondJob job{};
  NimBLEClient *client = nullptr;

  // POST /txpower?ble=off deinits host + controller; the slots and their
  // clients are gone with it. Bail before touching any of them.
  if (ble_backend::powered_off()) return false;
  if (g_mutex == nullptr || owner == kNoOwner) return false;

  xSemaphoreTake(g_mutex, portMAX_DELAY);
  Slot *s = find_by_addr_locked(address);
  // Owner-only, like every other per-link request: another client's link
  // is not ours to pair (or to stall behind SMP for 30 s).
  if (s == nullptr || s->state != State::Connected || s->owner != owner ||
      s->bond_busy) {
    xSemaphoreGive(g_mutex);
    return false;
  }
  client = s->client;
  job.slot = static_cast<uint8_t>(s - g_slots.data());
  job.gen = s->gen;
  job.address = address;
  job.owner = owner;
  job.kind = BondKind::Explicit;
  job.cb = cb;
  xSemaphoreGive(g_mutex);

  // Outside the mutex — see the ordering note in onConnect.
  if (client->getConnInfo().isEncrypted()) {
    if (cb != nullptr) {
      PairResult r{true, 0};
      cb(owner, address, r);
    }
    return true;
  }

  // Re-check under the lock and claim the slot: the link could have gone
  // away (or an auto-bond could have claimed it) while we were asking
  // the host about encryption.
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  Slot &slot = g_slots[job.slot];
  if (slot.gen != job.gen || slot.state != State::Connected ||
      slot.owner != owner || slot.bond_busy) {
    xSemaphoreGive(g_mutex);
    return false;
  }
  slot.bond_busy = true;
  xSemaphoreGive(g_mutex);

  if (!bond_enqueue(job)) {
    uint64_t discard_addr = 0;
    OwnerId discard_owner = kNoOwner;
    bond_finish(job, &discard_addr, &discard_owner);  // clears bond_busy
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
// The bond store is a host facility. Callers hold host_acquire(), which
// fails after power_off()'s NimBLEDevice::deinit(true) and keeps a
// concurrent power_off() from deinit-ing mid-call. Before
// ble_backend::start() (g_mutex still null) the host is not up yet.
bool find_bond(uint64_t address, NimBLEAddress *out) {
  if (g_mutex == nullptr) return false;
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

bool has_bond(uint64_t address) {
  if (!ble_backend::host_acquire()) return false;
  bool found = find_bond(address, nullptr);
  ble_backend::host_release();
  return found;
}

bool unpair(uint64_t address) {
  if (!ble_backend::host_acquire()) return false;
  NimBLEAddress a{};
  if (!find_bond(address, &a)) {
    ble_backend::host_release();
    return false;
  }
  bool ok = NimBLEDevice::deleteBond(a);
  ble_backend::host_release();
  ESP_LOGI(TAG, "unpair %012llx -> %s",
           static_cast<unsigned long long>(address), ok ? "ok" : "failed");
  return ok;
}

uint8_t bonded_addresses(uint64_t *out, uint8_t cap) {
  uint8_t n = 0;
  if (g_mutex == nullptr || !ble_backend::host_acquire()) return 0;
  int total = NimBLEDevice::getNumBonds();
  for (int i = 0; i < total && n < cap; ++i) {
    out[n++] = static_cast<uint64_t>(NimBLEDevice::getBondedAddress(i));
  }
  ble_backend::host_release();
  return n;
}

void set_auto_bond_list(const uint64_t *addrs, uint8_t n) {
  if (g_mutex == nullptr) return;
  if (n > AUTO_BOND_MAX) n = AUTO_BOND_MAX;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (uint8_t i = 0; i < n; ++i) g_auto_bond[i] = addrs[i];
  g_auto_bond_count = n;
  xSemaphoreGive(g_mutex);
}

uint8_t get_auto_bond_list(uint64_t *out, uint8_t cap) {
  if (g_mutex == nullptr) return 0;
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

// Ownership rule for CONNECT: a slot belongs to exactly one API client.
//
// Same client re-sends CONNECT for a link it already holds (bleak-esphome
// retries, or a reconnect after its own state got out of step): answer
// with the link's real state, as upstream ESPHome does — connected=true
// + MTU if it is up, nothing while it is still connecting (the pending
// result follows). Refusing with -100 would make the client treat its
// healthy link as dropped and never DISCONNECT it, so every later retry
// would get -100 too (the #391 symptom, with a single client).
//
// A CONNECT for an address ANOTHER client holds is refused (bt_handlers
// answers the requester alone with error -100). We deliberately do NOT let client B
// take over A's link: A may be mid-transaction (BMS command/response,
// notify stream), a takeover would silently kill it, and the two clients
// would then fight over the peer. The case that used to need a takeover —
// links left behind by a client that restarted while another client kept
// the proxy busy — no longer exists, because release_owner() drops a
// client's links the moment its API connection closes.
ConnectStatus connect(uint64_t address, uint8_t address_type, OwnerId owner,
                      ConnectCallback cb, uint16_t *mtu_out) {
  if (g_mutex == nullptr) return ConnectStatus::NoSlot;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  if (Slot *held = find_by_addr_locked(address); held != nullptr) {
    OwnerId holder = held->owner;
    State st = held->state;
    bool aborting = held->abort;
    uint16_t mtu = held->mtu;
#ifdef CONFIG_NBP_SMP
    // An auto-bond is withholding this link's connect result: to its
    // owner the connect is still in progress, the result follows from
    // the bonding worker. Answering connected=true now would let it start
    // discovery on the unauthenticated link the deferral exists to avoid.
    if (held->bond_holds_connect) st = State::Connecting;
#endif
    xSemaphoreGive(g_mutex);
    if (holder == owner && st == State::Connected) {
      ESP_LOGI(TAG, "%012llx already connected for owner=%lu — re-confirming",
               static_cast<unsigned long long>(address),
               static_cast<unsigned long>(owner));
      if (mtu_out) *mtu_out = mtu;
      return ConnectStatus::OwnConnected;
    }
    if (holder == owner && st == State::Connecting && !aborting) {
      ESP_LOGI(TAG, "%012llx connect already in progress for owner=%lu",
               static_cast<unsigned long long>(address),
               static_cast<unsigned long>(owner));
      return ConnectStatus::OwnInProgress;
    }
    // Another client's link, or the owner's own link mid-teardown (its
    // connected=false is on the way; the client may retry once it lands).
    ESP_LOGW(TAG, "%012llx already connected/connecting (owner=%lu, req=%lu)",
             static_cast<unsigned long long>(address),
             static_cast<unsigned long>(holder),
             static_cast<unsigned long>(owner));
    return ConnectStatus::HeldByOther;
  }
  Slot *s = alloc_locked();
  if (s == nullptr) {
    xSemaphoreGive(g_mutex);
    ESP_LOGW(TAG, "no free slot");
    return ConnectStatus::NoSlot;
  }
  s->state = State::Connecting;
  s->address = address;
  s->address_type = address_type;
  s->cb = cb;
  s->owner = owner;
  s->abort = false;
  if (s->client == nullptr) {
    // Created once per slot and kept. createClient() returns nullptr when
    // NimBLE's client table is full (the clone takes one too); its own
    // `new NimBLEClient` is the throwing form (oom_guard: waits, then
    // aborts), which we cannot change.
    s->client = NimBLEDevice::createClient();
    if (s->client == nullptr) {
      clear_locked(s);
      xSemaphoreGive(g_mutex);
      ESP_LOGW(TAG, "createClient failed (NimBLE client table full?)");
      return ConnectStatus::NoSlot;
    }
    s->client->setClientCallbacks(&g_client_cb, /*deleteCallbacks=*/false);
    // NimBLE-Cpp setConnectTimeout takes milliseconds (default 30000).
    s->client->setConnectTimeout(proxy::CONNECT_TIMEOUT_MS);
  }
  NimBLEClient *client = s->client;
  xSemaphoreGive(g_mutex);
  notify_free_change();

  // onDisconnect / onConnectFail free the slot (and announce it) from
  // INSIDE NimBLE's GAP event handler; NimBLE only resets the client to
  // DISCONNECTED (conn handle NONE) after our callback returns. A fast
  // reconnect that reuses this slot's NimBLEClient in that window gets
  // BLE_HS_EREJECT from connect(). The slot is ours (Connecting), so wait
  // here — on the api_client task, never the host task — for NimBLE to
  // finish. Normally that's microseconds; the bound covers the callback
  // being held up by a slow send (SO_SNDTIMEO) and keeps the old
  // fail-with--1 behaviour if it never settles.
  for (int i = 0; i < CLIENT_SETTLE_POLLS &&
                  client->getConnHandle() != BLE_HS_CONN_HANDLE_NONE;
       ++i) {
    vTaskDelay(pdMS_TO_TICKS(CLIENT_SETTLE_POLL_MS));
  }

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
  return ConnectStatus::Started;
}

DisconnectStatus disconnect(uint64_t address, OwnerId owner) {
  if (g_mutex == nullptr) return DisconnectStatus::NotFound;
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
  if (g_mutex == nullptr) return;
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
  if (g_mutex == nullptr) return proxy::MAX_CONNECTIONS;
  uint8_t n = 0;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (auto &s : g_slots) {
    // Same predicate alloc_locked() uses — a slot still pinned by a
    // bonding job would otherwise be advertised to HA as free and then
    // refused on the connect that follows.
    if (slot_available_locked(s)) ++n;
  }
  xSemaphoreGive(g_mutex);
  return n;
}

uint8_t in_use_addresses(uint64_t *out, uint8_t cap) {
  if (g_mutex == nullptr) return 0;
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

namespace {

const char *state_name(State st) {
  switch (st) {
    case State::Connecting: return "connecting";
    case State::Connected: return "connected";
    case State::Disconnecting: return "disconnecting";
    case State::Free: break;
  }
  return "free";
}

// TX power level the controller applies to connection `handle`, in dBm.
// False when the controller has no per-connection figure for it. The
// level enums differ per target (ESP32: -12..+9 dBm in 3 dB steps from
// index 0; C3/S3: -24..+20 dBm, index 15 = +20), so map by target and
// report nothing on targets this was not written against.
bool conn_tx_power_dbm(uint16_t handle, int8_t *out) {
#if CONFIG_IDF_TARGET_ESP32
  // Legacy controller: one power type per connection handle 0..8.
  if (handle > ESP_BLE_PWR_TYPE_CONN_HDL8) return false;
  esp_power_level_t lvl =
      esp_ble_tx_power_get(static_cast<esp_ble_power_type_t>(handle));
  if (lvl < ESP_PWR_LVL_N12 || lvl > ESP_PWR_LVL_P9) return false;
  *out = static_cast<int8_t>(-12 + 3 * static_cast<int>(lvl));
  return true;
#elif CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3 || \
    CONFIG_IDF_TARGET_ESP32C6
  esp_power_level_t lvl =
      esp_ble_tx_power_get_enhanced(ESP_BLE_ENHANCED_PWR_TYPE_CONN, handle);
  // Same index -> dBm mapping on all three; the C6 enum just starts at
  // N15 (= 3) instead of N24 (= 0).
#if CONFIG_IDF_TARGET_ESP32C6
  constexpr esp_power_level_t kLowest = ESP_PWR_LVL_N15;
#else
  constexpr esp_power_level_t kLowest = ESP_PWR_LVL_N24;
#endif
  if (lvl < kLowest || lvl > ESP_PWR_LVL_P20) return false;
  *out = lvl == ESP_PWR_LVL_P20
             ? 20
             : static_cast<int8_t>(-24 + 3 * static_cast<int>(lvl));
  return true;
#else
  (void)handle;
  (void)out;
  return false;
#endif
}

}  // namespace

uint8_t link_info(LinkInfo *out, uint8_t cap) {
  // After POST /txpower?ble=off the host is gone; nothing to ask.
  if (ble_backend::powered_off() || g_mutex == nullptr) return 0;
  uint16_t handles[proxy::MAX_CONNECTIONS];
  uint8_t n = 0;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (auto &s : g_slots) {
    if (s.state == State::Free || n >= cap ||
        n >= proxy::MAX_CONNECTIONS) {
      continue;
    }
    LinkInfo &li = out[n];
    li = LinkInfo{};
    li.address = s.address;
    li.owner = s.owner;
    li.state = state_name(s.state);
    li.mtu = s.mtu;
    li.connected_us = s.connected_us;
    // m_connHandle is a plain member read, no host lock.
    handles[n] = (s.state == State::Connected && s.client != nullptr)
                     ? s.client->getConnHandle()
                     : BLE_HS_CONN_HANDLE_NONE;
    ++n;
  }
  xSemaphoreGive(g_mutex);

  // Host calls only after g_mutex is released (lock ordering, see
  // CLAUDE.md), and under host_acquire() so a concurrent power_off()
  // cannot deinit the stack mid-query. If BLE went off since the snapshot,
  // report the links with null values rather than ask a dead host.
  // The handle may have been freed and reused by another link in between,
  // so every value is taken only if ble_gap_conn_find still shows the same
  // peer behind it.
  if (!ble_backend::host_acquire()) return n;
  for (uint8_t i = 0; i < n; ++i) {
    LinkInfo &li = out[i];
    const uint16_t h = handles[i];
    if (h == BLE_HS_CONN_HANDLE_NONE) continue;
    ble_gap_conn_desc d{};
    if (ble_gap_conn_find(h, &d) != 0) continue;
    if (address::nimble_le_to_uint64(d.peer_ota_addr.val) != li.address &&
        address::nimble_le_to_uint64(d.peer_id_addr.val) != li.address) {
      continue;
    }
    li.has_params = true;
    li.interval = d.conn_itvl;
    li.latency = d.conn_latency;
    li.supervision_timeout = d.supervision_timeout;
    li.encrypted = d.sec_state.encrypted;
    li.bonded = d.sec_state.bonded;
    int8_t rssi = 0;
    // HCI Read RSSI: 127 is the spec's "not available".
    if (ble_gap_conn_rssi(h, &rssi) == 0 && rssi != 127) {
      li.has_rssi = true;
      li.rssi = rssi;
    }
    li.has_tx_power = conn_tx_power_dbm(h, &li.tx_power);
  }
  ble_backend::host_release();
  return n;
}

void disconnect_unowned() {
  if (g_mutex == nullptr) return;
  NimBLEClient *clients[proxy::MAX_CONNECTIONS];
  Teardown actions[proxy::MAX_CONNECTIONS];
  uint8_t n = 0;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (auto &s : g_slots) {
    if (s.state == State::Free || s.owner != kNoOwner) continue;
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
  if (g_mutex == nullptr) return nullptr;
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

OwnerId owner_of_client(const void *client) {
  if (g_mutex == nullptr) return kNoOwner;
  OwnerId o = kNoOwner;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  for (auto &s : g_slots) {
    if (s.client == client && s.state == State::Connected) {
      o = s.owner;
      break;
    }
  }
  xSemaphoreGive(g_mutex);
  return o;
}

}  // namespace ble_backend::connection
