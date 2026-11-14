# api-tx-ring vs main on a real ESP32-S3 (2026-11-13)

This is a hardware bench of the per-client TX ring plus the `api_tx` sender task (branch
`api-tx-ring`, uncommitted) against `main` (214fa22). It ran on the USB S3 with the same
NAT/SoftAP profile on both builds. Raw data, scripts and the bench-only `main` patch are in
[`bench-api-tx-ring-s3/`](bench-api-tx-ring-s3/).

## Summary

- **A client that stops reading costs the healthy client its notifications on `main`, and
  none on the ring.** Client B floods 600 DeviceInfo requests and then stops reading
  (run `ABf`):
  - On `main`, client A lost **416 of 1657** notifications (by sequence number), with
    gaps of up to **20.1 s**. GATT read latency reached p95 **21.6 s**. The loss happens
    below the GAP notify counter (`notify_rx` = 1238 ≈ the 1237 that A received). So the
    BLE host side dropped them, not TCP. This fits the host task being held while the
    stalled client blocks the shared send path.
  - With the ring, A lost **0 of 3460** (16 KiB cap) and **0 of 3440** (8 KiB cap). Read
    p95 was 302 ms and 354 ms.
- **A survived every run on every build.** On the ring, the stalled client B was reaped
  20 s after its data stopped moving each time (B sees the broken pipe at 24–26 s). In
  `ring-ABf`, one of B's reaps came from a full TX ring.
- **`api_ring_reaps` stayed 0 for the healthy client in every run**, including the 8 KiB
  cap (`ring8k-*`). The one ring reap in the whole bench (`ring-ABf`) was the stalled B
  (log: `fd=53 … TX ring full (reaped)`, client id 6).
- **`api_tx` stack: at least 820 B free** (min over all runs, from the 2560 B stack). The
  required floor is 512 B.
- **Frames per send:** 1.08–1.22 under normal load and 1.88–1.91 with a stalled client.
- **Heap is the cost.** The ring build idles about 12 KB lower: 86.3 KB → 74.6 KB free at
  boot, from the two static 4 KiB rings plus the `api_tx` stack. Under `ABf`:

  | Build | `heap_min` | Smallest `heap_largest` |
  |---|---|---|
  | main | 13 996 | 15 360 |
  | ring (16 KiB cap) | 11 516 | 11 264 |
  | ring (8 KiB cap) | 14 928 | 12 800 |

  Each stalled flooding client took about 30 KB of heap while it lasted (1 Hz samples:
  62 KB → 30 KB). Nothing failed (`oom` 0), but the 16 KiB-cap ring came within 3.3 KB
  of the 8 KiB block that an api_server connection task needs.
- **A GetServices sent after notifications are on breaks notify forwarding on both
  builds.** This is the pre-existing bug, not the TX path. After the first repeated
  GetServices, A received 5 notifications and then nothing. The device logs
  `NimBLEClient: unknown handle: 27` about 56 times per run. Each GetServices took
  4.3–5.0 s, so it re-discovered every time. This is the "unknown handle" open question
  from issue #2, now reproduced on demand (see Findings).
- **Host-task blocking (injected `notify_cb` delay, `main`, A only, 20 Hz notifies):**

  | Delay | Effect |
  |---|---|
  | 50 ms | 0 lost. Reads starved: 15 reads in 60 s, p95 36 s. One BLE disconnect (reason 534). |
  | 200 ms | 0 lost. The source was throttled (325 sent in 60 s) and only one read completed. |
  | 500 ms | **98 lost** (by sequence number). The source sent 270. |

## Setup

- **Board.** `/dev/cu.usbmodem101`, ESP32-S3 (QFN56) rev v0.2, MAC
  `9C:13:9E:F4:04:98`. Read with esptool and checked through DeviceInfo on every image.
  IP 192.168.1.125 (`arp`). Before this bench it ran a fugu image (`fugu-firmware`,
  compile time Nov 13 17:25:02), which was overwritten as authorised.
