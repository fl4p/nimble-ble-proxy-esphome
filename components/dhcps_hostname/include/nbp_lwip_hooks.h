// Custom lwIP hook header, injected into the stack via
// ESP_IDF_LWIP_HOOK_FILENAME (see the project root CMakeLists, gated on
// CONFIG_NBP_NAT_ROUTER). IDF's lwip_default_hooks.h `#include`s whatever
// ESP_IDF_LWIP_HOOK_FILENAME points at, so the macro below is visible to
// every lwIP translation unit — but only the DHCP server actually invokes
// LWIP_HOOK_DHCPS_POST_STATE, on each received DHCP message after parsing.
//
// We use it to capture the client's hostname (DHCP option 12), which the
// built-in IDF 5.5 DHCP server otherwise parses past and discards. The
// state value is returned unchanged — this is observe-only. See
// dhcps_hostname.c for the registry + the dhcps_hostname_lookup() accessor.
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

#ifdef __cplusplus
extern "C" {
#endif

struct dhcps_msg;
struct pbuf;

// Records the option-12 hostname (if any) for msg->chaddr, then returns
// `state` verbatim. msg/len/state mirror the LWIP_HOOK_DHCPS_POST_STATE
// contract (msg is the parsed *incoming* packet).
s16_t nbp_dhcps_post_state(struct dhcps_msg *msg, s16_t len, s16_t state);

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

#define LWIP_HOOK_DHCPS_POST_STATE(msg, len, state) \
  nbp_dhcps_post_state((msg), (len), (state))

#if CONFIG_NBP_NAT_THROUGHPUT
#define LWIP_HOOK_IP4_CANFORWARD(p, dest) \
  nbp_ip4_canforward((p), (dest))
#endif
