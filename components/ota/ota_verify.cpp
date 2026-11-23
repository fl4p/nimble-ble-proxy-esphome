// OTA image verification (app rollback). See ota_verify.h for the contract
// and CLAUDE.md "OTA app rollback" for the IDF source it relies on.

#include "ota_verify.h"

#include "ota_verify_policy.h"
#include "http_confirm.h"

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/sockets.h"
#include "lwip/priv/tcpip_priv.h"
#include "nvs.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ota_verify {

namespace {

constexpr const char *TAG = "ota_verify";
constexpr const char *kNs = "otarb";
constexpr const char *kExpKey = "exp";  // image expected to boot next / now
constexpr const char *kRbKey = "rb";    // last rollback seen

constexpr uint32_t kExpMagic = 0x4F545845;  // "EXTO"
constexpr uint32_t kRbMagic = 0x4F544252;   // "RBTO"
constexpr uint32_t kRtcMagic = 0x4F545652;  // "RVTO"

enum Cause : uint8_t {
  kNone = 0,
  kReset = 1,      // bootloader aborted the pending image after a reset
  kDeadline = 2,   // criterion unmet at the deadline
  kStarts = 3,     // old bootloader: too many unverified starts
  kManual = 4,     // POST /ota?mark=invalid
  kBackstop = 5,   // worker stuck; esp_timer restarted the chip
  kNotBooted = 6,  // bootloader booted another slot; image never ran as pending
  kInvalid = 7,    // marked invalid, cause not recorded
};

const char *cause_name(uint8_t c) {
  switch (c) {
    case kReset: return "reset_while_pending";
    case kDeadline: return "deadline";
    case kStarts: return "unverified_starts";
    case kManual: return "manual";
    case kBackstop: return "backstop_restart";
    case kNotBooted: return "not_booted";
    case kInvalid: return "marked_invalid";
    default: return "none";
  }
}

const char *state_name(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED: return "undefined";
    default: return "unknown";
  }
}

const char *reset_name(int r) {
  switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
  }
}

// Persisted (NVS). Fixed layout; a size mismatch on read = absent.
struct Expect {
  uint32_t magic;
  uint8_t subtype;  // ESP_PARTITION_SUBTYPE_APP_OTA_x of the expected image
  uint8_t cause;    // set right before an app-initiated rollback
  uint8_t pad[2];
  uint8_t sha[8];   // app_elf_sha256[0..7]
  char version[32];
};

struct Rollback {
  uint32_t magic;
  uint8_t subtype;     // the abandoned image's slot
  uint8_t cause;
  uint8_t reset;       // esp_reset_reason() of the boot that found it
  uint8_t from_state;  // otadata state of the abandoned slot (low byte)
  uint8_t sha[8];
  char version[32];
};

// Survives SW/panic/WDT resets, not power-on (magic check).
struct RtcState {
  uint32_t magic;
  uint32_t addr;      // running image the counter belongs to
  uint32_t starts;    // unverified starts (old bootloader)
  uint32_t backstop;  // 1 = the backstop timer restarted the chip
};
RTC_NOINIT_ATTR RtcState s_rtc;

enum Phase : uint8_t { kNotPending, kPending, kBusy, kValid, kRollingBack };

std::atomic<uint8_t> g_phase{kNotPending};
const esp_partition_t *g_running = nullptr;
esp_ota_img_states_t g_state = ESP_OTA_IMG_UNDEFINED;
esp_err_t g_state_err = ESP_FAIL;
esp_bootloader_desc_t g_bl{};
esp_err_t g_bl_err = ESP_FAIL;
const char *g_bl_rb = "unknown";  // bootloader built with rollback: yes/no/unknown
const char *g_bl_src = "none";    // observed (first boot state) / desc (version field)
Rollback g_rb{};                  // last rollback (magic 0 = none)
bool g_rb_this_boot = false;
uint32_t g_starts = 0;
const char *g_valid_via = "";     // why it was marked valid
uint32_t g_valid_at_s = 0;

Hooks g_hooks{};
std::atomic<bool> g_hooks_set{false};
// Last evaluation, for the JSON (written by the worker only).
std::atomic<bool> g_last_sta{false}, g_last_uplink{false}, g_last_service{false};
const char *volatile g_last_via = "";

esp_timer_handle_t g_backstop = nullptr;

