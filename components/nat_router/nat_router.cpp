#include "nat_router.h"

#ifdef CONFIG_NBP_NAT_ROUTER

#include "esp_coexist.h"  // esp_coex_preference_set — bias RF to WiFi for NAT
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"  // wifi_sta_mac_ip_list_t, *_with_ip
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

// MAC→hostname registry populated by our lwIP DHCP-server hook. Lets the
// /nat client list show names like a consumer router does.
#include "dhcps_hostname.h"

// lwIP NAPT / port-mapping API. The declarations live behind
// #if IP_FORWARD / #if IP_NAPT, both of which our Kconfig `select`s turn
// on (LWIP_IP_FORWARD / LWIP_IPV4_NAPT), so they're visible here.
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "lwip/pbuf.h"  // struct pbuf (tot_len) for the forward-count hook

// Forward-count hook declaration (LWIP_HOOK_IP4_CANFORWARD). Lives in the
// dhcps_hostname component's include dir, available here via REQUIRES.
#include "nbp_lwip_hooks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// esp_coex_preference_set lives in IDF's coexist.c, which is only compiled
// when software (or external) WiFi/BLE coexistence is enabled — and SW coex
// is only on when BT is enabled. In a BLE-free build (CONFIG_NBP_BLE=n) the
// symbol doesn't exist, so calling it is an undefined-reference link error.
// There's also nothing to arbitrate without a second radio user, so make the
// preference call a no-op when coex isn't built in.
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE || CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE
#define NBP_COEX_PREFER(p) esp_coex_preference_set(p)
#else
#define NBP_COEX_PREFER(p) ((void)0)
#endif

#if CONFIG_NBP_NAT_THROUGHPUT
namespace {

// Forwarded-byte tallies for the dashboard throughput chart. Updated from
// the lwIP forward hook (nbp_ip4_canforward, below) which runs in the
// TCP/IP task, and read from the httpd task via get_ap_throughput — so the
// 64-bit values are guarded by a portMUX. These live at file scope (not in
// the nat_router anonymous namespace) because the extern "C" hook is a
// global-scope symbol that lwIP links against and must reach them too.
//   g_ap_down_bytes: forwarded TOWARD an AP client (dest in the AP subnet)
//   g_ap_up_bytes:   forwarded the other way (client -> STA uplink)
portMUX_TYPE g_thru_mux = portMUX_INITIALIZER_UNLOCKED;
uint64_t g_ap_down_bytes = 0;
uint64_t g_ap_up_bytes = 0;
// SoftAP subnet, host byte order, published by nat_router::start(). Until
// the mask is non-zero the hook can't classify, so it attributes traffic to
// "up" (the common case — client uplink dominates and misattributing a few
// boot-time packets is harmless for a rate chart).
uint32_t g_ap_net_host = 0;
uint32_t g_ap_mask_host = 0;

}  // namespace
#endif  // CONFIG_NBP_NAT_THROUGHPUT

