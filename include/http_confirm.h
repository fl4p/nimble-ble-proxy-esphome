// CSRF guard for state-changing dashboard POSTs: they need the header
// `X-NBP-Confirm: yes`.
//
// A custom header is not CORS-safelisted, so a page on another origin can
// only send it after a preflight, and httpd answers no OPTIONS. A drive-by
// form post or no-cors fetch() from any page open on the LAN therefore
// cannot reboot the device, flash it, retune the radio or rename it. This is
// not authentication: anyone on the LAN can still send the header. It does
// not cover DNS rebinding either (the page is then same-origin).
//
// /nat, /portmap, /bond and /crash, /crash-test predate this and keep their
// own X-Nat-Confirm / X-Bond-Confirm / X-Crash-Confirm headers. The BLE
// dashboard transport (ble_httpd) carries no headers and is not gated: a web
// page reaches it only through a user-picked Web Bluetooth device.

#pragma once

#include "esp_http_server.h"

#include <cstring>

namespace nbp_http {

inline constexpr const char *kConfirmHdr = "X-NBP-Confirm";

inline bool confirmed(httpd_req_t *req) {
  char v[8];
  return httpd_req_get_hdr_value_str(req, kConfirmHdr, v, sizeof(v)) ==
             ESP_OK &&
         std::strcmp(v, "yes") == 0;
}

// Sends 400 and returns what httpd_resp_send_err returns. Use as
//   if (!nbp_http::confirmed(req)) return nbp_http::send_unconfirmed(req);
inline esp_err_t send_unconfirmed(httpd_req_t *req) {
  return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "needs header X-NBP-Confirm: yes");
}

}  // namespace nbp_http
