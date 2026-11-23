#include "ota_sig.h"

#include "sdkconfig.h"

#if CONFIG_NBP_OTA

#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "esp_secure_boot.h"
#include "psa/crypto.h"

#include <cstdio>
#include <cstring>

namespace ota::sig {

namespace {

constexpr const char *TAG = "ota_sig";
constexpr uint32_t SECTOR = 4096;

// Secure Boot V2 RSA signature block (ets_secure_boot_sig_block_t in the ROM
// header). Offsets are written out so the classification also works in a
// build where the IDF check is compiled out (the known-bad calibration).
constexpr size_t BLK_SIZE = 1216;
constexpr size_t BLK_CRC_LEN = 1196;  // CRC_SIGN_BLOCK_LEN
constexpr size_t BLK_KEY_OFF = 36;    // magic, version, 2 reserved, digest[32]
constexpr size_t BLK_KEY_LEN = 776;   // n[384], e, rinv[384], mdash
constexpr uint8_t BLK_MAGIC = 0xE7;   // ETS_SECURE_BOOT_V2_SIGNATURE_MAGIC
constexpr uint8_t BLK_VER_RSA = 0x02;

enum class Verdict {
  NotAnImage,    // no/garbled image header or segment table
  Unsigned,      // no valid signature block where the image ends
  NoTrustedKey,  // the running image is unsigned: IDF refuses every image
  UntrustedKey,  // signed, by a key other than the running image's
  BadSignature,  // trusted key, image hash fine, RSA-PSS does not verify
  Corrupt,       // trusted key, but the appended SHA-256 does not match
  Other,         // esp_ota_end failed although the image looks fine
};

const char *verdict_name(Verdict v) {
  switch (v) {
    case Verdict::NotAnImage: return "not_an_image";
    case Verdict::Unsigned: return "unsigned";
    case Verdict::NoTrustedKey: return "no_trusted_key";
    case Verdict::UntrustedKey: return "untrusted_key";
    case Verdict::BadSignature: return "bad_signature";
    case Verdict::Corrupt: return "corrupt";
    case Verdict::Other: return "invalid";
  }
  return "?";
}

bool g_have_key = false;
uint8_t g_key[32];
uint32_t g_rejects = 0;
Verdict g_last = Verdict::Other;
char g_last_key[17] = "";  // first 8 bytes of the rejected image's key, hex

void hex(const uint8_t *b, size_t n, char *out) {
  static const char d[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = d[b[i] >> 4];
    out[2 * i + 1] = d[b[i] & 15];
  }
  out[2 * n] = '\0';
}

bool sha256(const uint8_t *in, size_t n, uint8_t out[32]) {
  size_t olen = 0;
  return psa_hash_compute(PSA_ALG_SHA_256, in, n, out, 32, &olen) ==
             PSA_SUCCESS &&
         olen == 32;
}

// Image length as esp_image_format.c computes it (header, segments, checksum
// padded to 16, appended SHA-256). False if the header/segments are bogus or
// run past `limit`.
bool image_len(const esp_partition_t *p, size_t limit, uint32_t *len,
               bool *hash_appended) {
  esp_image_header_t h;
  if (limit < sizeof(h) || esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK)
    return false;
  if (h.magic != ESP_IMAGE_HEADER_MAGIC || h.segment_count == 0 ||
      h.segment_count > ESP_IMAGE_MAX_SEGMENTS)
    return false;
  uint32_t pos = sizeof(h);
  for (int i = 0; i < h.segment_count; ++i) {
    esp_image_segment_header_t s;
    if (pos + sizeof(s) > limit ||
        esp_partition_read(p, pos, &s, sizeof(s)) != ESP_OK)
      return false;
    if (s.data_len > p->size) return false;
    pos += sizeof(s) + s.data_len;
    if (pos > limit) return false;
  }
  pos = (pos + 1 + 15) & ~15u;
  if (h.hash_appended) pos += 32;
  if (pos > limit) return false;
  *len = pos;
  *hash_appended = h.hash_appended != 0;
  return true;
}

// Reads signature block 0 of the image in `p`; true + key digest if valid.
bool block0_key(const esp_partition_t *p, uint32_t sig_off, size_t limit,
                uint8_t *blk /* BLK_SIZE */, uint8_t key[32]) {
  if (sig_off + BLK_SIZE > limit) return false;
  if (esp_partition_read(p, sig_off, blk, BLK_SIZE) != ESP_OK) return false;
  uint32_t crc;
  std::memcpy(&crc, blk + BLK_CRC_LEN, sizeof(crc));
  if (blk[0] != BLK_MAGIC || blk[1] != BLK_VER_RSA ||
      esp_rom_crc32_le(0, blk, BLK_CRC_LEN) != crc)
    return false;
  return sha256(blk + BLK_KEY_OFF, BLK_KEY_LEN, key);
}

// sha256 over [0, n) of the partition, streamed through `buf`.
bool flash_sha256(const esp_partition_t *p, uint32_t n, uint8_t *buf,
                  size_t buf_len, uint8_t out[32]) {
  psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
  if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
  for (uint32_t off = 0; off < n;) {
    size_t c = n - off < buf_len ? n - off : buf_len;
    if (esp_partition_read(p, off, buf, c) != ESP_OK ||
        psa_hash_update(&op, buf, c) != PSA_SUCCESS) {
      psa_hash_abort(&op);
      return false;
    }
    off += c;
  }
  size_t olen = 0;
  return psa_hash_finish(&op, out, 32, &olen) == PSA_SUCCESS && olen == 32;
}

Verdict classify(const esp_partition_t *p, size_t written, uint8_t *scratch,
                 size_t scratch_len, uint8_t key_out[32], bool *key_valid) {
  *key_valid = false;
  uint32_t len = 0;
  bool hashed = false;
  if (!image_len(p, written, &len, &hashed)) return Verdict::NotAnImage;
  const uint32_t sig_off = (len + SECTOR - 1) & ~(SECTOR - 1);
  if (scratch_len < BLK_SIZE ||
      !block0_key(p, sig_off, written, scratch, key_out))
    return Verdict::Unsigned;
  *key_valid = true;
  if (!g_have_key) return Verdict::NoTrustedKey;
  if (std::memcmp(key_out, g_key, 32) != 0) return Verdict::UntrustedKey;
  if (hashed) {
    uint8_t calc[32], stored[32];
    if (!flash_sha256(p, len - 32, scratch, scratch_len, calc) ||
        esp_partition_read(p, len - 32, stored, 32) != ESP_OK)
      return Verdict::Other;
    if (std::memcmp(calc, stored, 32) != 0) return Verdict::Corrupt;
  }
  return Verdict::BadSignature;
}

}  // namespace

bool check_active() {
#if CONFIG_SECURE_SIGNED_ON_UPDATE
  return true;
#else
  return false;
#endif
}

bool running_key(uint8_t out[32]) {
  init();
  if (g_have_key) std::memcpy(out, g_key, 32);
  return g_have_key;
}

namespace {
bool g_inited = false;
}

void init() {
  // Runs on the httpd task (8 KiB stack): reading the running image's
  // signature block needs ~2 KiB of stack (a 1216 B block on IDF's frame
  // alone), which overflowed the 3.5 KiB main task into the heap in QEMU.
  if (g_inited) return;
  g_inited = true;
  psa_crypto_init();
#if CONFIG_SECURE_SIGNED_ON_UPDATE && \
    (CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME || CONFIG_SECURE_SIGNED_APPS_ECDSA_V2_SCHEME)
  // Exactly what esp_ota_end() will trust (secure_boot_signatures_app.c,
  // get_secure_boot_key_digests → running app, compared at index 0 only).
  esp_image_sig_public_key_digests_t d = {};
  if (esp_secure_boot_get_signature_blocks_for_running_app(true, &d) ==
          ESP_OK &&
      d.num_digests > 0) {
    std::memcpy(g_key, d.key_digests[0], 32);
    g_have_key = true;
  }
#else
  // Check compiled out: report what the running image is signed with anyway.
  const esp_partition_t *run = esp_ota_get_running_partition();
  static uint8_t blk[BLK_SIZE];
  uint32_t len = 0;
  bool hashed = false;
  if (run && image_len(run, run->size, &len, &hashed))
    g_have_key = block0_key(run, (len + SECTOR - 1) & ~(SECTOR - 1),
                            run->size, blk, g_key);
#endif
  char k[65] = "";
  if (g_have_key) hex(g_key, 32, k);
  if (!check_active()) {
    ESP_LOGW(TAG, "app signature check OFF in this build; running image key %s",
             g_have_key ? k : "(unsigned)");
  } else if (g_have_key) {
    ESP_LOGI(TAG, "OTA accepts images signed by key sha256 %s", k);
  } else {
    ESP_LOGE(TAG,
             "running image is UNSIGNED: no trusted key, every OTA will be "
             "refused. Flash a signed build over USB (docs/signing.md)");
  }
}

esp_err_t reject(httpd_req_t *req, const esp_partition_t *target,
                 size_t written, esp_err_t end_err, uint8_t *scratch,
                 size_t scratch_len) {
  init();
  uint8_t key[32];
  bool key_valid = false;
  const Verdict v =
      classify(target, written, scratch, scratch_len, key, &key_valid);

  // The bootloader does not check signatures: make sure a fallback scan can
  // never boot the refused image. (esp_ota_begin already erased the old
  // contents of this slot, so nothing worth keeping is lost.)
  const esp_err_t er = esp_partition_erase_range(target, 0, SECTOR);

  ++g_rejects;
  g_last = v;
  char img_key[17] = "-";
  if (key_valid) hex(key, 8, img_key);
  std::snprintf(g_last_key, sizeof(g_last_key), "%s", key_valid ? img_key : "");
  char trusted[17] = "none";
  if (g_have_key) hex(g_key, 8, trusted);

  const bool sig = v == Verdict::Unsigned || v == Verdict::NoTrustedKey ||
                   v == Verdict::UntrustedKey || v == Verdict::BadSignature;
  ESP_LOGE(TAG,
           "OTA REFUSED: %s (esp_ota_end: %s; image key %s, trusted key %s, "
           "%u bytes); boot partition unchanged, %s header %s",
           verdict_name(v), esp_err_to_name(end_err), img_key, trusted,
           static_cast<unsigned>(written), target->label,
           er == ESP_OK ? "erased" : esp_err_to_name(er));

  char msg[240];
  switch (v) {
    case Verdict::Unsigned:
      std::snprintf(msg, sizeof(msg),
                    "refused: image is not signed (device trusts key %s…). "
                    "Upload the signed .bin\n",
                    trusted);
      break;
    case Verdict::NoTrustedKey:
      std::snprintf(msg, sizeof(msg),
                    "refused: this device runs an unsigned image and has no "
                    "trusted key, so it refuses every OTA. Flash a signed "
                    "build over USB\n");
      break;
    case Verdict::UntrustedKey:
      std::snprintf(msg, sizeof(msg),
                    "refused: image is signed by key %s…, device trusts key "
                    "%s… (dev vs release key?). Flash over USB to switch "
                    "keys\n",
                    img_key, trusted);
      break;
    case Verdict::BadSignature:
      std::snprintf(msg, sizeof(msg),
                    "refused: signature does not verify for key %s… (image "
                    "modified after signing)\n",
                    trusted);
      break;
    case Verdict::Corrupt:
      std::snprintf(msg, sizeof(msg),
                    "refused: image hash mismatch (corrupt upload?)\n");
      break;
    case Verdict::NotAnImage:
      std::snprintf(msg, sizeof(msg), "refused: not an app image\n");
      break;
    case Verdict::Other:
      std::snprintf(msg, sizeof(msg), "refused: image failed validation (%s)\n",
                    esp_err_to_name(end_err));
      break;
  }
  httpd_resp_set_status(req, sig ? "403 Forbidden" : "400 Bad Request");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "X-OTA-Refused", verdict_name(v));
  httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
  // ESP_OK: the body was consumed and the response is complete. ESP_FAIL
  // makes httpd close the socket, abortively under http_guard (SO_LINGER 0),
  // and with ESP_FAIL the QEMU self-test's client got the status line but
  // neither the X-OTA-Refused header nor the body.
  return ESP_OK;
}

esp_err_t status_get(httpd_req_t *req) {
  init();
  char k[65] = "";
  if (g_have_key) hex(g_key, 32, k);
  const esp_partition_t *run = esp_ota_get_running_partition();
  char body[320];
  std::snprintf(body, sizeof(body),
                "{\"sig_check\":%s,\"scheme\":\"%s\",\"running\":\"%s\","
                "\"running_signed\":%s,\"key_sha256\":\"%s\",\"rejects\":%lu,"
                "\"last_reject\":\"%s\",\"last_reject_key\":\"%s\"}",
                check_active() ? "true" : "false",
                check_active() ? "rsa3072-pss" : "none", run ? run->label : "",
                g_have_key ? "true" : "false", k,
                static_cast<unsigned long>(g_rejects),
                g_rejects ? verdict_name(g_last) : "", g_last_key);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

}  // namespace ota::sig

#endif  // CONFIG_NBP_OTA
