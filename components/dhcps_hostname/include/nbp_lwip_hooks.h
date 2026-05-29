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
// Pure C: this is included from C lwIP sources.

#pragma once

#include "lwip/arch.h"  // s16_t

#ifdef __cplusplus
extern "C" {
#endif

struct dhcps_msg;

// Records the option-12 hostname (if any) for msg->chaddr, then returns
// `state` verbatim. msg/len/state mirror the LWIP_HOOK_DHCPS_POST_STATE
// contract (msg is the parsed *incoming* packet).
s16_t nbp_dhcps_post_state(struct dhcps_msg *msg, s16_t len, s16_t state);

#ifdef __cplusplus
}
#endif

#define LWIP_HOOK_DHCPS_POST_STATE(msg, len, state) \
  nbp_dhcps_post_state((msg), (len), (state))
