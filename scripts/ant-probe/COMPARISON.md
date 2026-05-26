# nimble-ble-proxy vs syssi/esphome-ant-bms — same chip, same WiFi, A/B trace

Captured 2026-05-26 on the same ESP32-S3 dev board (`192.168.1.231`)
against the same ANT-BLE20PHUB at MAC `20:A1:11:02:23:45`. The
firmwares were swapped via the proxy's existing `/update` endpoint
(ota_0 ↔ ota_1), so chip, antenna, WiFi traffic profile, and
HTTP-server presence are all held constant.

Captures live next to this file:
- `proxy-trace.log` — nimble-ble-proxy session (failure)
- `esphome-trace.log`, `esphome-trace-boot.log` — esphome-ant-bms
  session (success)

## Outcome

| Firmware              | Stack              | Notify PDUs received | Status |
|-----------------------|--------------------|----------------------|--------|
| nimble-ble-proxy (orig) | NimBLE-Arduino   | **0 in 25 s**        | ❌ |
| syssi/esphome-ant-bms | Bluedroid          | dozens, steady       | ✅ |
| **nimble-ble-proxy (fixed)** | **NimBLE-Arduino** | **3+ in 5 s** | **✅** |

The fix: enable `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y` +
`CONFIG_BT_NIMBLE_GATT_SERVER=y` + `CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y`
in `sdkconfig.defaults`. Even though the proxy never advertises,
those flags are required to compile in NimBLE's ATT NOTIFY_REQ
(opcode 0x1B) dispatch handler — which is gated under
`#if MYNEWT_VAL(BLE_GATTS)` despite being the central's notify-RX
path. See `FINDINGS.md § ROOT CAUSE FOUND + FIXED`.

esphome's log shows the BMS pushing 20-byte notify PDUs containing
the expected status-frame bytes (`7E.A1.11.00.00.7E.05.02…` —
header + function 0x11 + cell voltages around `0x0D2C` ≈ 3.36 V).
The proxy's host-level NOTIFY_RX listener stays at zero through the
entire session. End-to-end success on the same chip with the same
WiFi+API load eliminates the most-cited hypotheses in `FINDINGS.md`:

- ❌ not WiFi+BT coexistence (esphome runs identical coex)
- ❌ not the BMS being stuck (it speaks fine ~30 s later from
  esphome)
- ❌ not RF (-60 dBm both sessions)
- ❌ not "the proxy abstraction" alone — the proxy *flow* (HA
  side discovery) doesn't apply when ant_bms_ble drives the chip
  locally, yet a Bluedroid bluetooth_proxy was also tested in the
  FINDINGS matrix and failed. So the wedge is **stack + flow**,
  not stack alone or flow alone.

The remaining variable that flips between ✅ and ❌ on identical
hardware is: **NimBLE-Arduino with full discovery + later CCCD
write + slow conn params** vs **Bluedroid with targeted
register_for_notify + fast conn params**.

## ATT timing on the proxy (from `proxy-trace.log`)

Boot-relative timestamps:

```
t=791801  GAP procedure initiated: connect
            scan_itvl=16 scan_window=16 (10 ms each)
            itvl_min=24 itvl_max=40 (30–50 ms)
            supervision_timeout=256 (2560 ms)
            own_addr_type=0
            peer_addr_type=0  peer=20:a1:11:02:23:45

t=792291  GAP procedure initiated: discovery   (+490 ms)
t=795501  ATT chr_val_handle=16 end_handle=18   (+3700 ms)
                                                 — FFE1 found
t=796361  ATT att_handle=16 len=10              (+4560 ms)
                                                 — device-info query
                                                   write (WRITE_CMD)
t=798091  L2CAP rxed signalling msg:            (+6290 ms)
            Connection Parameter Update Request
            min=12 max=20 latency=0 timeout=600
            (15–25 ms, 6000 ms supervision)
            NimBLE: Accepted peer params

(then 25 s of silence — no notify_rx)
```

## What esphome-ant-bms does instead

From `ant_bms_ble.cpp` (syssi/esphome-ant-bms@main) +
`esp32_ble_client::BLEClientBase`:

