#include "modbus_bridge.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_bit_defs.h"
#include "esp_http_server.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "http_confirm.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "soc/uart_pins.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "soc/usb_pins.h"
#endif

namespace modbus_bridge {
namespace {

constexpr const char *TAG = "mbridge";
constexpr const char *NVS_NS = "mbridge";
constexpr uart_port_t kPort = static_cast<uart_port_t>(CONFIG_NBP_MB_UART_NUM);
constexpr size_t kBufLen = 512;
constexpr size_t kMaxAdu = 256;
constexpr int kUartRxRing = 512;     // driver ring, must exceed the 128 B FIFO
constexpr int kUartQueueLen = 8;
constexpr uint32_t kStack = 3072;
constexpr UBaseType_t kPrio = 3;
constexpr UBaseType_t kTxPrio = 19;  // above tcpip (18) while the bus is ours
constexpr int kGatherMs = 3;         // join a request split over TCP segments
constexpr int kFrameStallMs = 100;   // partial frame, no line-idle seen
#if CONFIG_UART_ISR_IN_IRAM
constexpr int kIntrFlags = ESP_INTR_FLAG_IRAM;
#else
constexpr int kIntrFlags = 0;
#endif

std::atomic<uint32_t> g_baud{CONFIG_NBP_MB_BAUD};
std::atomic<uint8_t> g_parity{CONFIG_NBP_MB_PARITY};  // 0 N, 1 E, 2 O
std::atomic<uint32_t> g_allow{0};  // network order, 0 = any STA-subnet peer
std::atomic<uint32_t> g_line_gen{0};
int g_tx = CONFIG_NBP_MB_TX_GPIO, g_rx = CONFIG_NBP_MB_RX_GPIO,
    g_de = CONFIG_NBP_MB_DE_GPIO;
// NVS pins: -1 = Kconfig; de -2 = no direction pin.
int g_nvs_tx = -1, g_nvs_rx = -1, g_nvs_de = -1;
bool g_loaded = false, g_pins_ok = false;

std::atomic<uint32_t> g_client_ip{0};
std::atomic<uint32_t> g_tx_frames{0}, g_rx_frames{0}, g_tx_bytes{0},
    g_rx_bytes{0}, g_timeouts{0}, g_refused{0}, g_bad_req{0};
TaskHandle_t g_task = nullptr;
QueueHandle_t g_uq = nullptr;
uint8_t g_buf[kBufLen];
constexpr char kParity[] = "neo";

bool pin_ok(int p) {
  if (p < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(p)) return false;
  if (p == U0TXD_GPIO_NUM || p == U0RXD_GPIO_NUM) return false;  // console
  // Claimed by IDF (flash, PSRAM); a running bridge's UART claims its TX.
  if (esp_gpio_is_reserved(BIT64(p)) &&
      !(g_task && (p == g_tx || p == g_rx || p == g_de)))
    return false;
#if CONFIG_IDF_TARGET_ESP32S3
  // Octal PSRAM 33..37 is not reserved with quad flash.
  if (p >= 26 && p <= 37) return false;
  if (p == 0 || p == 3 || p == 45 || p == 46) return false;  // straps
  if (p == USBPHY_DM_NUM || p == USBPHY_DP_NUM) return false;
#endif
  return true;
}

bool pins_ok(int tx, int rx, int de) {
  return pin_ok(tx) && pin_ok(rx) && (de < 0 || pin_ok(de)) && tx != rx &&
         tx != de && rx != de;
}

uint16_t crc16(const uint8_t *p, size_t n) {
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= *p++;
    for (int i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return c;
}

bool crc_ok(const uint8_t *p, size_t n) {
  return n >= 4 && crc16(p, n - 2) == (p[n - 2] | (p[n - 1] << 8));
}

// Request length implied by the function code, 0 = unknown (CRC decides).
size_t req_len(const uint8_t *p, size_t n) {
  if (n < 2) return 0;
  if (p[1] >= 1 && p[1] <= 6) return 8;
  if (p[1] == 15 || p[1] == 16) return n < 7 ? 9 : 9 + p[6];
  return 0;
}

bool req_done(const uint8_t *p, size_t n) {
  size_t want = req_len(p, n);
  return want ? n >= want : crc_ok(p, n);
}

bool req_valid(const uint8_t *p, size_t n) {
  size_t want = req_len(p, n);
  return n <= kMaxAdu && (want == 0 || n == want) && crc_ok(p, n);
}

uart_parity_t parity_of(uint8_t p) {
  return p == 1 ? UART_PARITY_EVEN : p == 2 ? UART_PARITY_ODD
                                            : UART_PARITY_DISABLE;
}

// Line idle = Modbus t3.5: 3.5 chars, fixed 1.75 ms above 19200 baud.
// The RX timeout register holds 1023 bit times.
uint8_t idle_chars(uint32_t baud, uint8_t parity) {
  uint32_t bits = parity ? 11 : 10;
  uint32_t char_us = bits * 1000000u / baud;
  uint32_t n = (1750 + char_us - 1) / char_us;
  if (n < 4) n = 4;
  if (n > 1023 / bits) n = 1023 / bits;
  return static_cast<uint8_t>(n);
}

void set_rx_timeout(uint32_t b, uint8_t p) {
  esp_err_t e = uart_set_rx_timeout(kPort, idle_chars(b, p));
  if (e != ESP_OK) ESP_LOGE(TAG, "rx timeout: %s", esp_err_to_name(e));
}

void apply_line() {
  uint32_t b = g_baud.load();
  uint8_t p = g_parity.load();
  uart_set_baudrate(kPort, b);
  uart_set_parity(kPort, parity_of(p));
  set_rx_timeout(b, p);
  xQueueReset(g_uq);
  uart_flush_input(kPort);
}

void nvs_load() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  uint32_t u32;
  uint8_t u8;
  int8_t i8;
  if (nvs_get_u32(h, "baud", &u32) == ESP_OK && u32 >= 1200 && u32 <= 1000000)
    g_baud = u32;
  if (nvs_get_u8(h, "parity", &u8) == ESP_OK && u8 <= 2) g_parity = u8;
  if (nvs_get_u32(h, "allow", &u32) == ESP_OK) g_allow = u32;
  if (nvs_get_i8(h, "tx", &i8) == ESP_OK) g_nvs_tx = i8;
  if (nvs_get_i8(h, "rx", &i8) == ESP_OK) g_nvs_rx = i8;
  if (nvs_get_i8(h, "de", &i8) == ESP_OK) g_nvs_de = i8;
  nvs_close(h);
  int tx = g_nvs_tx >= 0 ? g_nvs_tx : g_tx, rx = g_nvs_rx >= 0 ? g_nvs_rx : g_rx,
      de = g_nvs_de >= 0 ? g_nvs_de : g_nvs_de == -2 ? -1 : g_de;
  if (pins_ok(tx, rx, de)) {
    g_tx = tx;
    g_rx = rx;
    g_de = de;
  } else {
    ESP_LOGW(TAG, "NVS pins tx=%d rx=%d de=%d invalid, using Kconfig", tx, rx, de);
  }
}

void load_once() {
  if (g_loaded) return;
  g_loaded = true;
  nvs_load();
  g_pins_ok = pins_ok(g_tx, g_rx, g_de);
  if (!g_pins_ok)
    ESP_LOGE(TAG, "pins tx=%d rx=%d de=%d not allowed, bridge off", g_tx, g_rx, g_de);
}

void drop_client(int &cfd) {
  if (cfd < 0) return;
  close(cfd);
  cfd = -1;
  g_client_ip = 0;
}

bool send_all(int &cfd, const uint8_t *p, size_t n) {
  while (n) {
    int m = send(cfd, p, n, 0);
    if (m <= 0) {
      ESP_LOGW(TAG, "send failed (errno %d), dropping client", errno);
      drop_client(cfd);
      return false;
    }
    p += m;
    n -= m;
  }
  return true;
}

// Waits up to first_wait for UART data, then collects until line idle and
// forwards it as one send(). Returns the bytes received.
size_t pump_uart(int &cfd, TickType_t first_wait) {
  size_t len = 0;
  bool dropped = false;
  uart_event_t ev;
  TickType_t wait = first_wait;
  while (xQueueReceive(g_uq, &ev, wait) == pdTRUE) {
    if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
      uart_flush_input(kPort);
      xQueueReset(g_uq);
      dropped = true;
      break;
    }
    if (ev.type != UART_DATA) continue;
    // What is buffered now, not ev.size: an event may outlive a flush.
    size_t avail = 0, room = kBufLen - len;
    uart_get_buffered_data_len(kPort, &avail);
    size_t want = avail < room ? avail : room;
    int r = want ? uart_read_bytes(kPort, g_buf + len, want, 0) : 0;
    if (r > 0) len += r;
    if (avail > want) {
      uart_flush_input(kPort);
      dropped = true;
    }
    if (ev.timeout_flag) break;
    wait = pdMS_TO_TICKS(kFrameStallMs);
  }
  if (dropped) ESP_LOGW(TAG, "UART overflow, frame truncated");
  if (len == 0) return 0;
  g_rx_frames++;
  g_rx_bytes += len;
  if (cfd >= 0) send_all(cfd, g_buf, len);
  return len;
}

void drop_rx() {
  xQueueReset(g_uq);
  uart_flush_input(kPort);
}

void transmit(const uint8_t *p, size_t n) {
  drop_rx();
  uint32_t baud = g_baud.load();
  uint32_t ms = n * 11 * 1000 / baud + 50;
  vTaskPrioritySet(nullptr, kTxPrio);
  if (g_de >= 0) gpio_set_level(static_cast<gpio_num_t>(g_de), 1);
  uart_write_bytes(kPort, p, n);
  esp_err_t e = uart_wait_tx_done(kPort, pdMS_TO_TICKS(ms));
  if (g_de >= 0) gpio_set_level(static_cast<gpio_num_t>(g_de), 0);
  vTaskPrioritySet(nullptr, kPrio);
  // Drop what RX saw meanwhile (floating RO or an auto-direction echo),
  // two character times past the last stop bit.
  uint32_t us = 22000000u / baud;
  if (us > 2000) vTaskDelay(pdMS_TO_TICKS(us / 1000) + 1);
  else esp_rom_delay_us(us);
  drop_rx();
  if (e != ESP_OK) ESP_LOGW(TAG, "tx_done timeout (%u B)", static_cast<unsigned>(n));
  g_tx_frames++;
  g_tx_bytes += n;
}

// Gathers one RTU request (complete by FC length or CRC, else at a
// kGatherMs gap) and transmits it only if valid; else drops the client.
void handle_request(int &cfd) {
  size_t n = 0;
  bool eof = false;
  for (;;) {
    int m = recv(cfd, g_buf + n, kBufLen - n, 0);
    if (m == 0 && n > 0) {
      eof = true;  // half-close after the request
      break;
    }
    if (m <= 0) {
      if (n == 0) ESP_LOGI(TAG, "client closed");
      drop_client(cfd);
      return;
    }
    n += m;
    if (n > kMaxAdu || req_done(g_buf, n)) break;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(cfd, &rf);
    timeval tv = {0, kGatherMs * 1000};
    if (select(cfd + 1, &rf, nullptr, nullptr, &tv) <= 0) break;
  }
  if (!req_valid(g_buf, n)) {
    g_bad_req++;
    ESP_LOGW(TAG, "bad request (%u B), dropping client", static_cast<unsigned>(n));
    drop_client(cfd);
    return;
  }
  transmit(g_buf, n);
  if (pump_uart(cfd, pdMS_TO_TICKS(CONFIG_NBP_MB_REPLY_TIMEOUT_MS)) == 0)
    g_timeouts++;
  if (eof) drop_client(cfd);
}

bool in_subnet(uint32_t a, const esp_netif_ip_info_t &i) {
  return i.ip.addr != 0 && ((a ^ i.ip.addr) & i.netmask.addr) == 0;
}

// STA side only: the connection must have come in on the STA address, and
// the peer must be the allowlisted IP or on the STA subnet, never a SoftAP
// client.
bool peer_ok(uint32_t local, uint32_t peer) {
  esp_netif_ip_info_t si = {}, ai = {};
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta == nullptr || esp_netif_get_ip_info(sta, &si) != ESP_OK ||
      si.ip.addr == 0 || local != si.ip.addr)
    return false;
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap != nullptr && esp_netif_get_ip_info(ap, &ai) == ESP_OK &&
      in_subnet(peer, ai))
    return false;
  uint32_t allow = g_allow.load();
  return allow ? peer == allow : in_subnet(peer, si);
}

