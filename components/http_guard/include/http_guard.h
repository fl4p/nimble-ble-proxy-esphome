// httpd under low heap (issue #2): the C3 has ~21-32 KB free in steady
// state, and an HTTP response keeps up to TCP_SND_BUF of full-MSS pbufs
// (~1.5 KiB each) until the client ACKs them. With the WiFi RX buffers
// (1.7 KB) failing next, ACKs are lost and the heap never comes back.
//
// - Every registered handler is wrapped (--wrap=httpd_register_uri_handler):
//   when it returns an error, the URI, free heap and largest block are
//   logged (httpd's own "uri handler execution failed" names no URI), and
//   SO_LINGER {1,0} makes httpd's close abortive, so unsent data does not
//   linger in FIN-WAIT retransmitting.
// - bulk_ok(): large responses refuse with 503 + Retry-After when the heap
//   is low.
// - send_chunk_paced(): a long download keeps at most kPaceSegs segments
//   queued in lwIP instead of TCP_SND_BUF worth.

#pragma once

#include <cstddef>

#include "esp_http_server.h"

namespace http_guard {

// Below either, bulk_ok() refuses.
inline constexpr size_t kBulkMinFree = 16 * 1024;
inline constexpr size_t kBulkMinLargest = 2 * 1024;
// send_chunk_paced(): queued pbufs (pcb->snd_queuelen) allowed before the
// next chunk goes out.
inline constexpr unsigned kPaceSegs = 2;

// True if the heap allows a large response now. Otherwise sends 503 with
// Retry-After: 10 and returns false (the handler then returns ESP_OK).
bool bulk_ok(httpd_req_t *req);

// httpd_resp_send_chunk after waiting until this socket's send queue holds
// at most kPaceSegs pbufs. Gives up (ESP_ERR_TIMEOUT) after the server's
// send timeout without progress.
esp_err_t send_chunk_paced(httpd_req_t *req, const char *buf, size_t len);

}  // namespace http_guard