constexpr policy::Params kParams{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    CONFIG_NBP_OTA_VERIFY_MIN_SECS, CONFIG_NBP_OTA_VERIFY_DEADLINE_SECS
#else
    0, 0
#endif
};
constexpr uint32_t kBackstopGraceS = 60;
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
constexpr bool kRollbackBuild = true;
#else
constexpr bool kRollbackBuild = false;
#endif

// Seconds since early_init(). Not esp_timer_get_time() alone: its counter
// is not reset by esp_restart() (QEMU S3: a boot after a SW reset logged
// 16 s at app_main), so a deadline on raw esp_timer time would fire early on
// the boot after a restart.
int64_t g_t0_us = 0;
uint32_t uptime_s() { return static_cast<uint32_t>((esp_timer_get_time() - g_t0_us) / 1000000); }

const esp_partition_t *by_subtype(uint8_t subtype) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  static_cast<esp_partition_subtype_t>(subtype), nullptr);
}

bool nvs_read(const char *key, void *out, size_t len) {
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
  size_t n = len;
  const bool ok = nvs_get_blob(h, key, out, &n) == ESP_OK && n == len;
  nvs_close(h);
  return ok;
}

bool nvs_write(const char *key, const void *in, size_t len) {
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
  bool ok = (in != nullptr ? nvs_set_blob(h, key, in, len) : nvs_erase_key(h, key)) == ESP_OK;
  if (in == nullptr && !ok) ok = true;  // already absent
  ok = ok && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

void fill_identity(const esp_partition_t *p, uint8_t sha[8], char version[32]) {
  esp_app_desc_t d{};
  if (p != nullptr && esp_ota_get_partition_description(p, &d) == ESP_OK) {
    std::memcpy(sha, d.app_elf_sha256, 8);
    std::snprintf(version, 32, "%s", d.version);
  } else {
    std::memset(sha, 0, 8);
    version[0] = '\0';
  }
}

void hex8(const uint8_t *b, char out[17]) {
  for (int i = 0; i < 8; ++i) std::snprintf(out + 2 * i, 3, "%02x", b[i]);
}

esp_err_t mark_valid_raw() {
#if NBP_OTA_SELFTEST_BREAK
  // Known-bad calibration for the QEMU self-test: claim success, write nothing.
  return ESP_OK;
#else
  return esp_ota_mark_app_valid_cancel_rollback();
#endif
}

// Pending -> Valid. Returns false (and stays pending) if otadata could not be
// written: an unwritten verdict must not be reported as one.
bool mark_valid(const char *via) {
  uint8_t expect = kPending;
  if (!g_phase.compare_exchange_strong(expect, kBusy)) return false;
  const esp_err_t err = mark_valid_raw();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s; still pending", esp_err_to_name(err));
    g_phase.store(kPending);
    return false;
  }
  g_valid_via = via;
  g_valid_at_s = uptime_s();
  s_rtc.magic = 0;
  nvs_write(kExpKey, nullptr, 0);
  if (g_backstop != nullptr) esp_timer_stop(g_backstop);
  g_phase.store(kValid);
  ESP_LOGW(TAG, "image %s marked VALID after %lu s (%s)", g_running ? g_running->label : "?",
           static_cast<unsigned long>(g_valid_at_s), via);
  return true;
}

// Mark the running image invalid and reboot into the previous one. Returns
// only on failure. With no rollback target there is nothing to go back to:
// the image is accepted instead (and says so).
[[maybe_unused]] void rollback(uint8_t cause) {
  uint8_t expect = kPending;
  if (!g_phase.compare_exchange_strong(expect, kRollingBack)) return;
  Expect e{};
  if (nvs_read(kExpKey, &e, sizeof(e)) && e.magic == kExpMagic) {
    e.cause = cause;
    nvs_write(kExpKey, &e, sizeof(e));
  }
  s_rtc.backstop = 0;
  ESP_LOGE(TAG, "image %s NOT verified (%s) after %lu s: rolling back",
           g_running ? g_running->label : "?", cause_name(cause),
           static_cast<unsigned long>(uptime_s()));
  vTaskDelay(pdMS_TO_TICKS(200));  // let the line reach /log and serial
  const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
  ESP_LOGE(TAG, "rollback failed: %s", esp_err_to_name(err));
  g_phase.store(kPending);
  if (err == ESP_ERR_OTA_ROLLBACK_FAILED) mark_valid("no_rollback_target");
}

[[maybe_unused]] void backstop_cb(void *) {
  if (g_phase.load() != kPending) return;
  // The worker should have decided by now. A plain restart is a rollback
  // under a rollback bootloader, and one more counted start under an old one.
  s_rtc.backstop = 1;
  esp_restart();
}

