// QEMU self-test for the static DHCP leases (compile define
// NBP_DHCPS_SELFTEST, never a firmware build). Runs IDF's real DHCP server
// on the lwIP loopback netif with the hooks from nbp_lwip_hooks.h, feeds it
// hand-built client packets through its UDP receive callback, and records
// every reply (type + yiaddr) through LWIP_HOOK_DHCPS_POST_APPEND_OPTS.
// Prints "DHCPS-SELFTEST <step>: PASS|FAIL" and a summary, then idles.

#if NBP_DHCPS_SELFTEST

#include "dhcps_hostname.h"
#include "nbp_lwip_hooks.h"

#include "dhcpserver/dhcpserver.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"

#include <stdio.h>
#include <string.h>

#define T_DISCOVER 1
#define T_REQUEST 3
#define T_RELEASE 7
#define R_OFFER 2
#define R_ACK 5
#define R_NAK 6
#define DYN -2

static int s_fail;
static int s_reply_type;
static uint8_t s_reply_ip[4];
static dhcps_t *s_d;
static struct netif *s_nif;
static SemaphoreHandle_t s_done;

void nbp_dhcps_selftest_reply(int type, const uint8_t *yiaddr) {
  s_reply_type = type;
  memcpy(s_reply_ip, yiaddr, 4);
}

static uint32_t ip4(int d) {  // 192.168.4.d, network order
  return (uint32_t)192 | (uint32_t)168 << 8 | (uint32_t)4 << 16 |
         (uint32_t)d << 24;
}

typedef struct {
  const uint8_t *mac;
  int type;
  int req;     // option 50 host part, 0 = none
  int ciaddr;  // host part, 0 = none
} pkt_t;

static void feed_in_tcpip(void *arg) {
  const pkt_t *k = (const pkt_t *)arg;
  struct udp_pcb *pcb = udp_pcbs;
  while (pcb != NULL && pcb->local_port != 67) pcb = pcb->next;
  if (pcb == NULL) {
    printf("DHCPS-SELFTEST no server pcb\n");
    xSemaphoreGive(s_done);
    return;
  }
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(struct dhcps_msg), PBUF_RAM);
  struct dhcps_msg *m = (struct dhcps_msg *)p->payload;
  memset(m, 0, sizeof(*m));
  m->op = 1;
  m->htype = 1;
  m->hlen = 6;
  m->xid[0] = 0x42;
  memcpy(m->chaddr, k->mac, 6);
  if (k->ciaddr) {
    uint32_t c = ip4(k->ciaddr);
    memcpy(m->ciaddr, &c, 4);
  }
  uint8_t *o = m->options;
  *o++ = 99; *o++ = 130; *o++ = 83; *o++ = 99;
  *o++ = 53; *o++ = 1; *o++ = (uint8_t)k->type;
  if (k->req) {
    uint32_t r = ip4(k->req);
    *o++ = 50; *o++ = 4;
    memcpy(o, &r, 4);
    o += 4;
  }
  *o++ = 255;
  ip_addr_t from = IPADDR4_INIT(0);
  pcb->recv(pcb->recv_arg, pcb, p, &from, 68);  // handle_dhcp frees p
  xSemaphoreGive(s_done);
}

static void feed(const uint8_t *mac, int type, int req, int ciaddr) {
  pkt_t k = {mac, type, req, ciaddr};
  s_reply_type = 0;
  memset(s_reply_ip, 0, 4);
  tcpip_callback(feed_in_tcpip, &k);
  xSemaphoreTake(s_done, portMAX_DELAY);
}

static void expect(const char *step, const uint8_t *mac, int type, int req,
                   int ciaddr, int want_type, int want_host) {
  feed(mac, type, req, ciaddr);
  // want_host: exact host part; -1 any; DYN any address that is not
  // reserved (.2/.4) and not 0 (the allocator's order is its own business).
  const int h = s_reply_ip[3];
  const bool ip_ok = want_host == -1 ||
                     (want_host == DYN ? h != 0 && h != 2 && h != 4
                                       : h == want_host);
  const bool ok = s_reply_type == want_type && ip_ok;
  if (!ok) ++s_fail;
  printf("DHCPS-SELFTEST %s: %s (reply type %d ip .%u, want %d .%d)\n", step,
         ok ? "PASS" : "FAIL", s_reply_type, s_reply_ip[3], want_type,
         want_host);
}

static void restart_in_tcpip(void *arg) {
  (void)arg;
  dhcps_stop(s_d, s_nif);
  ip4_addr_t ip = {.addr = ip4(1)};
  dhcps_start(s_d, s_nif, ip);
  xSemaphoreGive(s_done);
}

static void start_in_tcpip(void *arg) {
  (void)arg;
  s_nif = netif_list;
  while (s_nif != NULL && !(s_nif->name[0] == 'l' && s_nif->name[1] == 'o')) {
    s_nif = s_nif->next;
  }
  s_d = dhcps_new();
  ip4_addr_t ip = {.addr = ip4(1)};
  if (s_nif != NULL && s_d != NULL) dhcps_start(s_d, s_nif, ip);
  xSemaphoreGive(s_done);
}