- **Profile.** `sdkconfig.defaults` (NAT router + SoftAP, clone, 2 NimBLE links, 2 API
  clients) under IDF 6.0.3.
  - The `main` and ring sdkconfigs differ only in `CONFIG_NBP_API_CLIENT_TX_RING=4096`,
    a symbol `main` does not have.
  - `dependencies.lock` was copied into the `main` worktree, so mdns is 1.11.1 on both. A
    fresh resolve picked 1.15.1 and added `MDNS_ENABLE_BROWSE`.
  - The 8 KiB case adds `CONFIG_NBP_API_CLIENT_TXQ_CAP=8192` via
    `bench-api-tx-ring-s3/txq8k.defaults`.
- **Images, all archived** in `elf-archive/` (device `usbmodem101`):

  | ELF SHA (8) | Image | app_init compile | DeviceInfo compile |
  |---|---|---|---|
  | a639344d | main (clean) | 20:56:37 | 20:59:08 |
  | 1a967ca5 | main + inert delay hook (below) | 20:56:37 | 20:59:08 |
  | ff0088cd | api-tx-ring, cap 16 KiB (**left on the board**) | 20:55:01 | 21:20:33 |
  | 198df948 | api-tx-ring, cap 8 KiB | 20:58:39 | 20:58:46 |

- **Delay hook.** The `main` delay-injection image is `main` plus
  `bench-api-tx-ring-s3/main-delay-hook.diff`. It adds `vTaskDelay(g_bench_notify_delay_ms)`
  at the top of `notify_cb`, set with `POST /wifips?benchdelay=N` and 0 at boot.
  - The runs `main-A`, `main-AB`, `main-ABgs` and `main-ABgs-first` used clean `main`.
  - The runs `main-ABf` and `main-D*` used the hook image. `main-ABf` ran with delay 0,
    which costs one volatile load per notification.
- **Branch change made for this bench (uncommitted).** It adds the
  `/stats.json` field `api_tx_stack_free`, the `api_tx` stack high-water mark from
  `uxTaskGetStackHighWaterMark`, in bytes. Without it the stack could not be read on
  hardware.
- **Notifying peer: neither SmartShunt nor fugu-flat was usable.**
  - The SmartShunt was not on the air.
  - The Mac saw fugu-flat (`70:04:1D:A4:EA:36`) at −96 dBm. The proxy saw no advert from
    it in 30 s of scanning at 50 % duty. A connect attempt (address type 0) failed
    `onConnectFail reason=13` after 8 s. The PIN could not even be set: on this clone
    build `/passkey` is not registered, so the PIN goes through `/clone?passkey=`.
  - **Substitute peer: the Mac itself as a GATT peripheral**
    (`bench-api-tx-ring-s3/macperiph.py`, CoreBluetooth via pyobjc, public address
    `80:A9:97:1D:E2:C9`). It has a notify+read characteristic (handle 27) that carries a
    contiguous uint32 sequence number and 16 B of padding at 20 Hz, and a read
    characteristic (handle 30). The sequence number is only advanced when CoreBluetooth
    accepts the update, so a gap seen by A is a notification lost between the Mac's
    accepted queue and A. The Mac's `periph sent` count is in the table.
- **Clients.** `bench-api-tx-ring-s3/apibench.py` is a raw plaintext ESPHome-API client;
  aioesphomeapi is not installed here, and B needs socket-level control anyway.
  - **A** subscribes to adverts and connections-free, connects to the peer (V3 with
    cache), and runs GetServices. It enables notify on 27 (NotifyRequest plus a CCCD
    write) and reads handle 30 once a second (one read in flight).
  - **B** subscribes to adverts and connections-free, optionally sends N DeviceInfo
    requests, and then never reads. It pings every 2 s, so a reap shows up as a send
    error, then reconnects and stalls again.
  - Every run is preceded by `POST /reboot` and a 20 s settle. `/stats.json` is polled at
    1 Hz.
- **Runs** (`runset.sh`):

  | Run | What it does |
  |---|---|
  | `A` | 60 s, A only |
  | `AB` | 180 s, B stalls from t=15 s with no flood (advert-only stall) |
  | `ABf` | 180 s, B floods 600 DeviceInfo requests at each (re)connect, then stalls |
  | `ABgs` | 120 s, like `AB`, plus a GetServices from A every 10 s |
  | `D50` / `D200` / `D500` | 60 s, A only, `notify_cb` delay 50, 200 or 500 ms |