namespace nat_router {

namespace {

constexpr const char *TAG = "nat";

// NVS namespace for AP creds + the port-map blob. Separate from "wifi"
// (STA provisioning) and "stats" (telemetry tunables) so the three
// concerns don't intermix.
constexpr const char *NVS_NS = "nat";
constexpr const char *NVS_AP_SSID = "ap_ssid";
constexpr const char *NVS_AP_PSK = "ap_psk";
constexpr const char *NVS_PORTMAP = "portmap";
constexpr const char *NVS_ENABLED = "enabled";

// IPv4 protocol numbers (lwip/prot/ip.h: IP_PROTO_TCP/UDP) — duplicated
// as small literals so we don't pull a private lwip header just for two
// constants. These are also what lwIP's ip_portmap_add expects.
constexpr uint8_t PROTO_TCP = 6;
constexpr uint8_t PROTO_UDP = 17;

// DHCP option flag enabling the server to offer a DNS resolver (lwIP's
// OFFER_DNS in dhcpserver_options.h). Pass-by-pointer to dhcps_option.
constexpr uint8_t OFFER_DNS = 0x02;

constexpr size_t PORTMAP_MAX = CONFIG_NBP_NAT_PORTMAP_MAX;

// One inbound port-forwarding rule. Persisted verbatim as an NVS blob,
// so the layout is fixed: bump a version key if you ever reorder it.
//   proto  6=TCP / 17=UDP
//   mport  external (STA-side) port, host byte order
//   dport  internal (AP-client) port, host byte order
//   daddr  internal client IPv4, network byte order
struct PortmapEntry {
  uint8_t proto;
  uint8_t valid;
  uint16_t mport;
  uint16_t dport;
  uint16_t _pad;
  uint32_t daddr;
};

PortmapEntry g_portmap[PORTMAP_MAX] = {};
esp_netif_t *g_ap = nullptr;
uint32_t g_sta_ip = 0;  // STA IP, network byte order; NAPT external addr
char g_ap_ssid[33] = {0};
char g_ap_psk[64] = {0};
bool g_enabled = true;  // runtime SoftAP on/off (NVS-persisted)
// Whether pause_for_ota() actually dropped the AP, so resume_after_ota()
// only brings it back if it had been up (don't resurrect a user-disabled AP).
bool g_ota_resume_ap = false;

// Format a network-order IPv4 (ESP32 is little-endian, so the low byte is
// the first octet) into dotted-quad.
void ip_to_str(uint32_t net, char *out, size_t cap) {
  std::snprintf(out, cap, "%u.%u.%u.%u", static_cast<unsigned>(net & 0xff),
                static_cast<unsigned>((net >> 8) & 0xff),
                static_cast<unsigned>((net >> 16) & 0xff),
                static_cast<unsigned>((net >> 24) & 0xff));
}

int count_valid() {
  int n = 0;
  for (size_t i = 0; i < PORTMAP_MAX; ++i)
    if (g_portmap[i].valid) ++n;
  return n;
}

// ──────────────────────────── persistence ──────────────────────────

void load_ap_creds() {
  std::strncpy(g_ap_ssid, CONFIG_NBP_NAT_AP_SSID, sizeof(g_ap_ssid) - 1);
  std::strncpy(g_ap_psk, CONFIG_NBP_NAT_AP_PASSWORD, sizeof(g_ap_psk) - 1);
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  char tmp[64];
  size_t n = sizeof(tmp);
  if (nvs_get_str(h, NVS_AP_SSID, tmp, &n) == ESP_OK && tmp[0] != '\0') {
    std::strncpy(g_ap_ssid, tmp, sizeof(g_ap_ssid) - 1);
    g_ap_ssid[sizeof(g_ap_ssid) - 1] = '\0';
  }
  n = sizeof(tmp);
  if (nvs_get_str(h, NVS_AP_PSK, tmp, &n) == ESP_OK) {
    std::strncpy(g_ap_psk, tmp, sizeof(g_ap_psk) - 1);
    g_ap_psk[sizeof(g_ap_psk) - 1] = '\0';
  }
  nvs_close(h);
}

void save_ap_creds() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, NVS_AP_SSID, g_ap_ssid);
  nvs_set_str(h, NVS_AP_PSK, g_ap_psk);
  nvs_commit(h);
  nvs_close(h);
}

// SoftAP on/off, persisted. Defaults to on when no key is present so a
// fresh device behaves like the always-on build.
bool load_enabled() {
  nvs_handle_t h;
  uint8_t v = 1;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u8(h, NVS_ENABLED, &v);
    nvs_close(h);
  }
  return v != 0;
}

void save_enabled(bool en) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, NVS_ENABLED, en ? 1 : 0);
  nvs_commit(h);
  nvs_close(h);
}

void load_portmap() {
  std::memset(g_portmap, 0, sizeof(g_portmap));
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  size_t len = 0;
  if (nvs_get_blob(h, NVS_PORTMAP, nullptr, &len) == ESP_OK &&
      len == sizeof(g_portmap)) {
    nvs_get_blob(h, NVS_PORTMAP, g_portmap, &len);
  }
  // A size mismatch (PORTMAP_MAX changed across a rebuild) leaves the
  // zeroed table — old rules are dropped rather than misinterpreted.
  nvs_close(h);
}

void save_portmap() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, NVS_PORTMAP, g_portmap, sizeof(g_portmap));
  nvs_commit(h);
  nvs_close(h);
}

// ──────────────────────────── NAPT plumbing ────────────────────────

void remove_all_live() {
  for (size_t i = 0; i < PORTMAP_MAX; ++i) {
    if (g_portmap[i].valid)
      ip_portmap_remove(g_portmap[i].proto, g_portmap[i].mport);
  }
}