void accept_client(int lfd, int &cfd) {
  sockaddr_in peer = {}, local = {};
  socklen_t pl = sizeof(peer), ll = sizeof(local);
  int fd = accept(lfd, reinterpret_cast<sockaddr *>(&peer), &pl);
  if (fd < 0) return;
  char ip[16];
  inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
  // A connected client is replaced only by the allowlisted IP (peer_ok
  // admits no other then) or by its own IP reconnecting.
  bool busy = cfd >= 0 && g_allow.load() == 0 &&
              peer.sin_addr.s_addr != g_client_ip.load();
  if (busy || getsockname(fd, reinterpret_cast<sockaddr *>(&local), &ll) != 0 ||
      !peer_ok(local.sin_addr.s_addr, peer.sin_addr.s_addr)) {
    g_refused++;
    linger lg = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(fd);
    ESP_LOGW(TAG, "refused %s%s", ip, busy ? " (busy)" : "");
    return;
  }
  if (cfd >= 0) ESP_LOGI(TAG, "client replaced");
  drop_client(cfd);
  cfd = fd;
  int one = 1, idle = 60, intvl = 10, cnt = 3;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  // A dead client would otherwise hold the slot forever.
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
  timeval to = {2, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
  g_client_ip = peer.sin_addr.s_addr;
  ESP_LOGI(TAG, "client %s", ip);
}

int open_listener() {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(CONFIG_NBP_MB_TCP_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 ||
      listen(fd, 1) != 0) {
    ESP_LOGE(TAG, "listen :%d failed (errno %d)", CONFIG_NBP_MB_TCP_PORT, errno);
    close(fd);
    return -1;
  }
  return fd;
}

void bridge_task(void *) {
  int lfd = -1, cfd = -1;
  uint32_t gen = g_line_gen.load();
  for (;;) {
    if (gen != g_line_gen.load()) {
      gen = g_line_gen.load();
      apply_line();
    }
    if (lfd < 0 && (lfd = open_listener()) < 0) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(lfd, &rf);
    if (cfd >= 0) FD_SET(cfd, &rf);
    timeval tv = {0, cfd >= 0 ? 50000 : 500000};
    int r = select((cfd > lfd ? cfd : lfd) + 1, &rf, nullptr, nullptr, &tv);
    if (r < 0) {
      ESP_LOGW(TAG, "select errno %d, reopening", errno);
      drop_client(cfd);
      close(lfd);
      lfd = -1;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (r > 0 && FD_ISSET(lfd, &rf)) accept_client(lfd, cfd);
    else if (r > 0 && cfd >= 0 && FD_ISSET(cfd, &rf)) handle_request(cfd);
    pump_uart(cfd, 0);  // late replies and stray bytes
  }
}

esp_err_t modbus_get(httpd_req_t *req) {
  char ip[16] = "", allow[16] = "";
  in_addr a;
  if ((a.s_addr = g_client_ip.load())) inet_ntoa_r(a, ip, sizeof(ip));
  if ((a.s_addr = g_allow.load())) inet_ntoa_r(a, allow, sizeof(allow));
  char buf[320];
  int n = snprintf(
      buf, sizeof(buf),
      "{\"running\":%s,\"uart\":%d,\"tcp_port\":%d,\"baud\":%lu,\"parity\":\"%c\","
      "\"tx\":%d,\"rx\":%d,\"de\":%d,\"nvs_tx\":%d,\"nvs_rx\":%d,"
      "\"nvs_de\":%d,\"allow\":\"%s\",\"client\":\"%s\","
      "\"reply_timeout_ms\":%d,\"stack_free\":%u}",
      g_task ? "true" : "false", CONFIG_NBP_MB_UART_NUM, CONFIG_NBP_MB_TCP_PORT,
      static_cast<unsigned long>(g_baud.load()), kParity[g_parity.load()], g_tx,
      g_rx, g_de, g_nvs_tx, g_nvs_rx, g_nvs_de, allow, ip,
      CONFIG_NBP_MB_REPLY_TIMEOUT_MS,
      g_task ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(g_task)) : 0u);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

// POST /modbus?baud=9600&parity=n|e|o&allow=1.2.3.4|none&tx=&rx=&de=
// baud/parity/allow apply at once; pins (-1 = Kconfig, de=none = no
// direction pin) after a reboot.
esp_err_t modbus_post(httpd_req_t *req) {
  if (!nbp_http::confirmed(req)) return nbp_http::send_unconfirmed(req);
  char q[128], v[20];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  long baud = -1;
  int parity = -1, pins[3] = {g_nvs_tx, g_nvs_rx, g_nvs_de};
  bool set_allow = false, set_pins = false;
  uint32_t allow = 0;
  if (httpd_query_key_value(q, "baud", v, sizeof(v)) == ESP_OK) {
    baud = strtol(v, nullptr, 10);
    if (baud < 1200 || baud > 1000000)
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "baud 1200..1000000");
  }
  if (httpd_query_key_value(q, "parity", v, sizeof(v)) == ESP_OK) {
    const char *p = std::strchr(kParity, v[0] | 0x20);
    if (v[0] == 0 || p == nullptr)
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "parity n|e|o");
    parity = static_cast<int>(p - kParity);
  }
  if (httpd_query_key_value(q, "allow", v, sizeof(v)) == ESP_OK) {
    set_allow = true;
    if (v[0] && std::strcmp(v, "none") != 0) {
      in_addr a;
      if (inet_aton(v, &a) == 0 || a.s_addr == 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "allow: IPv4 or none");
      allow = a.s_addr;
    }
  }
  static const char *const kPinKeys[3] = {"tx", "rx", "de"};
  for (int i = 0; i < 3; i++) {
    if (httpd_query_key_value(q, kPinKeys[i], v, sizeof(v)) != ESP_OK) continue;
    pins[i] = (i == 2 && std::strcmp(v, "none") == 0) ? -2 : atoi(v);
    set_pins = true;
    if (pins[i] != -1 && !(i == 2 && pins[i] == -2) && !pin_ok(pins[i]))
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pin not allowed");
  }
  if (set_pins) {
    int t = pins[0] >= 0 ? pins[0] : CONFIG_NBP_MB_TX_GPIO,
        r = pins[1] >= 0 ? pins[1] : CONFIG_NBP_MB_RX_GPIO,
        d = pins[2] >= 0 ? pins[2] : pins[2] == -2 ? -1 : CONFIG_NBP_MB_DE_GPIO;
    if (t == r || (d >= 0 && (t == d || r == d)))
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pins must differ");
  }
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
  esp_err_t e = ESP_OK;
  if (baud > 0) e = nvs_set_u32(h, "baud", static_cast<uint32_t>(baud));
  if (e == ESP_OK && parity >= 0) e = nvs_set_u8(h, "parity", static_cast<uint8_t>(parity));
  if (e == ESP_OK && set_allow) e = nvs_set_u32(h, "allow", allow);
  for (int i = 0; set_pins && e == ESP_OK && i < 3; i++)
    e = nvs_set_i8(h, kPinKeys[i], static_cast<int8_t>(pins[i]));
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  if (e != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write");
  if (baud > 0) g_baud = static_cast<uint32_t>(baud);
  if (parity >= 0) g_parity = static_cast<uint8_t>(parity);
  if (set_allow) g_allow = allow;
  if (set_pins) {
    g_nvs_tx = pins[0];
    g_nvs_rx = pins[1];
    g_nvs_de = pins[2];
  }
  if (baud > 0 || parity >= 0) g_line_gen++;
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, set_pins ? "{\"ok\":true,\"reboot\":true}"
                                          : "{\"ok\":true}");
}

}  // namespace

