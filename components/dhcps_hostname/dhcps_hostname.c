// DHCP-client hostname capture. IDF 5.5's built-in DHCP server parses only
// a handful of options and throws away option 12 (Host Name), so a SoftAP
// client's name is never surfaced (cf. how consumer routers show it). We
// recover it without forking the server: the LWIP_HOOK_DHCPS_POST_STATE
// hook (wired via ESP_IDF_LWIP_HOOK_FILENAME) hands us each parsed incoming
// DHCP message, from which we read option 12 and store it keyed by MAC.
// nat_router reads it back via dhcps_hostname_lookup() to enrich /nat.

#include "nbp_lwip_hooks.h"
#include "dhcps_hostname.h"

#include "dhcpserver/dhcpserver.h"  // struct dhcps_msg

#include "freertos/FreeRTOS.h"

#include <string.h>

#define DHCP_OPT_HOST_NAME 12
#define DHCP_OPT_END 255
#define DHCP_OPT_PAD 0

// Stored name cap (incl. NUL). DHCP option 12 can be longer, but a short
// cap keeps the table small and matches what a UI row can show.
#define HN_NAME_MAX 32
// Tracked stations. Sized at the SoftAP client ceiling; a couple extra so
// a recently-departed client's name survives a brief reconnect gap.
#define HN_SLOTS 8

typedef struct {
  uint8_t mac[6];
  bool used;
  char name[HN_NAME_MAX];
} hn_entry_t;

static hn_entry_t s_tbl[HN_SLOTS];
static uint8_t s_next;  // round-robin victim when the table is full
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Parse option 12 out of `m->options` into `out` (NUL-terminated). Returns
// true if a non-empty, printable name was found. Runs outside the lock.
static bool extract_hostname(const struct dhcps_msg *m, char *out, size_t cap) {
  if (cap == 0) {
    return false;
  }
  out[0] = '\0';
  // options[] begins with the 4-byte BOOTP magic cookie; TLV options follow.
  const uint8_t *opt = m->options + 4;
  const uint8_t *end = m->options + sizeof(m->options);
  while (opt < end) {
    uint8_t code = opt[0];
    if (code == DHCP_OPT_END) {
      break;
    }
    if (code == DHCP_OPT_PAD) {
      ++opt;
      continue;
    }
    if (opt + 2 > end) {
      break;
    }
    uint8_t olen = opt[1];
    if (opt + 2 + olen > end) {
      break;
    }
    if (code == DHCP_OPT_HOST_NAME && olen > 0) {
      size_t n = olen < cap - 1 ? olen : cap - 1;
      size_t w = 0;
      for (size_t i = 0; i < n; ++i) {
        uint8_t c = opt[2 + i];
        if (c == '\0') {
          break;  // some clients NUL-pad; stop at the first
        }
        // Keep it printable so it drops straight into JSON / a table row.
        out[w++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
      }
      out[w] = '\0';
      return w > 0;
    }
    opt += 2 + olen;
  }
  return false;
}

static void record(const uint8_t mac[6], const char *name) {
  taskENTER_CRITICAL(&s_mux);
  int slot = -1;
  for (int i = 0; i < HN_SLOTS; ++i) {
    if (s_tbl[i].used && memcmp(s_tbl[i].mac, mac, 6) == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    for (int i = 0; i < HN_SLOTS; ++i) {
      if (!s_tbl[i].used) {
        slot = i;
        break;
      }
    }
  }
  if (slot < 0) {
    slot = s_next;
    s_next = (uint8_t)((s_next + 1) % HN_SLOTS);
  }
  memcpy(s_tbl[slot].mac, mac, 6);
  s_tbl[slot].used = true;
  strlcpy(s_tbl[slot].name, name, sizeof(s_tbl[slot].name));
  taskEXIT_CRITICAL(&s_mux);
}

s16_t nbp_dhcps_post_state(struct dhcps_msg *msg, s16_t len, s16_t state) {
  (void)len;
  if (msg != NULL) {
    char name[HN_NAME_MAX];
    if (extract_hostname(msg, name, sizeof(name))) {
      record(msg->chaddr, name);  // chaddr[0..5] = client MAC
    }
  }
  return state;
}

bool dhcps_hostname_lookup(const uint8_t mac[6], char *out, size_t cap) {
  if (cap > 0) {
    out[0] = '\0';
  }
  bool found = false;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < HN_SLOTS; ++i) {
    if (s_tbl[i].used && memcmp(s_tbl[i].mac, mac, 6) == 0) {
      if (cap > 0) {
        strlcpy(out, s_tbl[i].name, cap);
      }
      found = s_tbl[i].name[0] != '\0';
      break;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
  return found;
}
