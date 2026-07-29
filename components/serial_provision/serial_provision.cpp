#include "serial_provision.h"

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#endif

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace serial_provision {
namespace {

constexpr const char *TAG = "serial_prov";
constexpr const char *PREFIX = "NBP-PROV1 ";
constexpr const char *NVS_WIFI_NS = "wifi";
constexpr const char *NVS_WIFI_SSID = "ssid";
constexpr const char *NVS_WIFI_PSK = "psk";
constexpr int BOOT_WINDOW_MS = 10000;
constexpr int POLL_MS = 50;

struct RxLine {
  char data[256] = {};
  size_t len = 0;
};

int b64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool is_hex_string(const char *s) {
  for (; *s; ++s) {
    if (!std::isxdigit(static_cast<unsigned char>(*s))) return false;
  }
  return true;
}

bool decode_b64_token(const char *in, char *out, size_t cap, size_t max_len) {
  if (std::strcmp(in, "-") == 0) {
    out[0] = '\0';
    return true;
  }

  size_t in_len = std::strlen(in);
  if (in_len == 0 || in_len % 4 != 0) return false;

  size_t out_len = 0;
  for (size_t i = 0; i < in_len; i += 4) {
    int vals[4] = {};
    int pad = 0;
    for (int j = 0; j < 4; ++j) {
      char c = in[i + j];
      if (c == '=') {
        vals[j] = 0;
        ++pad;
      } else {
        if (pad != 0) return false;
        vals[j] = b64_value(c);
        if (vals[j] < 0) return false;
      }
    }
    if (pad > 2) return false;

    uint32_t triple = (static_cast<uint32_t>(vals[0]) << 18) |
                      (static_cast<uint32_t>(vals[1]) << 12) |
                      (static_cast<uint32_t>(vals[2]) << 6) |
                      static_cast<uint32_t>(vals[3]);
    int bytes = 3 - pad;
    for (int j = 0; j < bytes; ++j) {
      if (out_len >= max_len || out_len + 1 >= cap) return false;
      char decoded = static_cast<char>((triple >> (16 - 8 * j)) & 0xff);
      if (decoded == '\0') return false;
      out[out_len++] = decoded;
    }
  }

  out[out_len] = '\0';
  return true;
}

bool store_wifi_creds(const char *ssid, const char *psk) {
  nvs_handle_t h;
  if (nvs_open(NVS_WIFI_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_str(h, NVS_WIFI_SSID, ssid);
  if (err == ESP_OK) err = nvs_set_str(h, NVS_WIFI_PSK, psk);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

bool handle_line(char *line) {
  if (std::strncmp(line, PREFIX, std::strlen(PREFIX)) != 0) return false;

  char *ssid_b64 = line + std::strlen(PREFIX);
  while (*ssid_b64 == ' ') ++ssid_b64;
  char *psk_b64 = std::strchr(ssid_b64, ' ');
  if (psk_b64 == nullptr) return false;
  *psk_b64++ = '\0';
  while (*psk_b64 == ' ') ++psk_b64;

  char *end = psk_b64 + std::strlen(psk_b64);
  while (end > psk_b64 && std::isspace(static_cast<unsigned char>(end[-1]))) {
    *--end = '\0';
  }

  char ssid[33] = {};
  char psk[65] = {};
  if (!decode_b64_token(ssid_b64, ssid, sizeof(ssid), 32) || ssid[0] == '\0') {
    ESP_LOGW(TAG, "invalid provisioning SSID");
    return false;
  }
  if (!decode_b64_token(psk_b64, psk, sizeof(psk), 64)) {
    ESP_LOGW(TAG, "invalid provisioning PSK");
    return false;
  }
  size_t psk_len = std::strlen(psk);
  if (psk_len > 0 && psk_len < 8) {
    ESP_LOGW(TAG, "invalid provisioning PSK length");
    return false;
  }
  if (psk_len == 64 && !is_hex_string(psk)) {
    ESP_LOGW(TAG, "64-byte provisioning PSK must be hex");
    return false;
  }

  if (!store_wifi_creds(ssid, psk)) {
    ESP_LOGE(TAG, "failed to store provisioned WiFi credentials");
    return false;
  }

  ESP_LOGI(TAG, "stored provisioned WiFi credentials for SSID '%s'", ssid);
  return true;
}

constexpr uart_port_t CONSOLE_UART =
    static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);

void write_uart0(const char *s) {
  uart_write_bytes(CONSOLE_UART, s, std::strlen(s));
}

int read_uart0(uint8_t *buf, size_t len, TickType_t wait) {
  return uart_read_bytes(CONSOLE_UART, buf, len, wait);
}

void init_uart0() {
  esp_err_t err = uart_driver_install(CONSOLE_UART, 512, 0, 0, nullptr, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "UART provisioning driver install failed: %s",
             esp_err_to_name(err));
  }
}

#if SOC_USB_SERIAL_JTAG_SUPPORTED
void init_usb_serial_jtag() {
  if (usb_serial_jtag_is_driver_installed()) return;
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  esp_err_t err = usb_serial_jtag_driver_install(&cfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "USB-Serial/JTAG provisioning driver install failed: %s",
             esp_err_to_name(err));
  }
}

void write_usb_serial_jtag(const char *s) {
  if (!usb_serial_jtag_is_driver_installed()) return;
  usb_serial_jtag_write_bytes(s, std::strlen(s), pdMS_TO_TICKS(50));
  usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(50));
}

int read_usb_serial_jtag(uint8_t *buf, size_t len, TickType_t wait) {
  if (!usb_serial_jtag_is_driver_installed()) return 0;
  return usb_serial_jtag_read_bytes(buf, len, wait);
}
#endif

void append_rx(RxLine *line, uint8_t byte) {
  if (byte == '\r') return;
  if (byte == '\n') {
    line->data[line->len] = '\0';
    if (handle_line(line->data)) {
      const char *ok = "NBP-PROV-OK\n";
      write_uart0(ok);
#if SOC_USB_SERIAL_JTAG_SUPPORTED
      write_usb_serial_jtag(ok);
#endif
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
    }
    line->len = 0;
    line->data[0] = '\0';
    return;
  }
  if (line->len + 1 < sizeof(line->data)) {
    line->data[line->len++] = static_cast<char>(byte);
  } else {
    line->len = 0;
    line->data[0] = '\0';
  }
}

}  // namespace

void poll_boot_window() {
  init_uart0();
#if SOC_USB_SERIAL_JTAG_SUPPORTED
  init_usb_serial_jtag();
#endif

  const int64_t deadline = esp_timer_get_time() + BOOT_WINDOW_MS * 1000LL;
  RxLine uart_line;
#if SOC_USB_SERIAL_JTAG_SUPPORTED
  RxLine usb_line;
#endif
  uint8_t buf[64];

  ESP_LOGI(TAG, "serial WiFi provisioning window open for %d ms", BOOT_WINDOW_MS);
  while (esp_timer_get_time() < deadline) {
    int n = read_uart0(buf, sizeof(buf), pdMS_TO_TICKS(POLL_MS));
    for (int i = 0; i < n; ++i) append_rx(&uart_line, buf[i]);
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    n = read_usb_serial_jtag(buf, sizeof(buf), 0);
    for (int i = 0; i < n; ++i) append_rx(&usb_line, buf[i]);
#endif
  }
  ESP_LOGI(TAG, "serial WiFi provisioning window closed");
}

}  // namespace serial_provision
