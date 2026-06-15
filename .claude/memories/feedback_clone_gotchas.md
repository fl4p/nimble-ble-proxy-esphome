---
name: feedback-clone-gotchas
description: "BLE clone feature has 4 critical non-obvious gotchas that cause silent failures or crashes during development"
metadata: 
  type: feedback
---

The BLE clone feature (`CONFIG_NBP_CLONE`) has several implementation pitfalls discovered during bring-up that aren't obvious from the code or NimBLE docs. Hitting these unaware wastes hours.

**Why:** The clone bridges NimBLE client and server roles in one supervisor task, which constrains ordering in non-obvious ways. CoreBluetooth/Bleak have strict expectations about GATT structure. Some bugs only manifest under load or after rough disconnects.

**How to apply:** Before debugging clone issues, verify these four things first:

### 1. **Disconnect upstream BEFORE calling Server::start() — critical ordering**

`ble_gatts_mutable()` returns `false` while any BLE connection is active (even the upstream client). Calling `ble_gatts_add_svcs` while `ble_gatts_mutable()` is false returns `EBUSY` and triggers the `ble_svc_gap_init` panic.

**Fix:** The mirror's state machine must:
1. `build_from(client)` — capture upstream structure while connected
2. **Disconnect upstream** (wait for `onDisconnect` callback via `g_disconnect_sem`)
3. Call `finalize_server()` / `Server::start()`
4. Reconnect upstream and rebind pointers

If you skip the disconnect, you'll see: `E (XXX) ble_svc_gap: ble_svc_gap_init failed; rc=11` and a reboot loop.

### 2. **Skip descriptor mirroring — CoreBluetooth rejects partial descriptors**

Mirroring upstream descriptors (e.g. 0x2901 User Description) without replicating their values causes macOS CoreBluetooth to fail characteristic discovery with `CBErrorDomain Code=0 "Unknown error"`. Bleak then can't read any characteristics.

**Fix:** Skip ALL descriptors in the mirror (both custom and standard). The CCCD (0x2902) is auto-created by NimBLE when you register a NOTIFY characteristic, so you don't need to mirror it. The reference `clone.py` also skips descriptors entirely.

### 3. **Enable CONFIG_BT_NIMBLE_DYNAMIC_SERVICE=y — static GATT DB won't clear**

Without this flag, `ble_gatts_reset()` doesn't actually free the service-entries list. On reboot or re-target, the old services are still there, and `add_svcs` fails trying to re-add duplicates.

**Fix:** Add to `sdkconfig.defaults`:
```
CONFIG_BT_NIMBLE_DYNAMIC_SERVICE=y
```

### 4. **Use async connect with bounded timeout — sync connect hangs forever**

NimBLE-CPP's sync `client->connect(...)` has no real timeout if the peer doesn't respond (or stops responding mid-connect). The supervisor task blocks forever, triggering the watchdog.

**Fix:** Use `asyncConnect=true` and bound the wait with a `g_connect_sem` (or similar) that times out after `CONNECT_TIMEOUT_MS`. The supervisor should retry after backoff, not hang.

### 5. **Bump WDT timeout to 30s for slow peripherals**

GATT discovery on slower devices (e.g. BMS or Victron) can take >10s. The default 10s watchdog fires and reboots.

**Fix:** In `sdkconfig.defaults`:
```
CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
```

### Related gotchas (documented in docs/clone.md §13)

- **Mutex deadlock in `for_each_subscribable/readable`**: Snapshot pointers under mutex, release, then call BLE ops
- **Advertising name in scan response**: Call `setName()` BEFORE `enableScanResponse(true)`
- **Single-target only**: One GATT server, one adv context — multi-device cloning needs BLE 5 ext-adv + multiple NimBLE hosts (not supported on ESP32)

When the clone won't discover / won't reach Ready / crashes early, check these five in order: CONFIG flag → disconnect dance → descriptors → async connect → WDT.