// Lazily evaluated: the network checks only run once the cheaper ones pass,
// so the loopback connects (which the listeners log) happen about once.
policy::Inputs evaluate() {
  policy::Inputs in{};
  in.uptime_s = uptime_s();
  in.hooks_ready = g_hooks_set.load();
  if (!in.hooks_ready || in.uptime_s < kParams.min_s) return in;
  in.sta_up = g_hooks.sta_up == nullptr || g_hooks.sta_up();
  g_last_sta.store(in.sta_up);
  if (!in.sta_up) return in;
  const char *via = "n/a";
  in.uplink = g_hooks.uplink == nullptr || g_hooks.uplink(&via);
  g_last_via = in.uplink ? via : "";
  g_last_uplink.store(in.uplink);
  if (!in.uplink) return in;
  in.service = g_hooks.service == nullptr || g_hooks.service();
  g_last_service.store(in.service);
  return in;
}

[[maybe_unused]] void worker(void *) {
  while (g_phase.load() == kPending || g_phase.load() == kBusy) {
    const policy::Inputs in = evaluate();
    switch (policy::decide(in, kParams)) {
      case policy::Verdict::Valid:
        mark_valid(g_last_via);
        break;
      case policy::Verdict::Rollback:
        rollback(kDeadline);
        break;
      case policy::Verdict::Wait:
        break;
    }
    if (g_phase.load() == kValid) break;
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  ESP_LOGI(TAG, "worker done; stack high-water %u B",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  vTaskDelete(nullptr);
}

// The boot found the expected image abandoned: work out why and keep it.
void record_rollback(const Expect &e) {
  const esp_partition_t *p = by_subtype(e.subtype);
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  const bool have_st = p != nullptr && esp_ota_get_state_partition(p, &st) == ESP_OK;
  uint8_t cause = e.cause;
  if (cause == kNone) {
    if (have_st && st == ESP_OTA_IMG_ABORTED) {
      cause = (s_rtc.magic == kRtcMagic && s_rtc.backstop && p && s_rtc.addr == p->address)
                  ? kBackstop : kReset;
    } else if (have_st && st == ESP_OTA_IMG_INVALID) {
      cause = kInvalid;
    } else {
      cause = kNotBooted;
    }
  }
  Rollback rb{};
  rb.magic = kRbMagic;
  rb.subtype = e.subtype;
  rb.cause = cause;
  rb.reset = static_cast<uint8_t>(esp_reset_reason());
  rb.from_state = have_st ? static_cast<uint8_t>(st) : 0xFF;
  std::memcpy(rb.sha, e.sha, sizeof(rb.sha));
  std::memcpy(rb.version, e.version, sizeof(rb.version));
  rb.version[sizeof(rb.version) - 1] = '\0';
  nvs_write(kRbKey, &rb, sizeof(rb));
  nvs_write(kExpKey, nullptr, 0);
  g_rb = rb;
  g_rb_this_boot = true;
  char sha[17];
  hex8(rb.sha, sha);
  ESP_LOGE(TAG, "ROLLED BACK from %s (%s, elf %s): %s, otadata state %s, reset %s",
           p ? p->label : "?", rb.version, sha, cause_name(cause),
           have_st ? state_name(st) : "?", reset_name(rb.reset));
}

void classify_bootloader() {
  g_bl_err = esp_ota_get_bootloader_description(nullptr, &g_bl);
  if (g_state_err == ESP_OK && g_state == ESP_OTA_IMG_PENDING_VERIFY) {
    g_bl_rb = "yes";  // only a rollback bootloader writes PENDING_VERIFY
    g_bl_src = "observed";
  } else if (g_state_err == ESP_OK && g_state == ESP_OTA_IMG_NEW) {
    g_bl_rb = "no";   // it booted a NEW image without converting it
    g_bl_src = "observed";
  } else if (g_bl_err == ESP_OK) {
    // CONFIG_BOOTLOADER_PROJECT_VER is 2 in every build with rollback and was
    // the IDF default 1 in every earlier build of this project.
    g_bl_rb = g_bl.version >= 2 ? "yes" : "no";
    g_bl_src = "desc";
  }
}

// --- network helpers -------------------------------------------------------

// 1 = connected, 0 = refused (the peer answered), -1 = no answer / error.
int tcp_try(uint32_t ip_be, uint16_t port, int timeout_ms) {
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return -1;
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = ip_be;
  int res = -1;
  if (connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == 0) {
    res = 1;
  } else if (errno == EINPROGRESS) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (select(fd + 1, nullptr, &w, nullptr, &tv) == 1) {
      int so = 0;
      socklen_t sl = sizeof(so);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) == 0) {
        res = so == 0 ? 1 : (so == ECONNREFUSED || so == ECONNRESET) ? 0 : -1;
      }
    }
  } else if (errno == ECONNREFUSED) {
    res = 0;
  }
  close(fd);
  return res;
}

