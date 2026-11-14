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
// frames and ships it over the active client socket.
//
// `encode` returns the number of bytes written (0 → drop the message).
// Returns false if there is no active client OR send failed — caller
// should not retry from a callback hot path.
using EncodeFn = size_t (*)(void *ctx, uint8_t *buf, size_t cap);
bool send_async(uint16_t msg_type, EncodeFn encode, void *ctx);

// Targeted variant: send only to the API client with this id (the
// monotonic id api_server assigns each accepted connection and passes to
// bt_handlers as Context::client_id). Returns false — without encoding —
// if that client is no longer connected, so a late BLE callback for a
// client that left is dropped instead of reaching whoever got its fd.
bool send_to(uint32_t client_id, uint16_t msg_type, EncodeFn encode, void *ctx);

// True while a client is connected. Cheap, lock-free check for
// callbacks to bail out early before doing real work.
bool has_active_client();

// IPv4 address (network order) the API client `client_id` connected from.
// False if that client is gone. Loopback for the dashboard's /api bridge.
bool client_peer_ip(uint32_t client_id, uint32_t *ip_out);

// Send-queue cap bookkeeping for /stats.json (NBP_API_CLIENT_TXQ_CAP).
// Heap figures are the bytes lwIP holds for a client's unsent + unacked
// segments, measured at that client's last send.
struct TxqClient {
  uint32_t id;
  uint32_t last;  // at the last send
  uint32_t max;   // high-water mark of this connection
};
struct TxqStats {
  uint32_t cap;      // 0 = no cap
  uint32_t max_all;  // high-water mark since boot, all clients
  uint32_t drops;    // advert batches dropped for a client over the cap
  uint32_t reaps;    // clients reaped for staying over the cap
  uint32_t refused;  // connections refused: all client slots taken
  bool busy;         // a send held the TX lock; per-client figures omitted
  uint8_t n_clients;
  TxqClient clients[8];
};
TxqStats txq_stats();

// esp_timer_get_time() of the last frame from an API client that reached us
// over the STA uplink (not loopback, not the SoftAP subnet); 0 if none.
int64_t last_rx_us();

}  // namespace api_server
