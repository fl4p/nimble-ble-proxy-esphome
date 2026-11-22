// App image signature reporting for the OTA endpoint.
//
// The check itself is ESP-IDF's: with CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
// esp_ota_end() verifies the new image's Secure Boot V2 signature block 0
// against the key in block 0 of the *running* image (no eFuse, no compiled-in
// key). This file only explains a failed esp_ota_end() to the uploader,
// scrubs the rejected image so nothing can boot it, and reports which key the
// device trusts (GET /update). It never turns a failure into a success.
// See "App image signing" in CLAUDE.md and docs/signing.md.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_partition.h"

namespace ota::sig {

// Read the running image's trusted key once and log it. Idempotent; needs
// ~2 KiB of stack, so call it on the httpd task (ota::start queues it there),
// not on the 3.5 KiB main task. Every other function here calls it first.
void init();

// True when esp_ota_end() checks signatures in this build.
bool check_active();

// True when the running image carries a valid signature block 0; *out gets the
// sha256 of its public key (the same value `nbp_sign.py fingerprint` prints).
bool running_key(uint8_t out[32]);

// esp_ota_end() failed on `target` after `written` bytes. Classifies the
// failure, logs it, erases the image header in `target` (so a bootloader that
// does not check signatures cannot fall back to it) and sends 403 (signature)
// or 400 (malformed/corrupt image). `scratch` is a >= 4 KiB buffer.
// Returns ESP_OK (response sent, connection kept) so the 4xx text reaches
// the client; see the comment at the return.
esp_err_t reject(httpd_req_t *req, const esp_partition_t *target,
                 size_t written, esp_err_t end_err, uint8_t *scratch,
                 size_t scratch_len);

// GET /update: {"sig_check":…,"key_sha256":…,"rejects":…}.
esp_err_t status_get(httpd_req_t *req);

}  // namespace ota::sig