struct ArpCall {
  tcpip_api_call_data base;  // must be first
  netif *nif;
  ip4_addr_t ip;
  bool found;
};

err_t arp_find_fn(tcpip_api_call_data *c) {
  auto *a = reinterpret_cast<ArpCall *>(c);
  eth_addr *eth = nullptr;
  const ip4_addr_t *ip = nullptr;
  a->found = etharp_find_addr(a->nif, &a->ip, &eth, &ip) >= 0;
  return ERR_OK;
}

}  // namespace

bool gateway_reachable(const char **via) {
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t info{};
  if (sta == nullptr || esp_netif_get_ip_info(sta, &info) != ESP_OK || info.ip.addr == 0) {
    return false;
  }
  if (info.gw.addr == 0) {  // DHCP gave no router: the LAN is the uplink
    *via = "no_gateway";
    return true;
  }
  if (tcp_try(info.gw.addr, 80, 1000) >= 0) {
    *via = "gateway";
    return true;
  }
  // The connect attempt ARPed the gateway. An answer proves the L2 path even
  // when the gateway drops :80 (CLAUDE.md "Liveness watchdog targets").
  ArpCall a{};
  a.nif = static_cast<netif *>(esp_netif_get_netif_impl(sta));
  a.ip.addr = info.gw.addr;
  if (a.nif != nullptr && tcpip_api_call(&arp_find_fn, &a.base) == ERR_OK && a.found) {
    *via = "arp";
    return true;
  }
  return false;
}

bool loopback_listens(uint16_t port) {
  return tcp_try(htonl(INADDR_LOOPBACK), port, 1000) == 1;
}

// The worker may call esp_ota_mark_app_invalid_rollback_and_reboot(), which
// validates the rollback target; with app signing that is an RSA-3072
// verify (~2 KiB of stack on top of the worker's own ~1.6 KiB).
constexpr uint32_t kWorkerStack = 6144;

