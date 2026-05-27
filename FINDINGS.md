# nimble-ble-proxy — bring-up findings

End-to-end bring-up notes from 2026-05-26 onward. The firmware speaks
the aioesphomeapi plaintext protocol over TCP and exposes itself to
Home Assistant as a regular ESPHome Bluetooth proxy, but with NimBLE
as the BLE stack instead of Bluedroid.

## Status

Working and verified end-to-end against an unmodified HA 2026.5.4
instance:

- mDNS announce → HA auto-discovery → user confirm → config entry loaded
- HA registers the device as a 9-slot Bluetooth scanner (visible via
  `bluetooth/subscribe_connection_allocations`)
- Raw adverts flow from us to HA; HA routes peripherals to whichever
  scanner reports best RSSI — we win for some, lose for others (correct
  behaviour)
- `bluetooth_device_connect` round-trips: HA → our proxy → NimBLE
  async connect → onConnectFail callback → `ConnectionResponse{error:13}`
  back to HA.
- **Successful GATT connect + discovery validated** against an
  ANT-BLE20PHUB BMS and a Victron SmartShunt (the latter under
  `CONFIG_NBP_SMP` static-passkey pairing). Clone mode runs both as
  the source of truth — see `docs/clone.md` Status section for the
  verified-throughput numbers.

## Bugs found during bring-up

These were silent until end-to-end testing forced the issue. Worth
calling out because future-me will look here.

### 1. nanopb string sizing off by one

`max_size:17` for `mac_address` gave a `char[17]` array — only 16
chars + null, but `"AA:BB:CC:DD:EE:FF"` needs 18 bytes. `snprintf`
wrote past the array (UB), nanopb later complained
`pb_encode failed: unterminated string`. Symptom: HA's discovery flow
appeared to succeed but the API connection never finished the handshake
— HA looped silently reconnecting.

**Fix:** bumped all string `max_size` to include null terminator;
`format_mac` now takes an explicit capacity argument. Lesson: nanopb's
`max_size` includes the terminator, and our snprintf must match.

### 2. Single-client API server

`listen(fd, 1)` plus a single `g_active_fd` int meant only one client
could connect at a time. HA grabs the slot immediately after boot, so
the smoke test couldn't connect to verify anything.

**Fix:** multi-client server matching ESPHome's behaviour
(`MAX_API_CLIENTS=4`): per-connection FreeRTOS task with its own RX
buffer, `g_client_fds[]` set under mutex, `send_async` broadcasts to
every live client. `bt_handlers` subscription state is ref-counted;
counters reset only when the LAST client leaves.

### 3. REMOTE_CACHING feature flag required by aioesphomeapi

We initially advertised `0x23` = `PASSIVE_SCAN | ACTIVE_CONNECTIONS |
RAW_ADVERTISEMENTS`. Modern aioesphomeapi (and therefore HA's
bluetooth integration) refuses to issue connect requests through a
proxy without bit 2 (`REMOTE_CACHING`) and raises
`ValueError("update to 2022.12.0+")` client-side before sending
anything on the wire.

**Fix:** added bit 2 → `0x27`. We don't actually cache GATT services
(we re-discover on every connect), but HA only requires the flag to be
set; it never checks. The first connection through us is slightly
slower than a "true" caching proxy; subsequent ones in the same session
look identical. Real caching is a v0.2 concern.

### 4. Deadlock: TX mutex held during bt_handlers

`dispatch_one` held `g_tx_mutex` from "start of handshake" through
"end of bt_handlers". A bt handler that ran a synchronous NimBLE op
(connect, read, etc.) could trigger a NimBLE callback in the same task
that called `api_server::send_async`, which then tried to re-acquire
the same mutex → deadlock.

**Fix:** the dispatcher now holds the mutex *only* through the
handshake path. `bt_handlers::handle` runs with the mutex released;
all wire output from bt_handlers goes through `send_async`, which
acquires the mutex itself. The `Context.send_response` callback (and
`response_buf` staging area) were removed entirely.

### 5. MAC byte order was reversed in every adv (silent until verified)

`scanner::onResult` did
`rec.address = address::swap6(static_cast<uint64_t>(addr))`. That was
backwards: `NimBLEAddress::operator uint64_t()` already memcpys the
6 LE bytes onto a uint64 on an LE host, placing the MAC's MSB in bits
40-47 of the int — which is exactly the layout aioesphomeapi formats
back into MSB-first hex. Applying `swap6` *additionally* reversed it,
so every adv we forwarded had its MAC bytes flipped.

Symptom: undetectable until you check a specific MAC. HA still
"works" because HA just uses whatever address we report — both proxies
in our setup will agree on the (wrong) address for the same peripheral,
RSSI routing still functions, adv counts on the dashboard look fine.
The bug surfaced only when we scanned for the ANT BMS target
`20:A1:11:02:23:45` and saw `45:23:02:11:A1:20` in the output — exact
byte reversal.

