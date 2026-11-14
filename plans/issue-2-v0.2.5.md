# Issue #2: v0.2.4 field results, verification and fix plan (v0.2.5)

2026-11-13. Input: `docs/issue-2-v0.2.4-field-results.md` (batmon-ha session), the
reporter's comments on #2 (2026-11-12 18:04–18:26 UTC), and the two batmon logs on
batmon-ha#391. Code state: `main` = v0.2.4 + 4 commits (dbb8ecf raised the send-queue
cap 8 → 16 KiB, unreleased; the reporter runs v0.2.4, cap 8 KiB).

## Verification of the three claims

### 1. Notifications lost (`notify_rx` − `notifies`): partly right, wrong mechanism

- **The gap cannot contain send-queue drops.** `notify_cb` (bt_handlers.cpp:197) bumps
  `notifies` *before* `send_to`, and a notify is never "lossy" (`is_lossy` = adverts only):
  over the cap it waits, it is not dropped. So `notify_rx − notifies` counts only what was
  dropped before forwarding: NimBLE `unknown handle` drops (no attribute cache yet),
  notifications on a characteristic with no callback yet (before batmon's start_notify),
  and links with no owner.
- **Most of the gap is pre-subscription traffic.** The JK BMS notifies from the moment
  it connects. The log shows `NimBLEClient: unknown handle: 18` bursts right after every
  `onConnect`, before discovery: 11 lines before snapshot 1 (gap 30), 44 before
  snapshot 2 (gap 87). The rest is consistent with silent "no callback yet" drops. Those
  are not lost *data* — batmon had not subscribed yet. We cannot prove the split with the
  current counters.
- **The CRC failures are real and happen mid-session**, on consecutive JK frame
  counters, not only at subscribe. They line up with the proxy's send-queue over-cap
  episodes: mapping batmon's clock onto proxy uptime (its reconnect at 20:54:38 = proxy
  client id=2 at 291.255 s), 17 of 18 CRC failures fall within −1.5…+2 s of an over-cap
  log line, vs 47 % for random times in the same window. Moderate evidence (dense
  episodes, 1 s log resolution), not proof.
- **Likely mechanism (hypothesis, to measure):** in v0.2.4 an over-cap notify waits in
  `send_capped` in 50 ms polls *on the NimBLE host task*. While the host task is blocked
  it does not drain the controller's ACL buffers (`BT_NIMBLE_ACL_BUF_COUNT=8`). A JK
  frame is ~300 B over MTU 131 = 3 notifications ≈ 15 LL fragments; losing one fragment
  loses the ATT PDU before the GAP layer sees it, so `notify_rx` never counts it either.
  That would also explain why neither counter shows the loss.
- "0 CRC failures in v0.2.2/v0.2.3" is confounded: v0.2.3 delivered no notifications at all
  after a reconnect (the bug 0.2.4 fixed), so it had far fewer frames that could fail.

### 2. Heap OOM "triggered by the HTTP handlers": correlation right, causality unproven

- The handlers already stream: `/crash` through a 512 B `JsonOut`, `/coredump` in 2 KiB
  chunks, `/log` in 1 KiB. None allocates its whole output. So "stream instead of
  allocate" is not the fix.
- The `oom` reports begin within the 2 s window that contains
  `httpd_uri: uri handler execution failed` (221.1 s). Neither `/coredump aborted` nor
  `GET /crash failed` was logged, so the failing handler was either a silent path (the
  chunk/JsonOut `malloc` failing, or `/stats.json`/`/log` send failures) or another URI. We
  cannot tell which: httpd does not log the URI. The OOM may have caused the handler
  failure, not the other way round.
- What held the heap: free 7–9 KB, largest 1280 B, for 22 s, until the API client was
  reaped (`frame not out within 20000 ms (0/46 B)`); right after, free was 20 KB. So the
  stalled API socket held ~12 KB. The 1700 B failures are WiFi RX buffers. With them
  failing, the incoming ACKs are dropped, so the API queue cannot drain: the same
  death spiral as the NAT S3 (`docs/wifi-buffer-throughput.md`).
- The steady-state budget is the root: the boot heap is 49 KB. Steady state is 21–32 KB
  free with one API client at ~9.3 KB of send queue (claim 3). An HTTP response with
  ~6 KB in flight (`TCP_SND_BUF` 5760 B ≈ 4–5 full segments) then pushes it under the WiFi
  RX threshold. A big body (`/coredump?raw=1` = the whole 128 KiB region, which the boot
  log tells the user to fetch) keeps it there.
- After a handler fails, httpd closes the socket gracefully. Any queued response data then
  lingers in FIN-WAIT, retransmitting — the problem `set_abortive_close` fixed for API
  sockets in 0.2.3.

### 3. The cap measures heap overhead: observation right, proposed fix wrong

- Every over-cap line reads `9.3 KB heap, 6 segs, 300–650 B`: with `TCP_NODELAY` each
  small frame is its own segment, and `LWIP_NETIF_TX_SINGLE_PBUF` makes each one a
  full-MSS pbuf (~1.55 KiB), held until the late ACK (~250 ms) arrives. That heap is real.
  Measuring payload instead would hide exactly the memory that ran out in claim 2.