void early_init() {
  g_t0_us = esp_timer_get_time();
  g_running = esp_ota_get_running_partition();
  g_state_err = g_running ? esp_ota_get_state_partition(g_running, &g_state) : ESP_FAIL;
  classify_bootloader();

  Expect e{};
  const bool have_exp = nvs_read(kExpKey, &e, sizeof(e)) && e.magic == kExpMagic;
  if (have_exp && g_running != nullptr && e.subtype != g_running->subtype) {
    record_rollback(e);
  } else {
    Rollback rb{};
    if (nvs_read(kRbKey, &rb, sizeof(rb)) && rb.magic == kRbMagic) g_rb = rb;
  }

  const bool pending = g_state_err == ESP_OK &&
                       (g_state == ESP_OTA_IMG_NEW || g_state == ESP_OTA_IMG_PENDING_VERIFY);
  ESP_LOGI(TAG, "running %s, otadata state %s; bootloader v%lu (%s), rollback %s (%s)",
           g_running ? g_running->label : "?",
           g_state_err == ESP_OK ? state_name(g_state) : esp_err_to_name(g_state_err),
           static_cast<unsigned long>(g_bl_err == ESP_OK ? g_bl.version : 0),
           g_bl_err == ESP_OK ? g_bl.idf_ver : "no desc", g_bl_rb, g_bl_src);
  if (!pending) {
    if (have_exp && g_running != nullptr && e.subtype == g_running->subtype) {
      nvs_write(kExpKey, nullptr, 0);  // already verified (or an older app's OTA)
    }
    s_rtc.magic = 0;
    return;
  }
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  // Built without rollback, but installed by an app with it: confirm at once,
  // this build has no criterion to apply.
  g_phase.store(kPending);
  mark_valid("rollback_disabled_build");
  return;
#else
  g_phase.store(kPending);
  if (!esp_ota_check_rollback_is_possible()) {
    ESP_LOGW(TAG, "no valid image to roll back to; accepting %s", g_running->label);
    mark_valid("no_rollback_target");
    return;
  }
  if (!have_exp || e.subtype != g_running->subtype) {
    Expect mine{};
    mine.magic = kExpMagic;
    mine.subtype = static_cast<uint8_t>(g_running->subtype);
    fill_identity(g_running, mine.sha, mine.version);
    nvs_write(kExpKey, &mine, sizeof(mine));
  }
  if (g_state == ESP_OTA_IMG_NEW) {
    // Old bootloader: nothing rolls back on a reset, so count starts here.
    if (s_rtc.magic == kRtcMagic && s_rtc.addr == g_running->address) {
      s_rtc.starts++;
    } else {
      s_rtc = RtcState{kRtcMagic, g_running->address, 1, 0};
    }
    g_starts = s_rtc.starts;
    ESP_LOGW(TAG, "bootloader has no rollback: unverified start %lu of %d",
             static_cast<unsigned long>(g_starts), CONFIG_NBP_OTA_VERIFY_MAX_BOOTS);
    if (policy::starts_exceeded(g_starts, CONFIG_NBP_OTA_VERIFY_MAX_BOOTS)) {
      rollback(kStarts);  // returns only if that failed
    }
  } else {
    s_rtc = RtcState{kRtcMagic, g_running->address, 1, 0};
    g_starts = 1;
  }
  if (g_phase.load() != kPending) return;

  const esp_timer_create_args_t args = {
      .callback = &backstop_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "ota_backstop",
      .skip_unhandled_events = false,
  };
  if (esp_timer_create(&args, &g_backstop) == ESP_OK) {
    esp_timer_start_once(g_backstop,
                         static_cast<uint64_t>(kParams.deadline_s + kBackstopGraceS) * 1000000);
  }
  if (xTaskCreate(&worker, "ota_verify", kWorkerStack, nullptr, 2, nullptr) != pdPASS) {
    // No worker: the backstop restart still ends this attempt.
    ESP_LOGE(TAG, "xTaskCreate(ota_verify) failed; the backstop will restart");
  }
  ESP_LOGW(TAG, "image unverified: valid after >= %lu s with network proof, else rollback at %lu s",
           static_cast<unsigned long>(kParams.min_s), static_cast<unsigned long>(kParams.deadline_s));
#endif
}

void set_hooks(const Hooks &h) {
  g_hooks = h;
  g_hooks_set.store(true);
}

bool pending() {
  const uint8_t p = g_phase.load();
  return p == kPending || p == kBusy || p == kRollingBack;
}

unsigned left_s() {
  if (!pending()) return 0;
  const uint32_t t = uptime_s();
  return t >= kParams.deadline_s ? 0 : kParams.deadline_s - t;
}

void note_update(const esp_partition_t *target) {
  if (target == nullptr) return;
  Expect e{};
  e.magic = kExpMagic;
  e.subtype = static_cast<uint8_t>(target->subtype);
  fill_identity(target, e.sha, e.version);
  if (!nvs_write(kExpKey, &e, sizeof(e))) ESP_LOGW(TAG, "could not record the expected image");
}

namespace {
struct Buf {
  char *p;
  size_t len, n;
  bool ok;
  __attribute__((format(printf, 2, 3))) void f(const char *fmt, ...) {
    if (!ok) return;
    va_list ap;
    va_start(ap, fmt);
    const int k = std::vsnprintf(p + n, len - n, fmt, ap);
    va_end(ap);
    if (k < 0 || static_cast<size_t>(k) >= len - n) {
      ok = false;
      return;
    }
    n += static_cast<size_t>(k);
  }
};

// Version strings are from our own image headers, but keep the JSON valid.
void sanitize(const char *in, char *out, size_t len) {
  size_t j = 0;
  for (size_t i = 0; in[i] != '\0' && j + 1 < len; ++i) {
    const char c = in[i];
    out[j++] = (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) ? '_' : c;
  }
  out[j] = '\0';
}
}  // namespace

