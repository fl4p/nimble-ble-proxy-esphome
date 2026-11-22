// QEMU-only self-test of the OTA signature check (never in firmware: compiled
// only with -DNBP_SIG_SELFTEST=1, and it takes over app_main):
//
//   idf.py -B build-qemu-sig -DIDF_TARGET=esp32s3
//     -DSDKCONFIG=build-qemu-sig/sdkconfig -DNBP_SIG_SELFTEST=1 build
//   idf.py -B build-qemu-sig -DSDKCONFIG=build-qemu-sig/sdkconfig qemu
//
// The build signs the app with the dev key (root CMakeLists) and writes test
// vectors (scripts/nbp_sign.py selftest-blob) into the last 8 KiB of the
// coredump partition: a signature sector over the same image by a throwaway
// key, and a patch list that flips one byte of the app description and fixes
// the image checksum and SHA-256 (only the RSA signature can catch it).
//
// Boot 1 starts the real httpd (ota::start) on lwIP loopback and POSTs to
// /update: unsigned, wrong key, tampered (each must get 403, leave the boot
// partition alone and have the slot's header erased), then the signed image
// (200, boot partition switched). It reboots; boot 2 checks it runs from the
// new slot. Prints `SIG-SELFTEST <step>: PASS|FAIL`.
//
// Known-bad calibration: add sdkconfig.defaults.sigtest-nocheck (turns
// CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT off); the three refusal steps
// must FAIL.

#include "sdkconfig.h"

#if NBP_SIG_SELFTEST

#include "ota.h"
#include "ota_sig.h"

#include "esp_attr.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef NBP_SIG_SELFTEST_KEY
#error "NBP_SIG_SELFTEST_KEY (expected key sha256) comes from the root CMakeLists"
#endif

