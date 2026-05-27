#include "mdns_announce.h"

#include "proxy_config.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"

#include <cstdio>

namespace mdns_announce {

namespace {

constexpr const char *TAG = "mdns";

// Pretty-print the WiFi STA MAC into `out` as 12 lowercase hex chars,
// no separators — matches how ESPHome publishes the `mac` TXT value.
void format_mac_lower_nosep(char *out, size_t out_size) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  std::snprintf(out, out_size, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
}

}  // namespace

void start() {
  ESP_ERROR_CHECK(mdns_init());
  ESP_ERROR_CHECK(mdns_hostname_set(proxy::hostname()));
  ESP_ERROR_CHECK(mdns_instance_name_set(proxy::FRIENDLY_NAME));

  char mac_buf[13];
  format_mac_lower_nosep(mac_buf, sizeof(mac_buf));

  // HA only checks that the TXT records *exist* with plausible values.
  // config_hash is opaque to HA but expected — use a fixed placeholder.
  mdns_txt_item_t txt[] = {
      {"friendly_name", const_cast<char *>(proxy::FRIENDLY_NAME)},
      {"version",       const_cast<char *>(proxy::FAKE_ESPHOME_VERSION)},
      {"config_hash",   const_cast<char *>("00000000000000000000000000000000")},
      {"mac",           mac_buf},
      {"platform",      const_cast<char *>("ESP32")},
      {"board",         const_cast<char *>(proxy::MODEL)},
      {"network",       const_cast<char *>("wifi")},
  };
  size_t txt_count = sizeof(txt) / sizeof(txt[0]);

  ESP_ERROR_CHECK(mdns_service_add(nullptr, "_esphomelib", "_tcp",
                                   proxy::API_PORT, txt, txt_count));
  ESP_LOGI(TAG, "_esphomelib._tcp on %s:%u announced", proxy::hostname(),
           proxy::API_PORT);
}

}  // namespace mdns_announce