void early_init() {
  if (g_loaded) return;
  load_once();
  if (!g_pins_ok) return;
  if (g_de >= 0) {
    gpio_num_t de = static_cast<gpio_num_t>(g_de);
    gpio_set_level(de, 0);
    gpio_config_t io = {};
    io.pin_bit_mask = BIT64(g_de);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(de, 0);
  }
  // TX idles high (mark) until the UART owns it.
  gpio_num_t tx = static_cast<gpio_num_t>(g_tx);
  gpio_set_direction(tx, GPIO_MODE_INPUT);
  gpio_pullup_en(tx);
}

void start() {
  if (g_task != nullptr) return;
  early_init();
  if (!g_pins_ok) return;

  uart_config_t uc = {};
  uc.baud_rate = static_cast<int>(g_baud.load());
  uc.data_bits = UART_DATA_8_BITS;
  uc.parity = parity_of(g_parity.load());
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_DEFAULT;
  esp_err_t e =
      uart_driver_install(kPort, kUartRxRing, 0, kUartQueueLen, &g_uq, kIntrFlags);
  if (e == ESP_OK) e = uart_param_config(kPort, &uc);
  if (e == ESP_OK)
    e = uart_set_pin(kPort, g_tx, g_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "UART%d setup failed: %s", CONFIG_NBP_MB_UART_NUM, esp_err_to_name(e));
    return;
  }
  // RO tri-states while transmitting; IDF does not pull a matrix-routed RX pad.
  gpio_pullup_en(static_cast<gpio_num_t>(g_rx));
  set_rx_timeout(g_baud.load(), g_parity.load());
  if (xTaskCreate(&bridge_task, "mbridge", kStack, nullptr, kPrio, &g_task) != pdPASS) {
    ESP_LOGE(TAG, "xTaskCreate failed");
    g_task = nullptr;
    return;
  }
  ESP_LOGI(TAG, "UART%d tx=%d rx=%d de=%d %lu %c81 <-> tcp :%d",
           CONFIG_NBP_MB_UART_NUM, g_tx, g_rx, g_de,
           static_cast<unsigned long>(g_baud.load()), "NEO"[g_parity.load()],
           CONFIG_NBP_MB_TCP_PORT);
}