void apply_portmap() {
  if (g_sta_ip == 0) return;  // no external addr yet — applied on GOT_IP
  for (size_t i = 0; i < PORTMAP_MAX; ++i) {
    if (g_portmap[i].valid) {
      ip_portmap_add(g_portmap[i].proto, g_sta_ip, g_portmap[i].mport,
                     g_portmap[i].daddr, g_portmap[i].dport);
    }
  }
}

// Hand the AP's DHCP server the upstream resolver so AP clients can
// actually resolve names (not just get a route). Re-run whenever the STA
// IP/DNS changes.
void set_ap_dns() {
  if (g_ap == nullptr) return;
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta == nullptr) return;
  esp_netif_dns_info_t dns = {};
  if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) return;
  if (dns.ip.u_addr.ip4.addr == 0) return;  // upstream DNS not known yet
  esp_netif_dhcps_stop(g_ap);
  esp_netif_set_dns_info(g_ap, ESP_NETIF_DNS_MAIN, &dns);
  uint8_t offer = OFFER_DNS;
  esp_netif_dhcps_option(g_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                         &offer, sizeof(offer));
  esp_netif_dhcps_start(g_ap);
}

void configure_ap() {
  // Single-radio APSTA: the AP must sit on the STA's channel. Read the
  // live STA channel (the STA is already associated when start() runs).
  uint8_t primary = 0;
  wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&primary, &sec);

  wifi_config_t ap = {};
  std::strncpy(reinterpret_cast<char *>(ap.ap.ssid), g_ap_ssid,
               sizeof(ap.ap.ssid) - 1);
  ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(g_ap_ssid));
  ap.ap.channel = primary ? primary : 1;
  ap.ap.max_connection = CONFIG_NBP_NAT_AP_MAX_CONN;
  if (std::strlen(g_ap_psk) < 8) {
    ap.ap.authmode = WIFI_AUTH_OPEN;  // WPA2 needs an 8+ char PSK
  } else {
    std::strncpy(reinterpret_cast<char *>(ap.ap.password), g_ap_psk,
                 sizeof(ap.ap.password) - 1);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  }
  esp_wifi_set_config(WIFI_IF_AP, &ap);
}

// Bring the SoftAP up: create the AP netif on first use, switch to APSTA
// (STA association is preserved), configure the AP, hand out DNS, enable
// NAPT, and apply persisted port mappings. Idempotent — safe to call when
// already up. The AP netif is created once and reused across toggles.
void ap_up() {
  if (g_ap == nullptr) {
    g_ap = esp_netif_create_default_wifi_ap();
    esp_netif_ip_info_t ap_ip = {};
    ap_ip.ip.addr = esp_ip4addr_aton(CONFIG_NBP_NAT_AP_IP);
    ap_ip.gw.addr = ap_ip.ip.addr;
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(g_ap);
    esp_netif_set_ip_info(g_ap, &ap_ip);
    esp_netif_dhcps_start(g_ap);
#if CONFIG_NBP_NAT_THROUGHPUT
    // Publish the AP subnet (host byte order) so the forward hook can tell
    // client-bound traffic from uplink traffic. lwIP hands the hook the
    // destination in host order, so match in the same space.
    uint32_t ip_h = lwip_ntohl(ap_ip.ip.addr);
    uint32_t mask_h = lwip_ntohl(ap_ip.netmask.addr);
    portENTER_CRITICAL(&g_thru_mux);
    g_ap_mask_host = mask_h;
    g_ap_net_host = ip_h & mask_h;
    portEXIT_CRITICAL(&g_thru_mux);
#endif
  }
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  // Bias the WiFi/BLE coexistence arbiter toward WiFi while repeating. On
  // this single-radio S3 a ~50%-duty BLE scan competing with APSTA traffic
  // starves WiFi management frames — the SoftAP's PMF SA-Query exchange times
  // out, clients get disassoc'd (reason 209) and reconnect-storm, which both
  // hammers the heap and makes the dashboard crawl. PREFER_WIFI (vs the
  // BALANCE default) gives WiFi more RF opportunity; BLE scan/forwarding is
  // best-effort here anyway. Restored to BALANCE in ap_down().
  NBP_COEX_PREFER(ESP_COEX_PREFER_WIFI);
  configure_ap();
  set_ap_dns();
  // The AP netif comes up asynchronously (WIFI_EVENT_AP_START handler), but
  // esp_netif_napt_enable returns ESP_FAIL if the netif isn't up yet. On the
  // first enable the freshly-created netif was already up so it raced
  // through; on a re-enable (after ap_down switched to STA-only) the netif
  // is brought back up a beat later, so enabling NAPT synchronously here
  // failed. Wait (up to ~1 s) for the netif to be up first, then enable.
  for (int i = 0; i < 40 && !esp_netif_is_netif_up(g_ap); ++i) {
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  esp_err_t err = esp_netif_napt_enable(g_ap);
  if (err != ESP_OK) {
    // The netif occasionally needs another beat; one bounded retry.
    vTaskDelay(pdMS_TO_TICKS(100));
    err = esp_netif_napt_enable(g_ap);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_napt_enable failed: %s (NAT non-functional)",
             esp_err_to_name(err));
  }
  apply_portmap();
  g_enabled = true;

  char ap_s[16], sta_s[16];
  esp_netif_ip_info_t info = {};
  esp_netif_get_ip_info(g_ap, &info);
  ip_to_str(info.ip.addr, ap_s, sizeof(ap_s));
  ip_to_str(g_sta_ip, sta_s, sizeof(sta_s));
  ESP_LOGI(TAG, "NAT SoftAP up: '%s' (%s) %s -> STA %s, %d port mapping(s)",
           g_ap_ssid, std::strlen(g_ap_psk) < 8 ? "open" : "wpa2", ap_s, sta_s,
           count_valid());
}