```
esp_ble_gattc_open()
   conn_params: FAST_MIN/MAX = 0x06 (7.5 ms)
                FAST_CONN_TIMEOUT = 1000 (10000 ms)

ESP_GATTC_CONNECT_EVT
   -> esp_ble_gattc_send_mtu_req()

ESP_GATTC_OPEN_EVT
   -> esp_ble_gattc_search_service(filter=NULL)
        // services only; characteristics + descriptors are lazy

ESP_GATTC_SEARCH_CMPL_EVT
   -> get_characteristic(FFE0, FFE1)    // table lookup, no wire op
   -> esp_ble_gattc_register_for_notify(char_handle)

BLEClientBase ESP_GATTC_REG_FOR_NOTIFY_EVT  (legacy path)
   -> esp_ble_gattc_get_descr_by_char_handle(handle, 0x2902)
        // only the FFE1 CCCD is touched; 1800/1801 descriptors
        // never enumerated
   -> esp_ble_gattc_write_char_descr(cccd_handle, 0x0001,
                                     ESP_GATT_WRITE_TYPE_RSP)
   // also switches to MEDIUM_MIN/MAX conn params

ant_bms_ble ESP_GATTC_REG_FOR_NOTIFY_EVT
   -> esp_ble_gattc_write_char(FFE1, query_frame,
                               ESP_GATT_WRITE_TYPE_NO_RSP)

ESP_GATTC_NOTIFY_EVT (×N from this point on)
```

## Concrete differences (proxy → esphome)

### 1. Connection interval during discovery — 4–6× slower on proxy

| Window | Proxy (NimBLE-Cpp default) | esphome (Bluedroid `FAST_*`) |
|---|---|---|
| Connect through CCCD-write | 30–50 ms interval | **7.5 ms interval** |
| Supervision timeout | 2560 ms | 10000 ms |

After the BMS-initiated L2CAP CPU goes through, both end up at
roughly the same medium params (15–25 ms / 6 s). But the *first
seconds* — connect, MTU exchange, service discovery, CCCD write,
first request — happen with completely different on-air pacing.

This is the strongest unexplained difference and the easiest to
test: NimBLE-Cpp `setConnParams(6, 6, 0, 1000)` before
`client->connect(...)` puts us in the same window.

### 2. Discovery footprint — exhaustive vs targeted

NimBLE-Arduino's `discoverAttributes()` walks every service, every
characteristic, every descriptor — including the GAP (`0x1800`) and
GATT (`0x1801`) services and the Service Changed CCCD at handle
`0x000d`. The proxy log shows the disc procedure running 3.2 seconds
before FFE1 even gets touched.

