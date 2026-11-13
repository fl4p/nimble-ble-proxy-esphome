# WiFi dynamic buffers 32 → 16: throughput and heap cost (2026-11-13)

Question: does cutting `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` and
`DYNAMIC_TX_BUFFER_NUM` from 32 to 16 cost throughput or latency on the NAT
S3 (.231), and does it bound the RX burst that took the heap to 1.9 KB on
2026-11-12 18:02 (12 failed 1700 B allocations, caps 0x1800)?

Short answer: it costs no measurable throughput, but it does not bound the
burst either. Under a reproduced burst the heap still falls to 1.4–5.2 KB.
The change only moves where frames are dropped: B drops them in the driver,
while A drops them through failed `malloc` calls.

## Configurations

All three images were built from the same tree (branch `wifi-bench`,
`dbb8ecf` plus the bench commit) with IDF 6.0.3 and a fresh `sdkconfig` from
`sdkconfig.defaults`. Each was boot-tested in QEMU (it reaches `wifi_init` and
`phy_init`, the furthest QEMU gets without WiFi).

| | build | dyn RX | dyn TX | bench | ELF |
|---|---|---|---|---|---|
| A | 0.2.4-benchA | 32 | 32 | on | cce9358a |
| B | 0.2.4-benchB | 16 | 16 | on | 12158bf2 |
| final | 0.2.4-heap2r | 32 | 32 | off | 1a03559e |

The built `sdkconfig` of A and B differs only in the two buffer counts.
Common to both: `STATIC_RX_BUFFER_NUM` 10,
`TX_BUFFER_TYPE` 1 (dynamic), `AMPDU_TX/RX` on with `TX_BA_WIN`/`RX_BA_WIN` 6,
`RX_MGMT_BUF_NUM_DEF` 5, `LWIP_TCP_SND_BUF_DEFAULT`/`TCP_WND_DEFAULT` 5760,
`TCP_MSS` 1440, tcpip mbox 32, tcp/udp recvmbox 6. The final image has the
same `size` output as 0.2.4-heap2 (fc6a6199), with text, data and bss
identical.

C (RX 16 only) and a 24/24 midpoint were not run, because A and B did not
differ materially in throughput.

## Method

- **Bench endpoint** (`CONFIG_NBP_BENCH`, default n, `components/bench`). It
  runs in its own task with its own listener on :8081, because the dashboard
  httpd serves one request at a time and a transfer there would block the
  `/stats.json` polling. `GET /bench?bytes=N` streams N bytes. `POST /bench`
  drains the body and answers `Expect: 100-continue`. `GET /benchmin[?reset=1]`
  reports the heap and a heap low-water mark that can be reset with
  `heap_caps_monitor_local_minimum_free_size_start()`. A UDP sink on :5009
  counts datagrams. The bench task costs about 7 KB of heap in A and B: idle
  heap is ~52 KB instead of ~59 KB without it.
- **Driver**: `scripts/nbpbench.py`, run on havan (eth0, 192.168.1.200). Each
  phase resets the low-water mark, runs, waits 1 s, then reads `/benchmin`.
  Throughout every phase, `/stats.json` is polled at 1 Hz and
  `ping -i 0.2 192.168.1.231` runs. The sequence is: 60 s idle; 5 × GET 4 MB;
  5 × POST 4 MB; UDP 1400 B at 500, 1000, 2000, 4000 and 8000 pps for 3 s,
  then unthrottled for 2 s; UDP 100 B unthrottled for 2 s, about 180 k pps
  offered (the burst reproduction); 20 s idle. Order: A1, then B1 (cut short by
  a WiFi drop, see below), then B2. B3 repeats the 100 B flood 3 times on B.
  The first `/stats.json` poll came at least 50 s after each OTA.
- **Not available**: `iperf3`/`iperf` are not installed on havan (checked with
  `which`), and nothing was installed. The bench endpoint with `curl` timing was
  used instead, as planned. There is no NAT throughput test (see below).