void register_endpoints(void *httpd_handle) {
  if (httpd_handle == nullptr) return;
  auto srv = static_cast<httpd_handle_t>(httpd_handle);
  httpd_uri_t u = {};
  u.uri = "/modbus";
  u.method = HTTP_GET;
  u.handler = &modbus_get;
  httpd_register_uri_handler(srv, &u);
  u.method = HTTP_POST;
  u.handler = &modbus_post;
  httpd_register_uri_handler(srv, &u);
}

int append_stats_json(char *buf, size_t cap) {
  return std::snprintf(
      buf, cap,
      ",\"mb_client\":%d,\"mb_tx_frames\":%lu,\"mb_rx_frames\":%lu,"
      "\"mb_tx_bytes\":%lu,\"mb_rx_bytes\":%lu,\"mb_timeouts\":%lu,"
      "\"mb_refused\":%lu,\"mb_bad_req\":%lu",
      g_client_ip.load() ? 1 : 0, static_cast<unsigned long>(g_tx_frames.load()),
      static_cast<unsigned long>(g_rx_frames.load()),
      static_cast<unsigned long>(g_tx_bytes.load()),
      static_cast<unsigned long>(g_rx_bytes.load()),
      static_cast<unsigned long>(g_timeouts.load()),
      static_cast<unsigned long>(g_refused.load()),
      static_cast<unsigned long>(g_bad_req.load()));
}

}  // namespace modbus_bridge
