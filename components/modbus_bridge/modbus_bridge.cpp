#include "modbus_bridge.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "http_confirm.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"

namespace modbus_bridge {
namespace {

constexpr const char *TAG = "mbridge";
constexpr const char *NVS_NS = "mbridge";
constexpr uart_port_t kPort = static_cast<uart_port_t>(CONFIG_NBP_MB_UART_NUM);
constexpr size_t kBufLen = 512;      // max RTU ADU is 256 B
constexpr int kUartRxRing = 512;     // driver ring, must exceed the 128 B FIFO
constexpr int kUartQueueLen = 8;
constexpr uint32_t kStack = 3072;
constexpr UBaseType_t kPrio = 3;
constexpr UBaseType_t kTxPrio = 19;  // above tcpip (18) while the bus is ours
constexpr int kGatherMs = 3;         // join a request split over TCP segments
constexpr int kFrameStallMs = 100;   // partial frame, no line-idle seen

std::atomic<uint32_t> g_baud{CONFIG_NBP_MB_BAUD};
std::atomic<uint8_t> g_parity{CONFIG_NBP_MB_PARITY};  // 0 N, 1 E, 2 O
std::atomic<uint32_t> g_allow{0};  // network order, 0 = any STA-subnet peer
std::atomic<uint32_t> g_line_gen{0};
int g_tx = CONFIG_NBP_MB_TX_GPIO, g_rx = CONFIG_NBP_MB_RX_GPIO,
    g_de = CONFIG_NBP_MB_DE_GPIO;
// NVS pins: -1 = Kconfig; de -2 = no direction pin.
int g_nvs_tx = -1, g_nvs_rx = -1, g_nvs_de = -1;

std::atomic<uint32_t> g_client_ip{0};
std::atomic<uint32_t> g_tx_frames{0}, g_rx_frames{0}, g_tx_bytes{0},
    g_rx_bytes{0}, g_timeouts{0}, g_refused{0};
TaskHandle_t g_task = nullptr;
QueueHandle_t g_uq = nullptr;
uint8_t g_buf[kBufLen];
constexpr char kParity[] = "neo";

bool pin_ok(int p) {
  if (p < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(p)) return false;
  if (p >= 26 && p <= 37) return false;  // flash + octal PSRAM
  if (p == 0 || p == 3 || p == 45 || p == 46) return false;  // straps
  if (p == 19 || p == 20 || p == 43 || p == 44) return false;  // USB, console
  return true;
}

uart_parity_t parity_of(uint8_t p) {
  return p == 1 ? UART_PARITY_EVEN : p == 2 ? UART_PARITY_ODD
                                            : UART_PARITY_DISABLE;
}

// Line idle = Modbus t3.5: 3.5 chars, fixed 1.75 ms above 19200 baud.
uint8_t idle_chars(uint32_t baud, uint8_t parity) {
  uint32_t char_us = (parity ? 11000000u : 10000000u) / baud;
  uint32_t n = (1750 + char_us - 1) / char_us;
  if (n < 4) n = 4;
  if (n > 100) n = 100;
  return static_cast<uint8_t>(n);
}

void apply_line() {
  uint32_t b = g_baud.load();
  uint8_t p = g_parity.load();
  uart_set_baudrate(kPort, b);
  uart_set_parity(kPort, parity_of(p));
  uart_set_rx_timeout(kPort, idle_chars(b, p));
  uart_flush_input(kPort);
  xQueueReset(g_uq);
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
  if (pin_ok(tx) && pin_ok(rx) && (de < 0 || pin_ok(de)) && tx != rx &&
      tx != de && rx != de) {
    g_tx = tx;
    g_rx = rx;
    g_de = de;
  } else {
    ESP_LOGW(TAG, "NVS pins tx=%d rx=%d de=%d invalid, using Kconfig", tx, rx, de);
  }
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
    size_t room = kBufLen - len;
    size_t want = ev.size < room ? ev.size : room;
    int r = uart_read_bytes(kPort, g_buf + len, want, 0);
    if (r > 0) len += r;
    if (ev.size > want) {
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

void transmit(const uint8_t *p, size_t n) {
  uart_flush_input(kPort);
  xQueueReset(g_uq);
  uint32_t ms = n * 11 * 1000 / g_baud.load() + 50;
  vTaskPrioritySet(nullptr, kTxPrio);
  if (g_de >= 0) gpio_set_level(static_cast<gpio_num_t>(g_de), 1);
  uart_write_bytes(kPort, p, n);
  esp_err_t e = uart_wait_tx_done(kPort, pdMS_TO_TICKS(ms));
  if (g_de >= 0) gpio_set_level(static_cast<gpio_num_t>(g_de), 0);
  vTaskPrioritySet(nullptr, kPrio);
  // Drop what RX saw meanwhile (floating RO or an auto-direction echo).
  uart_flush_input(kPort);
  xQueueReset(g_uq);
  if (e != ESP_OK) ESP_LOGW(TAG, "tx_done timeout (%u B)", static_cast<unsigned>(n));
  g_tx_frames++;
  g_tx_bytes += n;
}

void handle_request(int &cfd) {
  int n = recv(cfd, g_buf, kBufLen, 0);
  if (n <= 0) {
    ESP_LOGI(TAG, "client closed");
    drop_client(cfd);
    return;
  }
  while (static_cast<size_t>(n) < kBufLen) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(cfd, &rf);
    timeval tv = {0, kGatherMs * 1000};
    if (select(cfd + 1, &rf, nullptr, nullptr, &tv) <= 0) break;
    int m = recv(cfd, g_buf + n, kBufLen - n, 0);
    if (m <= 0) {
      drop_client(cfd);
      return;
    }
    n += m;
  }
  transmit(g_buf, n);
  if (pump_uart(cfd, pdMS_TO_TICKS(CONFIG_NBP_MB_REPLY_TIMEOUT_MS)) == 0)
    g_timeouts++;
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
  if (getsockname(fd, reinterpret_cast<sockaddr *>(&local), &ll) != 0 ||
      !peer_ok(local.sin_addr.s_addr, peer.sin_addr.s_addr)) {
    g_refused++;
    linger lg = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(fd);
    ESP_LOGW(TAG, "refused %s", ip);
    return;
  }
  if (cfd >= 0) ESP_LOGI(TAG, "client replaced");
  drop_client(cfd);
  cfd = fd;
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
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
  char buf[288];
  int n = snprintf(
      buf, sizeof(buf),
      "{\"uart\":%d,\"tcp_port\":%d,\"baud\":%lu,\"parity\":\"%c\","
      "\"tx\":%d,\"rx\":%d,\"de\":%d,\"nvs_tx\":%d,\"nvs_rx\":%d,"
      "\"nvs_de\":%d,\"allow\":\"%s\",\"client\":\"%s\","
      "\"reply_timeout_ms\":%d,\"stack_free\":%u}",
      CONFIG_NBP_MB_UART_NUM, CONFIG_NBP_MB_TCP_PORT,
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
  if (baud > 0) {
    g_baud = static_cast<uint32_t>(baud);
    nvs_set_u32(h, "baud", g_baud);
  }
  if (parity >= 0) {
    g_parity = static_cast<uint8_t>(parity);
    nvs_set_u8(h, "parity", g_parity);
  }
  if (set_allow) {
    g_allow = allow;
    nvs_set_u32(h, "allow", allow);
  }
  if (set_pins) {
    g_nvs_tx = pins[0];
    g_nvs_rx = pins[1];
    g_nvs_de = pins[2];
    for (int i = 0; i < 3; i++) nvs_set_i8(h, kPinKeys[i], static_cast<int8_t>(pins[i]));
  }
  esp_err_t e = nvs_commit(h);
  nvs_close(h);
  if (baud > 0 || parity >= 0) g_line_gen++;
  if (e != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs commit");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, set_pins ? "{\"ok\":true,\"reboot\":true}"
                                          : "{\"ok\":true}");
}

}  // namespace

void start() {
  if (g_task != nullptr) return;
  nvs_load();
  if (g_de >= 0) {
    gpio_num_t de = static_cast<gpio_num_t>(g_de);
    gpio_set_level(de, 0);
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << g_de;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(de, 0);
  }

  uart_config_t uc = {};
  uc.baud_rate = static_cast<int>(g_baud.load());
  uc.data_bits = UART_DATA_8_BITS;
  uc.parity = parity_of(g_parity.load());
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_DEFAULT;
  esp_err_t e = uart_driver_install(kPort, kUartRxRing, 0, kUartQueueLen, &g_uq, 0);
  if (e == ESP_OK) e = uart_param_config(kPort, &uc);
  if (e == ESP_OK)
    e = uart_set_pin(kPort, g_tx, g_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "UART%d setup failed: %s", CONFIG_NBP_MB_UART_NUM, esp_err_to_name(e));
    return;
  }
  // RO tri-states while transmitting; IDF does not pull a matrix-routed RX pad.
  gpio_pullup_en(static_cast<gpio_num_t>(g_rx));
  uart_set_rx_timeout(kPort, idle_chars(g_baud.load(), g_parity.load()));
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
      "\"mb_refused\":%lu",
      g_client_ip.load() ? 1 : 0, static_cast<unsigned long>(g_tx_frames.load()),
      static_cast<unsigned long>(g_rx_frames.load()),
      static_cast<unsigned long>(g_tx_bytes.load()),
      static_cast<unsigned long>(g_rx_bytes.load()),
      static_cast<unsigned long>(g_timeouts.load()),
      static_cast<unsigned long>(g_refused.load()));
}

}  // namespace modbus_bridge
