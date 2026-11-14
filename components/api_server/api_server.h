// Plaintext aioesphomeapi server. Up to NBP_API_MAX_CLIENTS clients. Spins a
// FreeRTOS task that owns the listening socket and per-connection
// handlers; main just calls start() once after wifi is up.

#pragma once

#include <cstddef>
#include <cstdint>

namespace api_server {

// Spawn the listener task. Idempotent — second call is a no-op.
void start();

// Async send entry point for ble_backend (called from NimBLE host task,
// timer tasks, etc.). Acquires the TX mutex internally, asks `encode`
// to write the payload starting at the caller-supplied buffer, then
// frames it and copies it into every client's TX ring; a sender task puts
// it on the wire. Never does socket I/O. Only a client's own connection
// task waits (bounded by the 20 s send deadline), and only for that
// client's ring; it then calls `encode` again, so `encode` must be a pure
// function of ctx. Every other caller (NimBLE host task, timers, the advert
// flush task, other clients' tasks) only copies.
//
// `encode` returns the number of bytes written (0 → drop the message).
// Returns true if the frame was queued for at least one client; false if
// there is no client, or it was dropped (advert, ring full) or the client
// was reaped (other frame, ring full). Callers should not retry from a
// callback hot path.
using EncodeFn = size_t (*)(void *ctx, uint8_t *buf, size_t cap);
bool send_async(uint16_t msg_type, EncodeFn encode, void *ctx);

// Targeted variant: send only to the API client with this id (the
// monotonic id api_server assigns each accepted connection and passes to
// bt_handlers as Context::client_id). Returns false — without encoding —
// if that client is no longer connected, closing or reaped, so a late BLE
// callback for a client that left is dropped instead of reaching whoever
// got its fd. Returns true once the frame is queued in the client's ring
// (not when it is on the wire); false if it did not fit and the client was
// reaped for it.
bool send_to(uint32_t client_id, uint16_t msg_type, EncodeFn encode, void *ctx);

// True while a client is connected. Cheap, lock-free check for
// callbacks to bail out early before doing real work.
bool has_active_client();

// Transmit bookkeeping for /stats.json. Heap figures are the bytes lwIP
// holds for a client's unsent + unacked segments (NBP_API_CLIENT_TXQ_CAP),
// measured at the sender's last look; ring figures are bytes waiting in
// the client's TX ring (NBP_API_CLIENT_TX_RING).
struct TxqClient {
  uint32_t id;
  int fd;             // server-side socket (self-tests: fd reuse)
  uint32_t last;      // lwIP queue heap at the last look
  uint32_t max;       // its high-water mark this connection
  uint32_t ring;      // bytes in the ring now
  uint32_t ring_max;  // ring high-water mark this connection
};
struct TxqStats {
  uint32_t cap;        // 0 = no cap
  uint32_t ring_size;  // per-client ring bytes
  uint32_t max_all;    // lwIP queue high-water mark since boot, all clients
  uint32_t drops;      // advert batches dropped: ring (half) full
  uint32_t ring_reaps; // clients reaped: a frame that must not be lost did not fit
  uint32_t reaps;      // clients reaped: queued data not moving for 20 s
  uint32_t refused;    // connections refused: all client slots taken
  uint32_t frames;     // frames queued since boot
  uint32_t sends;      // sendmsg calls that moved bytes (frames/sends = coalescing)
  uint32_t bytes;      // bytes moved into lwIP
  uint32_t enq_us_max; // longest send_async/send_to that did not wait for room
  bool busy;           // always false (kept for the api_txq_busy field)
  uint32_t tx_stack_free; // api_tx stack high-water: bytes never used
  uint8_t n_clients;
  TxqClient clients[8];
};
TxqStats txq_stats();

// esp_timer_get_time() of the last frame from an API client that reached us
// over the STA uplink (not loopback, not the SoftAP subnet); 0 if none.
int64_t last_rx_us();

}  // namespace api_server