**Fix:** removed both call sites (scanner + notify_cb) and deleted
`swap6` from `ble_backend::address`. After the fix the ANT BMS shows
up at its real MAC and is reachable at -76 dBm; the earlier "out of
range" verdict was a misdiagnosis caused by the reversed address.

Lesson: any time the proxy invents an integer that's printed back as a
MAC by the client, validate it against a known peripheral early.
"All MACs look plausible" is not a verification — every MAC reversed
also looks plausible.

### 6. `setConnectTimeout` takes ms, not seconds

`s->client->setConnectTimeout(proxy::CONNECT_TIMEOUT_MS / 1000)` —
divided 8000 ms by 1000 because the previous lib version (or our
assumption from `ble_gap_connect`'s API) took seconds. NimBLE-Cpp 2.5
takes **milliseconds**, default 30000. We were telling it to give up
after 8 ms.

Symptom: every connect failed with `BLE_HS_ETIMEOUT` (reason=13) about
40 ms after issue, no matter peer state, distance, or address. Looked
like "the proxy isn't actually trying" but was actually "the proxy gave
up before the controller could send a single scan packet."

Verified with serial: pre-fix `connect → onConnectFail` delta 40 ms;
post-fix delta = configured timeout (8 s) almost to the millisecond.

**Fix:** drop the `/ 1000`. Commit `cc2e4d3`.

### 7. Scanner stays dead after every connect attempt

NimBLE auto-suspends the active scan when starting a GAP connect
procedure (only one GAP procedure at a time on the controller). After
`onConnect` / `onConnectFail` / `onDisconnect`, the scan is **not**
auto-resumed by NimBLE — and our code didn't restart it either.

Symptom subtle and counterintuitive: dashboard showed 30+ adverts/s
right after boot, then the first connect attempt killed the scan
forever, and aioesphomeapi clients subscribing to raw adverts saw
zero packets. `/stats.json` froze at the adverts count from before the
first connect attempt, even after 20+ seconds of "should be scanning."

Easy to miss because the adv counter was added *for* diagnosing this,
and proved its own usefulness immediately: counter at 166 → wait 20 s
→ counter at 166. No more guessing about RF range vs. forwarding bugs.

**Fix:** `scanner::resume()` helper (idempotent `start()` if not
currently scanning), called from `onConnect`, `onConnectFail`, and
`onDisconnect` in `connection.cpp`. Serial confirms within ~10 ms of
any connect event: `NimBLE: GAP procedure initiated: discovery; …
ble.scan: scan resumed`.

### 8. **`NimBLEAddress(uint8_t[6], type)` reverses the bytes** (the actual cure)

This was the headline bug of today's session. NimBLE-Cpp's
byte-array address constructor:

```cpp
NimBLEAddress::NimBLEAddress(const uint8_t address[6], uint8_t type) {
    std::reverse_copy(address, address + 6, this->val);
    this->type = type;
}
```

It **reverses** whatever you give it before storing into `val[]`. Our
connect path was:

```cpp
uint8_t le[6];
address::uint64_to_nimble_le(address, le);   // [0x45,0x23,0x02,0x11,0xa1,0x20]
NimBLEAddress nimble_addr(le, address_type); // val = [0x20,0xa1,0x11,...,0x45]
```

So we ended up with `val[]` in MSB-first order, but NimBLE uses `val[]`
directly as the on-air LE wire bytes — meaning the CONNECT_REQ was sent
for an entirely fictional peer. Every connect timed out (reason=13)
because that fictional address never advertised.

How it masked itself:

- The scanner path works fine — adverts come from NimBLE in
  `NimBLEAdvertisedDevice` form, so we never hit this constructor; the
  rec.address int we forward via `static_cast<uint64_t>(addr)` is
  already correct end-to-end.
- The connect serial log shows `peer_addr=45:23:02:11:a1:20`, which
  *looks* like NimBLE's normal LSB-first print convention for the
  correct address — but is actually the reversed bytes printed in
  storage order. Two wrongs cancelling out visually.
- We spent significant time chasing RF range / NimBLE timeouts /
  scan params / explicit scan-stop before checking what bytes the
  constructor actually stored. Lesson: when "the controller can't see
  a device it just saw," verify the bytes that went into the HCI
  command, not just the bytes the application thinks it sent.

**Fix:** use `NimBLEAddress(uint64_t, type)` — the uint64 constructor
takes MSB-first hex (`0x20a111022345` → MAC 20:A1:11:02:23:45), which
matches what aioesphomeapi sends. Verified end-to-end:
`onConnect 20a111022345 mtu=136` 290 ms after issuing connect, at
-60 dBm.

The byte-array `uint64_to_nimble_le` helper is now unused on the
connect path.

### 9. `handle_get_services` stack-overflows the 8 KiB api_client task

The first time a real peer completed GATT discovery, the proxy
crashed and the TCP socket got RST'd mid-`bluetooth_gatt_get_services`.
Symptom from HA / bleak-esphome was a 25 s timeout followed by
"unexpected disconnect from ESPHome API."

Root cause: `gatt_discovery::run()` stack-allocated a
`ServicesEncodeCtx` wrapping `proxyapi_BluetoothGATTGetServicesResponse`.
nanopb generates that struct with worst-case fixed-size arrays — the
`_size` macros say `proxyapi_BluetoothGATTGetServicesResponse_size =
25171` (~25 KiB). The api_client tasks have an 8 KiB stack. Every
real discovery overran the canary and panic-rebooted the chip.

Hadn't surfaced during bring-up because at that time the only candidate
peer was out of range, so the connect path was only exercised through
`onConnectFail` — `discoverAttributes()` never ran.

**Fix:** `std::unique_ptr<ServicesEncodeCtx>` for the heap allocation;
`publish::send_async` invokes the encode callback synchronously before
returning, so the unique_ptr scoped to `run()` is sufficient — no
ownership transfer needed. Verified end-to-end against the ANT BMS:
connect → get_services returns 3 services (0x1800 + 0x1801 + 0xFFE0
with chars 0xFFE1 + 0xFFE2) → disconnect cleanly.

Lesson: when a generated struct's `_size` macro is in the tens of KB,
the C struct is at least that large even when only partially populated,
because the inner arrays are statically sized. Don't put it on a stack
that fits in `idf.py size`.

### 10. Synchronous NimBLE connect blocks the per-client task

`NimBLEClient::connect(..., asyncConnect=false)` blocks the caller for
up to 8 seconds while it scans + connects. During that time the client
task can't read its socket, so dropped peers pile up as half-open
connections. Once all 4 client slots are stuck, HTTPD (sharing the
LWIP socket pool) starts logging `accept errno=23` (ENFILE) and the
OTA endpoint becomes unreachable. Recovery required a serial reset.

**Fix:** `asyncConnect=true`. The connect() call returns immediately
after issuing the GAP command; `ClientCb::onConnect` /
`onConnectFail` from the NimBLE host task drive the success/failure
path and emit the `BluetoothDeviceConnectionResponse` via send_async.
Slot bookkeeping moved into the callbacks.

## ANT BMS specifics (observed on `20:A1:11:02:23:45`)

Captured during the connect bring-up because it's the canonical test
peripheral and useful context for anyone adding BMS-aware features
later.

- **Address**: Public (`addr_type=0`). Stable across reboots/cycles.
- **Adv PDU**: `advType=0` (ADV_IND) — connectable + scannable +
  legacy. Goes "stealth" (stops advertising) while a connection is
  active, accepts only one client at a time.
- **Adv interval**: ~100 ms when idle. Multiple adverts per scan
  window at strong signal.
- **Adv payload** (21 B, AD-formatted):
  - `02 01 06` — Flags = LE General Discoverable + BR/EDR not supported
  - `05 02 e0ff e7fe` — Incomplete 16-bit service UUIDs: **0xFFE0**
    (primary, matches the `aiobmsble` matcher) + 0xFEE7
  - `0b ff 5706 88a0 20a111012345` — Manufacturer Specific Data,
    company ID **0x0657** (not the `0x2313` that `ant_bms.py` matches
    against — this unit is a different vendor revision or an
    `ant_leg_bms` variant; matcher in HA may need adjusting)
- **MTU after connect**: 136 (NimBLE negotiates this from 247 down).
- **GATT shape** (per `aiobmsble/bms/ant_bms.py`, not yet verified in
  proxy): service `0xFFE0`, single characteristic `0xFFE1` with both
  notify and write properties. Application protocol uses frames
  `HEAD=0x7E 0xA1 … TAIL=0xAA 0x55` with Modbus CRC; status command
  `0x01`, device command `0x02`.
- **Range**: at the desk where this was developed, the BMS sits at
  -55 to -69 dBm via the proxy's ESP32-S3 onboard antenna. At -80 dBm
  (across a wall) the connect succeeds but is unreliable; below -85
  expect frequent `BLE_HS_ETIMEOUT`. Once connected, the link is far
  more tolerant — supervision timeout = 2.56 s by default.
- **Phone app coexistence**: if the user's official BMS app is
  connected, the proxy connect fails fast because the BMS suppresses
  adverts while connected. No way to detect this from the proxy side
  beyond "we used to see it, now we don't" — worth logging if the
  scanner stops seeing a previously-known address for >N seconds.

The reference Python implementations in `/Users/fab/dev/pv/micropython-blebms`
(uPython aioble, batmon-ha via bleak) both connect successfully and
have been our ground-truth for protocol behaviour.

## Architecture

### Component layout

```
nimble-ble-proxy/
├── components/
│   ├── api_proto/      nanopb-generated subset of api.proto + wire IDs
│   ├── api_server/     plaintext frame codec, dispatcher, handshake, bt handlers
│   ├── ble_backend/    NimBLE wrapper: scanner, connection slots, gatt discovery
│   ├── mdns_announce/  _esphomelib._tcp.local on port 6053
│   ├── ota/            HTTP POST /update on port 80
│   ├── stats/          status endpoint (user-added)
│   └── wifi_sta/       STA bring-up from gitignored wifi_creds.h
└── main/main.cpp       wires everything together after WiFi has an IP
```

### Cross-component publish hook

`ble_backend` doesn't depend on `api_server` at build time. Instead,
`ble_backend/publish.{h,cpp}` exposes a small DI seam:

```cpp
ble_backend::publish::install(&api_server::send_async,
                              &api_server::has_active_client);
```

main wires this at boot. This breaks what would otherwise be a build
cycle (api_server requires ble_backend for the bt_handlers bridge;
ble_backend wants to publish async via api_server).

### Concurrency

| Task                  | Stack | Role |
|-----------------------|------:|------|
| listener_task         |  4 KiB | accept() loop on :6053 |
| api_client* (×N)      |  8 KiB | per-connection dispatch; one spawn per accepted client |
| ble_adv_flush         |  4 KiB | drains scanner ring; wakes on notify or 100 ms tick |
| NimBLE host task      |  5 KiB | NimBLE callbacks (onResult, onConnect, etc.) |
| HTTPD                 |  8 KiB | OTA + stats endpoints |
| wifi/lwip/etc.        |   IDF  | system |

`g_tx_mutex` serializes everyone's writes to the shared `g_tx_buf`
+ socket sends. `g_clients_mutex` guards the `g_client_fds[]` set.
Per-slot connection state has its own mutex in connection.cpp.

### Wire framing

`[0x00 indicator][size varint ≤3B][type varint ≤2B][payload]`.
`prepend_header` writes into the front of a buffer the caller has
already filled at `&buf[MAX_HEADER_LEN]`, then returns an offset so
the entire frame can be sent in one `send()`. Matches
`esphome/components/api/api_frame_helper_plaintext.cpp:72-180`.

### Address conversion

NimBLE stores 6-byte addresses in little-endian order; aioesphomeapi
packs them into a uint64 MSB-first. `ble_backend::address::swap6`
handles the conversion at the NimBLE boundary.

## Verification

What was tested and how, in order:

1. **Boot log via serial** — confirms WiFi, mDNS, NimBLE, API listener
   come up cleanly with the configured max_conn.
2. **`device_info` round-trip via aioesphomeapi** — confirms full
   handshake (Hello/Connect/DeviceInfo/Ping/ListEntities) and that
   `bluetooth_proxy_feature_flags=0x27` round-trips correctly.
3. **15s raw-adv subscription** — counted adverts and unique addresses
   to confirm scanner batching + flush task work. 220 adverts from 10
   unique MACs with sensible RSSI distribution.
4. **HA websocket queries** to verify HA's side:
   - `config_entries/get` → our entry loaded
   - `config/device_registry/list` → device record with MAC binding
   - `bluetooth/subscribe_connection_allocations` → we appear as a
     9-slot scanner
   - `bluetooth/subscribe_advertisements` → adverts attributed to us
     beat the other proxy on 14/25 events in a 10s window
5. **Multi-client smoke** — smoke test connected concurrently with
   HA; both saw expected protocol state.
6. **Connect probe** to an out-of-range BMS — exercised the failure
   path and confirmed `ConnectionResponse{error:13}` makes it back to
   the client.
7. **OTA round-trip** — `curl --data-binary @firmware.bin
   http://nimble-proxy.local/update` flashed a new build to the
   inactive partition, device rebooted into it, ping-ponging between
   ota_0 and ota_1.

## Update 2026-05-26 (Session 3) — Direct A/B with esphome-ant-bms on the same chip

Built `scripts/ant-probe/ant-probe.yaml` — an ESPHome firmware using
syssi/esphome-ant-bms@main as an external component, baked with our
8 MB partition table so it OTA-flashes cleanly into `ota_1` via the
proxy's existing `/update` endpoint. Return path is a template button
in the esphome YAML whose lambda calls
`esp_ota_set_boot_partition(<ota_0>)` + `esp_restart()`. Proxy in
ota_0 is never overwritten — round-trip is just two button presses
worth of partition flips.

**Result on the same chip, same WiFi+API load, same RF, same BMS:**

| Firmware | Stack | Notify PDUs in 25 s | Status |
|---|---|---|---|
| nimble-ble-proxy | NimBLE-Arduino | **0** | ❌ |
| syssi/esphome-ant-bms | Bluedroid | dozens, steady | ✅ |

esphome's log shows the BMS pushing 20-byte notify PDUs with
`7E.A1.11.…` status frames and cell voltages around `0x0D2C` (3.36 V
per cell). That **eliminates** the most-cited hypotheses from earlier
sessions:

- ❌ Not WiFi+BT coexistence (esphome runs identical coex)
- ❌ Not the BMS being stuck (it speaks fine from esphome ~30 s later)
- ❌ Not RF (-60 dBm both sessions)
- ❌ Not "the proxy abstraction alone" — ESPHome's Bluedroid
  bluetooth_proxy was tested earlier in this matrix and *also* fails,
  so the wedge is "stack + flow", not flow alone

The variable that flips between ✅ and ❌ on identical hardware is:
**NimBLE-Arduino + the proxy's discovery/CCCD/timing flow** vs
**Bluedroid with targeted `register_for_notify`**.

### Diagnostic infrastructure added this session

- `/trace?on={0,1}` on the proxy — stops the radio scan + silences
  scanner log tags + resets `/log` ring cursor, so a clean ATT-byte
  trace fits in the 64 KiB ring during a single BMS bring-up.
- `scripts/ant-probe/capture_proxy.py` — drives `/trace`, connects
  via aioesphomeapi, walks
  connect → get_services → start_notify → **write_descriptor(CCCD)**
  → write device-info query → wait 25 s, then `/trace off`. Polls
  `/log` to a file throughout.
- `scripts/ant-probe/capture_esphome.py` — subscribes to esphome's
  native API logs at `VERY_VERBOSE`.

Captures + write-up live in `scripts/ant-probe/` (see
`COMPARISON.md`).

### Test-orchestration bug found while iterating

`start_notify` on our proxy uses the `setNotifyCallback` shim from
the earlier patches — it registers the callback but does **not**
write the CCCD; the caller must send a separate
`write_descriptor`. The first iteration of `capture_proxy.py`
forgot that step, so the proxy session was running without CCCD
enabled. After fixing — single ATT_WRITE_REQ on handle 17 with
value `0x0001`, BMS ACKs with WRITE_RESP — **still 0 notifies**.
The bug existed in the test harness, not the proxy.

### Experiment: NimBLE fast conn params (7.5 ms / 10 s supervision)

Hypothesis: matching ESPHome's `FAST_MIN/MAX_CONN_INTERVAL = 0x06` +
`FAST_CONN_TIMEOUT = 1000` during the connect+discovery window would
let the BMS start notifying. Wired
`client->setConnectionParams(6, 6, 0, 1000)` into
`connection.cpp::connect`. Verified on the HCI Create Connection
bytes: `itvl_min=6 itvl_max=6 supervision_timeout=1000`.

**Result: still 0 notifies.** Discovery time dropped (3.7 s → 2.1 s),
time-to-first-query dropped (4.5 s → 3.0 s), but the BMS still
sends only a L2CAP CPU request and zero notify PDUs over 25 s.

Conn params during discovery are **not** the cause. The kept change
is harmless — matches what bluetooth_proxy peers expect — and stays
in.

### Experiment: `chr->subscribe(true)` + suppress bleak-esphome's CCCD write_descriptor (single on-air CCCD, still 0 notifies)

Iteration 2 of the subscribe-experiment. Code shipped:

- `handle_notify` calls `chr->subscribe(true, &notify_cb, true)` —
  CCCD written via NimBLE's gattc state machine, handle marked in
  NimBLE's local subscription table.
- A `(address, char_handle)` set tracks chars we subscribed to.
- `handle_write_descriptor` looks up the target descriptor's UUID;
  if it's CCCD (0x2902) and the parent char is in the set,
  synthesize a `BluetoothGATTWriteResponse` without touching the
  wire. Cleared on disconnect so a reconnect re-enables real CCCD
  writes.

Wire trace (`scripts/ant-probe/proxy-trace-suppressed.log`):

```
t=11250  ATT_WRITE_REQ handle=0x0011 value=0x0001  (single CCCD, subscribe(true))
t=11410  api.bt: suppressed redundant CCCD write_descriptor
t=11440  ATT_WRITE_CMD handle=0x0010 …             device-info query
t=14490  L2CAP CPU from BMS → accepted
t=37460  terminate connection (HA-side, end of 25 s wait)
```

Connection stayed up the full 25 s window (no supervision timeout —
confirms single-CCCD-write removes the regression from iteration 1).
**Notify_rx still 0.** ACL RX events post-write: 4 — CCCD WRITE_RESP,
L2CAP CPU req, L2CAP CPU resp, HCI subevent. **No notify ACL.**

So **the BMS still does not transmit notifies to NimBLE-Arduino even
with a single, gattc-layer-registered CCCD write that byte-matches
what aioble (working) does.**

This rules out ATT byte sequence and CCCD-write-count as the cause.
The shipped change (`subscribe(true)` + suppression in
`handle_write_descriptor`) is kept because:
- It's strictly closer to aioble's working flow
- It removes the iteration-1 supervision-timeout regression
- It's a correctness improvement for any peripheral that's
  sensitive to double-CCCD-writes

Hypothesis: `setNotifyCallback` only registers the dispatch callback
in NimBLE-Arduino's wrapper; `subscribe(true)` writes the CCCD
*through NimBLE's gattc state machine* — the same path Bluedroid's
`register_for_notify` takes. Maybe the BMS only emits notifies when
the central's GATT layer has flagged the handle as "subscribed"
internally, not just because CCCD=0x0001 lives on the peer.

**Result: regressed — the link dies.** Wire sequence captured in
`scripts/ant-probe/proxy-trace-subscribe.log`:

```
t=13581  att_handle=17 len=2   first CCCD write (subscribe(true))
t=13771  att_handle=17 len=2   second CCCD write (bleak-esphome's write_descriptor)
t=14091  att_handle=16 len=10  device-info query (WRITE_CMD)
t=16851  L2CAP CPU request → NimBLE accepts (15–25 ms, 6 s sv-tmo)
t=23011  onDisconnect reason=0x208  HCI supervision timeout (8.9 s)
```

After the **second CCCD write 190 ms after the first**, the BMS goes
radio-silent — no notifies, no LL ACKs, no empty PDUs — for 6 s
straight, which blows the post-CPU 6000 ms supervision window and
the controller drops the connection.

So the BMS *actively dislikes* the double CCCD write. Reverted to
`setNotifyCallback`. Aligns with FINDINGS § "Things tried" entry
"Duplicate CCCD write…" — the workaround was already in place, this
experiment just re-confirmed it's load-bearing.

The followup probe that comes out of this: **bleak-esphome sends the
CCCD write separately because that's how a generic BLE proxy
works**. Our only options for "single CCCD write driven inside
NimBLE's gattc layer" require *intercepting* bleak-esphome's
`write_descriptor` request when it targets a CCCD we already
subscribed locally. Coupling those two requests is doable in
`bt_handlers::handle_write_descriptor` (detect CCCD descriptors,
no-op the write if `subscribe(true)` already enabled it), but it's
fragile — easy to break for peripherals that need the explicit
CCCD-write path.

### Experiment: re-enable BLE-5 PHY features (2M + Coded) in LL_FEATURE_REQ

Hypothesis: Bluedroid advertises 2M + Coded PHY support in
LL_FEATURE_REQ; we'd disabled both to silence a benign
`BLE_ERR_UNSUPP_REM_FEATURE` HCI error on MTU exchange. Maybe the
BMS gates notify-emission on something it sees in our feature set.

Set in `sdkconfig.defaults`:

```
CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT=y
CONFIG_BT_NIMBLE_LL_CFG_FEAT_LE_2M_PHY=y
CONFIG_BT_NIMBLE_LL_CFG_FEAT_LE_CODED_PHY=y
```

Capture: `scripts/ant-probe/proxy-trace-phy.log` (plus
`proxy-trace-phy-debug.log` after restoring DEBUG runtime level).

**Result:**
- `BLE_ERR_UNSUPP_REM_FEATURE` returns on MTU exchange — BMS is
  BLE-4.x and rejects the broader feature set, as expected.
- mtu=136 still negotiates, connect+discovery still succeeds.
- **0 notify_rx in 25 s.** No effect on the failure mode.

Reverted: PHY flags back to `n` to keep MTU exchange clean.

### sdkconfig hygiene fix (this session)

Two compile-time settings that *had* been set interactively via
`idf.py menuconfig` got silently lost when `sdkconfig` was
regenerated for the PHY experiment:

```
CONFIG_BT_NIMBLE_LOG_LEVEL_DEBUG=y
CONFIG_NIMBLE_CPP_LOG_LEVEL_DEBUG=y
```

Symptom was `final_seq=3383` from a 25 s capture (vs 110 KB
previously). At INFO level, NimBLE-host's per-byte HCI debug
`ESP_LOGD` calls are *compiled out*, so runtime `/level?nimble=4`
has nothing to surface — `/log` collected only the high-level
state-machine lines.

Both flags are now pinned in `sdkconfig.defaults` so they survive
any future `idf.py reconfigure`. Compile size: ~5 KB extra over
INFO; runtime cost: zero unless `/level?nimble=4` is active.

### Experiment: notify-probe against fugu-fry (NUS peripheral, different vendor)

Until this session every "0 notify_rx" result came from the ANT
BMS. To check whether the proxy's notify-RX path works on
*anything*, ran `scripts/ant-probe/capture_notify_probe.py` against
fugu-fry at `70:04:1D:A6:AB:32` — Nordic UART Service, Apple
manufacturer ID, completely different device:

```
connect → mtu=247 → full discovery (GAP+GATT+NUS) → single CCCD
write on FFE3 RX-char @h17 → Write complete; status=0 → 20 s silence
```

`notify_rx = 0` here too. Inconclusive in isolation: NUS RX-char is
the read-direction, peripherals only notify on it when triggered by
a write to the TX char. But it does mean: no run of the proxy has
ever surfaced a notify-RX event from any peripheral, **on any
peripheral, ever**. The "works for everything except ANT" line in
the v1 bring-up notes is misleading — connect + discovery have been
verified, notify reception has not.

Capture: `scripts/ant-probe/fugu-fry-trace.log`.

### Experiment: NimBLE host task pinned to CPU 1 + doubled MSYS pools (FAILED, ACTIVELY HARMFUL)

Hypothesis: WiFi traffic on CPU 0 preempts NimBLE host dispatch long
enough to drop notify ACL packets at `os_mbuf_alloc`. Moving host to
CPU 1 + giving it more mbufs (24×256 + 48×320 ≈ 21 KB total) would
relieve that.

Set in `sdkconfig.defaults`:

```
CONFIG_BT_NIMBLE_PINNED_TO_CORE_1=y
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=24
CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT=48
```

**Result: regressed ANT.** Connection completes, GATT discovery
*starts*, then drops with HCI reason 0x208 (supervision timeout)
before the `BluetoothGATTGetServicesResponse` reaches HA. Symptom on
the aioesphomeapi side: `BluetoothConnectionDroppedError`.
fugu-fry still 0 notify_rx.

Theory: NimBLE host on CPU 1 competes with `httpd`, `api_client`,
`ble_adv_flush`, and the FreeRTOS app task. Controller stays on
CPU 0. The host↔controller IPC across cores adds latency the BMS's
2.5 s initial supervision window doesn't tolerate during discovery.

Reverted in `sdkconfig.defaults`. Captures:
`scripts/ant-probe/proxy-trace-pinned.log`,
`fugu-fry-pinned.log`.

### ✅ ROOT CAUSE FOUND + FIXED (2026-05-26)

**`BT_NIMBLE_ROLE_PERIPHERAL=n` silently disables NimBLE's
ATT-layer `BLE_ATT_OP_NOTIFY_REQ` (opcode 0x1B) dispatch handler.
Incoming notifications were being dropped at the ATT layer before
`ble_gap_notify_rx_event` could ever fire.**

The hunt that found it: capture_notify_probe.py against fugu-fry
(NUS, MAC `70:04:1D:A6:AB:32`) consistently produced `notify_rx=0`,
matching ANT. Direct bleak Mac→fugu-flat (same firmware family,
different device) returned 5 notifies in 8 s with the exact
"> uptime\r\n..." reply we'd been trying to elicit through the
proxy. So peers were genuinely transmitting notifies; the proxy
was silently dropping them somewhere between controller and host.

A DEBUG-level proxy log (with `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y`
finally pinned in defaults so `/level?nimble=4` actually surfaces
the bytes) showed `ble_hs_hci_evt_acl_process()` firing with
**ACL RX packets containing the exact ATT notification payload**:

```
ACL RX pb=2 len=251 data:
  f7 00   ← L2CAP length 247
  04 00   ← L2CAP CID 4 (ATT)
  1b      ← ATT opcode = HANDLE_VALUE_NOTIFICATION (0x1B)
  10 00   ← attr handle 16 (FFE1 / NUS RX)
  3e 20 75 70 74 69 6d 65 0d 0a 49 …  ← "> uptime\r\nI…"
```

So the notify ACL was reaching the host. Yet our
`BLE_GAP_EVENT_NOTIFY_RX` listener (registered via
`ble_gap_event_listener_register`) never fired. That ruled out
"controller dropping" and pointed at host-side ATT dispatch.

`components/bt/host/nimble/nimble/nimble/host/src/ble_att.c:45-81`:

```c
static const struct ble_att_rx_dispatch_entry ble_att_rx_dispatch[] = {
#if MYNEWT_VAL(BLE_GATTC)
    { BLE_ATT_OP_ERROR_RSP,            ble_att_clt_rx_error },
    { BLE_ATT_OP_WRITE_RSP,            ble_att_clt_rx_write },
    { BLE_ATT_OP_INDICATE_RSP,         ble_att_clt_rx_indicate },
    ...
#endif
#if MYNEWT_VAL(BLE_GATTS)
    ...
    { BLE_ATT_OP_NOTIFY_REQ,           ble_att_svr_rx_notify },   ← here
    { BLE_ATT_OP_INDICATE_REQ,         ble_att_svr_rx_indicate },
    ...
#endif
};
```

NimBLE puts the `NOTIFY_REQ` (opcode 0x1B) dispatch handler under
**`#if MYNEWT_VAL(BLE_GATTS)`** — the GATT-*server* block — despite
the fact that opcode 0x1B is what the peer-side server sends *to the
central client*. The function it dispatches to (`ble_att_svr_rx_notify`)
has a misleading "svr" prefix; it is the central's-eyes
notify-receive path.

Our kconfig had `BT_NIMBLE_ROLE_PERIPHERAL=n` (central-only proxy →
no need to advertise). `BT_NIMBLE_GATT_SERVER`'s Kconfig
`depends on BT_NIMBLE_ROLE_PERIPHERAL`, so the option was hidden →
defaulted off → `MYNEWT_VAL_BLE_GATTS=0` → the dispatch entry was
compiled out → every notification we ever received in the entire
investigation was silently discarded at
`ble_att_rx_dispatch_lookup()`.

**Fix:**

```
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_GATT_SERVER=y
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y  # for NimBLE-Cpp to compile
```

We never call `ble_gap_adv_start`, so the radio still operates as
central-only at runtime. Only the ATT dispatch table is affected.

Verified after rebuild + OTA:

- **fugu-fry** (NUS) → 6 notifies in 6 s, including the
  `"> uptime\r\n..."` response and 5 telemetry frames. Decoded
  voltages, currents, MPPT state all match.
- **ANT BMS** (`20:A1:11:02:23:45`) → 3 notifies, payload
  `7e a1 12 6c 02 20 30 50 48 42 30 54 42 31 32 30 41 …` —
  byte-for-byte identical to the working aioble-micropython
  capture documented in this file. Device-info reply with model
  `"0PHB0TB120A"` and firmware version `"20PHUB00-211026A"`.

`notify_rx_total` counter no longer reads 0. NimBLE-Arduino's
per-char `setNotifyCallback` dispatches correctly. The proxy is
finally end-to-end functional.

### Lessons + future-proofing

- The misleading `svr` prefix on `ble_att_svr_rx_notify` cost
  multiple sessions of debugging. Don't trust function names —
  trace from on-wire bytes upward.
- The single most important debugging change was pinning
  `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` in `sdkconfig.defaults`.
  Without it, runtime `esp_log_level_set("NimBLE", DEBUG)` is
  silently clipped at INFO. The earlier captures that "lost" the
  raw HCI byte dumps after a `sdkconfig` regenerate were doing
  exactly that. The DEBUG log of the ACL RX bytes is what finally
  proved notifies reached the host.
- All the *experimental* knobs probed during the hunt
  (fast conn params via `setConnectionParams(6, 6, 0, 1000)` in
  `connection.cpp`, `chr->subscribe(true)` + CCCD-write_descriptor
  suppression in `bt_handlers.cpp`, NimBLE host pinning + mbuf
  bumps, BLE-5 PHY toggling) turned out to be unnecessary once
  the GATTS dispatch was enabled, and were reverted before the
  commit. Only the three `sdkconfig.defaults` flags + the log-level
  compile-time settings + the `/trace` diagnostic harness stayed.
- Future audit: if any future kconfig change touches
  `BT_NIMBLE_ROLE_PERIPHERAL` or `BT_NIMBLE_GATT_SERVER`,
  re-verify notify-RX with `capture_notify_probe.py`.

### What's still untested (lower priority now)

1. **Air sniffer (nRF52840).** Still the cleanest answer — directly
   shows whether the BMS transmits notify PDUs at all during a
   failing NimBLE session. The subscribe(true) experiment also
   showed the BMS *can* unilaterally go silent for 6 s+ on this
   stack, which means "0 notify ACL" really might mean "0
   transmitted by peer."
2. **Trigger fugu-fry's TX path with a known-good payload.** If
   we can get *any* notify_rx from *any* peripheral, the proxy's
   notify-RX path is verified and ANT can be cleanly attributed
   to a peer-side issue. Without a verified working notify
   reception, we cannot rule out a general proxy bug.

## Known limitations / unfinished

- **Successful GATT connect / discovery tested end-to-end.** The
  ANT BMS at `20:A1:11:02:23:45` connects + discovers fine through
  the proxy; only the data path (notify PDUs from BMS → controller)
  is broken. Other peripherals tested (smoke peripherals from the
  earlier session) work end-to-end.
- **No real GATT service cache.** REMOTE_CACHING is advertised but every
  connect runs a fresh `discoverAttributes()`. HA never checks; works
  in practice but slower than ESPHome's actual caching proxies.
- **GATT discovery chunking is not real.** A single
  `BluetoothGATTGetServicesResponse` is emitted; peripherals exceeding
  the static caps (8 services / 12 chars / 6 descriptors per service)
  get truncated with a warning. Real chunking against
  `GATT_DISCOVERY_CHUNK_BYTES` is a v0.2 concern.
- ~~Subscription bookkeeping is global, not per-client.~~ Fixed: each
  connection task owns a `ClientSubs{sub_adv, sub_free}` on its stack;
  handlers flip those flags idempotently and bump atomic global counts.
  `on_client_disconnect` decrements based on the flags so a crashing
  client releases its refs without needing the "last client" sweep.
- **No Noise encryption.** Plaintext only. Fine on a trusted LAN, not
  internet-exposed.
- **No pairing / cache-clearing / scanner state+mode / connection
  params** — bits 3, 4, 6, 7 not advertised, those request types
  return a graceful error (`-99`).
- **OTA has no auth.** Anyone on the LAN can flash arbitrary firmware
  by POSTing to `/update`. Trusted-LAN assumption only.

## Quick reference

### Bring-up

```bash
cd /Users/fab/dev/ha/nimble-ble-proxy
git clone --depth 1 https://github.com/nanopb/nanopb.git components/api_proto/nanopb
cp include/wifi_creds.h.example include/wifi_creds.h   # then edit
. /Users/fab/dev/esp/idf5.5/export.sh
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

### OTA

```bash
curl --data-binary @build/nimble_ble_proxy.bin \
     http://nimble-proxy.local/update
```

### Recover stuck device

`/dev/cu.usbmodem59720648061` is the recovery / USB-JTAG side of the
ESP32-S3 dev board. If `idf.py flash` on `usbmodem1101` hangs, reset
through the recovery port:

```bash
python $IDF_PATH/components/esptool_py/esptool/esptool.py \
    --chip esp32s3 -p /dev/cu.usbmodem59720648061 --no-stub run
```

### Smoke test

`/tmp/proxytest/smoke.py` — connects via aioesphomeapi, prints
device info, subscribes to raw adverts for 15s, reports counts per MAC.