size_t write_json(char *buf, size_t len) {
  Buf b{buf, len, 0, len > 0};
  const uint8_t ph = g_phase.load();
  const char *phase = ph == kValid ? "valid" : ph == kRollingBack ? "rolling_back"
                      : (ph == kPending || ph == kBusy) ? "pending" : "not_pending";
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  const bool have_st = g_running && esp_ota_get_state_partition(g_running, &st) == ESP_OK;
  char idf[32];
  sanitize(g_bl_err == ESP_OK ? g_bl.idf_ver : "", idf, sizeof(idf));
  b.f("{\"rollback_build\":%s,\"running\":\"%s\",\"state\":\"%s\",\"phase\":\"%s\","
      "\"bootloader\":{\"ver\":%lu,\"idf\":\"%s\",\"rollback\":\"%s\",\"source\":\"%s\"},",
      kRollbackBuild ? "true" : "false",
      g_running ? g_running->label : "?", have_st ? state_name(st) : "none", phase,
      static_cast<unsigned long>(g_bl_err == ESP_OK ? g_bl.version : 0), idf, g_bl_rb, g_bl_src);
  b.f("\"verify\":{\"uptime_s\":%lu,\"min_s\":%lu,\"deadline_s\":%lu,\"left_s\":%u,"
      "\"starts\":%lu,\"hooks\":%s,\"sta\":%s,\"uplink\":%s,\"via\":\"%s\",\"service\":%s,"
      "\"valid_at_s\":%lu,\"valid_via\":\"%s\"},",
      static_cast<unsigned long>(uptime_s()), static_cast<unsigned long>(kParams.min_s),
      static_cast<unsigned long>(kParams.deadline_s), left_s(),
      static_cast<unsigned long>(g_starts), g_hooks_set.load() ? "true" : "false",
      g_last_sta.load() ? "true" : "false", g_last_uplink.load() ? "true" : "false",
      g_last_via, g_last_service.load() ? "true" : "false",
      static_cast<unsigned long>(g_valid_at_s), g_valid_via);
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  if (other != nullptr) {
    esp_ota_img_states_t ost = ESP_OTA_IMG_UNDEFINED;
    const bool have_ost = esp_ota_get_state_partition(other, &ost) == ESP_OK;
    uint8_t sha[8];
    char ver[32], vs[32], shs[17];
    fill_identity(other, sha, ver);
    sanitize(ver, vs, sizeof(vs));
    hex8(sha, shs);
    b.f("\"other\":{\"label\":\"%s\",\"state\":\"%s\",\"version\":\"%s\",\"elf\":\"%s\"},",
        other->label, have_ost ? state_name(ost) : "none", vs, shs);
  }
  if (g_rb.magic == kRbMagic) {
    const esp_partition_t *p = by_subtype(g_rb.subtype);
    char vs[32], shs[17];
    sanitize(g_rb.version, vs, sizeof(vs));
    hex8(g_rb.sha, shs);
    b.f("\"last_rollback\":{\"this_boot\":%s,\"from\":\"%s\",\"version\":\"%s\",\"elf\":\"%s\","
        "\"cause\":\"%s\",\"from_state\":\"%s\",\"reset_reason\":\"%s\"}}",
        g_rb_this_boot ? "true" : "false", p ? p->label : "?", vs, shs, cause_name(g_rb.cause),
        g_rb.from_state == 0xFF ? "none" : state_name(static_cast<esp_ota_img_states_t>(g_rb.from_state)),
        reset_name(g_rb.reset));
  } else {
    b.f("\"last_rollback\":null}");
  }
  return b.ok ? b.n : 0;
}

namespace {
esp_err_t ota_get(httpd_req_t *req) {
  char buf[1024];
  const size_t n = write_json(buf, sizeof(buf));
  if (n == 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json overflow");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, static_cast<ssize_t>(n));
}

esp_err_t ota_post(httpd_req_t *req) {
  if (!nbp_http::confirmed(req)) return nbp_http::send_unconfirmed(req);
  char q[48], v[12];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
      httpd_query_key_value(q, "mark", v, sizeof(v)) != ESP_OK ||
      (std::strcmp(v, "valid") != 0 && std::strcmp(v, "invalid") != 0)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use ?mark=valid|invalid");
  }
  if (g_phase.load() != kPending) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "running image is not pending verification\n");
  }
  if (std::strcmp(v, "valid") == 0) {
    if (!mark_valid("manual")) {
      return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mark valid failed");
    }
    return httpd_resp_sendstr(req, "ok: marked valid\n");
  }
  httpd_resp_sendstr(req, "ok: rolling back, rebooting\n");
  vTaskDelay(pdMS_TO_TICKS(300));
  rollback(kManual);  // returns only on failure
  return ESP_OK;
}
}  // namespace

