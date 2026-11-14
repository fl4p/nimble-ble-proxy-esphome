# Issue #2: v0.2.4 field results on the reporter's ESP32-C3

Written 2026-11-13 by the batmon-ha session for the next nimble-ble-proxy session.

Sources:

- The reporter's comments on nimble #2, 2026-11-12 18:04–18:26 UTC (proxy logs, `/stats.json`, `/wifihist`, `/crash`).
- Two batmon logs on fl4p/batmon-ha#391:
  - https://github.com/user-attachments/files/34762769/2af0a32d_batmon_2026-11-12T18-05-13.704Z.log
  - https://github.com/user-attachments/files/34763037/2af0a32d_batmon_2026-11-12T18-18-02.095Z.log

Setup: the proxy is an ESP32-C3 Super Mini at 192.168.2.83. WiFi TX power is 8 dBm, and RSSI is -23 to -30 (the reporter: "the esp is always in the same place"). One API client (batmon) holds two JK BMS links (c8478009c501, c84780712397).

## Fixed in 0.2.4

On-demand discovery works. The log shows `api.bt: C8:47:80:71:23:97: no GetServices since connect; discovering on demand`, and batmon receives data again after a reconnect.

## Problem 1: notifications are lost

| `/stats.json` snapshot | `notify_rx` | `notifies` | Lost |
|---|---|---|---|
| 1 | 2435 | 2405 | 30 |
| 2 | 5007 | 4920 | 87 (1.7 %) |

The proxy received these notifications from BLE but never forwarded them.

In the batmon logs, 32 frames failed with `JK BMS crc check failed, discarding frame 0x02`, over about 25 samples. A JK frame is about 300 B over MTU 131, so one lost fragment ruins the whole frame.

The batmon logs from v0.2.2 and v0.2.3 had 0 CRC failures. So the loss is new in 0.2.4, or it is caused by problems 2 and 3.

Suggestion: count where each lost notification goes (an OOM or a txq drop). Under queue pressure, drop only adverts, never notifications.

## Problem 2: heap OOM

The log repeats `oom: N failed heap allocation(s) ... last 1700 B caps 0x1800`. These are internal allocations, probably WiFi buffers.

- **Worst stretch:**
  - free about 8 KB, largest block 1280 B, minimum 1720 B.
  - 10–30 failed allocations every 2 s, for about 30 s.
- **Trigger:** it began right after `httpd_uri: uri handler execution failed`, while the reporter was fetching `/stats.json`, `/crash` or `/coredump?raw=1`.

Suggestion: have the HTTP handlers stream their output instead of allocating it, or refuse the request when the largest free block is below a threshold.

## Problem 3: the send-queue cap trips on overhead

The queue sits at about 9.3 KB of heap with only 300–650 B unacked or unsent (6 segments), over `api_txq_cap` 8192. This causes:

- **Dropped adverts:** the proxy repeatedly logs `dropping its advert batches`.
- **A reap:** once the queue stayed over the cap for 20 s, and the proxy reaped the client: `over the send-queue cap for 20000 ms; reaping`. The reap releases both BMS links.

The heap overhead per segment seems to dominate: about 1.5 KB per segment for about 90 B of payload. So the cap measures overhead, not real backlog.

Seen from batmon:

- `unexpected disconnect from ESPHome API` every 1–2 minutes.
- BMS links last 20–130 s.

Suggestion: measure the cap in payload bytes, not heap.

## Crash record

`/crash` shows `store_was_valid: false` with no records, and the core dump is `invalid` (`ESP_ERR_INVALID_SIZE`). The earlier JK panic record (v0.2.3) was lost, probably when the reporter reflashed.

## Stock ESPHome (batmon #391, handled by the batmon session)

The reporter says the stock ESPHome proxy `bluetooth-proxy-garaj` is "more stable". It still drops for a few minutes, "apparently a reboot", then works for a while, and the cycle repeats.
