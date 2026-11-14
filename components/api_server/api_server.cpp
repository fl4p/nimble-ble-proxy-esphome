#include "api_server.h"

#include "api_proto.h"
#include "bt_handlers.h"
#include "frame_codec.h"
#include "handshake.h"
#include "proxy_config.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/api.h"
// sockets_priv.h has no extern "C" guard (lwip_socket_dbg_get_socket).
extern "C" {
#include "lwip/priv/sockets_priv.h"
}
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace api_server {

namespace {

constexpr const char *TAG = "api_server";
constexpr size_t LISTENER_STACK = 4096;
constexpr size_t CONN_STACK = 8192;
// The sender only moves bytes from the rings into lwIP (txq_query, sendmsg,
// shutdown, a log line); the heap self-test prints its stack high-water.
// QEMU high-water with 3072: 1608 B used (1464 free), so 2560.
constexpr size_t SENDER_STACK = 2560;
constexpr UBaseType_t LISTENER_PRIO = 5;
constexpr UBaseType_t CONN_PRIO = 5;
// Above the connection tasks (draining beats parsing more requests), below
// tcpip (18) and the NimBLE host (21).
constexpr UBaseType_t SENDER_PRIO = 6;

// ---- transmit path: per-client ring + one sender task ----
//
// No producer does socket I/O. Every frame for a client is encoded once
// (into g_tx_buf, under g_tx_mutex) and copied whole, header included, into
// that client's ring (under g_clients_mutex, which also guards the client
// table, so the id check and the copy are one step). The NimBLE host task
// (notifications, connection states, ConnectionsFree), the advert flush task
// and the connection tasks therefore never wait on the network. One sender
// task, api_tx, drains every ring into its socket with MSG_DONTWAIT
// sendmsg(), as many queued frames per call as lwIP takes (at most two
// iovecs when the ring wraps), so one TCP segment carries many small frames.
//
// Ring full (the client is not draining):
// - advert batches (lossy) are only admitted while the ring is under half
//   full, so they never crowd out frames that must not be lost;
// - frames from the client's own connection task (handshake replies,
//   GetServices, GATT replies) are admitted up to RING - RING_RESERVE and
//   otherwise wait for room (bounded by the send deadline), re-encoding
//   from the caller's ctx afterwards;
// - anything else that must not be lost (NimBLE host task, timers, another
//   client's task) may use the whole ring; if it does not fit, the client
//   is marked reaped. A producer never shuts a socket down itself: it marks the slot
//   and wakes api_tx, which does the abortive shutdown.
// A frame is queued whole or not at all; a reap discards the ring.
//
// Stall: a ring that holds data while its tail has not moved for
// CLIENT_SEND_DEADLINE_MS (lwIP full, over the heap cap, or no memory) gets
// its client reaped by api_tx. The host task is unaffected meanwhile.
// 20 s: lwIP's RTO starts at ~1.5 s and doubles, so 20 s covers about three
// retransmissions on a lossy but live link (issue #2: 5 s reaped healthy C3
// clients and their BMS links).
//
// The heap cap (NBP_API_CLIENT_TXQ_CAP) gates api_tx: it moves nothing into
// lwIP while the client's unsent + unacked segments hold the cap, so a
// stalled peer pins at most the cap plus one send, and the ring.
//
// Coalescing: while the client has segments in flight and the ring holds
// less than one MSS, api_tx holds the data for up to TX_COALESCE_MS so that
// the next frames join the same segment. With nothing in flight it sends at
// once. lwIP's Nagle stays off (TCP_NODELAY): it would hold a lone frame
// until the peer's ACK (~250 ms on the field links).
//
// Teardown: the slot's `io_busy` flag is set (under g_clients_mutex) while
// api_tx does I/O on its fd; unregister_client marks the slot retired and
// waits for io_busy to clear, so a closed and reissued fd never receives a
// byte meant for the old client.
constexpr int CLIENT_SEND_DEADLINE_MS = 20000;
// A waiting producer re-checks for ring room this often.
constexpr int RING_WAIT_POLL_MS = 10;
// api_tx's wake-up period while some ring holds data it could not move.
constexpr int SENDER_POLL_MS = 10;
// Longest a small frame waits for company while segments are in flight.
constexpr int TX_COALESCE_MS = 20;
// After a protocol close (DisconnectResponse, Hello reject), how long the
// connection task waits for api_tx to hand the last frame to lwIP.
constexpr int PROTOCOL_CLOSE_FLUSH_MS = 2000;
// "no memory" send failures: logged at most this often (all clients).
constexpr int NOMEM_LOG_MS = 5000;
// An over-cap episode is logged once it has lasted this long.
constexpr int TXQ_LOG_AFTER_MS = 1000;

// TCP keepalive so a half-open client (host crashed or lost power, no
// FIN/RST) is detected even when we send it nothing: ~30 s idle + 3 × 10 s
// probes → recv() fails, the connection_task exits and releases the
// client's BLE links instead of holding them indefinitely.
constexpr int CLIENT_KEEPIDLE_S = 30;
constexpr int CLIENT_KEEPINTVL_S = 10;
constexpr int CLIENT_KEEPCNT = 3;

bool g_started = false;

constexpr size_t RING = proxy::API_CLIENT_TX_RING;
constexpr size_t MAX_FRAME = frame_codec::MAX_HEADER_LEN + proxy::MAX_MESSAGE_SIZE;
// Largest notify (also GATT read) and advert-batch frames.
constexpr size_t MAX_NOTIFY_FRAME =
    frame_codec::MAX_HEADER_LEN + proxyapi_BluetoothGATTNotifyDataResponse_size;
constexpr size_t MAX_ADV_FRAME = frame_codec::MAX_HEADER_LEN +
                                 proxyapi_BluetoothLERawAdvertisementsResponse_size;
// Ring room that frames from a waiting (bulk) producer leave free: two
// max-size notifies (see fits()).
constexpr size_t RING_RESERVE = 2 * MAX_NOTIFY_FRAME;
static_assert(RING >= MAX_FRAME + RING_RESERVE,
              "NBP_API_CLIENT_TX_RING must hold a max-size frame plus two "
              "max-size notifies");
static_assert(RING / 2 >= MAX_ADV_FRAME,
              "NBP_API_CLIENT_TX_RING / 2 must hold a full advert batch");

// Reap reasons, shown as the disconnect `reason=`.
constexpr const char *WHY_RING_FULL = "TX ring full (reaped)";
constexpr const char *WHY_STALLED = "send stalled 20 s (reaped)";
constexpr const char *WHY_SEND_ERR = "send failed (reaped)";

// Active clients. fd -1 in unused slots. Guarded by g_clients_mutex, as
// are the ring heads below.
//
// `id` is a monotonic per-connection id (never 0, never reused while the
// device is up). BLE links are owned by id, not fd: lwIP reissues the
// lowest free fd, so a reconnecting client usually gets the fd the old
// one had, and an fd-keyed owner would hand it the old client's links
// and late callbacks. Producers address slots by id.
struct ClientEntry {
  int fd = -1;
  uint32_t id = 0;
  // The client's connection_task (set by the task itself).
  TaskHandle_t task = nullptr;
  // No new frames (protocol close or teardown in progress). api_tx still
  // drains what is queued until `retired`.
  bool closing = false;
  // unregister in progress: api_tx must not touch the fd any more.
  bool retired = false;
  // api_tx is doing I/O on fd (outside the mutex).
  bool io_busy = false;
  // Non-null: reaped (why). Producers reject the slot; api_tx shuts it
  // down abortively once (`shut`) and discards the ring.
  const char *reap_why = nullptr;
  bool shut = false;
  // The client's own connection task waits in deliver() for ring room for a
  // bulk frame. Meanwhile advert batches are refused for this client and
  // api_tx does not hold data back for coalescing, so the ring empties
  // instead of being kept topped up by adverts (on the C3 a 2054 B frame
  // fits only into a near-empty ring).
  bool bulk_waiting = false;
  uint32_t peer_ip = 0;  // IPv4, network order, from accept(); for /links
};
std::array<ClientEntry, proxy::MAX_API_CLIENTS> g_clients;

// Per-client TX ring, indexed like g_clients. Producers write at `head`
// (under g_clients_mutex); api_tx reads from `tail` and is the only one to
// move it (while io_busy). `used` hands bytes over between the two. Reset
// only under g_clients_mutex with io_busy clear.
struct Ring {
  uint8_t buf[RING];
  size_t head = 0;
  size_t tail = 0;
  std::atomic<size_t> used{0};
  size_t hwm = 0;  // high-water mark of `used` this connection
};
std::array<Ring, proxy::MAX_API_CLIENTS> g_rings;

void ring_reset(Ring &r) {
  r.head = 0;
  r.tail = 0;
  r.used.store(0, std::memory_order_relaxed);
}

// api_tx state per slot. Written by api_tx while the slot is io_busy, and
// reset by register/unregister under g_clients_mutex while it is not.
struct TxqState {
  TickType_t pending_since = 0;  // ring non-empty since (0 = empty)
  TickType_t last_progress = 0;  // tail last moved, or the ring was empty
  TickType_t over_since = 0;     // lwIP queue first seen full, 0 = not
  bool over = false;
  bool logged = false;  // this over-cap episode has been logged
  bool nomem_logged = false;
  uint32_t last = 0;  // heap the lwIP queue held at the last look
  uint32_t max = 0;   // high-water mark for this connection
};
std::array<TxqState, proxy::MAX_API_CLIENTS> g_txq;
static_assert(proxy::MAX_API_CLIENTS <= sizeof(TxqStats::clients) /
                                          sizeof(TxqStats::clients[0]),
              "TxqStats::clients is too small for NBP_API_MAX_CLIENTS");
// Since boot, all clients.
uint32_t g_txq_max_all = 0;
std::atomic<uint32_t> g_ring_drops{0};   // lossy frames: ring half full
std::atomic<uint32_t> g_ring_reaps{0};   // a frame that must not be lost did not fit
std::atomic<uint32_t> g_txq_reaps{0};    // ring stalled for the deadline
std::atomic<uint32_t> g_tx_frames{0};    // frames queued
std::atomic<uint32_t> g_tx_sends{0};     // sendmsg calls that moved bytes
std::atomic<uint32_t> g_tx_bytes{0};     // bytes moved into lwIP
std::atomic<uint32_t> g_enq_us_max{0};   // longest send_* that did not wait
std::atomic<uint32_t> g_refused{0};
std::atomic<uint32_t> g_next_client_id{1};
SemaphoreHandle_t g_clients_mutex = nullptr;
std::atomic<uint8_t> g_active_count{0};

// Protects g_tx_buf, the shared encode area. Held for encode + copy only,
// never across a wait or any socket call.
SemaphoreHandle_t g_tx_mutex = nullptr;
TaskHandle_t g_sender = nullptr;

uint8_t g_tx_buf[frame_codec::MAX_HEADER_LEN + proxy::MAX_MESSAGE_SIZE];

// Make the next close/shutdown of `fd` abortive (RST) when data is still
// queued. A graceful close of a stalled socket leaves lwIP retransmitting
// that data in FIN-WAIT for minutes, holding up to TCP_SND_BUF of pbufs per
// dead connection. On a C3, a few stall/reconnect cycles of that drained
// the heap (issue #2: heap_min 744 B). Needs CONFIG_LWIP_SO_LINGER.
void set_abortive_close(int fd) {
  linger lg{};
  lg.l_onoff = 1;
  lg.l_linger = 0;
  if (::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) != 0) {
    ESP_LOGW(TAG, "fd=%d: SO_LINGER failed (errno=%d)", fd, errno);
  }
}

// esp_timer time of the last frame received from an upstream API client
// (0 = none yet). The liveness watchdog takes it as proof that the STA path
// works, so clients on loopback (the WS bridge) or on our own SoftAP subnet
// don't count: they would keep a dead uplink looking alive.
std::atomic<int64_t> g_last_rx_us{0};

ssize_t socket_read(void *ctx, uint8_t *buf, size_t n) {
  int fd = *static_cast<int *>(ctx);
  return ::recv(fd, buf, n, 0);
}

// True if `peer` (network order) reaches us over the STA uplink: not
// loopback and not inside the SoftAP's subnet.
bool is_upstream_peer(uint32_t peer_be) {
  const uint32_t host = ntohl(peer_be);
  if ((host >> 24) == 127) return false;
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  esp_netif_ip_info_t info{};
  if (ap != nullptr && esp_netif_is_netif_up(ap) &&
      esp_netif_get_ip_info(ap, &info) == ESP_OK && info.netmask.addr != 0 &&
      (peer_be & info.netmask.addr) == (info.ip.addr & info.netmask.addr)) {
    return false;
  }
  return true;
}

// ---- per-client send-queue measurement (NBP_API_CLIENT_TXQ_CAP) ----
//
// The heap a client's TCP send queue holds is NOT its byte count: IDF builds
// lwIP with LWIP_NETIF_TX_SINGLE_PBUF, so tcp_write allocates a full-MSS pbuf
// (~1.5 KiB) for every new segment, however small the write. A peer that
// stops ACKing (WiFi stall) pins up to TCP_SND_QUEUELEN (16) such segments,
// ~25 KiB per client, while lwIP's byte accounting (TCP_SND_BUF 5760) shows
// a fraction of that. So the cap is on the measured heap: the allocated size
// of every segment and pbuf on the pcb's unsent + unacked lists, read in the
// tcpip thread.
struct TxqCall {
  tcpip_api_call_data base;  // must be first: lwIP passes &base back
  int fd;
  bool ok;
  uint32_t heap;  // heap held by unsent + unacked segments
  uint16_t segs;  // pcb->snd_queuelen (pbufs)
  uint16_t bytes; // TCP_SND_BUF - snd_buf (payload bytes)
};

uint32_t seg_list_heap(const tcp_seg *seg) {
  uint32_t n = 0;
  for (; seg != nullptr; seg = seg->next) {
    // MEMP_MEM_MALLOC=1 and MEM_LIBC_MALLOC=1 (IDF lwipopts.h): segments and
    // pbufs are plain heap blocks. Containing-block size, so a MEMP_SIZE
    // offset in front of the object does not matter.
    n += heap_caps_get_containing_block_size(const_cast<tcp_seg *>(seg));
    for (const pbuf *p = seg->p; p != nullptr; p = p->next) {
      n += heap_caps_get_containing_block_size(const_cast<pbuf *>(p));
    }
  }
  return n;
}

err_t txq_query_fn(tcpip_api_call_data *c) {
  auto *q = reinterpret_cast<TxqCall *>(c);
  q->ok = false;
  lwip_sock *sock = lwip_socket_dbg_get_socket(q->fd);
  if (sock == nullptr || sock->conn == nullptr ||
      NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_TCP) {
    return ERR_OK;
  }
  const tcp_pcb *pcb = sock->conn->pcb.tcp;
  if (pcb == nullptr) return ERR_OK;
  q->heap = seg_list_heap(pcb->unsent) + seg_list_heap(pcb->unacked);
  q->segs = pcb->snd_queuelen;
  q->bytes = static_cast<uint16_t>(TCP_SND_BUF - pcb->snd_buf);
  q->ok = true;
  return ERR_OK;
}

// Heap held by `fd`'s send queue, or false if it cannot be read (socket
// already torn down: the send itself then reports the error).
bool txq_query(int fd, TxqCall *q) {
  *q = TxqCall{};
  q->fd = fd;
  if (tcpip_api_call(&txq_query_fn, &q->base) != ERR_OK) return false;
  return q->ok;
}

// Advertisement batches are the only frames a client can miss without a
// protocol consequence (the next batch carries the same devices).
bool is_lossy(uint16_t msg_type) {
  return msg_type == proxyapi::MSG_BLUETOOTH_LE_RAW_ADVERTISEMENTS_RESPONSE;
}

void raise_max(std::atomic<uint32_t> &a, uint32_t v) {
  uint32_t cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

bool live(const ClientEntry &c) {
  return c.fd >= 0 && !c.closing && c.reap_why == nullptr;
}

// Mark a slot reaped. Caller holds g_clients_mutex. api_tx does the shutdown.
void mark_reaped_locked(ClientEntry &c, const char *why) {
  if (c.reap_why == nullptr) c.reap_why = why;
}

struct Target {
  int slot;
  uint32_t id;
};

void reap_full(const Target &t) {
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  ClientEntry &c = g_clients[t.slot];
  if (c.id == t.id) mark_reaped_locked(c, WHY_RING_FULL);
  xSemaphoreGive(g_clients_mutex);
  g_ring_reaps.fetch_add(1, std::memory_order_relaxed);
}

// Ring admission. Three levels, so that the frames that must never wait
// (notifies, connection states from the NimBLE host task) always find room
// while the client drains at all:
// - Lossy (advert batches): up to half the ring.
// - Bulk (the client's own connection task: handshake replies, GetServices,
//   GATT replies; these may wait): up to RING - RESERVE, or anything into
//   an empty ring. RESERVE holds two max-size notifies, which is what the
//   host task may queue between two api_tx passes while a bulk producer
//   keeps the rest topped up (issue #2 / batmon #391: GetServices of link 2
//   while link 1 notifies).
// - Strict (everyone else; never waits, reaps on no room): up to RING.
enum class Cls { Lossy, Bulk, Strict };

bool fits(size_t used, size_t n, Cls cls) {
  switch (cls) {
    case Cls::Lossy:
      return used + n <= RING / 2;
    case Cls::Bulk:
      return used == 0 || used + n <= RING - RING_RESERVE;
    case Cls::Strict:
    default:
      return used + n <= RING;
  }
}

enum class Put { Ok, Full, Gone };

// Copy a whole frame into the ring of client (slot, id), or nothing.
// `me`: the calling task; a frame from the client's own connection task is
// Bulk. *cls gets the class used.
Put ring_put(int slot, uint32_t id, const uint8_t *p, size_t n, bool lossy,
             TaskHandle_t me, Cls *cls) {
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  ClientEntry &c = g_clients[slot];
  if (c.id != id || !live(c)) {
    xSemaphoreGive(g_clients_mutex);
    return Put::Gone;
  }
  *cls = lossy ? Cls::Lossy : (c.task == me ? Cls::Bulk : Cls::Strict);
  Ring &r = g_rings[slot];
  const size_t used = r.used.load(std::memory_order_acquire);
  if ((lossy && c.bulk_waiting) || !fits(used, n, *cls)) {
    xSemaphoreGive(g_clients_mutex);
    return Put::Full;
  }
  const size_t first = std::min(n, RING - r.head);
  std::memcpy(r.buf + r.head, p, first);
  if (n > first) std::memcpy(r.buf, p + first, n - first);
  r.head = (r.head + n) % RING;
  const size_t u = r.used.fetch_add(n, std::memory_order_release) + n;
  if (u > r.hwm) r.hwm = u;
  xSemaphoreGive(g_clients_mutex);
  g_tx_frames.fetch_add(1, std::memory_order_relaxed);
  return Put::Ok;
}

// Set or clear the bulk-waiting mark of client (slot, id).
void set_bulk_waiting(const Target &t, bool on) {
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  ClientEntry &c = g_clients[t.slot];
  if (c.id == t.id) c.bulk_waiting = on;
  xSemaphoreGive(g_clients_mutex);
}

// Room for a Bulk frame of n bytes in client (slot, id)? Gone if it left.
Put bulk_room(const Target &t, size_t n) {
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  const ClientEntry &c = g_clients[t.slot];
  Put p = Put::Gone;
  if (c.id == t.id && live(c)) {
    p = fits(g_rings[t.slot].used.load(std::memory_order_acquire), n, Cls::Bulk)
            ? Put::Ok
            : Put::Full;
  }
  xSemaphoreGive(g_clients_mutex);
  return p;
}

// Writes a payload into buf (cap bytes): *type and *n. False = drop.
// Must be a pure function of ctx: a waiting producer encodes again.
struct Encoder {
  bool (*fn)(void *ctx, uint8_t *buf, size_t cap, uint16_t *type, size_t *n);
  void *ctx;
};

// Encode once (into g_tx_buf, under g_tx_mutex) and queue the frame for
// every target. Called with no lock held. Returns how many targets got the
// frame. *waited: the caller waited for room.
//
// Only a client's own connection task waits, and only for that client's
// ring: it releases g_tx_mutex, waits outside every lock (bounded by the
// send deadline; api_tx's stall reap ends it sooner), then encodes again
// from the caller's ctx. No copy, no allocation. A broadcast from a
// connection task treats every other client as Strict, so one stalled
// client never freezes another's task.
int deliver(const Target *t, int k, const Encoder &enc, bool *waited) {
  const TaskHandle_t me = xTaskGetCurrentTaskHandle();
  Target pend[proxy::MAX_API_CLIENTS];
  int np = k;
  for (int i = 0; i < k; ++i) pend[i] = t[i];
  int ok = 0;
  const TickType_t t0 = xTaskGetTickCount();
  Target marked{-1, 0};  // slot whose bulk_waiting we set
  while (np > 0) {
    xSemaphoreTake(g_tx_mutex, portMAX_DELAY);
    uint8_t *payload = &g_tx_buf[frame_codec::MAX_HEADER_LEN];
    const size_t cap = sizeof(g_tx_buf) - frame_codec::MAX_HEADER_LEN;
    uint16_t type = 0;
    size_t plen = 0;
    if (!enc.fn(enc.ctx, payload, cap, &type, &plen)) {
      xSemaphoreGive(g_tx_mutex);
      break;
    }
    const size_t start = frame_codec::prepend_header(g_tx_buf, type, plen);
    const uint8_t *frame = g_tx_buf + start;
    const size_t n = (frame_codec::MAX_HEADER_LEN - start) + plen;
    const bool lossy = is_lossy(type);
    Target wait_for{-1, 0};
    bool kick = false;
    for (int i = 0; i < np; ++i) {
      Cls cls = Cls::Strict;
      const Put p = ring_put(pend[i].slot, pend[i].id, frame, n, lossy, me, &cls);
      if (p == Put::Ok) {
        ++ok;
        kick = true;
      } else if (p == Put::Full) {
        if (cls == Cls::Lossy) {
          g_ring_drops.fetch_add(1, std::memory_order_relaxed);
        } else if (cls == Cls::Bulk) {
          wait_for = pend[i];  // own slot: at most one
        } else {
          // Never wait on behalf of the host task, a timer, or another
          // client's task: a client that cannot take a frame it must not
          // lose goes.
          reap_full(pend[i]);
          kick = true;
        }
      }
    }
    xSemaphoreGive(g_tx_mutex);
    if (kick) xTaskNotifyGive(g_sender);
    if (wait_for.slot < 0) break;

    // Kept set across the re-encode below (cleared once the frame is in or
    // the wait ends), so adverts cannot take the room it waited for.
    if (marked.slot < 0) {
      set_bulk_waiting(wait_for, true);
      marked = wait_for;
    }
    *waited = true;
    np = 0;
    while (true) {
      if (xTaskGetTickCount() - t0 >= pdMS_TO_TICKS(CLIENT_SEND_DEADLINE_MS)) {
        reap_full(wait_for);
        xTaskNotifyGive(g_sender);
        break;
      }
      xTaskNotifyGive(g_sender);
      vTaskDelay(pdMS_TO_TICKS(RING_WAIT_POLL_MS));
      const Put p = bulk_room(wait_for, n);
      if (p == Put::Gone) break;
      if (p == Put::Ok) {
        pend[np++] = wait_for;  // encode again and queue
        break;
      }
    }
  }
  // Every exit: queued, client gone, deadline reap, encoder declined.
  if (marked.slot >= 0) set_bulk_waiting(marked, false);
  return ok;
}

// Adapter for the public EncodeFn (0 bytes = drop).
struct PlainEnc {
  EncodeFn fn;
  void *ctx;
  uint16_t type;
};
bool plain_enc(void *v, uint8_t *buf, size_t cap, uint16_t *type, size_t *n) {
  auto *e = static_cast<PlainEnc *>(v);
  *n = e->fn(e->ctx, buf, cap);
  *type = e->type;
  return *n > 0;
}

// ---- api_tx ----

// One look at slot `i`. Returns the ticks until it needs another look
// (portMAX_DELAY: nothing pending).
TickType_t drain_slot(int i) {
  ClientEntry &c = g_clients[i];
  Ring &r = g_rings[i];
  TxqState &st = g_txq[i];
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  if (c.fd < 0 || c.retired || c.shut) {
    xSemaphoreGive(g_clients_mutex);
    return portMAX_DELAY;
  }
  const int fd = c.fd;
  const char *reap = c.reap_why;
  const bool bulk_waiting = c.bulk_waiting;
  c.io_busy = true;
  xSemaphoreGive(g_clients_mutex);

  TickType_t next = portMAX_DELAY;
  if (reap != nullptr) {
    // SHUT_RDWR runs lwIP's full close path, so with linger 0 the queued
    // data is dropped and the PCB freed now (RST), not retried. The
    // connection task's recv() then fails and it tears the client down.
    set_abortive_close(fd);
    ::shutdown(fd, SHUT_RDWR);
    ESP_LOGW(TAG, "fd=%d %s; %u B dropped from its ring", fd, reap,
             static_cast<unsigned>(r.used.load(std::memory_order_relaxed)));
    xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
    c.shut = true;
    ring_reset(r);
    c.io_busy = false;
    xSemaphoreGive(g_clients_mutex);
    return portMAX_DELAY;
  }

  const TickType_t now = xTaskGetTickCount();
  size_t avail = r.used.load(std::memory_order_acquire);
  const char *new_reap = nullptr;
  if (avail == 0) {
    st.pending_since = 0;
    st.last_progress = now;
  } else {
    if (st.pending_since == 0) {
      // Data after an idle spell: the stall clock starts now, not at the
      // last progress before the idle (which may be minutes ago).
      st.pending_since = now;
      st.last_progress = now;
    }
    bool full = false, hold = false, queried = false;
    size_t room = avail;
    TxqCall q;
    if (txq_query(fd, &q)) {
      queried = true;
      st.last = q.heap;
      if (q.heap > st.max) st.max = q.heap;
      if (q.heap > g_txq_max_all) g_txq_max_all = q.heap;
      room = TCP_SND_BUF > q.bytes ? TCP_SND_BUF - q.bytes : 0;
      // Full: over the heap cap, no payload room, or no pbuf slots left (a
      // write may add up to 2).
      full = (proxy::API_CLIENT_TXQ_CAP > 0 &&
              q.heap >= proxy::API_CLIENT_TXQ_CAP) ||
             room == 0 || q.segs + 2u > TCP_SND_QUEUELEN;
      // No coalescing hold while a bulk producer waits for this ring to
      // empty.
      hold = !full && !bulk_waiting && q.segs > 0 && avail < TCP_MSS &&
             now - st.pending_since < pdMS_TO_TICKS(TX_COALESCE_MS);
      if (full) {
        if (!st.over) {
          st.over = true;
          st.over_since = now;
        }
        // Brief touches of the cap are normal on WiFi (ACKs ~250 ms late);
        // log only an episode that lasts.
        if (!st.logged && now - st.over_since >= pdMS_TO_TICKS(TXQ_LOG_AFTER_MS)) {
          st.logged = true;
          ESP_LOGW(TAG, "fd=%d send queue full: %lu B heap (%u segs, %u B "
                   "unacked or unsent), cap %u, for %d ms; %u B wait in its "
                   "ring",
                   fd, static_cast<unsigned long>(q.heap), q.segs, q.bytes,
                   static_cast<unsigned>(proxy::API_CLIENT_TXQ_CAP),
                   TXQ_LOG_AFTER_MS, static_cast<unsigned>(avail));
        }
      } else {
        if (st.logged) {
          ESP_LOGI(TAG, "fd=%d send queue drained (%lu B) after %lu ms", fd,
                   static_cast<unsigned long>(q.heap),
                   static_cast<unsigned long>(pdTICKS_TO_MS(now - st.over_since)));
        }
        st.over = false;
        st.logged = false;
        st.over_since = 0;
      }
    }
    if (!full && !hold) {
      const size_t n = std::min(avail, room);
      iovec iov[2];
      const size_t first = std::min(n, RING - r.tail);
      iov[0].iov_base = r.buf + r.tail;
      iov[0].iov_len = first;
      iov[1].iov_base = r.buf;
      iov[1].iov_len = n - first;
      msghdr msg{};
      msg.msg_iov = iov;
      msg.msg_iovlen = n > first ? 2 : 1;
      // One sendmsg = one lwIP write sequence and one tcp_output, so both
      // halves of a wrapped ring share segments. MSG_DONTWAIT per call: the
      // socket itself stays blocking for the connection task's recv().
      // (lwip_sendmsg rejects MSG_NOSIGNAL with EOPNOTSUPP; lwIP raises no
      // SIGPIPE anyway.)
      const ssize_t w = ::sendmsg(fd, &msg, MSG_DONTWAIT);
      if (w > 0) {
        r.tail = (r.tail + static_cast<size_t>(w)) % RING;
        r.used.fetch_sub(static_cast<size_t>(w), std::memory_order_release);
        avail -= static_cast<size_t>(w);
        st.last_progress = now;
        st.pending_since = avail > 0 ? now : 0;
        st.nomem_logged = false;
        g_tx_sends.fetch_add(1, std::memory_order_relaxed);
        g_tx_bytes.fetch_add(static_cast<uint32_t>(w), std::memory_order_relaxed);
      } else if (w < 0) {
        const int e = errno;
        if (e == ENOMEM || e == ENOBUFS || e == EAGAIN || e == EWOULDBLOCK) {
          // With MSG_DONTWAIT lwIP reports tcp_write's ERR_MEM as
          // EWOULDBLOCK, whatever its cause. The query just showed payload
          // room and pbuf slots (not `full`), so here it was a failed
          // segment/pbuf allocation. Retry next poll, until the deadline;
          // log once per episode and at most every NOMEM_LOG_MS overall.
          static TickType_t s_nomem_log = 0;
          if (queried && !st.nomem_logged &&
              (s_nomem_log == 0 ||
               now - s_nomem_log >= pdMS_TO_TICKS(NOMEM_LOG_MS))) {
            st.nomem_logged = true;
            s_nomem_log = now;
            ESP_LOGW(TAG, "send fd=%d: no memory (errno=%d, %u B room, %u "
                     "segs; free %u largest %u); retrying",
                     fd, e, static_cast<unsigned>(room), q.segs,
                     static_cast<unsigned>(esp_get_free_heap_size()),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(
                         MALLOC_CAP_DEFAULT)));
          }
        } else {
          ESP_LOGW(TAG, "send fd=%d failed: errno=%d", fd, e);
          new_reap = WHY_SEND_ERR;
        }
      }
    }
    if (new_reap == nullptr && avail > 0) {
      if (now - st.last_progress >= pdMS_TO_TICKS(CLIENT_SEND_DEADLINE_MS)) {
        g_txq_reaps.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGW(TAG, "fd=%d: %u B queued, nothing sent for %d ms", fd,
                 static_cast<unsigned>(avail), CLIENT_SEND_DEADLINE_MS);
        new_reap = WHY_STALLED;
      } else if (hold) {
        const TickType_t left =
            pdMS_TO_TICKS(TX_COALESCE_MS) - (now - st.pending_since);
        next = std::max<TickType_t>(left, 1);
      } else {
        next = std::max<TickType_t>(pdMS_TO_TICKS(SENDER_POLL_MS), 1);
      }
    }
  }
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  if (new_reap != nullptr) {
    mark_reaped_locked(c, new_reap);
    next = 0;  // shut it down on the next pass
  }
  c.io_busy = false;
  xSemaphoreGive(g_clients_mutex);
  return next;
}

void sender_task(void *) {
  TickType_t wait = portMAX_DELAY;
  while (true) {
    // A notify given while a pass runs is kept and ends the next wait at
    // once, so none is lost.
    if (wait != 0) ulTaskNotifyTake(pdTRUE, wait);
    wait = portMAX_DELAY;
    // Round-robin: one look (at most one sendmsg of up to TCP_SND_BUF) per
    // client per pass.
    for (size_t i = 0; i < g_clients.size(); ++i) {
      wait = std::min(wait, drain_slot(static_cast<int>(i)));
    }
  }
}

// Add/remove a client fd from the active set. Returns the client's id,
// or 0 if no slot is free; in that case the caller should reject the
// connection.
uint32_t register_client(int fd, uint32_t peer_ip) {
  uint32_t id = 0;
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (auto &s : g_clients) {
    if (s.fd < 0) {
      const size_t i = &s - g_clients.data();
      // Free slot: api_tx skips it and no producer resolves it, so its
      // ring and state can be reset before it is published.
      ring_reset(g_rings[i]);
      g_rings[i].hwm = 0;
      g_txq[i] = TxqState{};
      g_txq[i].last_progress = xTaskGetTickCount();
      id = g_next_client_id.fetch_add(1, std::memory_order_relaxed);
      if (id == 0) id = g_next_client_id.fetch_add(1, std::memory_order_relaxed);
      s = ClientEntry{};
      s.id = id;
      s.fd = fd;
      s.peer_ip = peer_ip;
      break;
    }
  }
  xSemaphoreGive(g_clients_mutex);
  if (id != 0) g_active_count.fetch_add(1, std::memory_order_release);
  return id;
}

// Retire the slot of `fd` and free it. Waits for api_tx to finish any I/O
// on it, so once this returns nothing writes to `fd` and the caller may
// close it. Holds no lock across the wait.
void unregister_client(int fd) {
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (size_t i = 0; i < g_clients.size(); ++i) {
    ClientEntry &s = g_clients[i];
    if (s.fd != fd) continue;
    s.closing = true;
    s.retired = true;
    while (s.io_busy) {
      xSemaphoreGive(g_clients_mutex);
      vTaskDelay(1);
      xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
    }
    ring_reset(g_rings[i]);
    g_txq[i] = TxqState{};
    s = ClientEntry{};
    break;
  }
  xSemaphoreGive(g_clients_mutex);
  g_active_count.fetch_sub(1, std::memory_order_release);
}

// Handshake request -> reply, as an Encoder (handshake::handle is pure).
struct HsCtx {
  uint16_t msg_type;
  const uint8_t *rx;
  size_t rx_len;
  handshake::Result hr;
};
bool hs_enc(void *v, uint8_t *buf, size_t cap, uint16_t *type, size_t *n) {
  auto *h = static_cast<HsCtx *>(v);
  h->hr = handshake::handle(h->msg_type, h->rx, h->rx_len, buf, cap);
  if (!h->hr.handled || h->hr.response_type == 0) return false;
  *type = h->hr.response_type;
  *n = h->hr.payload_len;
  return true;
}

// `*why` is set when this returns false: "peer closed", "read error",
// "send failed" or "closed by protocol".
bool dispatch_one(int fd, int slot, uint32_t client_id, uint8_t *rx_payload,
                  size_t rx_cap, bt_handlers::ClientSubs *subs,
                  const char **why) {
  uint16_t msg_type = 0;
  size_t payload_len = 0;
  frame_codec::Error e = frame_codec::read_frame(socket_read, &fd, rx_payload,
                                                 rx_cap, &msg_type, &payload_len);
  if (e != frame_codec::Error::Ok) {
    if (e != frame_codec::Error::Eof) {
      ESP_LOGW(TAG, "fd=%d read_frame error: %d", fd, static_cast<int>(e));
      *why = "read error";
    } else {
      *why = "peer closed";
    }
    return false;
  }

  // Handshake replies go through the client's ring like every other frame
  // (one FIFO per client), encoded straight into g_tx_buf by deliver().
  // BT handlers do NOT run under the TX mutex: they call
  // api_server::send_async / send_to, which take it.
  HsCtx hs{msg_type, rx_payload, payload_len, {}};
  const Target t{slot, client_id};
  bool waited = false;
  const int got = deliver(&t, 1, Encoder{&hs_enc, &hs}, &waited);
  if (hs.hr.handled) {
    bool keep_open = true;
    if (hs.hr.response_type != 0 && got != 1) {
      keep_open = false;
      *why = "send failed";
    }
    if (hs.hr.close_after && keep_open) {
      keep_open = false;
      *why = "closed by protocol";
    }
    return keep_open;
  }

  bt_handlers::Context bctx{fd, client_id, subs};
  if (!bt_handlers::handle(msg_type, rx_payload, payload_len, bctx)) {
    ESP_LOGD(TAG, "fd=%d unhandled msg_type=%u (len=%zu)", fd, msg_type,
             payload_len);
  }
  return true;
}

// Slot index of `fd` (registered before its task is created, so always
// present while the task runs). Records the calling task as the client's
// connection task.
int adopt_slot(int fd, uint32_t *id) {
  int idx = -1;
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (size_t i = 0; i < g_clients.size(); ++i) {
    if (g_clients[i].fd == fd) {
      g_clients[i].task = xTaskGetCurrentTaskHandle();
      *id = g_clients[i].id;
      idx = static_cast<int>(i);
      break;
    }
  }
  xSemaphoreGive(g_clients_mutex);
  return idx;
}

// Per-connection task. One per active client; exits on disconnect.
void connection_task(void *arg) {
  int client_fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  uint32_t client_id = 0;
  const int slot = adopt_slot(client_fd, &client_id);
  // Captured now: after an abort/RST the PCB is gone and getpeername fails,
  // which is exactly the case the disconnect log needs it for.
  char peer_s[16] = "?";
  bool upstream = false;
  {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    if (::getpeername(client_fd, reinterpret_cast<sockaddr *>(&peer),
                      &peer_len) == 0) {
      inet_ntoa_r(peer.sin_addr, peer_s, sizeof(peer_s));
      upstream = is_upstream_peer(peer.sin_addr.s_addr);
    }
  }
  ESP_LOGI(TAG, "client connected (fd=%d, id=%lu, total=%u)", client_fd,
           static_cast<unsigned long>(client_id),
           g_active_count.load(std::memory_order_acquire));

  // Per-connection state. The RX buffer and ClientSubs both live on
  // this task's stack — gone when the task exits.
  uint8_t rx_payload[proxy::MAX_MESSAGE_SIZE];
  bt_handlers::ClientSubs subs;

  const char *why = "unknown";
  while (slot >= 0 && dispatch_one(client_fd, slot, client_id, rx_payload,
                                   sizeof(rx_payload), &subs, &why)) {
    if (upstream) g_last_rx_us.store(esp_timer_get_time(), std::memory_order_relaxed);
  }
  // A reaped socket's recv() fails as a side effect of api_tx's shutdown,
  // so the reap is the real cause. Read it (and stop new frames) before
  // unregister clears the slot.
  const char *reap_why = nullptr;
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  if (slot >= 0) {
    reap_why = g_clients[slot].reap_why;
    g_clients[slot].closing = true;
  }
  xSemaphoreGive(g_clients_mutex);
  // A peer that sends DisconnectRequest may close as soon as it has the
  // response, and api_tx's next send then fails: that is still a protocol
  // close. Any other reap is the real cause.
  if (reap_why != nullptr &&
      !(reap_why == WHY_SEND_ERR &&
        std::strcmp(why, "closed by protocol") == 0)) {
    why = reap_why;
  }
  // A protocol close (DisconnectResponse, Hello reject) has just queued its
  // last frame: let api_tx hand it to lwIP, then close gracefully so HA
  // reads it. Abortive if it does not get out in time, and on every other
  // exit (the ring is discarded with the slot).
  bool abortive = reap_why != nullptr ||
                  std::strcmp(why, "closed by protocol") != 0;
  if (!abortive && slot >= 0) {
    int t = 0;
    for (; t < PROTOCOL_CLOSE_FLUSH_MS &&
           g_rings[slot].used.load(std::memory_order_acquire) != 0;
         t += RING_WAIT_POLL_MS) {
      xTaskNotifyGive(g_sender);
      vTaskDelay(pdMS_TO_TICKS(RING_WAIT_POLL_MS));
    }
    if (t >= PROTOCOL_CLOSE_FLUSH_MS) abortive = true;
  }

  // Release this client's subscription refs FIRST so any in-flight
  // async sends to other clients don't keep targeting our (now-closing)
  // fd.
  bt_handlers::on_client_disconnect(subs);
  // Leave the client set: waits for api_tx to be off our fd, so nothing
  // writes to it once it is closed and possibly reissued.
  unregister_client(client_fd);
  // INFO on purpose: with the per-address "release …" lines that follow,
  // this shows which side dropped first (batmon reconnect-loop debugging).
  ESP_LOGI(TAG, "client disconnected (fd=%d, id=%lu, peer=%s, reason=%s, "
           "remaining=%u); releasing its BLE links",
           client_fd, static_cast<unsigned long>(client_id), peer_s, why,
           g_active_count.load(std::memory_order_acquire));
  // Drop exactly the BLE links this client opened, even though other
  // clients stay connected. After unregister, so any state callback for
  // them finds no recipient; the remaining clients get ConnectionsFree.
  bt_handlers::on_client_links_release(client_id);
  // Backstop: with nobody left, drop everything (incl. anything orphaned).
  if (g_active_count.load(std::memory_order_acquire) == 0) {
    bt_handlers::on_last_client_disconnect();
  }

  // Abortive: a dead peer must not keep this PCB and its queued data alive
  // in FIN-WAIT for minutes. lwIP only aborts if data is still queued; a
  // quiet socket closes normally either way.
  if (abortive) set_abortive_close(client_fd);
  ::shutdown(client_fd, SHUT_RDWR);
  ::close(client_fd);
  vTaskDelete(nullptr);
}

void listener_task(void *) {
  int listen_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_fd < 0) {
    ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
    vTaskDelete(nullptr);
    return;
  }
  int one = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(proxy::API_PORT);
  if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
    ::close(listen_fd);
    vTaskDelete(nullptr);
    return;
  }
  if (::listen(listen_fd, proxy::MAX_API_CLIENTS) < 0) {
    ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
    ::close(listen_fd);
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(TAG, "listening on :%u (max_clients=%u, tx ring %u B)",
           proxy::API_PORT, proxy::MAX_API_CLIENTS, static_cast<unsigned>(RING));

  while (true) {
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &plen);
    if (client_fd < 0) {
      ESP_LOGW(TAG, "accept() failed: errno=%d", errno);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    // Visibility: print the moment accept() returns, before any task
    // scheduling delay. If "accepted fd=X" appears with no matching
    // "client connected" later, xTaskCreate was the holdup.
    ESP_LOGI(TAG, "accepted fd=%d peer=%s:%u", client_fd,
             inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
    // api_tx coalesces (see the transmit-path comment); lwIP's Nagle would
    // only add an ACK wait on top.
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int keepidle = CLIENT_KEEPIDLE_S;
    int keepintvl = CLIENT_KEEPINTVL_S;
    int keepcnt = CLIENT_KEEPCNT;
    ::setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    uint32_t client_id = register_client(client_fd, peer.sin_addr.s_addr);
    if (client_id == 0) {
      // All NBP_API_MAX_CLIENTS slots taken: refuse with an RST right away
      // (nothing was sent, so nothing lingers in FIN-WAIT), no task, no
      // buffers. aioesphomeapi sees "connection reset" and retries later.
      const uint32_t n = g_refused.fetch_add(1, std::memory_order_relaxed) + 1;
      ESP_LOGW(TAG, "no client slot (max_clients=%u); refusing fd=%d "
               "(refused %lu since boot)", proxy::MAX_API_CLIENTS, client_fd,
               static_cast<unsigned long>(n));
      set_abortive_close(client_fd);
      ::close(client_fd);
      continue;
    }

    char name[24];
    std::snprintf(name, sizeof(name), "api_client%d", client_fd);
    if (xTaskCreate(&connection_task, name, CONN_STACK,
                    reinterpret_cast<void *>(static_cast<intptr_t>(client_fd)),
                    CONN_PRIO, nullptr) != pdPASS) {
      ESP_LOGE(TAG, "xTaskCreate for fd=%d failed", client_fd);
      // Same teardown as connection_task: retire the slot (api_tx off the
      // fd, ring reset) before the fd is closed and possibly reissued.
      unregister_client(client_fd);
      set_abortive_close(client_fd);
      ::close(client_fd);
    }
  }
}

}  // namespace