void register_endpoints(httpd_handle_t srv) {
  if (srv == nullptr) return;
  httpd_uri_t get = {.uri = "/ota", .method = HTTP_GET, .handler = &ota_get, .user_ctx = nullptr};
  httpd_uri_t post = {.uri = "/ota", .method = HTTP_POST, .handler = &ota_post, .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &get);
  httpd_register_uri_handler(srv, &post);
}

#if NBP_OTA_SELFTEST
// QEMU self-test, one phase per boot (RTC_NOINIT). The real early_init() runs
// before each boot's phase, against the real bootloader and otadata; only the
// network hooks are fakes. Phases: START installs a copy of the running image
// into the other slot (esp_ota_begin/write/end/set_boot_partition); VALID
// expects it to boot unverified and to mark itself valid once the hooks pass;
// PANIC installs again and panics before the criterion (a rollback
// bootloader aborts it at once, an old one after MAX_BOOTS starts); DEADLINE
// installs again, keeps the hooks failing and expects the deadline rollback.
// Expected bootloader: the build's own (rollback) or, with a flash image
// patched with a no-rollback bootloader, "no" (see CLAUDE.md).
namespace {
RTC_NOINIT_ATTR uint32_t s_st_magic;
RTC_NOINIT_ATTR uint32_t s_st_phase;
RTC_NOINIT_ATTR uint32_t s_st_target;  // address of the image under test
constexpr uint32_t kStMagic = 0x4F545354;
enum : uint32_t { ST_START, ST_VALID, ST_PANIC, ST_DEADLINE, ST_DONE };
volatile bool st_net = false;
int st_fails = 0;

bool st_sta() { return st_net; }
bool st_uplink(const char **via) {
  *via = "selftest";
  return st_net;
}
bool st_service() { return st_net; }

void st_check(const char *step, bool ok, const char *what) {
  if (!ok) ++st_fails;
  std::printf("OTA-SELFTEST %s: %s: %s\n", step, ok ? "PASS" : "FAIL", what);
}

void st_dump() {
  static char buf[1024];
  const size_t n = write_json(buf, sizeof(buf));
  std::printf("OTA-SELFTEST-JSON %.*s\n", static_cast<int>(n), buf);
}

// A real OTA install of the running image into the other slot.
bool st_install_copy() {
  const esp_partition_t *src = esp_ota_get_running_partition();
  const esp_partition_t *dst = esp_ota_get_next_update_partition(nullptr);
  if (src == nullptr || dst == nullptr) return false;
  const esp_partition_pos_t pos{src->address, src->size};
  esp_image_metadata_t md{};
  std::printf("OTA-SELFTEST reading image metadata of %s\n", src->label);
  std::fflush(stdout);
  if (esp_image_get_metadata(&pos, &md) != ESP_OK) return false;
#if CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
  // image_len ends before the signature sector; esp_ota_end() refuses the
  // copy without it ("unsigned").
  md.image_len = ((md.image_len + 4095u) & ~4095u) + 4096u;
  if (md.image_len > src->size) return false;
#endif
  std::printf("OTA-SELFTEST installing %lu B from %s into %s\n",
              static_cast<unsigned long>(md.image_len), src->label, dst->label);
  std::fflush(stdout);
  esp_ota_handle_t h = 0;
  esp_err_t err = esp_ota_begin(dst, md.image_len, &h);
  if (err != ESP_OK) {
    std::printf("OTA-SELFTEST esp_ota_begin: %s\n", esp_err_to_name(err));
    return false;
  }
  std::printf("OTA-SELFTEST esp_ota_begin ok\n");
  std::fflush(stdout);
  static uint8_t buf[4096];
  for (uint32_t off = 0; off < md.image_len; off += sizeof(buf)) {
    const uint32_t n = std::min<uint32_t>(sizeof(buf), md.image_len - off);
    if (esp_partition_read(src, off, buf, n) != ESP_OK || esp_ota_write(h, buf, n) != ESP_OK) {
      esp_ota_abort(h);
      return false;
    }
  }
  if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(dst) != ESP_OK) return false;
  note_update(dst);
  s_st_target = dst->address;
  std::printf("OTA-SELFTEST installed %lu B into %s\n", static_cast<unsigned long>(md.image_len),
              dst->label);
  return true;
}

void st_next(uint32_t phase) {
  const bool ok = st_install_copy();
  st_check("install", ok, "copy installed into the other slot");
  if (!ok) return;
  s_st_phase = phase;
  std::fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
}

__attribute__((noinline)) void st_panic() {
  *reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(0x4)) = 0xDEADBEEF;
}
}  // namespace

