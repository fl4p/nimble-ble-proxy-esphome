// Lookup side of the DHCP-client hostname registry populated by the
// LWIP_HOOK_DHCPS_POST_STATE hook in dhcps_hostname.c. Callers (nat_router)
// pass a station MAC and get back the hostname the client advertised in
// DHCP option 12, if one was seen. Safe to call from any task — the
// registry is guarded by a spinlock.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Copy the most-recently-seen DHCP hostname for station `mac` (6 bytes,
// wire order) into `out` (NUL-terminated, capped at `cap`). Returns true
// and a non-empty string when a hostname was recorded for that MAC; false
// (and out[0]='\0' when cap>0) otherwise.
bool dhcps_hostname_lookup(const uint8_t mac[6], char *out, size_t cap);

// Static SoftAP DHCP leases (dhcps_static.c). The DHCP server always gives
// `mac` the address `ip` and never gives `ip` to another client, whatever
// the join order. nat_router owns the persisted table and pushes it here;
// the lwIP hook reads it on every DHCP packet. A change applies from the
// next DHCP packet; a client that already holds another address keeps it
// until it renews or rejoins.
#define DHCPS_STATIC_LEASE_MAX 8

typedef struct {
  uint8_t mac[6];  // wire order
  uint32_t ip;     // IPv4, network byte order
} dhcps_static_lease_t;

// Replace the whole table (n <= DHCPS_STATIC_LEASE_MAX; extra entries are
// dropped). The caller validates the entries.
void dhcps_static_set(const dhcps_static_lease_t *tbl, size_t n);

// The static IP for `mac` (network order), or 0 when it has none.
uint32_t dhcps_static_lookup(const uint8_t mac[6]);

// Whether `ip` (network order) is reserved for some MAC.
bool dhcps_static_is_reserved(uint32_t ip);

#if NBP_DHCPS_SELFTEST
// QEMU self-test (dhcps_static_selftest.c); never returns.
void dhcps_static_selftest_run(void);
#endif

#ifdef __cplusplus
}
#endif
