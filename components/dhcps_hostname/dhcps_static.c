// Static SoftAP DHCP leases (MAC -> fixed IP) for the built-in IDF DHCP
// server, which has no notion of a reserved address: it hands out the pool
// in join order, so port-forwards to fixed IPs hit whichever client joined
// first after a reboot.
//
// The server keeps its leases in a private, IP-sorted linked list
// (dhcps->plist) and, for every packet, first looks the client's MAC up in
// that list; a hit decides the address (OFFER it, ACK a REQUEST for it, NAK
// any other). The allocator also skips every IP already in the list. So a
// list entry (static MAC, static IP) that is always present IS a static
// lease, served by the server's own state machine.
//
// LWIP_HOOK_DHCPS_POST_STATE (nbp_lwip_hooks.h) runs inside handle_dhcp()
// after parse_msg(). nbp_dhcps_static_fix() then
//   1. pins: removes any entry that holds a reserved IP for another MAC, or
//      holds a static MAC on another IP, and inserts the missing
//      (static MAC, static IP) entries with a lease timer that never runs
//      out (dhcps_tmr counts it down from UINT32_MAX: 136 years);
//   2. reports whether this packet was answered from the wrong state: a
//      static MAC got another address, or a dynamic client got a reserved
//      one. The hook then runs parse_msg() once more against the pinned
//      list and pins again (parse_msg resets the matched entry's timer).
// Pinning on every packet also covers dhcps_stop()/start() (set_ap_dns()
// restarts the server, which frees the list) and lease expiry: no packet
// can be answered before the pinned entries are back.
//
// IDF-version coupled, like the hostname hook: it mirrors dhcpserver.c's
// private `list_node` and relies on the public `struct dhcps_pool`
// (dhcpserver.h). Both are unchanged between IDF 5.5 and 6.0.3.

#include "nbp_lwip_hooks.h"
#include "dhcps_hostname.h"

#include "dhcpserver/dhcpserver.h"  // struct dhcps_msg, struct dhcps_pool
#include "lwip/mem.h"               // mem_calloc / mem_free (server's allocator)

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

static const char *TAG = "dhcps";

// Mirror of dhcpserver.c's private list node (IDF 5.5 and 6.0.3).
typedef struct nbp_list_node {
  void *pnode;  // struct dhcps_pool *
  struct nbp_list_node *pnext;
} nbp_list_node;

// dhcpserver.c's DHCPS_STATE_* values (private there).
#define NBP_DHCPS_STATE_OFFER 1
#define NBP_DHCPS_STATE_ACK 3
#define NBP_DHCPS_STATE_NAK 4

static dhcps_static_lease_t s_leases[DHCPS_STATIC_LEASE_MAX];
static size_t s_count;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void dhcps_static_set(const dhcps_static_lease_t *tbl, size_t n) {
  if (n > DHCPS_STATIC_LEASE_MAX) {
    n = DHCPS_STATIC_LEASE_MAX;
  }
  taskENTER_CRITICAL(&s_mux);
  if (n > 0 && tbl != NULL) {
    memcpy(s_leases, tbl, n * sizeof(*tbl));
  }
  s_count = n;
  taskEXIT_CRITICAL(&s_mux);
}