void selftest::run() {
  if (s_st_magic != kStMagic) {
    s_st_magic = kStMagic;
    s_st_phase = ST_START;
    s_st_target = 0;
  }
  const bool on_target = g_running != nullptr && g_running->address == s_st_target;
  const esp_partition_t *target = nullptr;
  if (s_st_target != 0) {
    target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    for (; it != nullptr; it = esp_partition_next(it)) {
      const esp_partition_t *p = esp_partition_get(it);
      if (p->address == s_st_target) target = p;
    }
    esp_partition_iterator_release(it);
  }
  std::printf("OTA-SELFTEST boot: phase %lu, running %s, bootloader rollback %s (%s)\n",
              static_cast<unsigned long>(s_st_phase), g_running ? g_running->label : "?",
              g_bl_rb, g_bl_src);
  st_dump();
  const Hooks hooks{&st_sta, &st_uplink, &st_service};
  const bool old_bl = std::strcmp(g_bl_rb, "no") == 0;

  switch (s_st_phase) {
    case ST_START:
      st_check("start", !pending() && g_rb.magic != kRbMagic,
               "serially flashed image is not pending, no rollback recorded");
      st_next(ST_VALID);
      break;
    case ST_VALID: {
      st_check("first_boot", on_target && pending(), "installed image boots unverified");
      st_check("bootloader_observed", std::strcmp(g_bl_src, "observed") == 0,
               old_bl ? "state NEW seen: bootloader without rollback"
                      : "state PENDING_VERIFY seen: bootloader with rollback");
      st_net = true;
      set_hooks(hooks);
      for (int i = 0; i < 4 * static_cast<int>(kParams.min_s) + 20 && pending(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(500));
      }
      esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
      const bool have = esp_ota_get_state_partition(g_running, &st) == ESP_OK;
      Expect e{};
      const bool exp_left = nvs_read(kExpKey, &e, sizeof(e)) && e.magic == kExpMagic;
      st_check("valid_after_criterion",
               g_phase.load() == kValid && have && st == ESP_OTA_IMG_VALID && !exp_left &&
                   uptime_s() >= kParams.min_s,
               "criterion met -> otadata VALID, marker cleared");
      st_dump();
      st_next(ST_PANIC);
      break;
    }
    case ST_PANIC:
      if (on_target) {
        st_check("panic_boot", pending(), "installed image boots unverified");
        set_hooks(hooks);  // st_net false: criterion never met
        std::printf("OTA-SELFTEST panicking before verification (start %lu)\n",
                    static_cast<unsigned long>(g_starts));
        std::fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
        st_panic();
      }
      st_check("rollback_on_panic",
               g_rb_this_boot && target != nullptr && g_rb.subtype == target->subtype &&
                   !pending() &&
                   (old_bl ? g_rb.cause == kStarts && g_rb.from_state == ESP_OTA_IMG_INVALID
                           : g_rb.cause == kReset && g_rb.from_state == ESP_OTA_IMG_ABORTED &&
                                 g_rb.reset == ESP_RST_PANIC),
               old_bl ? "old bootloader: app rolled back after MAX_BOOTS unverified starts"
                      : "bootloader aborted the pending image after the panic");
      st_next(ST_DEADLINE);
      break;
    case ST_DEADLINE:
      if (on_target) {
        st_check("deadline_boot", pending(), "installed image boots unverified");
        set_hooks(hooks);  // st_net false
        std::printf("OTA-SELFTEST waiting for the %lu s deadline\n",
                    static_cast<unsigned long>(kParams.deadline_s));
        vTaskDelay(pdMS_TO_TICKS((kParams.deadline_s + 20) * 1000));
        st_check("rollback_on_deadline", false, "still running 20 s past the deadline");
        break;
      }
      st_check("rollback_on_deadline",
               g_rb_this_boot && target != nullptr && g_rb.subtype == target->subtype &&
                   g_rb.cause == kDeadline && g_rb.from_state == ESP_OTA_IMG_INVALID &&
                   !pending(),
               "deadline -> marked invalid -> previous image boots");
      s_st_phase = ST_DONE;
      std::printf("OTA-SELFTEST done: bootloader rollback %s\n", g_bl_rb);
      break;
    default:
      std::printf("OTA-SELFTEST done (idle)\n");
      break;
  }
  std::printf("OTA-SELFTEST end of boot: %d failed\n", st_fails);
  std::fflush(stdout);
  while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
#endif  // NBP_OTA_SELFTEST

}  // namespace ota_verify
