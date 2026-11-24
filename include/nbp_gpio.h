// GPIO checks shared by the opt-in peripherals (modbus bridge, env sensors).

#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_bit_defs.h"
#include "esp_private/esp_gpio_reserve.h"
#include "sdkconfig.h"
#include "soc/uart_pins.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "soc/usb_pins.h"
#endif

namespace nbp_gpio {

inline uint64_t bit(int p) { return p >= 0 && p < 64 ? BIT64(p) : 0; }

// Pins configured by one of our peripherals (set at init, one task).
inline uint64_t g_claimed = 0;

inline void claim(uint64_t m) { g_claimed |= m; }

// A free output-capable pin: not the console, not claimed by IDF (flash,
// PSRAM, a running driver) or by another of our peripherals. Pins in `own`
// (the caller's current ones) are exempt from both claims.
inline bool allowed(int p, uint64_t own = 0) {
  if (p < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(p)) return false;
  if (p == U0TXD_GPIO_NUM || p == U0RXD_GPIO_NUM) return false;
  uint64_t b = BIT64(p);
  if (!(own & b) && ((g_claimed & b) || esp_gpio_is_reserved(b))) return false;
#if CONFIG_IDF_TARGET_ESP32S3
  // Octal PSRAM 33..37 is not reserved with quad flash.
  if (p >= 26 && p <= 37) return false;
  if (p == 0 || p == 3 || p == 45 || p == 46) return false;  // straps
  if (p == USBPHY_DM_NUM || p == USBPHY_DP_NUM) return false;
#endif
  return true;
}

// Strapping and USB pins that allowed() lets through on targets other than
// the S3 (which excludes its own there). For outputs that sit behind a
// pull-up/pull-down and are held across resets, like the switch output: a
// strap held at the wrong level when the chip resets selects download mode
// (ESP32 GPIO0, C3/C6 GPIO9 low) or the wrong flash voltage (ESP32 GPIO12
// high), and the USB pins carry the console. The I2C/UART peripherals keep
// using allowed() alone (their pull-ups leave the straps at boot level).
inline bool strap_or_usb(int p) {
#if CONFIG_IDF_TARGET_ESP32
  return p == 0 || p == 2 || p == 5 || p == 12 || p == 15;
#elif CONFIG_IDF_TARGET_ESP32C3
  return p == 2 || p == 8 || p == 9 || p == 18 || p == 19;
#elif CONFIG_IDF_TARGET_ESP32C6
  return p == 8 || p == 9 || p == 15 || p == 12 || p == 13;
#else
  (void)p;
  return false;  // S3: already refused by allowed()
#endif
}

}  // namespace nbp_gpio
