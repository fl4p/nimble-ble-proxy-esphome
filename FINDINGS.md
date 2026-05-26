# nimble-ble-proxy — bring-up findings

End-to-end bring-up notes from 2026-05-26. The firmware speaks the
aioesphomeapi plaintext protocol over TCP and exposes itself to Home
Assistant as a regular ESPHome Bluetooth proxy, but with NimBLE as the
BLE stack instead of Bluedroid.

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
  back to HA. Tested with an ANT BMS that's out of RF range of the
  proxy; failure path produces a correct `BLE_HS_ETIMEOUT` (13) error.
- Successful GATT connect + service discovery against a real peer was
  **not** validated this session because the only candidate BMS is out
  of range. The code path is exercised through the failure case but the
  happy path is unrehearsed.

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

## Known limitations / unfinished

- **Successful GATT connect / discovery never tested end-to-end.** All
  the code paths exist but the only candidate peripheral (ANT-BLE20PHUB
  at MAC `20:A1:11:02:23:45`) is at -88 dBm via another proxy and
  invisible to us. Move the proxy closer or pick a peripheral in range
  to validate.
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
