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

#ifdef __cplusplus
}
#endif
