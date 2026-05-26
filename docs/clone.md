# BLE Clone Mode (`CONFIG_NBP_CLONE`)

Status: **implemented + verified** against an ANT-BLE20PHUB (MAC
`20:A1:11:02:23:45`). Reaches `state=Ready` with `advertising=true`,
1 service / 2 chars mirrored. Reconnect-on-supervision-timeout cycle
stable (~3 s recovery).

Inspired by `clone.py` in [micropython-blebms](https://github.com/PvMz/micropython-blebms),
adapted to NimBLE-CPP on ESP-IDF and adapted to live alongside the
existing ESPHome-API proxy.

## 1. Goal

Mirror one BLE peripheral so that:

1. The proxy presents an **identical-looking GATT server** locally (same
   services, characteristics, descriptors, properties, UUIDs).
2. **Multiple local centrals** (a HA companion app + a vendor app, say)
   can connect simultaneously and all see the same upstream device.
   NimBLE handles notify fan-out; we serialize writes.
3. The existing ESPHome-style raw-advertisement proxy and GATT proxy
   **keep working** — clone runs alongside, not instead of.

Build-flag gated (`CONFIG_NBP_CLONE`, default `n`). Disabled = zero cost.

## 2. Non-goals

- **Multiple cloned upstream devices at once.** v1 mirrors exactly one.
  Multi-device would multiply attribute-table cost and complicate adv
  scheduling; revisit if a user asks.
- **Service Changed propagation.** NimBLE's GATT DB is one-shot; if the
  upstream's table changes, reboot.
- **Encrypted-link forwarding.** If `CONFIG_NBP_SMP=y` and upstream
  requires pairing, the upstream link encrypts as usual. The local
  peripheral image does **not** require pairing; encrypted-only
  upstream chars surface to local centrals as auth-required.
- **Indicate confirmation routing.** Upstream `INDICATE` is downgraded
  to `NOTIFY` locally. The local central never confirms back to the
  upstream peer; the upstream peer's confirmation requirement is met
  by NimBLE's auto-ack on the upstream link.
- **Bond replication.** The cloned image and the upstream bond are
  independent.

## 3. Architecture

```
                 ┌──────────────────────────────────────────────┐
                 │            ble_clone component               │
                 │                                              │
   upstream      │   ┌────────────┐    ┌──────────────────┐     │
   peripheral ◄──┼───┤  upstream  │    │      mirror      │     │
   (one BLE      │   │ (central)  │◄──►│ (local periph)   │     │
    conn)        │   └─────┬──────┘    │  N centrals ─────┼─────┼──► HA app
                 │         │           │                  │     │   vendor app
                 │   ┌─────▼──────┐    │                  │     │   ...
                 │   │ supervisor │    │                  │     │
                 │   │  (task)    │    └──────────────────┘     │
                 │   └─────┬──────┘                             │
                 │         │                                    │
                 │   ┌─────▼──────┐    /clone HTTP endpoint     │
                 │   │   config   │    (NVS backed)             │
                 │   └────────────┘                             │
                 └──────────────────────────────────────────────┘
                          │
                 ┌────────▼─────────────────────────────────────┐
                 │  ble_backend (existing)                      │
                 │   scanner ─ raw adv → ESPHome API            │
                 │   connection slots ─ GATT proxy for HA       │
                 └──────────────────────────────────────────────┘
```

One NimBLE host, two roles active simultaneously:

| Role | Owner | Conn slots used |
|---|---|---|
| Central → upstream cloned peripheral | `ble_clone::upstream` | 1 |
| Central → arbitrary devices (HA GATT proxy) | `ble_backend::connection` | 0..7 |
| Peripheral ← local clone-consumers | `ble_clone::mirror` (NimBLE auto) | 0..N |
| Observer (passive scan) | `ble_backend::scanner` | 0 (radio time only) |

All four share `BT_NIMBLE_MAX_CONNECTIONS` (currently 9). The clone's
upstream link is permanent; local centrals are transient. If you expect
> 1 simultaneous local central, bump max-connections accordingly.

## 4. Lifecycle

`main.cpp` calls, after `ble_backend::start()`:

```cpp
ble_clone::config::load();
ble_clone::init();
ble_clone::register_endpoints(ota::handle());
```

`init()` spawns a supervisor task that runs the following loop:

```
LOAD ──► snapshot = config::snapshot()
  │
  │  snapshot.enabled == false → state=Disabled, sleep 1 s, re-check
  │
  ▼
SCAN ──► wait for advert matching snapshot.address
  │      (uses ble_backend::scanner's existing callback — no second scanner)
  ▼
CONNECT (state=Connecting)
  │   NimBLEClient::connect(addr, async)
  │   on failure → exp backoff, retry
  ▼
DISCOVER (state=Discovering)
  │   client->discoverAttributes() — full walk
  ▼
BUILD (one-shot, only on first successful discovery for this boot)
  │   mirror::build_from(client)
  │     - per upstream svc/char/desc: create matching local NimBLE entity
  │     - register NimBLECharacteristicCallbacks per char
  │     - seed read cache via initial reads of every READ-capable char
  │     - register GATT DB with the host (ble_gatts_start())
  ▼
SUBSCRIBE
  │   for each NOTIFY/INDICATE-capable upstream char:
  │     upstream::subscribe(handle, indicate=false)
  │   notifies start flowing into mirror::on_upstream_notify
  ▼
ADVERTISE (state=Ready)
  │   mirror::start_advertising(upstream_name + name_suffix)
  │
  ▼  (steady state)
  │
  │  upstream drops? → state=Reconnecting
  │                    GOTO CONNECT (skip BUILD on this boot)
  │  config disabled? → stop_advertising + disconnect, state=Disabled
```

The GATT DB is built **once per boot**. Reconnect re-runs DISCOVER only
to verify upstream is still the same shape (logged warning if not) and
re-subscribes to notify handles. Local centrals stay subscribed even
while upstream is reconnecting; they just stop receiving notifications
until upstream is back.

## 5. Module breakdown

Headers already drafted in `components/ble_clone/`:

| Header | Public API | Responsibility |
|---|---|---|
| `clone.h` | `init()`, `register_endpoints()` | top-level wiring |
| `clone_config.h` | `Target`, `load()`, `snapshot()`, `set()` | NVS-backed target |
| `clone_upstream.h` | `State`, `Status`, `init()`, `read()`, `write()`, `subscribe()`, `unsubscribe()`, `current_client()`, `status()` | upstream NimBLEClient + supervisor + write mutex |
| `clone_mirror.h` | `build_from()`, `start_advertising()`, `on_upstream_notify()`, `stats()` | local GATT image + per-char callbacks |

### 5.1 `clone_config`

NVS namespace `"nbp_clone"`. Keys:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `addr` | `u64` | 0 | MSB-first packed MAC (same layout the scanner table uses) |
| `type` | `u8` | 0 | NimBLE address type |
| `enabled` | `u8` | 0 | gate; 0 disables the supervisor |
| `name_suffix` | `str` | `_cloned` | appended to upstream local name in adv |

In-memory snapshot is a `std::atomic<Target>` (or a plain `Target` behind
a mutex if it doesn't fit lock-free). The supervisor re-reads on each
loop iteration.

### 5.2 `clone_upstream`

Owns one NimBLEClient. To avoid touching `ble_backend::connection`'s
slot-pool semantics, the upstream uses its **own** NimBLEClient created
directly via `NimBLEDevice::createClient()` — NimBLE itself caps total
client+server connections at `BT_NIMBLE_MAX_CONNECTIONS`, so there's no
double-bookkeeping needed.

Concurrency: a single FreeRTOS mutex (`g_upstream_mutex`) serializes
all upstream GATT ops. The local-side callback context is the NimBLE
host task — blocking the host on an upstream round-trip is acceptable
because (a) it's <100 ms typically and (b) the upstream is at the front
of the host queue anyway.

`NotifyHook` is invoked from the NimBLE host task; the mirror dispatches
straight to `local_char->notify()` without re-entering the upstream
mutex.

### 5.3 `clone_mirror`

Tables sized at compile time via Kconfig:

- `NBP_CLONE_MAX_CHARS` (default 64) — caps total mirrored chars.
- `NBP_CLONE_VALUE_CACHE_BYTES` (default 64) — per-char read cache.

For each mirrored characteristic:

```
struct MirrorChar {
  uint16_t upstream_value_handle;
  uint16_t local_value_handle;        // assigned by NimBLE on register
  uint8_t  cache_len;
  uint8_t  cache[NBP_CLONE_VALUE_CACHE_BYTES];
  uint8_t  subscribers;               // ref-count from local CCCDs
  uint8_t  props;                     // BLE property mask (for routing)
  NimBLECharacteristic *local;        // owned by NimBLE
};
```

Lookup by `upstream_value_handle` for notify routing is a linear scan
over a `std::array<MirrorChar, NBP_CLONE_MAX_CHARS>` — N is small.

### 5.4 GATT operation routing

| Local-central op | What the mirror callback does |
|---|---|
| Read | If `cache_len > 0` → return cache. Else call `upstream::read()`, fill cache, return. |
| Write w/ response | `upstream::write(..., with_response=true)`, then invalidate cache. NimBLE returns the result code to the local central. |
| Write w/o response | Same, but `with_response=false`, fire-and-forget local ack. |
| Subscribe (CCCD write) | `++subscribers`; if was 0 and char has NOTIFY/INDICATE, call `upstream::subscribe()`. |
| Unsubscribe | `--subscribers`; if drops to 0, call `upstream::unsubscribe()`. |

Upstream notify (`upstream::NotifyHook`):

1. Look up `MirrorChar` by `upstream_value_handle`.
2. Copy payload into `cache` (truncate to `NBP_CLONE_VALUE_CACHE_BYTES`).
3. Call `local->notifyValue(data, len)`. NimBLE fans out to every
   subscribed local central automatically.

## 6. HTTP endpoint contract

Mirrors the `/passkey` style under `CONFIG_NBP_SMP`. Mounted on the OTA
httpd to avoid a second listener.

```
GET  /clone           → {"enabled":bool,"addr":"AA:BB:CC:DD:EE:FF",
                         "type":0|1,"name_suffix":"_cloned",
                         "state":"Ready"|...,"reconnects":N,
                         "services":N,"characteristics":N,
                         "notifies_in":N,"notifies_out":N,
                         "connected_centrals":N}

POST /clone?addr=AA:BB:CC:DD:EE:FF&type=0&enabled=1
                      → 200 + {"ok":true,"reboot_required":true}

POST /clone/disable   → 200; stops adv + drops upstream link (no reboot)
```

`reboot_required` is `true` whenever `addr` changes — see §7.

## 7. NimBLE constraints

**GATT DB is one-shot.** `ble_gatts_start()` builds the host's
attribute table from the current registered services and freezes it.
Re-registering services after `start()` is not supported by NimBLE.
Therefore: target MAC change → reboot. Disable/enable while target
unchanged → safe, no reboot.

**Scanner suspension during connects.** The existing
`scanner::resume()` already handles this for `ble_backend::connection`.
The clone uses the same mechanism — `connection::connect`'s
explicit `scan->stop()` is the right pattern, copy it.

**Conn budget.** `BT_NIMBLE_MAX_CONNECTIONS=9` is shared by ALL
connections. Worst case at v1: upstream(1) + HA-proxy(8) + local
centrals(?). If you regularly run > 0 local centrals, bump this.

**Adv vs scan radio time.** Continuous scan duty is 50% (window 30 ms /
interval 60 ms). Advertising slots into the off-window. Some local
centrals' connect attempts will need 1–2 adv intervals to land; that's
fine.

## 8. Threading

| Context | Reads | Writes |
|---|---|---|
| NimBLE host task | upstream notify dispatch; local GATT callbacks; advertising; scan callbacks | local char value updates via NimBLE API |
| Clone supervisor task | config snapshot; upstream state; reconnect loop | upstream state machine |
| HTTP handler | config snapshot read | `config::set()` (NVS write + atomic swap) |
| Existing `ble_adv_flush` | (unchanged) | — |

Locking:

- `config`: atomic snapshot (Target is < 32 B, may need a mutex if the
  compiler refuses lock-free; either way exposes only `snapshot()` /
  `set()`).
- `upstream`: one mutex serializes all GATT ops + state transitions.
- `mirror`: subscriber ref-counts updated under the NimBLE host context
  only (CCCD writes deliver in that context), so no extra lock needed.

## 9. Sizing impact

(See estimate from §"Code size" / §"Flash impact" in the original
proposal — repeated here for completeness.)

| | When `NBP_CLONE=n` | When `NBP_CLONE=y`, not enabled in NVS | When active, one device cloned |
|---|---|---|---|
| Flash | 0 | +~20 KB | +~20 KB |
| BSS / heap | 0 | ~1 KB (supervisor task stack reserve) | ~10–15 KB |
| NimBLE attr table | 0 | 0 | 3–4 KB (depends on device) |
| Conn slots | 0 | 0 | 1 + (local centrals) |

`~1000 LOC` of new C++ total across the four `.cpp` files (estimated
per-file: clone 250 / upstream 200 / mirror 300 / config 100).

## 10. Failure modes

| Failure | Recovery |
|---|---|
| Upstream not seen by scanner | Loop in SCAN state, log every 30 s |
| Connect fails | Exp backoff 1 → 30 s, never give up |
| Discovery fails / truncates | Log warning with counts; proceed with what we have |
| Local mirror build exceeds `NBP_CLONE_MAX_CHARS` | Truncate, log, continue (vendor app sees fewer chars but proxy is alive) |
| Upstream drops mid-session | Mark cache stale, transition to Reconnecting; local centrals' reads start hitting upstream (which will fail until reconnect — surface as `BLE_ATT_ERR_UNLIKELY` to local central) |
| Local central writes to a char whose upstream is currently disconnected | Return `BLE_ATT_ERR_UNLIKELY` |
| NVS write of new target fails | `set()` returns false; HTTP returns 500 |
| Upstream peripheral's GATT shape changes between sessions | Log "shape changed since boot" warning; routing table still uses the boot-time handle map — chars whose handles moved will misroute. Suggested fix: prompt user to reboot. |

## 11. Open questions

1. **Bonded upstream.** Should we persist the bond under
   `BT_NIMBLE_NVS_PERSIST=y` so the clone survives reboots without
   re-pairing? Probably yes (it's free given `CONFIG_NBP_SMP=y`).
2. **Dashboard surface.** Worth adding a row in `/stats.json` and a
   panel below the device table? Likely yes — reuse the `web_ui` pattern
   from `stats.cpp`.
3. **Read cache TTL.** Currently invalidates only on write or notify.
   Should reads also re-validate if older than N seconds? Defer.
4. **Adv name length.** ESP32 + NimBLE caps adv name at 29 B. Long
   upstream names + suffix may need truncation. Truncate from middle?
   From end? Probably end.
5. **MTU.** Upstream and each local central have independent MTUs. We
   should call `client->exchangeMTU()` upstream and `setMTU(247)` (per
   `ble_backend.cpp`) is already in effect for local. Notifies will be
   capped at `min(upstream_mtu, smallest_local_mtu) - 3`.

## 12. Plan

1. Land Kconfig + skeleton headers + CMakeLists (done).
2. `clone_config.cpp` + `/clone` HTTP — testable on its own with no BLE.
3. `clone_upstream.cpp` + supervisor task — testable by scanning &
   connecting to a fixed MAC and printing the discovered table.
4. `clone_mirror.cpp` — gate this behind a build but unit-testable by
   pointing at a known device (a Victron SmartShunt is a good target,
   matches existing SMP work).
5. End-to-end: clone the SmartShunt, then connect to the proxy with
   VictronConnect app + Home Assistant Victron integration
   simultaneously and verify both see live data.
6. Dashboard panel.

## 13. Implementation pitfalls (discovered during bring-up)

These are the non-obvious things I tripped over getting clone to
`Ready`. If you're re-implementing this in another codebase / on
another stack, read this section first.

### 13.1 NimBLE-CPP's `Server::start()` requires the host to be IDLE

`NimBLEServer::start()` calls `resetGATT()` which calls
`ble_gatts_reset()` → `ble_svc_gap_init()` → `ble_gatts_add_svcs()`.
The last function returns `BLE_HS_EBUSY` while ANY BLE connection is
up, then hits `SYSINIT_PANIC_ASSERT(rc == 0)` at
`ble_svc_gap.c:395` and panics. **Reproducer:** connect a client →
discover → call `Server::start()`. Panic.

Workaround: **drop the upstream link before calling `Server::start()`**,
then reconnect. The supervisor flow is:

```
connect → discover → mirror::build_from(client)   [extracts UUIDs,
                                                    builds local svc
                                                    + char tree but
                                                    doesn't start the
                                                    server]
       → disconnect upstream (wait for onDisconnect — not just
                              isConnected; see 13.3)
       → mirror::finalize_server()  [calls Server::start() with the
                                     host now mutable]
       → reconnect upstream
       → discover (again — quick this time)
       → mirror::rebind_upstream(client)  [match by UUID; refresh the
                                            RemoteCharacteristic* ptrs]
       → prime caches + subscribe + start advertising
```

This is why `mirror::build_from` and `mirror::finalize_server` are
separate calls.

### 13.2 `CONFIG_BT_NIMBLE_DYNAMIC_SERVICE=y` is mandatory

Without it, `ble_gatts_reset()` skips the loop that frees the
`svc_entries` list (gated `#if MYNEWT_VAL(BLE_DYNAMIC_SERVICE)` in
`ble_gatts.c:3314`). The next call to `ble_svc_gap_init` finds the
GAP service still registered, `ble_gatts_add_svcs` returns
EALREADY-equivalent, panic. Required for any deferred-registration
pattern, not just clone.

### 13.3 Disconnect is async; `isConnected()` is unreliable

`NimBLEClient::disconnect()` returns immediately. `isConnected()`
flips at the API level but the host's `ble_hs` state hasn't yet
processed the disconnect-complete event. `ble_gatts_mutable()` only
returns true after the host has seen the link close.

Solution: wait for the actual `NimBLEClientCallbacks::onDisconnect`
callback. The supervisor holds a `g_disconnect_sem` that the cb signals;
the supervisor blocks on it with a 3 s timeout. After the sem fires,
add a small extra `vTaskDelay(50ms)` for any in-flight `ble_hs` work
to settle.

### 13.4 NimBLE-CPP sync `connect()` blocks forever on an unresponsive peer

`NimBLEClient::connect(addr, ..., asyncConnect=false, ...)` blocks on
`NimBLEUtils::taskWait(taskData, BLE_NPL_TIME_FOREVER)`. The
`setConnectTimeout()` value applies to async only — there is no
bounded wait in sync mode. If the peer doesn't respond (out of range,
already paired with someone else, just power-cycled), the supervisor
task locks up forever.

Solution: **always use async connect** for the supervisor.
`g_connect_sem` is signalled from `onConnect` or `onConnectFail`;
supervisor blocks on it with a `CONNECT_TIMEOUT_MS` bound and calls
`disconnect()` on timeout.

### 13.5 Watchdog kicks during long discoveries

`ble_gattc_disc_*` walks the upstream attribute table one round-trip
at a time. A device with ~50 chars takes >10 s. Default ESP-IDF task
watchdog timeout (`CONFIG_ESP_TASK_WDT_TIMEOUT_S=10`) will fire,
panic the supervisor, reboot. Bump to **30 s** for the clone bring-up
window. (The idle-task-on-CPU0 watchdog hint applies even though the
supervisor task itself yields on the GATT-wait semaphore — the BLE
host on CPU 0 stays busy long enough to starve idle.)

### 13.6 Skip the standard services 0x1800 and 0x1801

NimBLE auto-registers GAP (0x1800) and GATT (0x1801) services as
part of host init. Mirroring them via `createService()` adds
duplicates that NimBLE silently rejects (or asserts on, depending on
version). Detect by 16-bit UUID and skip in `build_from`.

### 13.7 CCCD is auto-created — don't mirror 0x2902

Every NimBLECharacteristic with NOTIFY or INDICATE in its property
mask automatically gets a CCCD (0x2902) descriptor from NimBLE. If
you replicate the upstream's CCCD with `createDescriptor`, the local
char ends up with two CCCDs and NimBLE rejects the second. Filter
out 0x2902 explicitly in the descriptor walk.

### 13.8 Compile-time vs runtime NimBLE log level

`CONFIG_BT_NIMBLE_LOG_LEVEL_DEBUG=y` adds ~40 KB of BSS (string
literals plus the 64 KiB web-console ring) — enough to leave so
little heap that `discoverAttributes` runs out of mbufs and crashes
silently. Even if you compile DEBUG in, the runtime level is set by
`apply_log_overrides_from_nvs()` (defaults to WARN). To see verbose
NimBLE output, `POST /level?nimble=4` at runtime.

For development: keep compile-time at INFO, flip runtime to DEBUG
when investigating.

### 13.9 ESP32-S3 caps NimBLE at 9 connections

`CONFIG_BT_NIMBLE_MAX_CONNECTIONS` has Kconfig `range 1 9` for
ESP32-S3. Can't go higher. With clone using 1 slot for upstream,
that leaves 8 for HA-proxy GATT. If you regularly drive HA at full
capacity, the clone's upstream may collide with the 9th proxy
connection — but in practice we never see 9 simultaneous HA-side
sessions.

### 13.10 Worker-task pattern for upstream writes

NimBLE-CPP's `writeValue()` with response calls
`NimBLEUtils::taskWait` to block on the GATT response. If this is
called from the NimBLE host task (e.g. inside an `onWrite` callback
on a LOCAL characteristic), the host blocks waiting for itself to
deliver the response. Deadlock.

The mirror's `onWrite` callback never calls `writeValue` directly.
It captures the value and enqueues a `WriteRequest` onto a queue.
The supervisor task (not the host task) drains the queue and does
the actual upstream `writeValue`. Write-no-response would be safe to
do inline (it doesn't block) but for simplicity all writes go through
the queue.

### 13.11 `for_each_*` must snapshot before iterating

The mirror's iteration helpers (`for_each_subscribable`,
`for_each_readable`) used to hold `mirror::g_mutex` across the
callback. The callback called `chr->subscribe()` or `chr->readValue()`
synchronously — those block on a GATT round-trip. If an upstream
NOTIFY arrives in that window, `on_upstream_notify` tries to take
`g_mutex` from the NimBLE host task and the host blocks. Then the
GATT round-trip we're waiting on can't deliver. Deadlock.

Fix: snapshot the `(chr, handle, …)` tuples under the mutex into a
local `std::array`, release the mutex, then iterate without it.

### 13.12 HTTP URI handler cap

`esp_http_server` defaults to `max_uri_handlers = 8`. The stats
component already registers 14, OTA adds /update = 15. Adding /clone
GET + POST hits 17 and the POST silently fails (404). Bumped to **20**
in `ota.cpp`. If you grow more endpoints, raise it again.

### 13.13 Skip `target_link_libraries(... PUBLIC ...)` on INTERFACE libs

`idf_component_register` makes the component library INTERFACE when
SRCS is empty (i.e. when the component is gated off via Kconfig).
CMake rejects `target_link_libraries(<INTERFACE> PUBLIC <lib>)`. The
fix in both `ble_clone/CMakeLists.txt` and `ble_httpd/CMakeLists.txt`:

```cmake
if(CONFIG_NBP_CLONE)   # or NBP_BLE_HTTPD
    idf_component_get_property(nimble_lib h2zero__esp-nimble-cpp COMPONENT_LIB)
    if(nimble_lib)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${nimble_lib})
    endif()
endif()
```

### 13.14 Pairing-required upstream needs `onPassKeyEntry`

If the upstream peer demands SMP (Victron SmartShunt and similar),
`ClientCb::onPassKeyEntry` must respond with the static passkey via
`NimBLEDevice::injectPassKey`. Without it, `discoverAttributes`
times out because most characteristic reads need an encrypted link.
The clone reuses `ble_backend::connection::get_passkey()` so it's
configured via the same `/passkey` HTTP endpoint as the ESPHome-API
side.

## 14. Reference implementation map

| File | Responsibility |
|---|---|
| `clone_config.{h,cpp}` | NVS-backed target (addr, type, enabled, name_suffix); thread-safe snapshot |
| `clone_upstream.{h,cpp}` | Supervisor task; async `connect` + `g_connect_sem` + `g_disconnect_sem`; write queue drain loop; per-upstream notify hook |
| `clone_mirror.{h,cpp}` | `MirrorChar` table; `build_from` (extract + create local entities) + `finalize_server` (register GATT DB); `rebind_upstream`; per-char callback that enqueues writes |
| `clone.{h,cpp}` | Top-level init wiring; `/clone` GET/POST HTTP endpoints |

Tasks (FreeRTOS):

- `clone_sup` — supervisor task (12 KB stack, prio 4). Runs the
  state machine; drains the write queue between iterations.
- NimBLE host task (existing) — fires `onConnect`/`onDisconnect` and
  the per-char notify callback. Mirror's local-side `onWrite`
  callback runs here too.

State machine snapshot (`upstream::State`): `Disabled → Idle → Scanning
→ Connecting → Discovering → Ready → Reconnecting → Connecting → …`.
