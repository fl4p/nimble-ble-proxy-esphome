// Browser bridge: a WebSocket endpoint on the shared dashboard httpd that
// tunnels the plaintext aioesphomeapi protocol. A browser opens a binary
// WebSocket to ws://<host>/api; each WS message carries raw ESPHome
// protocol bytes. The bridge relays those bytes to/from the existing TCP
// api_server over an on-device loopback connection (127.0.0.1:API_PORT),
// so api_server treats the browser exactly like any other HA client — its
// handshake, Bluetooth* handlers, send_async broadcast and fd-reaping are
// all untouched.
//
// Compiled in only when CONFIG_NBP_WS_PROXY=y (which also selects
// HTTPD_WS_SUPPORT so esp_http_server links its WebSocket layer).

#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_NBP_WS_PROXY

namespace ws_proxy {

// Register the GET /api WebSocket route on the shared dashboard httpd
// (pass ota::handle()). No-op if the handle is null. Call once after the
// httpd is up; api_server need not be listening yet — the loopback
// connection is opened lazily on the first browser WebSocket.
void register_endpoint(void *httpd_handle);

}  // namespace ws_proxy

#endif  // CONFIG_NBP_WS_PROXY