// Tear the SoftAP down: disable NAPT, drop the live port mappings, and put
// the radio back into STA-only mode. The STA link (and the dashboard) stay
// up; the AP netif is kept registered for a fast re-enable. This is the
// runtime knob that brings the device back to the cooler STA-only duty.
void ap_down() {
  if (g_ap != nullptr) esp_netif_napt_disable(g_ap);
  remove_all_live();
  esp_wifi_set_mode(WIFI_MODE_STA);
  // No SoftAP to protect anymore — hand RF arbitration back to BALANCE so the
  // BLE scan/proxy regains its share when the device is STA-only.
  NBP_COEX_PREFER(ESP_COEX_PREFER_BALANCE);
  g_enabled = false;
  ESP_LOGI(TAG, "NAT SoftAP disabled; radio back to STA-only");
}

void on_sta_got_ip(void * /*arg*/, esp_event_base_t base, int32_t id,
                   void *data) {
  if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) return;
  auto *e = static_cast<ip_event_got_ip_t *>(data);
  uint32_t new_ip = e->ip_info.ip.addr;
  bool changed = (new_ip != g_sta_ip);
  g_sta_ip = new_ip;
  if (!g_enabled) return;  // AP off — nothing to NAT
  if (!changed) {
    set_ap_dns();  // same IP, but DNS may have refreshed
    return;
  }
  remove_all_live();
  apply_portmap();
  set_ap_dns();
  char ip[16];
  ip_to_str(g_sta_ip, ip, sizeof(ip));
  ESP_LOGI(TAG, "STA IP now %s; reapplied %d port mapping(s)", ip,
           count_valid());
}