## Raw results

Column notes:

- `seq lost` = missing sequence numbers among the notifications A received.
- `notify_rx Δ` = the device GAP-level notify counter over the run.
- `heap min seen` is the lowest of the 1 Hz `heap` samples. `heap_min` and `largest min`
  are the device's own low-water marks.
- `txq max` and `ring max` are per-client high-water marks.
- Counters are taken from the last sample.
- `periph sent` = notifications the Mac had accepted during the run.

| run | build | A alive | A notifies | seq lost | notify_rx Δ | read n | read p50/p95/max ms | notify gap p99/max ms | GS bursts (ms p50/max) | B sessions/reaps (s) | heap min seen | heap_min | largest min | txq max | ring max | txq_drops | txq_reaps | ring_reaps | frames/send | enq_us_max | api_tx stack free | oom | periph sent |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| main-A | Nov 13 2026 20:59:08 | yes | 1032 | 0 | 1034 | 51 | 211.4/425.2/919.4 | 139.0/239.5 | 1 (4603.3/4603.3) | 0/0 [] | 68848 | 58436 | 31744 | 9312 | 0 | 0 | 0 | – | – | – | – | 0 | – |
| ring-A | Nov 13 2026 21:20:33 | yes | 1075 | 0 | 1076 | 53 | 207.8/318.6/611.6 | 114.2/179.7 | 1 (4606.8/4606.8) | 0/0 [] | 58428 | 49748 | 31744 | 6212 | 350 | 0 | 0 | 0 | 1.22 | 3401 | 944 | 0 | 1081 |
| ring8k-A | Nov 13 2026 20:58:46 | yes | 1044 | 0 | 1047 | 52 | 247.2/352.3/720.3 | 152.2/308.7 | 1 (4891.4/4891.4) | 0/0 [] | 60060 | 51324 | 31744 | 6220 | 350 | 0 | 0 | 0 | 1.20 | 3972 | 868 | 0 | 1050 |
| main-AB | Nov 13 2026 20:59:08 | yes | 3458 | 0 | 3462 | 169 | 210.1/313.1/767.6 | 121.5/201.9 | 1 (4814.2/4814.2) | 1/0 [] | 58116 | 47100 | 31744 | 12424 | 0 | 0 | 0 | – | – | – | – | 0 | 3465 |
| ring-AB | Nov 13 2026 21:20:33 | yes | 3438 | 0 | 3440 | 168 | 241.3/328.1/1491.5 | 149.1/259.8 | 1 (4580.4/4580.4) | 1/0 [] | 48372 | 30212 | 31744 | 7788 | 350 | 0 | 0 | 0 | 1.15 | 6433 | 872 | 0 | 3443 |
| ring8k-AB | Nov 13 2026 20:58:46 | yes | 3440 | 0 | 3442 | 169 | 249.9/353.7/653.1 | 146.7/259.7 | 1 (4731.7/4731.7) | 1/0 [] | 48768 | 33252 | 31744 | 7788 | 350 | 0 | 0 | 0 | 1.15 | 6759 | 948 | 0 | 3446 |
| main-ABf | Nov 13 2026 20:59:08 (hook, delay 0) | yes | 1237 | **416** | 1238 | 52 | 227.4/**21598.5/22213.7** | 236.3/**20066.4** | 1 (4890.5/4890.5) | 6/5 [28.1, 26.1, 24.0, 30.1, 26.1] | 42028 | 13996 | 15360 | 17132 | – | 1 | 6 | – | – | – | – | 0 | 1657 |
| ring-ABf | Nov 13 2026 21:20:33 | yes | 3454 | 0 | 3456 | 169 | 210.3/302.1/722.3 | 133.9/220.6 | 1 (4571.9/4571.9) | 7/6 [24.0, 26.1, 26.0, 24.0, 24.0, 24.0] | 29140 | 11516 | 11264 | 12456 | 3000 | 435 | 5 | 1 (B) | 1.88 | 8361 | 824 | 0 | 3460 |
| ring8k-ABf | Nov 13 2026 20:58:46 | yes | 3434 | 0 | 3436 | 168 | 212.4/354.4/770.4 | 136.7/452.9 | 1 (4815.9/4815.9) | 7/6 [24.0, 24.0, 24.0, 24.0, 24.0, 24.0] | 27660 | 14928 | 12800 | 10904 | 3001 | 529 | 6 | 0 | 1.91 | 12633 | 820 | 0 | 3440 |
| main-ABgs | Nov 13 2026 20:59:08 | yes | 5 | 0 | 2297 | 75 | 213.6/4325.5/4745.0 | 121.2/121.2 | 12 (4639.5/4968.5) | 1/0 [] | 61692 | 45884 | 31744 | 10872 | – | 0 | 0 | – | – | – | – | 0 | 2292 |
| ring-ABgs | Nov 13 2026 21:20:33 | yes | 5 | 0 | 2289 | 77 | 213.6/4010.5/4358.2 | 106.3/106.3 | 12 (4314.7/4815.5) | 1/0 [] | 50352 | 32192 | 31744 | 4668 | 393 | 0 | 0 | 0 | 1.08 | 5561 | 948 | 0 | 2292 |
| main-D50 | hook, delay 50 ms | yes | 986 | 0 | 986 | 15 | 351.8/36306.8/36306.8 | 118.1/166.9 | 1 (4615.7/4615.7) | – | 70588 | 56004 | 31744 | 9316 | – | 0 | 0 | – | – | – | – | 0 | 992 |
| main-D200 | hook, delay 200 ms | yes | 270 | 0 | 280 | 1 | 819.4/819.4/819.4 | 264.8/333.7 | 1 (4597.9/4597.9) | – | 70360 | 60668 | 31744 | 9324 | – | 0 | 0 | – | – | – | – | 0 | 325 |
| main-D500 | hook, delay 500 ms | yes | 107 | **98** | 113 | 1 | 2152.0/2152.0/2152.0 | 543.0/589.4 | 1 (4797.3/4797.3) | – | 70376 | 59932 | 31744 | 10868 | – | 0 | 0 | – | – | – | – | 0 | 270 |

- `main-ABgs-first` is an earlier run of `ABgs` on clean `main`, with a 180 s duration and
  without B subscribing to connections-free. It gives the same picture: A received 5
  notifications against `notify_rx` 3497, read p95 4.0 s and 18 GetServices bursts of
  4.3–4.5 s.
- **Idle heap after boot:**

  | Build | `heap` | `heap_largest` |
  |---|---|---|
  | main | 86 332 | 40 960 |
  | ring | 74 640 (72 640 after the final flash) | 31 744 |

- **Disconnect reasons in the `ABf` logs.** The `/log` ring holds 4 KiB, so early lines
  rolled off and these counts cover the later part of each run only:

  | Build | Reasons |
  |---|---|
  | main | `send failed` ×6 (main's 20 s "send queue still full … reaping"), `closed by protocol` ×1 |
  | ring | `send stalled 20 s (reaped)` ×3, `send failed (reaped)` ×2, `TX ring full (reaped)` ×1 |
  | ring 8k | `send stalled 20 s (reaped)` ×4, `send failed (reaped)` ×2 |

## Findings and caveats

1. **The headline difference shows only when the stalled client has bulk traffic
   (`ABf`).** In an advert-only stall (`AB`), the Mac's receive buffer absorbs B's adverts
   for the whole run, so no build ever reaps B and A is unaffected everywhere.
   - `SO_RCVBUF` 1024 was raised to 32 942 by macOS.
   - Measured separately: after 45 s of stall only 8.9 KB of adverts was waiting.

   A real HA host's buffer is likewise large, so an advert-only stall is mild. The `ABf`
   stall is the one that fills lwIP and the ring.
2. **The loss on `main` in `ABf` happens before the GAP counter.**
   - `notify_rx` (1238) ≈ A's 1237, while the Mac sent 1657 contiguous sequence numbers.
     So about 419 notifications never reached NimBLE's GAP event. The Mac also sent only
     about half as many as in the ring runs (1657 vs 3460), so the link was
     flow-controlled.
   - Both fit the host task being held (`main` sends from the host task under the shared
     TX lock while a stalled client's connection task blocks in `send()`). Its ACL
     buffers then back up.
   - This is inference from the counters. No trace of the host task was taken.
3. **The delay injection agrees with item 2.** Holding the host task for 500 ms per
   notification loses notifications outright (98). At 50–200 ms the source is throttled
   and GATT reads starve instead.
   - `main-D50` also had one BLE disconnect (reason 534 = 0x216, terminated by local
     host) at 56 s. A reconnected within 1 s.
4. **The GetServices re-discovery bug affects both builds** (`ABgs`). After a repeated
   GetServices, notifications on the existing subscription are dropped as `unknown
   handle: 27`. The device's `notify_rx` keeps counting (2297) while A receives nothing.
   - Each GetServices took about 4.6 s, so `ensure_discovered` re-discovered every time.
     The likely cause is that discovery of the Mac's encrypted characteristics (17, 22;
     NotifyRequest on 19 returned −3) fails part-way, `g_partial` stays set, and the next
     GetServices retries and replaces the cache.
   - This matches the "unknown handle" open question in CLAUDE.md and issue #2. It
     reproduces on demand with this peer. It is not caused or fixed by the ring, and it
     needs its own fix.
5. **Heap margin on the 16 KiB ring under `ABf`** was the lowest measured: `heap_min`
   11 516 and `heap_largest` 11 264 (main: 13 996 / 15 360). The 8 KiB cap was better
   (14 928 / 12 800), as on the C3.
   - The static rings cost about 12 KB of idle free heap. A flooding stalled client
     transiently costs about 30 KB on top: its lwIP queue (about 7.5–12.4 KB), its
     connection task, and the RX pbufs of its unread requests.
   - Nothing failed (`oom` 0, no `xTaskCreate` failure), but 11 KB largest is close to
     the 8 KB connection stack. Consider the 8 KiB cap on the S3 too, or a smaller ring.
6. **`api_enq_us_max` reached 8.4 ms (16 KiB) and 12.6 ms (8 KiB) in `ABf`**, and 3–7 ms
   elsewhere. That is the longest send call that did not wait for ring room, so some
   producer (possibly the host task) spends milliseconds on lock contention or encoding
   while a stalled client is being served. It cost no notifications here. The test does
   not show which caller it was.
7. **Log classification (minor).** When A sends DisconnectRequest and closes immediately
   (RST), the ring build logs `reason=send failed (reaped)` and `main` logs `closed by
   protocol`. The client's behaviour is the same; only the label differs.
8. **Tooling notes.**
   - The `idf_ext.py` ELF-archive hook did not fire for `idf.py -B <dir>
     -DSDKCONFIG=… -p … flash` (no `elf-archive` line, no index entry). Every image was
     archived by hand with `elf_archive.py archive usbmodem101 --method serial`.
   - aioesphomeapi is not installed, so the raw client stands in for it.
   - The board has no esp-bootguard bootloader; this project does not use it. Every
     image here had already passed QEMU or been run before flashing.

## Board state

- The board was left running **api-tx-ring, 16 KiB cap** (ELF ff0088cd, DeviceInfo
  compile 21:20:33). After the final flash, `api_tx_stack_free` was 1876 and `heap` was
  72 640.
- The Mac peripheral was stopped.
- The `main` worktree in the scratchpad was removed; its patch is saved as
  `bench-api-tx-ring-s3/main-delay-hook.diff`.
- `/clone` passkey state: not changed. The `/passkey` POST attempt returned 404 on this
  build.

## Follow-up 2026-11-14: GetServices from cache

Fix: `gatt_discovery::run` walks the peer only when the attribute cache is
empty; a repeat GetServices answers from the cache (after a partial walk it
fills only the gaps). Same `ABgs` phase (`runset.sh gsfix ABgs`), api-tx-ring
+ 8 KiB cap, ELF 063beb35:

| | ring-ABgs (before) | gsfix-ABgs (after) |
|---|---|---|
| A notifies (handle 27) | 5 | 2240 (peripheral sent 2246; seq_lost 0) |
| `unknown handle` lines in /log | 58 | 0 |
| GetServices time, first / repeats | 4618 ms / 4144–4816 ms | 4618 ms / 21–408 ms |
| A read p95 | 4010 ms | 313 ms |

The device log shows `services from cache` for each repeat. Raw: `runs/gsfix-ABgs*`.