- `CONFIG_LWIP_TCP_OVERSIZE_DISABLE` cannot shrink the pbufs: lwIP `#error`s when
  `TX_SINGLE_PBUF` lacks `TCP_OVERSIZE` (init.c:232), and with `TX_SINGLE_PBUF` the
  pbuf is always `max_length`, whatever the OVERSIZE choice (tcp_out.c:238).
- The fix is fewer segments: coalesce small frames into one segment.
- The 20 s reap (109.3 s) happened with no BMS link up, so only advert batches were
  arriving. The queue sat at 6 segments for 20 s, which is a genuine stall (no ACKs), not a
  miscount. Separately: dbb8ecf's 16 KiB cap stops the healthy-client trips. On a C3,
  though, 2 clients × 16 KiB exceeds the whole steady-state free heap.

### Also found (not in the doc)

- **BMS links drop with `reason=520`** (0x208 = HCI 0x08, supervision timeout), every
  20–130 s, while RSSI is fine. These are BLE radio losses, not proxy reaps; the reaps
  are rarer. Coex is left at `BALANCE` on a STA-only build, and the scan runs at 50 %
  duty (30/60 ms) next to 2 connections.
- **batmon opens two API clients** (`nimble-proxy @ 192.168.2.83` and `192.168.2.83` in
  its log). With `NBP_API_MAX_CLIENTS=2`, a third connection (HA, or a reconnect while the
  old socket is still registered) is refused, as `Connection reset by peer` / `EOF
  received` on connect. → batmon-ha session.
- `/coredump` boot warning on a fresh flash: `ESP_ERR_INVALID_SIZE` means the header
  word is neither blank (0xFFFFFFFF) nor plausible — leftover data in 0x3E0000 from an
  older image, not a crash.

## Plan

### A. v0.2.5 (small, targeted)

1. **Coalesce API frames: Nagle on (drop `TCP_NODELAY`)** on api_server sockets. A healthy
   client then holds ~1 unacked + 1 filling segment (~3 KiB) instead of 6 × 1.55 KiB.
   Cost: a frame can wait one ACK (≤ ~250 ms here) — fine for BMS notifies and GATT
   ops. Measure on the S3 bench (.231, batmon + HA) before choosing: `api_txq` last/max,
   heap, GATT read latency, with and without.
2. **Cap per target**: keep 16 KiB on the S3. On the C3, set 8 KiB in
   `sdkconfig.defaults.esp32c3` (with Nagle, a healthy client stays far below it).
3. **Notify-loss accounting** in `/stats.json`: `notify_unknown` (no attribute),
   `notify_unsubscribed` (no callback), `notify_unowned`, and
   `notify_lost = notify_rx − (forwarded + the three)`. Notifies on a subscribed
   characteristic that never reach `send_to` would then be visible. Also `notify_wait_ms_max`:
   the longest time a notify blocked the host task.
4. **HTTP under low heap:**
   - (a) A handler wrapper logs URI, heap and largest block whenever a handler fails
     (the missing evidence).
   - (b) Abortive close (SO_LINGER {1,0}) on the httpd socket when a handler fails.
   - (c) Large bodies (`/coredump`, `/coredump?raw=1`, `/devices`, the dashboard page)
     return 503 when the largest block is < 16 KiB.
   - (d) `/coredump` streams paced: before each chunk, wait until the socket's queue is
     ≤ 2 segments (the `txq_query` used by api_server). Give up (abortive) after the
     httpd send timeout.
   - (e) The boot hint names the size and asks for `?raw=1` only when needed.
5. **Bench test of the host-task-blocking hypothesis** before claiming it on #2:
   - Inject a 50/200/500 ms delay in `notify_cb` on a test build, with a notifying peer.
     The fugu BLE target is one; it needs a sequence/CRC check on the receiving side
     (a small aioesphomeapi script).
   - Count gaps.
   - If confirmed, plan B is the fix and A.1 the mitigation.

### B. Next (bigger): no socket I/O on the NimBLE host task

A per-client TX ring (~2–4 KiB static each), drained by one sender task:

- It writes many frames per `send()`, the way ESPHome's own API batches per loop.
- The host task never blocks.
- When the ring is full, adverts are dropped; a notify that does not fit reaps that client
  instead of stalling BLE for everyone.

This removes the 20 s host-task hold documented in CLAUDE.md. Separate PR, after A
ships and the bench test says how much it matters.

### C. Radio (reason=520), investigate after A

- Try `esp_coex_preference_set(ESP_COEX_PREFER_BT)` while a GATT link is up on STA-only
  builds (it is WiFi-biased only for the SoftAP).
- Pause or thin the scan while both slots are connected.
- Measure on the C3 against a peer: link lifetime, reason codes.

### Communication (after Fab approves and A ships)

- **Post on #2:**
  - What 0.2.5 changes.
  - Ask the reporter to say which URL they were fetching when the OOM began.
  - Ask for `/stats.json` with the new counters.
  - Explain that `ESP_ERR_INVALID_SIZE` is leftover flash data, not a crash.
- **Message the batmon-ha session:**
  - the CRC/over-cap correlation;
  - the two batmon API clients and the 2-client limit;
  - ask batmon to log the length of a CRC-failed JK frame, which tells a missing
    notification (a 128 B hole) from corruption.