// Append a JSON "stations":[...] array of currently-associated SoftAP
// clients: MAC, the IP the DHCP server leased them, RSSI, and the DHCP
// option-12 hostname our lwIP hook captured (empty string if the client
// sent none). `sl` is the already-fetched station list (reused for the
// client count); `off` is the current write offset. Returns the new offset.
int append_stations_json(char *buf, int off, size_t cap,
                         const wifi_sta_list_t &sl) {
  // Best-effort MAC↔IP pairing from the DHCP server. Order may differ from
  // sl, so we match by MAC below rather than by index.
  wifi_sta_mac_ip_list_t ipl = {};
  esp_wifi_ap_get_sta_list_with_ip(&sl, &ipl);

  off += std::snprintf(buf + off, cap - off, "\"stations\":[");
  bool first = true;
  for (int i = 0; i < sl.num && off < static_cast<int>(cap) - 160; ++i) {
    const uint8_t *mac = sl.sta[i].mac;

    uint32_t ip = 0;
    for (int j = 0; j < ipl.num; ++j) {
      if (std::memcmp(ipl.sta[j].mac, mac, 6) == 0) {
        ip = ipl.sta[j].ip.addr;
        break;
      }
    }
    char ips[16];
    ip_to_str(ip, ips, sizeof(ips));

    char host[32];
    dhcps_hostname_lookup(mac, host, sizeof(host));
    // host is sanitized to printable ASCII upstream, but that still admits
    // " and \ — escape them for valid JSON.
    char esc[2 * sizeof(host)];
    size_t e = 0;
    for (const char *p = host; *p != '\0' && e < sizeof(esc) - 2; ++p) {
      if (*p == '"' || *p == '\\') esc[e++] = '\\';
      esc[e++] = *p;
    }
    esc[e] = '\0';

    off += std::snprintf(
        buf + off, cap - off,
        "%s{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ip\":\"%s\","
        "\"rssi\":%d,\"hostname\":\"%s\"}",
        first ? "" : ",", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ips,
        static_cast<int>(sl.sta[i].rssi), esc);
    first = false;
  }
  off += std::snprintf(buf + off, cap - off, "]");
  return off;
}

// ──────────────────────────── HTTP endpoints ───────────────────────

esp_err_t nat_get(httpd_req_t *req) {
  esp_netif_ip_info_t ap_info = {};
  if (g_ap != nullptr) esp_netif_get_ip_info(g_ap, &ap_info);
  char ap_ip[16], sta_ip[16];
  ip_to_str(ap_info.ip.addr, ap_ip, sizeof(ap_ip));
  ip_to_str(g_sta_ip, sta_ip, sizeof(sta_ip));

  wifi_sta_list_t sl = {};
  esp_wifi_ap_get_sta_list(&sl);

  char buf[3072];
  int off = std::snprintf(
      buf, sizeof(buf),
      "{\"enabled\":%s,\"ssid\":\"%s\",\"open\":%s,\"ap_ip\":\"%s\","
      "\"sta_ip\":\"%s\",\"clients\":%d,\"max\":%d,",
      g_enabled ? "true" : "false", g_ap_ssid,
      std::strlen(g_ap_psk) < 8 ? "true" : "false", ap_ip, sta_ip, sl.num,
      CONFIG_NBP_NAT_AP_MAX_CONN);

  off = append_stations_json(buf, off, sizeof(buf), sl);
  off += std::snprintf(buf + off, sizeof(buf) - off, ",\"portmaps\":[");

  bool first = true;
  for (size_t i = 0; i < PORTMAP_MAX && off < static_cast<int>(sizeof(buf)) - 96;
       ++i) {
    if (!g_portmap[i].valid) continue;
    char dip[16];
    ip_to_str(g_portmap[i].daddr, dip, sizeof(dip));
    off += std::snprintf(buf + off, sizeof(buf) - off,
                         "%s{\"proto\":\"%s\",\"mport\":%u,\"daddr\":\"%s\","
                         "\"dport\":%u}",
                         first ? "" : ",",
                         g_portmap[i].proto == PROTO_TCP ? "tcp" : "udp",
                         g_portmap[i].mport, dip, g_portmap[i].dport);
    first = false;
  }
  off += std::snprintf(buf + off, sizeof(buf) - off, "]}");

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, off);
}