bool has_active_client() {
  return g_active_count.load(std::memory_order_acquire) > 0;
}

int64_t last_rx_us() { return g_last_rx_us.load(std::memory_order_relaxed); }

bool send_async(uint16_t msg_type, EncodeFn encode, void *ctx) {
  if (g_active_count.load(std::memory_order_acquire) == 0) return false;
  if (g_tx_mutex == nullptr || encode == nullptr) return false;
  const int64_t t0 = esp_timer_get_time();
  // Snapshot the live clients first: nothing to encode for nobody.
  Target t[proxy::MAX_API_CLIENTS];
  int k = 0;
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (size_t i = 0; i < g_clients.size(); ++i) {
    if (live(g_clients[i])) t[k++] = Target{static_cast<int>(i), g_clients[i].id};
  }
  xSemaphoreGive(g_clients_mutex);
  if (k == 0) return false;

  bool waited = false;
  PlainEnc pe{encode, ctx, msg_type};
  const int ok = deliver(t, k, Encoder{&plain_enc, &pe}, &waited);
  if (!waited) {
    raise_max(g_enq_us_max, static_cast<uint32_t>(esp_timer_get_time() - t0));
  }
  return ok > 0;
}

bool send_to(uint32_t client_id, uint16_t msg_type, EncodeFn encode,
             void *ctx) {
  if (client_id == 0 || g_tx_mutex == nullptr || encode == nullptr) return false;
  const int64_t t0 = esp_timer_get_time();
  // Resolve the id first: a client that left, is closing or was reaped
  // gets nothing, and nothing is encoded for it.
  int slot = -1;
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (size_t i = 0; i < g_clients.size(); ++i) {
    if (g_clients[i].id == client_id && live(g_clients[i])) {
      slot = static_cast<int>(i);
      break;
    }
  }
  xSemaphoreGive(g_clients_mutex);
  if (slot < 0) return false;

  bool waited = false;
  PlainEnc pe{encode, ctx, msg_type};
  const Target t{slot, client_id};
  const bool sent = deliver(&t, 1, Encoder{&plain_enc, &pe}, &waited) == 1;
  if (!waited) {
    raise_max(g_enq_us_max, static_cast<uint32_t>(esp_timer_get_time() - t0));
  }
  return sent;
}