namespace ota::sig_selftest {

namespace {

constexpr uint32_t kMagic = 0x5349474eu;  // "NGIS"
struct State {
  uint32_t magic;
  uint32_t phase;
  uint32_t target_addr;
  int failures;
};
RTC_NOINIT_ATTR State g_state;

int g_fail = 0;
uint8_t g_buf[4096];

void result(const char *name, bool ok, const char *fmt = "", ...)
    __attribute__((format(printf, 3, 4)));
void result(const char *name, bool ok, const char *fmt, ...) {
  char detail[200];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);
  if (!ok) ++g_fail;
  std::printf("SIG-SELFTEST %s: %s %s\n", name, ok ? "PASS" : "FAIL", detail);
}

[[noreturn]] void halt() {
  while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

struct Patch {
  uint32_t off, len;
  uint8_t data[32];
};
struct Blob {
  uint32_t signed_len = 0;
  int n_patches = 0;
  Patch patches[4];
  const esp_partition_t *part = nullptr;
  uint32_t base = 0;  // offset of the blob in `part`
};

bool load_blob(Blob *b) {
  b->part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                     ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                     nullptr);
  if (!b->part) return false;
  b->base = b->part->size - 8192;
  uint8_t h[256];
  if (esp_partition_read(b->part, b->base, h, sizeof(h)) != ESP_OK) return false;
  if (std::memcmp(h, "NBPSIGT1", 8) != 0) return false;
  std::memcpy(&b->signed_len, h + 8, 4);
  uint32_t n;
  std::memcpy(&n, h + 12, 4);
  if (n > 4) return false;
  size_t pos = 16;
  for (uint32_t i = 0; i < n; ++i) {
    Patch &p = b->patches[i];
    std::memcpy(&p.off, h + pos, 4);
    std::memcpy(&p.len, h + pos + 4, 4);
    if (p.len > 32) return false;
    std::memcpy(p.data, h + pos + 8, p.len);
    pos += 8 + ((p.len + 3) & ~3u);
  }
  b->n_patches = static_cast<int>(n);
  return true;
}

int connect_httpd() {
  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return -1;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(80);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0) {
    ::close(fd);
    return -1;
  }
  timeval tv{.tv_sec = 60, .tv_usec = 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

bool send_all(int fd, const void *p, size_t n) {
  auto *c = static_cast<const uint8_t *>(p);
  while (n) {
    ssize_t r = ::send(fd, c, n, 0);
    if (r <= 0) return false;
    c += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

struct Resp {
  int status = 0;
  char refused[32] = "";
  char body[400] = "";
};

void read_resp(int fd, Resp *r) {
  static char buf[1024];
  size_t n = 0;
  // Until the server closes, or the body announced by Content-Length is in
  // (a refused upload keeps the connection open).
  while (n < sizeof(buf) - 1) {
    ssize_t k = ::recv(fd, buf + n, sizeof(buf) - 1 - n, 0);
    if (k <= 0) break;
    n += static_cast<size_t>(k);
    buf[n] = '\0';
    const char *end = std::strstr(buf, "\r\n\r\n");
    const char *cl = std::strstr(buf, "Content-Length: ");
    if (end && cl && cl < end &&
        n >= static_cast<size_t>(end + 4 - buf) +
                 static_cast<size_t>(std::atoi(cl + 16)))
      break;
  }
  buf[n] = '\0';
  if (std::strncmp(buf, "HTTP/1.1 ", 9) == 0) r->status = std::atoi(buf + 9);
  if (const char *h = std::strstr(buf, "X-OTA-Refused: ")) {
    h += 15;
    size_t i = 0;
    while (h[i] && h[i] != '\r' && i < sizeof(r->refused) - 1) {
      r->refused[i] = h[i];
      ++i;
    }
    r->refused[i] = '\0';
  }
  if (const char *b = std::strstr(buf, "\r\n\r\n"))
    std::snprintf(r->body, sizeof(r->body), "%s", b + 4);
}

// Source of the uploaded bytes: the running (signed) image, optionally with
// a different signature sector and/or byte patches.
enum class Sig { None, Own, Wrong };

bool post_image(const Blob &b, Sig sig, bool tamper, Resp *r) {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const uint32_t len = b.signed_len + (sig == Sig::None ? 0 : 4096);
  int fd = connect_httpd();
  if (fd < 0) return false;
  char head[200];
  int hn = std::snprintf(head, sizeof(head),
                         "POST /update HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                         "X-NBP-Confirm: yes\r\nContent-Length: %lu\r\n\r\n",
                         static_cast<unsigned long>(len));
  bool ok = send_all(fd, head, hn);
  for (uint32_t off = 0; ok && off < len; off += sizeof(g_buf)) {
    const uint32_t c = len - off < sizeof(g_buf) ? len - off : sizeof(g_buf);
    esp_err_t e;
    if (sig == Sig::Wrong && off >= b.signed_len) {
      e = esp_partition_read(b.part, b.base + 4096 + (off - b.signed_len),
                             g_buf, c);
    } else {
      e = esp_partition_read(run, off, g_buf, c);
    }
    if (e != ESP_OK) ok = false;
    if (tamper) {
      for (int i = 0; i < b.n_patches; ++i) {
        const Patch &p = b.patches[i];
        for (uint32_t j = 0; j < p.len; ++j) {
          const uint32_t a = p.off + j;
          if (a >= off && a < off + c) g_buf[a - off] = p.data[j];
        }
      }
    }
    ok = ok && send_all(fd, g_buf, c);
  }
  if (ok) read_resp(fd, r);
  ::close(fd);
  return ok;
}

bool get_status(Resp *r) {
  int fd = connect_httpd();
  if (fd < 0) return false;
  const char req[] = "GET /update HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  bool ok = send_all(fd, req, sizeof(req) - 1);
  if (ok) read_resp(fd, r);
  ::close(fd);
  return ok;
}

bool header_erased(const esp_partition_t *p) {
  uint32_t w = 0;
  return esp_partition_read(p, 0, &w, sizeof(w)) == ESP_OK && w == 0xFFFFFFFFu;
}

void refusal(const char *name, const Blob &b, Sig sig, bool tamper,
             const char *want) {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t *boot_before = esp_ota_get_boot_partition();
  Resp r;
  const bool sent = post_image(b, sig, tamper, &r);
  const esp_partition_t *boot_after = esp_ota_get_boot_partition();
  const bool ok = sent && r.status == 403 && std::strcmp(r.refused, want) == 0 &&
                  boot_after == boot_before && boot_after == run &&
                  header_erased(target);
  // Strip the trailing newline for the log.
  size_t bl = std::strlen(r.body);
  if (bl && r.body[bl - 1] == '\n') r.body[bl - 1] = '\0';
  result(name, ok, "status=%d refused=%s (want 403 %s) boot=%s header_erased=%d | %s",
         r.status, r.refused, want, boot_after ? boot_after->label : "?",
         header_erased(target), r.body);
}

void body(void *);

}  // namespace

// The main task's 3.5 KiB stack is too small for the socket client below.
void run() {
  xTaskCreate(body, "sigtest", 8192, nullptr, 5, nullptr);
  halt();
}

namespace {

void body(void *) {
  if (g_state.magic == kMagic && g_state.phase == 1) {
    // Boot 2: the accepted image must be the one running now.
    const esp_partition_t *run = esp_ota_get_running_partition();
    g_fail = g_state.failures;
    result("good_boots", run && run->address == g_state.target_addr,
           "running %s @0x%lx (want 0x%lx)", run ? run->label : "?",
           run ? static_cast<unsigned long>(run->address) : 0ul,
           static_cast<unsigned long>(g_state.target_addr));
    // The new image trusts the same key (read on the httpd task, ota_sig.h).
    ESP_ERROR_CHECK(esp_netif_init());
    ota::start();
    vTaskDelay(pdMS_TO_TICKS(300));
    Resp st;
    get_status(&st);
    char want_key[100];
    std::snprintf(want_key, sizeof(want_key), "\"key_sha256\":\"%s\"",
                  NBP_SIG_SELFTEST_KEY);
    result("good_still_trusts_key", std::strstr(st.body, want_key) != nullptr,
           "%s", st.body);
    g_state.magic = 0;
    std::printf("SIG-SELFTEST summary: %d failure(s)\n", g_fail);
    halt();
  }

  ESP_ERROR_CHECK(esp_netif_init());
  ota::start();
  vTaskDelay(pdMS_TO_TICKS(300));

  Blob b;
  if (!load_blob(&b)) {
    result("blob", false, "test vectors missing from the coredump partition");
    halt();
  }
  const esp_partition_t *run = esp_ota_get_running_partition();
  result("setup", run != nullptr, "running %s, signed_len %lu, %d patches",
         run ? run->label : "?", static_cast<unsigned long>(b.signed_len),
         b.n_patches);

  // The device reports the key it trusts; it must be the build's key.
  Resp st;
  get_status(&st);
  char want_key[100];
  std::snprintf(want_key, sizeof(want_key), "\"key_sha256\":\"%s\"",
                NBP_SIG_SELFTEST_KEY);
  result("fingerprint", st.status == 200 && std::strstr(st.body, want_key) &&
                            std::strstr(st.body, "\"sig_check\":true"),
         "%s", st.body);

  refusal("unsigned", b, Sig::None, false, "unsigned");
  refusal("wrong_key", b, Sig::Wrong, false, "untrusted_key");
  refusal("tampered", b, Sig::Own, true, "bad_signature");

  Resp st2;
  get_status(&st2);
  result("reject_counter", std::strstr(st2.body, "\"rejects\":3") != nullptr,
         "%s", st2.body);

  // The signed image: accepted, boot partition switched to the target.
  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  Resp r;
  const bool sent = post_image(b, Sig::Own, false, &r);
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  result("good_accepted", sent && r.status == 200 && boot == target,
         "status=%d boot=%s target=%s", r.status, boot ? boot->label : "?",
         target ? target->label : "?");

  std::printf("SIG-SELFTEST boot1: %d failure(s); rebooting into %s\n", g_fail,
              target ? target->label : "?");
  g_state = {kMagic, 1, target ? target->address : 0, g_fail};
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
}

}  // namespace

}  // namespace ota::sig_selftest

#endif  // NBP_SIG_SELFTEST