// POST /nat?enabled=0|1            — toggle the SoftAP at runtime
//      /nat?ssid=...&psk=...       — update AP credentials live + persist
// Params are independent; any combination is accepted.
esp_err_t nat_post(httpd_req_t *req) {
  char query[160];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  char en[8] = {0};
  char ssid[33] = {0};
  char psk[64] = {0};
  bool have_en =
      httpd_query_key_value(query, "enabled", en, sizeof(en)) == ESP_OK;
  bool have_ssid =
      httpd_query_key_value(query, "ssid", ssid, sizeof(ssid)) == ESP_OK;
  bool have_psk =
      httpd_query_key_value(query, "psk", psk, sizeof(psk)) == ESP_OK;
  if (!have_en && !have_ssid && !have_psk) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "need enabled=, ssid= or psk=");
  }

  // Credentials first so an enable in the same request brings the AP up
  // with the new values.
  if (have_ssid) {
    size_t n = std::strlen(ssid);
    if (n < 1 || n > 32) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "ssid must be 1..32 chars");
    }
    std::strncpy(g_ap_ssid, ssid, sizeof(g_ap_ssid) - 1);
  }
  if (have_psk) {
    size_t n = std::strlen(psk);
    if (n != 0 && (n < 8 || n > 63)) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "psk must be empty or 8..63 chars");
    }
    std::strncpy(g_ap_psk, psk, sizeof(g_ap_psk) - 1);
  }
  if (have_ssid || have_psk) {
    save_ap_creds();
    if (g_enabled) configure_ap();  // live; associated clients re-handshake
    ESP_LOGI(TAG, "AP creds updated: ssid='%s' (%s)", g_ap_ssid,
             std::strlen(g_ap_psk) < 8 ? "open" : "wpa2");
  }

  if (have_en) {
    bool want = (en[0] == '1');
    if (want != g_enabled) {
      if (want)
        ap_up();
      else
        ap_down();
      save_enabled(want);
    }
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}

// POST /portmap?proto=tcp&mport=8080&daddr=192.168.4.50&dport=80
// POST /portmap?del=1&proto=tcp&mport=8080
esp_err_t portmap_post(httpd_req_t *req) {
  char query[160];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  char val[40];

  // proto (required for both add + delete)
  if (httpd_query_key_value(query, "proto", val, sizeof(val)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing proto");
  }
  uint8_t proto;
  if (std::strcmp(val, "tcp") == 0) {
    proto = PROTO_TCP;
  } else if (std::strcmp(val, "udp") == 0) {
    proto = PROTO_UDP;
  } else {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "proto=tcp|udp");
  }

  if (httpd_query_key_value(query, "mport", val, sizeof(val)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing mport");
  }
  int mport = std::atoi(val);
  if (mport < 1 || mport > 65535) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "mport out of range");
  }

  bool del = httpd_query_key_value(query, "del", val, sizeof(val)) == ESP_OK &&
             val[0] == '1';

  if (del) {
    for (size_t i = 0; i < PORTMAP_MAX; ++i) {
      if (g_portmap[i].valid && g_portmap[i].proto == proto &&
          g_portmap[i].mport == mport) {
        g_portmap[i].valid = 0;
        save_portmap();
        ip_portmap_remove(proto, static_cast<uint16_t>(mport));
        ESP_LOGI(TAG, "portmap removed: %s :%d",
                 proto == PROTO_TCP ? "tcp" : "udp", mport);
        break;
      }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
  }

  // add: needs daddr + dport
  if (httpd_query_key_value(query, "daddr", val, sizeof(val)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing daddr");
  }
  ip4_addr_t daddr;
  if (!ip4addr_aton(val, &daddr)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad daddr");
  }
  if (httpd_query_key_value(query, "dport", val, sizeof(val)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing dport");
  }
  int dport = std::atoi(val);
  if (dport < 1 || dport > 65535) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "dport out of range");
  }

  // Overwrite an existing same-proto+mport rule, else take a free slot.
  int slot = -1;
  for (size_t i = 0; i < PORTMAP_MAX; ++i) {
    if (g_portmap[i].valid && g_portmap[i].proto == proto &&
        g_portmap[i].mport == mport) {
      slot = static_cast<int>(i);
      break;
    }
  }
  if (slot < 0) {
    for (size_t i = 0; i < PORTMAP_MAX; ++i) {
      if (!g_portmap[i].valid) {
        slot = static_cast<int>(i);
        break;
      }
    }
  }
  if (slot < 0) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "portmap table full");
  }

  g_portmap[slot] = PortmapEntry{proto, 1, static_cast<uint16_t>(mport),
                                 static_cast<uint16_t>(dport), 0, daddr.addr};
  save_portmap();
  if (g_sta_ip != 0) {
    ip_portmap_add(proto, g_sta_ip, static_cast<uint16_t>(mport), daddr.addr,
                   static_cast<uint16_t>(dport));
  }
  char dip[16];
  ip_to_str(daddr.addr, dip, sizeof(dip));
  ESP_LOGI(TAG, "portmap added: %s :%d -> %s:%d",
           proto == PROTO_TCP ? "tcp" : "udp", mport, dip, dport);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}

}  // namespace