- Raw data: `docs/wifi-buffer-throughput-raw.tgz`, which holds `stats.jsonl`
  and `phases.jsonl` per run, plus `heap2-pre` (idle on 0.2.4-heap2 before
  any OTA) and `final-idle`.

## Results

| | A (32/32) | B (16/16) |
|---|---|---|
| GET ESP→havan, MB/s, median (min–max) | 0.190 (0.150–0.289), n=5 | 0.228 (0.204–0.259), n=10 |
| POST havan→ESP, MB/s | 0.131 (0.092–0.178), n=5 | 0.144 (0.080–0.230), n=8 ¹ |
| ping during GET, p50 / p95, median of runs; loss | 32 / 113 ms; 3/544 | 25 / 73 ms; 3/936 |
| ping during POST | 48 / 581 ms; 20/869 | 36 / 168 ms; 28/1203 |
| ping idle, p50 / p95 | 13 / 72 ms | 10–11 / 58 ms |
| BLE notifies forwarded, idle | 20.9 /s | 21.8 /s |
| BLE notifies during TCP phases | 15.5–22.0 /s | 17.6–22.6 /s |
| `api_txq_max` (drops, reaps) | 13 968 (0, 0) | 12 448 (0, 0) |
| heap low-water during GET, median (min) | 13.7 KB (1.4 KB ²) | 13.7 KB (10.1 KB) |
| heap low-water during POST | 14.2 KB (7.8 KB) | 18.7 KB (13.4 KB) |
| UDP 1400 B received by the sink per 3 s, 500 pps … unthrottled | 239–857 | 303–902 |
| **100 B flood: heap low-water** | **1.9 KB** (n=1) | **1.4, 5.2, 1.4, 4.3 KB** (n=4) |
| **100 B flood: failed allocations** | **189 × 1700 B** | 0–2 per flood, 1230–1285 B, caps 0x80c |
| 100 B flood: BLE notify forwarding | continued at ~21 /s | stopped for 2–3 s in 3 of 4 floods |

For reference, 0.2.4-heap2 idle before the test showed 21.9 notifies/s, heap
52–59 KB and ping p50 / p95 of 13 / 60 ms. The final image idle shows
21.3 /s, heap 56–60 KB and 11 / 61 ms.

¹ B1's fourth POST was cut off by a 47 s STA disconnect; see below.
² One A GET run fell to 1.4 KB with no failed allocation. Nothing in that run
points at the buffers; other traffic (NAT, API) shares the heap.

Batmon (`docker logs app_local_batmon`) logged no errors during any test
phase in A or B. Its only errors were around the three OTA reboots and the
B1 WiFi drop.

### Throughput

The STA path runs at only ~0.2 MB/s (~1.7 Mbit/s) in either config. TCP
runs varied 2–3× between runs, and the UDP sink took in only 100–300
datagrams/s whatever rate was offered. The bottleneck is airtime and the
radio (APSTA plus a BLE link plus scanning on one radio, under coex), not
buffers. Each ESP-terminated socket also has a 5760 B window, so it never
has more than about 4 segments in flight, far below 16 buffers. B is not
slower, and in these runs it was slightly faster with lower ping tails;
that difference is within run-to-run noise.

### Heap and the burst

The 100 B flood reproduces the field event. In A, the heap fell to 1.9 KB
with 189 failed 1700 B allocations, the same size as the 18:02 failures.

In B the 1700 B failures are gone: the driver stops at 16 buffers and drops
frames itself. **The heap still reached 1.4–5.2 KB in all four floods.**
Sixteen RX buffers are ~27 KB, which is about the whole free-heap margin of
this device. Heap at the 1 Hz polls swings between ~30 and ~50 KB even when
idle in the bench builds, and between 52 and 59 KB on heap2. The 1 Hz polls
cannot separate how much of the drop is RX buffers and how much is lwIP
pbufs, mbox messages and other traffic. The few failures in B are a
different allocation (1.2–1.3 KB, caps 0x80c = internal|DMA|8-bit), not the
1700 B RX buffer.

