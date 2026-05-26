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

### 5. Synchronous NimBLE connect blocks the per-client task

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
- **Subscription bookkeeping is global, not per-client.** Ref counts
  `g_sub_adv_count` / `g_sub_free_count` rise on subscribe, fall on
  unsubscribe, and reset to zero only when the LAST client disconnects.
  A client that subscribes then crashes will leave its increment until
  another connection drains the slot. Tolerable for HA + occasional
  CLI; not what you want with many clients.
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