uint32_t dhcps_static_lookup(const uint8_t mac[6]) {
  uint32_t ip = 0;
  taskENTER_CRITICAL(&s_mux);
  for (size_t i = 0; i < s_count; ++i) {
    if (memcmp(s_leases[i].mac, mac, 6) == 0) {
      ip = s_leases[i].ip;
      break;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
  return ip;
}

bool dhcps_static_is_reserved(uint32_t ip) {
  bool r = false;
  taskENTER_CRITICAL(&s_mux);
  for (size_t i = 0; i < s_count && ip != 0; ++i) {
    if (s_leases[i].ip == ip) {
      r = true;
      break;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
  return r;
}

static size_t snapshot(dhcps_static_lease_t *out) {
  taskENTER_CRITICAL(&s_mux);
  size_t n = s_count;
  if (n > 0) {
    memcpy(out, s_leases, n * sizeof(*out));
  }
  taskEXIT_CRITICAL(&s_mux);
  return n;
}

// Same order as dhcpserver.c's node_insert_to_list: ascending raw
// (network-order) address, which the allocator's "skip used IPs" walk needs.
static void insert_sorted(nbp_list_node **head, nbp_list_node *ins) {
  const uint32_t ip = ((struct dhcps_pool *)ins->pnode)->ip.addr;
  nbp_list_node **pp = head;
  while (*pp != NULL && ((struct dhcps_pool *)(*pp)->pnode)->ip.addr <= ip) {
    pp = &(*pp)->pnext;
  }
  ins->pnext = *pp;
  *pp = ins;
}

static void pin(nbp_list_node **head, const dhcps_static_lease_t *tbl,
                size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const dhcps_static_lease_t *e = &tbl[i];
    nbp_list_node *keep = NULL;
    nbp_list_node **pp = head;
    while (*pp != NULL) {
      nbp_list_node *node = *pp;
      struct dhcps_pool *pool = (struct dhcps_pool *)node->pnode;
      const bool mac_eq = memcmp(pool->mac, e->mac, 6) == 0;
      const bool ip_eq = pool->ip.addr == e->ip;
      if (mac_eq && ip_eq && keep == NULL) {
        keep = node;
        pp = &node->pnext;
      } else if (mac_eq || ip_eq) {
        *pp = node->pnext;  // unlink: wrong IP for this MAC, or IP taken
        mem_free(pool);
        mem_free(node);
      } else {
        pp = &node->pnext;
      }
    }
    if (keep == NULL) {
      struct dhcps_pool *pool =
          (struct dhcps_pool *)mem_calloc(1, sizeof(struct dhcps_pool));
      nbp_list_node *node =
          (nbp_list_node *)mem_calloc(1, sizeof(nbp_list_node));
      if (pool == NULL || node == NULL) {
        // The reservation is retried on the next DHCP packet.
        mem_free(pool);
        mem_free(node);
        continue;
      }
      pool->ip.addr = e->ip;
      memcpy(pool->mac, e->mac, 6);
      node->pnode = pool;
      insert_sorted(head, node);
      keep = node;
    }
    ((struct dhcps_pool *)keep->pnode)->lease_timer = UINT32_MAX;
  }
}

bool nbp_dhcps_static_fix(void **plist, uint32_t client_ip,
                          const struct dhcps_msg *msg, s16_t state) {
  dhcps_static_lease_t tbl[DHCPS_STATIC_LEASE_MAX];
  const size_t n = snapshot(tbl);
  if (n == 0 || plist == NULL || msg == NULL) {
    return false;
  }
  bool redo = false;
  if (state == NBP_DHCPS_STATE_OFFER || state == NBP_DHCPS_STATE_ACK ||
      state == NBP_DHCPS_STATE_NAK) {
    uint32_t want = 0;
    for (size_t i = 0; i < n; ++i) {
      if (memcmp(tbl[i].mac, msg->chaddr, 6) == 0) {
        want = tbl[i].ip;
        break;
      }
    }
    bool reserved = false;
    for (size_t i = 0; i < n && want == 0; ++i) {
      if (tbl[i].ip == client_ip) {
        reserved = true;
      }
    }
    redo = want != 0 ? client_ip != want : reserved;
    if (redo) {
      const uint8_t *m = msg->chaddr;
      const uint32_t shown = want != 0 ? want : client_ip;
      ESP_LOGI(TAG,
               "%02x:%02x:%02x:%02x:%02x:%02x: %s %u.%u.%u.%u (first answer "
               "%s %u.%u.%u.%u); re-parsing",
               m[0], m[1], m[2], m[3], m[4], m[5],
               want != 0 ? "static lease" : "not giving reserved",
               (unsigned)(shown & 0xff), (unsigned)((shown >> 8) & 0xff),
               (unsigned)((shown >> 16) & 0xff), (unsigned)(shown >> 24),
               state == NBP_DHCPS_STATE_OFFER ? "OFFER"
               : state == NBP_DHCPS_STATE_ACK ? "ACK"
                                              : "NAK",
               (unsigned)(client_ip & 0xff), (unsigned)((client_ip >> 8) & 0xff),
               (unsigned)((client_ip >> 16) & 0xff),
               (unsigned)(client_ip >> 24));
    }
  }
  pin((nbp_list_node **)plist, tbl, n);
  return redo;
}

s16_t nbp_dhcps_static_pin(void **plist, s16_t state) {
  dhcps_static_lease_t tbl[DHCPS_STATIC_LEASE_MAX];
  const size_t n = snapshot(tbl);
  if (n > 0 && plist != NULL) {
    pin((nbp_list_node **)plist, tbl, n);
  }
  return state;
}