During B floods, BLE notify forwarding stopped for 2–3 s in 3 of 4 floods.
In the single A flood it kept going. With n=1 for A, this is not
established, but it is a possible side effect to check before shipping.

### WiFi drop during B1

At uptime 366 s, during B1 `tcp_post_3`, the STA disconnected and took 47 s
to reconnect. The log shows `wifi: disconnected; retrying` ×9 and
`Haven't to connect to a suitable AP now!`. Both API clients were dropped,
and batmon lost the BMS from 09:28:39 to 09:29:45 UTC. The reason code is
not logged: `wifi_sta` prints no reason and the driver is at WARN.

The batmon log shows the same signature twice on 0.2.4-heap2 that morning:
notify timeouts, then an API disconnect about 30 s later, then a 0.05 s
reconnect, at 05:25 and 07:07. No drop happened in B2 or B3, about 15 more
minutes of load on B. One event cannot be attributed to B. Logging
`wifi_event_sta_disconnected_t.reason` in `wifi_sta.cpp` would settle this
next time.

## NAT path (not measured)

No NAT throughput test was possible without touching fry or flat. The
port-forwards (`GET /nat`) map 232–235 → :23 and 242–244 → :24 on the
SoftAP clients:

- :23 is the fugu telnet console, which takes commands and produces no bulk
  data.
- :24 is the fugu `ScopeService`. A client connecting to it makes the board
  raise its WiFi TX power to 21 dBm and start streaming raw ADC samples,
  which changes the board's state. Neither was used.

The NAT counters did not differ between configs: uplink from fry/flat
(`ap_rx`) was ~2.9 KB/s and downlink (`ap_tx`) ~0.2 KB/s in A, B and the
final image.

The NAT forward path is where `DYNAMIC_TX_BUFFER_NUM` matters most: LAN
traffic queued toward a slow SoftAP client. An ESP-terminated socket never
exercises it. A proper test needs:

- a client you control joined to `pwr-statione`, such as a laptop or Pi with
  WiFi (the PSK is in Fab's private config). havan has only eth0.
- `iperf3` on that client and on havan. Installing it on havan needs Fab's
  OK.
- either a temporary `/portmap` to the client, or the client connecting
  out through NAT to havan. Run both directions, plus a UDP flood toward the
  client, and watch `/stats.json` `heap`/`oom` and fry/flat telemetry.

## Recommendation

Do not ship 16/16 as the fix for the burst. It is free in throughput on this
link, but it does not keep the heap off the floor: the 16-buffer pool is
still about as large as the free heap. It turns failed 1700 B `malloc`s into
driver drops, which is cosmetic for the `oom` counter. It may stall BLE
notifies during a flood (n=1 comparison). TX 16 is untested on the NAT path.

If the goal is to survive such a burst, either:

- cap RX lower, so that the pool is clearly below the free heap. RX 8 is
  ~13.6 KB, but that is untested and close to the RX BA window of 6, so
  measure A-MPDU throughput first.
- free more permanent heap from the levers in CLAUDE.md, "Heap budget".

Either way, first add the disconnect reason to the WiFi log, and run the NAT
test above before changing TX.

## Side effects of the test on .231

- Three OTAs, at 09:12:53, 09:22:16 and 09:40:51 UTC (reboot times). Each
  cost fry/flat ~20–30 s (SoftAP down from upload start through boot), and
  batmon ~50–55 s from upload start to the BMS reconnecting. The B1 WiFi drop caused one more gap,
  about 70 s for batmon. No crash records were added; `/crash` holds only
  nonces 21489805 and dc58a065 throughout.
- **The SoftAP DHCP leases changed after the reboots.** Before: fry
  192.168.4.2, flat .4. After the final OTA: flat .2, fry .3. The
  port-forwards are by IP, so 232/242 now reach flat, 233/243 reach fry, and
  234/244 (.4) reach nobody. They were not changed.