void pause_for_ota() {
  // Drop the SoftAP for the duration of an OTA so the radio isn't split
  // between APSTA forwarding and the upload. ap_down() keeps the STA link
  // (and thus the OTA TCP stream + dashboard) up — only the AP goes away.
  g_ota_resume_ap = g_enabled;
  if (g_enabled) {
    ESP_LOGI(TAG, "OTA: dropping SoftAP to free the radio");
    ap_down();
  }
}

void resume_after_ota() {
  // Only on OTA failure (success reboots). Restore the AP iff we dropped it.
  if (g_ota_resume_ap) {
    ESP_LOGI(TAG, "OTA failed: restoring SoftAP");
    ap_up();
  }
  g_ota_resume_ap = false;
}

void start() {
  load_ap_creds();
  load_portmap();
  g_enabled = load_enabled();

  // Seed the current STA IP so a runtime enable can apply port mappings
  // immediately, and re-apply them whenever the STA (re)acquires an IP.
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t si = {};
  if (sta != nullptr && esp_netif_get_ip_info(sta, &si) == ESP_OK) {
    g_sta_ip = si.ip.addr;
  }
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                      &on_sta_got_ip, nullptr, nullptr);

  if (g_enabled) {
    ap_up();
  } else {
    ESP_LOGI(TAG,
             "NAT router compiled in but SoftAP disabled; "
             "enable at runtime via POST /nat?enabled=1");
  }
}

void register_endpoints(void *httpd_handle) {
  if (httpd_handle == nullptr) {
    ESP_LOGW(TAG, "no httpd handle; NAT endpoints disabled");
    return;
  }
  auto srv = static_cast<httpd_handle_t>(httpd_handle);
  httpd_uri_t nat_g = {.uri = "/nat",
                       .method = HTTP_GET,
                       .handler = &nat_get,
                       .user_ctx = nullptr};
  httpd_uri_t nat_p = {.uri = "/nat",
                       .method = HTTP_POST,
                       .handler = &nat_post,
                       .user_ctx = nullptr};
  httpd_uri_t pm_p = {.uri = "/portmap",
                      .method = HTTP_POST,
                      .handler = &portmap_post,
                      .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &nat_g);
  httpd_register_uri_handler(srv, &nat_p);
  httpd_register_uri_handler(srv, &pm_p);
  ESP_LOGI(TAG, "NAT config at /nat, port-forwarding at /portmap");
}

#if CONFIG_NBP_NAT_THROUGHPUT
void get_ap_throughput(uint64_t *rx_bytes, uint64_t *tx_bytes) {
  // rx = bytes from clients (uplink), tx = bytes to clients (downlink).
  portENTER_CRITICAL(&g_thru_mux);
  uint64_t up = g_ap_up_bytes;
  uint64_t down = g_ap_down_bytes;
  portEXIT_CRITICAL(&g_thru_mux);
  if (rx_bytes) *rx_bytes = up;
  if (tx_bytes) *tx_bytes = down;
}
#endif  // CONFIG_NBP_NAT_THROUGHPUT

}  // namespace nat_router

#if CONFIG_NBP_NAT_THROUGHPUT
// lwIP forward hook (LWIP_HOOK_IP4_CANFORWARD). Runs in the TCP/IP task for
// every datagram about to be forwarded — including both NAT directions, since
// inbound packets are de-NAT'd to the client IP and then re-forwarded. We add
// the IP-layer byte count to the up/down tally and return -1 so lwIP's normal
// forward-eligibility check proceeds unchanged. Global C linkage: lwIP links
// against this symbol; nat_router.o is pulled into the image to resolve it.
extern "C" int nbp_ip4_canforward(struct pbuf *p, u32_t dest) {
  if (p != nullptr) {
    portENTER_CRITICAL(&g_thru_mux);
    if (g_ap_mask_host != 0 &&
        (dest & g_ap_mask_host) == g_ap_net_host) {
      g_ap_down_bytes += p->tot_len;  // toward an AP client
    } else {
      g_ap_up_bytes += p->tot_len;  // from a client, out the uplink
    }
    portEXIT_CRITICAL(&g_thru_mux);
  }
  return -1;  // no decision — let ip4_canforward() decide as usual
}
#endif  // CONFIG_NBP_NAT_THROUGHPUT

#endif  // CONFIG_NBP_NAT_ROUTER