void dhcps_static_selftest_run(void) {
  static const uint8_t FRY[6] = {0x70, 0x04, 0x1d, 0xa6, 0xab, 0x30};
  static const uint8_t FLAT[6] = {0x70, 0x04, 0x1d, 0xa4, 0xea, 0x34};
  static const uint8_t X[6] = {0x02, 0, 0, 0, 0, 0x01};
  static const uint8_t Y[6] = {0x02, 0, 0, 0, 0, 0x02};
  static const uint8_t Z[6] = {0x02, 0, 0, 0, 0, 0x03};

  s_done = xSemaphoreCreateBinary();
  esp_netif_init();
  vTaskDelay(pdMS_TO_TICKS(200));
  tcpip_callback(start_in_tcpip, NULL);
  xSemaphoreTake(s_done, portMAX_DELAY);
  if (s_nif == NULL || s_d == NULL) {
    printf("DHCPS-SELFTEST setup: FAIL (no loopback netif or dhcps)\n");
    goto out;
  }

  // 1. No static leases: plain join order. X takes .2.
  expect("no table: X discover -> .2", X, T_DISCOVER, 0, 0, R_OFFER, 2);
  expect("dynamic X request", X, T_REQUEST, 2, 0, R_ACK, 2);

  // 2. Reserve .2 for fry and .4 for flat. X still holds .2 in the list.
  dhcps_static_lease_t tbl[2];
  memcpy(tbl[0].mac, FRY, 6);
  tbl[0].ip = ip4(2);
  memcpy(tbl[1].mac, FLAT, 6);
  tbl[1].ip = ip4(4);
  dhcps_static_set(tbl, 2);

  // X renews .2 (ciaddr set): reserved for fry now, so NAK.
  expect("X renew of reserved .2 refused", X, T_REQUEST, 0, 2, R_NAK, -1);
  // flat joins first (the post-reboot order that broke the forwards).
  expect("flat discover first -> .4", FLAT, T_DISCOVER, 0, 0, R_OFFER, 4);
  expect("flat request .4", FLAT, T_REQUEST, 4, 0, R_ACK, 4);
  // flat asks for its old dynamic .2 (INIT-REBOOT): NAK.
  expect("flat init-reboot .2 refused", FLAT, T_REQUEST, 2, 0, R_NAK, -1);
  // X rediscovers: neither .2 nor .4.
  expect("X rediscover -> dynamic", X, T_DISCOVER, 0, 0, R_OFFER, DYN);
  const int xh = s_reply_ip[3];
  expect("X request its offer", X, T_REQUEST, xh, 0, R_ACK, xh);
  // fry asks for .3 (its old dynamic address): NAK, then gets .2.
  expect("fry init-reboot .3 refused", FRY, T_REQUEST, 3, 0, R_NAK, -1);
  expect("fry discover -> .2", FRY, T_DISCOVER, 0, 0, R_OFFER, 2);
  expect("fry request .2", FRY, T_REQUEST, 2, 0, R_ACK, 2);
  expect("fry renew .2", FRY, T_REQUEST, 0, 2, R_ACK, 2);
  // A third dynamic client skips .2/.3/.4.
  expect("Y discover -> dynamic", Y, T_DISCOVER, 0, 0, R_OFFER, DYN);
  expect("Y request .4 refused", Y, T_REQUEST, 4, 0, R_NAK, -1);

  // 3. Server restart frees its whole list (set_ap_dns does this).
  tcpip_callback(restart_in_tcpip, NULL);
  xSemaphoreTake(s_done, portMAX_DELAY);
  expect("after restart: Z discover -> dynamic", Z, T_DISCOVER, 0, 0, R_OFFER,
         DYN);
  expect("after restart: fry renew .2", FRY, T_REQUEST, 0, 2, R_ACK, 2);
  expect("after restart: flat init-reboot .4", FLAT, T_REQUEST, 4, 0, R_ACK, 4);

  // 4. fry releases; .2 stays reserved.
  feed(FRY, T_RELEASE, 0, 2);
  expect("after fry release: Y discover -> dynamic", Y, T_DISCOVER, 0, 0,
         R_OFFER, DYN);
  expect("after fry release: fry discover -> .2", FRY, T_DISCOVER, 0, 0,
         R_OFFER, 2);

  // 5. Table cleared: plain behaviour again (fry keeps its entry).
  dhcps_static_set(NULL, 0);
  expect("no table: fry renew .2", FRY, T_REQUEST, 0, 2, R_ACK, 2);

out:;
  // tcpip thread stack headroom after the hook ran (incl. its log lines).
  TaskStatus_t ts[16];
  const UBaseType_t nt = uxTaskGetSystemState(ts, 16, NULL);
  for (UBaseType_t i = 0; i < nt; ++i) {
    printf("DHCPS-SELFTEST stack high-water %s: %u B free\n", ts[i].pcTaskName,
           (unsigned)ts[i].usStackHighWaterMark);
  }
  printf("DHCPS-SELFTEST summary: %d failure(s)\n", s_fail);
  while (true) vTaskDelay(portMAX_DELAY);
}

#endif  // NBP_DHCPS_SELFTEST