esphome's `search_service` returns only the service table.
Characteristics and descriptors are lazy: `register_for_notify`
only ever queries the CCCD on the *one* characteristic it's
registering — handle 0x11 (FFE1's CCCD). 0x1800/0x1801 descriptors
are never read.

FINDINGS already tried the minimal-discovery variant and reported
"still no notifies" — so this alone isn't the cause. But it might
be a *necessary* condition: maybe ANT tolerates exhaustive discovery
*if* it happens fast enough (7.5 ms intervals → 3.2 s walk becomes
500 ms), and dies if it takes multiple seconds.

### 3. CCCD write driven locally vs over-the-API

esphome calls `esp_ble_gattc_register_for_notify` once;
Bluedroid issues the CCCD write inside its own state machine in
the same connection event window. No host↔proxy round-trip.

Our proxy receives a `BluetoothGATTWriteDescriptorRequest` from
bleak-esphome over TCP (HA-side), executes the CCCD write, and
later receives a separate `BluetoothGATTWriteRequest` for the
device-info query. Each of those is gated by TCP RTT + HA
scheduling — probably another 50–200 ms total wall-clock
between CCCD-enable and first-query on top of the 6 s already
spent in discovery.

## Experiment 1: fast conn params on the proxy

Wired `NimBLEClient::setConnectionParams(6, 6, 0, 1000)` into
`connection::connect()` to match ESPHome's `FAST_MIN/MAX_CONN_INTERVAL`
during the connect+discovery window. Verified on the wire — the
HCI LE Create Connection now reports `itvl_min=6 itvl_max=6
supervision_timeout=1000`. Capture: `proxy-trace-fast.log`.

Result: **still 0 notify_rx in 25 s**. Discovery dropped from 3.7 s
to 2.1 s, device-info query went out at 4.5 s → 3.0 s after
connect. BMS still sends the same L2CAP CPU request (to 15–25 ms /
6000 ms) ~3 s after our query, and then silence.

Conn params during discovery alone are **not** the cause.

## Test-orchestration bug found (and fixed)

While debugging, noticed our `capture_proxy.py` was issuing only
`bluetooth_gatt_start_notify` — which on our proxy just calls
`setNotifyCallback` (per FINDINGS #11 patch) and does *not* write
the CCCD. The matching `write_descriptor` is sent by bleak-esphome
in HA's normal flow but our script was missing it.

Fixed: capture now sends an explicit
`bluetooth_gatt_write_descriptor(cccd_handle=17, b"\x01\x00")` between
start_notify and the device-info write. Verified on the wire:

```
t=146512  ATT_WRITE_REQ (0x12) handle=0x0011 value=0x0001
t=146662  ATT_WRITE_RESP (0x13) — BMS acks the CCCD enable
t=146722  ATT_WRITE_CMD (0x52) handle=0x0010 ant query frame
```

Capture: `proxy-trace-fast-cccd.log`.

Result: **still 0 notify_rx in 25 s**. The BMS:
- Accepts the CCCD write (sends WRITE_RESP)
- Receives the device-info query (no on-air ack since WRITE_CMD,
  but BMS proves it's still on the link by sending an L2CAP CPU)
- Sends 0 notify PDUs over the entire 25 s window

Counted ACL RX events post-write: **4** (CCCD response + L2CAP
CPU request + L2CAP CPU response + the connection-update-complete
HCI subevent — no notify ACL packets at all).

So the BMS *chooses* not to send notifications to our NimBLE-Arduino
proxy. CCCD is on, query was delivered, link is healthy, but no
data comes back.

## Experiment 3: subscribe(true) + suppress bleak-esphome's CCCD write_descriptor

Iteration 2 of subscribe-experiment with the followup wired in.

`handle_notify` now calls `chr->subscribe(true, &notify_cb, true)`
and tracks the `(address, char_handle)` in a small set.
`handle_write_descriptor` checks each incoming write — if the target
descriptor's UUID is 0x2902 (CCCD) and the parent char is in the
tracking set, the proxy synthesizes a `BluetoothGATTWriteResponse`
without writing anything on the wire. The set is cleared per-peer
on `onDisconnect` so reconnect re-enables real CCCD writes.

Capture: `proxy-trace-suppressed.log`. Wire timeline:

```
t=11250  ATT_WRITE_REQ handle=0x0011 value=0x0001  (subscribe(true))
t=11410  api.bt: "suppressed redundant CCCD write_descriptor"
t=11440  ATT_WRITE_CMD handle=0x0010 …             device-info query
t=14490  L2CAP CPU → accepted (15–25 ms / 6 s sv-tmo)
t=37460  HA-side disconnect (end of 25 s wait, normal terminate)
```

Single CCCD write on the wire. Connection stayed up the full 25 s.
Post-write ACL RX events: 4 (CCCD response, CPU req, CPU resp, one
HCI subevent — no notify ACL).

**Still 0 notify_rx.**

Kept in the tree — strictly closer to aioble's working sequence and
eliminates the supervision-timeout regression from Experiment 2.

## Experiment 2: `chr->subscribe(true)` in handle_notify

Hypothesis: maybe the BMS only sends notifies when the central's
local gattc layer has the handle flagged as "subscribed"
internally, not just because CCCD=0x0001 lives on the peer.
NimBLE's `subscribe(true)` writes CCCD via the gattc state machine
(same path Bluedroid's `register_for_notify` takes); plain
`setNotifyCallback` only registers the dispatch callback.

Wired into `bt_handlers.cpp::handle_notify`. Capture:
`proxy-trace-subscribe.log`.

**Result: regression — connection dies at t≈9 s with HCI reason
0x208 (supervision timeout).**

```
t=13581  ATT_WRITE_REQ handle=0x0011 value=0x0001   (subscribe(true))
t=13771  ATT_WRITE_REQ handle=0x0011 value=0x0001   (bleak-esphome's write_descriptor)
t=14091  ATT_WRITE_CMD handle=0x0010 …             device-info query
t=16851  L2CAP CPU request from BMS → accepted
t=23011  onDisconnect reason=0x208 (BLE_HS_HCI_BASE + HCI 0x08 = sv-timeout)
```

The 190 ms between the two CCCD writes is enough to push the BMS
into a state where it stops transmitting *anything* — no notifies,
no LL ACKs, no empty PDUs — for 6 s, which exhausts the
just-negotiated 6 s supervision window.

So the double-CCCD-write isn't just inefficient — it actively
breaks the link on ANT-BLE20PHUB. Reverted. Confirms the existing
`setNotifyCallback` workaround is load-bearing for this BMS family.

Followup that comes out of this:
**suppress bleak-esphome's CCCD `write_descriptor` on the proxy side
when we already enabled notify locally**. Detectable in
`handle_write_descriptor`: look up the descriptor's UUID, if 0x2902
and the parent char has a notify callback registered, synthesize a
WriteResponse without touching the wire. Lets us use `subscribe(true)`
for the *internal* gattc state setup *and* keep the on-air CCCD
write count at exactly one. That's the next probe.

## What's still on the table

What still differs between proxy (NimBLE-Arduino, fails) and
esphome-ant-bms (Bluedroid, works) on the same chip:

1. **LL features advertised to the peer.** Our `sdkconfig.defaults`
   has `CONFIG_BT_NIMBLE_LL_CFG_FEAT_LE_2M_PHY=n` and
   `_LE_CODED_PHY=n`, which silences `UNSUPP_REM_FEATURE` noise
   during MTU exchange but means our LL_FEATURE_REQ advertises a
   *narrower* set than Bluedroid's defaults. Unlikely to matter for
   a BLE-4.x BMS — but it's an unfalsified difference. **Test:**
   re-enable both, accept the HCI error noise, see if the BMS now
   notifies.

2. **The CCCD write opcode.** The proxy used ATT_WRITE_REQ (0x12)
   for CCCD — matches both Bluedroid and aioble. Already correct,
   nothing to vary.

3. **`setNotifyCallback` vs `chr->subscribe()`.** The proxy's
   handler uses `setNotifyCallback` (no CCCD write); bleak-esphome
   sends the CCCD write separately. NimBLE-Arduino's
   `subscribe(true)` instead does CCCD write + dispatch hookup
   atomically inside the gattc layer. Possibly hooks the gattc
   per-handle subscription table differently and the controller's
   ATT layer might consult that table. **Test:** in
   `bt_handlers::handle_notify`, fall back to `subscribe(true)` for
   the experiment and skip bleak-esphome's later write_descriptor
   (or no-op the descriptor write handler). Single CCCD path,
   matching aioble exactly.

4. **NimBLE host task pinning / mbuf pool / ACL buffer count.**
   NimBLE's defaults on ESP-IDF: host pinned to CPU 0, MSYS_1=12
   blocks @ 256 B, MSYS_2=24 blocks @ 320 B. Bluedroid uses
   different allocations. If notify PDUs from the controller hit a
   host-side allocation failure they're dropped silently. **Test:**
   bump `CONFIG_BT_NIMBLE_MSYS_*_BLOCK_COUNT`, pin host to CPU 1,
   re-run.

5. **An nRF52840 sniffer.** Still the cleanest answer — it directly
   shows whether the BMS transmits notify PDUs over the air at all
   during our session. The 4-ACL-RX evidence above is suggestive
   but not conclusive (the controller could theoretically silently
   drop). $10 + ~2 days shipping per FINDINGS § "Definitive next
   step".