bool client_peer_ip(uint32_t client_id, uint32_t *ip_out) {
  if (client_id == 0 || g_clients_mutex == nullptr) return false;
  bool found = false;
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (const auto &c : g_clients) {
    if (c.fd >= 0 && c.id == client_id) {
      *ip_out = c.peer_ip;
      found = true;
      break;
    }
  }
  xSemaphoreGive(g_clients_mutex);
  return found;
}

TxqStats txq_stats() {
  TxqStats out{};
  out.cap = static_cast<uint32_t>(proxy::API_CLIENT_TXQ_CAP);
  out.ring_size = static_cast<uint32_t>(RING);
  out.drops = g_ring_drops.load(std::memory_order_relaxed);
  out.ring_reaps = g_ring_reaps.load(std::memory_order_relaxed);
  out.reaps = g_txq_reaps.load(std::memory_order_relaxed);
  out.refused = g_refused.load(std::memory_order_relaxed);
  out.frames = g_tx_frames.load(std::memory_order_relaxed);
  out.sends = g_tx_sends.load(std::memory_order_relaxed);
  out.bytes = g_tx_bytes.load(std::memory_order_relaxed);
  out.enq_us_max = g_enq_us_max.load(std::memory_order_relaxed);
  out.busy = false;  // no lock is held across I/O any more
  out.tx_stack_free = g_sender ? static_cast<uint32_t>(
      uxTaskGetStackHighWaterMark(g_sender) * sizeof(StackType_t)) : 0;
  out.max_all = g_txq_max_all;  // word read
  if (g_clients_mutex == nullptr) return out;
  xSemaphoreTake(g_clients_mutex, portMAX_DELAY);
  for (size_t i = 0; i < g_clients.size(); ++i) {
    if (g_clients[i].fd < 0) continue;
    TxqClient &c = out.clients[out.n_clients++];
    c.id = g_clients[i].id;
    c.fd = g_clients[i].fd;
    c.last = g_txq[i].last;  // word reads of api_tx's figures
    c.max = g_txq[i].max;
    c.ring = static_cast<uint32_t>(g_rings[i].used.load(std::memory_order_relaxed));
    c.ring_max = static_cast<uint32_t>(g_rings[i].hwm);
  }
  xSemaphoreGive(g_clients_mutex);
  return out;
}

void start() {
  if (g_started) return;
  g_started = true;
  g_tx_mutex = xSemaphoreCreateMutex();
  g_clients_mutex = xSemaphoreCreateMutex();
  for (auto &s : g_clients) s = ClientEntry{};
  if (xTaskCreate(&sender_task, "api_tx", SENDER_STACK, nullptr, SENDER_PRIO,
                  &g_sender) != pdPASS) {
    // Without api_tx nothing would ever leave: don't serve at all.
    ESP_LOGE(TAG, "xTaskCreate(api_tx) failed; API server not started");
    return;
  }
  xTaskCreate(&listener_task, "api_listener", LISTENER_STACK, nullptr,
              LISTENER_PRIO, nullptr);
}

}  // namespace api_server
