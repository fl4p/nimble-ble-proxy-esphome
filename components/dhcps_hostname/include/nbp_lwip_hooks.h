// Custom lwIP hook header, injected into the stack via
// ESP_IDF_LWIP_HOOK_FILENAME (see the project root CMakeLists, gated on
// CONFIG_NBP_NAT_ROUTER). IDF's lwip_default_hooks.h `#include`s whatever
// ESP_IDF_LWIP_HOOK_FILENAME points at, so the macro below is visible to
// every lwIP translation unit — but only the DHCP server actually invokes
// LWIP_HOOK_DHCPS_POST_STATE, on each received DHCP message after parsing.
//
// We use it to capture the client's hostname (DHCP option 12), which the
// built-in IDF 5.5 DHCP server otherwise parses past and discards (see
// dhcps_hostname.c), and to serve static leases (dhcps_static.c), which may
// re-run the parse and so change the state the server acts on.
//
// It also defines LWIP_HOOK_IP4_CANFORWARD, which ip4_canforward() invokes
// for every datagram the router is about to forward (in BOTH NAT
// directions). We use it purely to tally forwarded bytes for the dashboard
// throughput chart, then return -1 ("no decision") so lwIP's normal
// forward-eligibility logic runs unchanged. The counter lives in nat_router
// (which knows the SoftAP subnet needed to split up/down) — see
// nat_router::get_ap_throughput.
//
// Pure C: this is included from C lwIP sources.

#pragma once

#include "sdkconfig.h"   // CONFIG_NBP_NAT_THROUGHPUT gate
#include "lwip/arch.h"  // s16_t, u32_t
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dhcps_msg;
struct pbuf;

// Records the option-12 hostname (if any) for msg->chaddr, then returns
// `state` verbatim. msg/len/state mirror the LWIP_HOOK_DHCPS_POST_STATE
// contract (msg is the parsed *incoming* packet).
s16_t nbp_dhcps_post_state(struct dhcps_msg *msg, s16_t len, s16_t state);

// Static leases (dhcps_static.c). `plist` is &dhcps->plist, the server's
// private lease list; `client_ip` the address parse_msg() chose for this
// packet. Pins the reserved entries into the list and returns true when the
// packet must be parsed again because it was answered from the unpinned list.
bool nbp_dhcps_static_fix(void **plist, u32_t client_ip,
                          const struct dhcps_msg *msg, s16_t state);
// Re-pins after that second parse_msg() (it resets the matched entry's lease
// timer) and returns `state` unchanged.
s16_t nbp_dhcps_static_pin(void **plist, s16_t state);

#if CONFIG_NBP_NAT_THROUGHPUT
// Observe-only forward hook: adds p->tot_len to the SoftAP up/down byte
// tallies (classified by whether `dest` falls in the AP subnet), then
// always returns -1 so ip4_canforward() proceeds with its default decision.
// `dest` is the destination IPv4 in HOST byte order (as lwIP passes it).
// Implemented in nat_router.cpp.
int nbp_ip4_canforward(struct pbuf *p, u32_t dest);
#endif

#ifdef __cplusplus
}
#endif

// NAT builds only: a heap self-test build without the NAT router also
// includes this header (for the input hook below) but links no dhcps_hostname.
#if CONFIG_NBP_NAT_ROUTER
// Expands inside dhcpserver.c's handle_dhcp(), so it uses that function's
// locals `dhcps` and `tlen` and the file-static parse_msg() (the second
// parse is the same call handle_dhcp() just made). Re-verify on an IDF bump:
// a rename there is a compile error here, not a silent no-op.
#define LWIP_HOOK_DHCPS_POST_STATE(msg, len, state)                          \
  nbp_dhcps_post_state(                                                      \
      (msg), (len),                                                          \
      nbp_dhcps_static_fix((void **)&dhcps->plist,                           \
                           dhcps->client_address.addr, (msg), (state))       \
          ? nbp_dhcps_static_pin(                                            \
                (void **)&dhcps->plist,                                      \
                parse_msg(dhcps, (msg),                                      \
                          tlen >= 240 ? (u16_t)(tlen - 240) : 0))            \
          : (state))
#endif

#if CONFIG_NBP_NAT_THROUGHPUT
#define LWIP_HOOK_IP4_CANFORWARD(p, dest) \
  nbp_ip4_canforward((p), (dest))
#endif

#if NBP_DHCPS_SELFTEST
// QEMU static-lease self-test only (dhcps_static_selftest.c): record each
// reply's type and yiaddr. Expands inside send_offer/send_ack/send_nak,
// whose outgoing message is the local `m`.
#ifdef __cplusplus
extern "C" {
#endif
void nbp_dhcps_selftest_reply(int type, const unsigned char *yiaddr);
#ifdef __cplusplus
}
#endif
#define LWIP_HOOK_DHCPS_POST_APPEND_OPTS(netif, dhcps, state, pp_opts) \
  nbp_dhcps_selftest_reply((state), m->yiaddr);
#endif

#if NBP_HEAP_SELFTEST
// QEMU heap self-test only (compile define, never firmware): drops inbound
// TCP segments to one port so a loopback client looks like a stalled WiFi
// peer (data sent, never ACKed). Implemented in api_server/heap_selftest.cpp.
struct netif;
#ifdef __cplusplus
extern "C" {
#endif
int nbp_selftest_ip4_input(struct pbuf *p, struct netif *inp);
#ifdef __cplusplus
}
#endif
#define LWIP_HOOK_IP4_INPUT(p, inp) nbp_selftest_ip4_input((p), (inp))
#endif
