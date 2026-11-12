// Panic capture that survives the reboot. See crash_capture.h and the
// NBP_CRASH_CAPTURE help text in main/Kconfig.projbuild.
//
// Two layers:
//  1. RTC record (always). -Wl,--wrap=esp_panic_handler routes IDF's
//     port-level panic handler through __wrap_esp_panic_handler, which copies
//     reason, abort/assert text, registers, task name and a walked backtrace
//     into RTC_NOINIT memory, then calls the real handler (UART print, core
//     dump, reboot). RTC memory survives the panic reset, so the next boot can
//     report it. Works without any partition-table change.
//  2. Flash core dump (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH). IDF writes an ELF
//     core to the `coredump` data partition. Devices flashed before that
//     partition was added to partitions.csv still run the old table (an app
//     OTA never rewrites 0x8000), so init() registers the free flash after
//     the last partition as a coredump partition at runtime and re-runs
//     esp_core_dump_init() so the panic path picks it up.

#include "crash_capture.h"

#include "sdkconfig.h"

#if CONFIG_NBP_CRASH_CAPTURE

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_debug_helpers.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include "esp_cpu_utils.h"
#include "xtensa/corebits.h"
#include "xtensa_context.h"
#else
#include "riscv/rvruntime-frames.h"
#endif

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_partition.h"
#endif

extern "C" void __real_esp_panic_handler(panic_info_t *info);
extern "C" void __wrap_esp_panic_handler(panic_info_t *info);

namespace crash_capture {
namespace {

constexpr const char *TAG = "crash";

constexpr int kFrames = 16;  // backtrace entries kept per panic
constexpr int kSlots = 3;    // most recent panics kept

// flags
constexpr uint8_t F_CORRUPTED = 1;  // walk stopped on an invalid frame
constexpr uint8_t F_TRUNCATED = 2;  // more frames than kFrames
constexpr uint8_t F_HEURISTIC = 4;  // RISC-V: entries after [0..1] are a
                                    // stack scan for code addresses

struct Record {
  uint32_t seq;        // panic number since the store was initialised
  uint32_t uptime_ms;  // time since boot when it panicked
  uint8_t core;
  uint8_t exception;  // panic_exception_t
  uint8_t flags;
  uint8_t depth;     // valid entries in bt_pc/bt_sp
  uint32_t pc;       // faulting PC (Xtensa pc / RISC-V mepc)
  uint32_t addr;     // EXCVADDR / MTVAL
  uint32_t cause;    // EXCCAUSE / MCAUSE
  uint32_t sp;       // A1 / sp
  uint32_t ra;       // A0 / ra
  char task[16];
  char reason[40];
  char details[128];  // abort/assert message, else the exception description
  uint8_t elf_sha[8];  // first 8 bytes of app_elf_sha256 of the crashed image
  uint32_t bt_pc[kFrames];
  uint32_t bt_sp[kFrames];
};

struct Store {
  uint32_t magic;
  uint32_t version;
  uint32_t count;     // panics recorded since the store was initialised
  uint32_t reported;  // `count` value already logged at boot
  Record slot[kSlots];
  uint32_t crc;  // over everything above
};

constexpr uint32_t kMagic = 0x4E425043u;  // "NBPC"
constexpr uint32_t kVersion = 1;

RTC_NOINIT_ATTR Store s_store;

// Filled at init. The capture path runs from flash like IDF's own
// esp_panic_handler: the port-level handler re-enables the flash cache first
// (panic_enable_cache), so nothing here needs IRAM.
uint8_t s_elf_sha[8];
bool s_in_capture = false;

uint32_t store_crc() {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&s_store),
                          offsetof(Store, crc));
}

bool store_valid() {
  return s_store.magic == kMagic && s_store.version == kVersion &&
         s_store.crc == store_crc();
}

void store_reset() {
  std::memset(&s_store, 0, sizeof(s_store));
  s_store.magic = kMagic;
  s_store.version = kVersion;
  s_store.crc = store_crc();
}

// .rodata strings are readable (cache is on); refuse pointers that would
// fault anyway.
bool readable(const void *p) {
  return p != nullptr && (esp_ptr_byte_accessible(p) || esp_ptr_in_drom(p));
}

void copy_str(char *dst, size_t cap, const char *src) {
  if (!readable(src)) {
    dst[0] = '\0';
    return;
  }
  size_t i = 0;
  for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

#if CONFIG_IDF_TARGET_ARCH_XTENSA
void capture_frame(Record &r, const void *frame) {
  const auto *f = static_cast<const XtExcFrame *>(frame);
  if (!esp_ptr_byte_accessible(f)) return;
  r.pc = f->pc;
  r.addr = f->excvaddr;
  r.cause = f->exccause;
  r.sp = f->a1;
  r.ra = f->a0;
  // Same walk as esp_backtrace_print_from_frame().
  esp_backtrace_frame_t fr = {.pc = static_cast<uint32_t>(f->pc),
                              .sp = static_cast<uint32_t>(f->a1),
                              .next_pc = static_cast<uint32_t>(f->a0),
                              .exc_frame = f};
  int n = 0;
  r.bt_pc[n] = esp_cpu_process_stack_pc(fr.pc);
  r.bt_sp[n] = fr.sp;
  ++n;
  bool corrupted =
      !(esp_stack_ptr_is_sane(fr.sp) &&
        (esp_ptr_executable(reinterpret_cast<void *>(esp_cpu_process_stack_pc(fr.pc))) ||
         f->exccause == EXCCAUSE_INSTR_PROHIBITED));
  while (n < kFrames && fr.next_pc != 0 && !corrupted) {
    if (!esp_backtrace_get_next_frame(&fr)) corrupted = true;
    r.bt_pc[n] = esp_cpu_process_stack_pc(fr.pc);
    r.bt_sp[n] = fr.sp;
    ++n;
  }
  r.depth = static_cast<uint8_t>(n);
  if (corrupted) r.flags |= F_CORRUPTED;
  else if (fr.next_pc != 0) r.flags |= F_TRUNCATED;
}
#else
// No unwinder without frame pointers / eh_frame: record mepc and ra, then
// scan the stack for words that point into executable memory. Candidates,
// not a call chain (F_HEURISTIC) — symbolicate and judge by eye.
void capture_frame(Record &r, const void *frame) {
  const auto *f = static_cast<const RvExcFrame *>(frame);
  if (!esp_ptr_byte_accessible(f)) return;
  r.pc = f->mepc;
  r.addr = f->mtval;
  r.cause = f->mcause;
  r.sp = f->sp;
  r.ra = f->ra;
  int n = 0;
  r.bt_pc[n] = f->mepc;
  r.bt_sp[n++] = f->sp;
  r.bt_pc[n] = f->ra;
  r.bt_sp[n++] = f->sp;
  r.flags |= F_HEURISTIC;
  if (!esp_stack_ptr_is_sane(f->sp)) {
    r.flags |= F_CORRUPTED;
  } else {
    const uint32_t *w = reinterpret_cast<const uint32_t *>(f->sp);
    for (int i = 0; i < 256 && n < kFrames; ++i) {
      if (!esp_ptr_byte_accessible(w + i)) break;
      const uint32_t v = w[i];
      if (v != f->ra && esp_ptr_executable(reinterpret_cast<void *>(v))) {
        r.bt_pc[n] = v;
        r.bt_sp[n++] = reinterpret_cast<uint32_t>(w + i);
      }
    }
  }
  r.depth = static_cast<uint8_t>(n);
}
#endif

void capture(const panic_info_t *info) {
  if (s_in_capture) return;  // nested panic (e.g. inside the core dump)
  s_in_capture = true;
  if (!store_valid()) store_reset();
  Record &r = s_store.slot[s_store.count % kSlots];
  std::memset(&r, 0, sizeof(r));
  r.seq = ++s_store.count;
  r.uptime_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  r.core = static_cast<uint8_t>(info->core);
  if (g_panic_abort) {
    // esp_panic_handler() overrides reason/details for aborts after we run.
    r.exception = PANIC_EXCEPTION_ABORT;
    copy_str(r.reason, sizeof(r.reason), "abort");
    copy_str(r.details, sizeof(r.details), g_panic_abort_details);
  } else {
    r.exception = static_cast<uint8_t>(info->exception);
    copy_str(r.reason, sizeof(r.reason), info->reason);
    copy_str(r.details, sizeof(r.details), info->description);
  }
  TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(info->core);
  if (t != nullptr) copy_str(r.task, sizeof(r.task), pcTaskGetName(t));
  std::memcpy(r.elf_sha, s_elf_sha, sizeof(r.elf_sha));
  if (info->frame != nullptr) capture_frame(r, info->frame);
  s_store.crc = store_crc();
}

const char *exception_name(uint8_t e) {
  switch (e) {
    case PANIC_EXCEPTION_DEBUG: return "debug";
    case PANIC_EXCEPTION_IWDT: return "int_wdt";
    case PANIC_EXCEPTION_TWDT: return "task_wdt";
    case PANIC_EXCEPTION_ABORT: return "abort";
    case PANIC_EXCEPTION_FAULT: return "fault";
    default: return "?";
  }
}

void sha_hex(const uint8_t *sha, char out[17]) {
  for (int i = 0; i < 8; ++i) std::snprintf(out + 2 * i, 3, "%02x", sha[i]);
}

// "0xPC:0xSP 0xPC:0xSP …" (the format IDF prints and idf.py monitor decodes).
size_t format_bt(const Record &r, int from, int to, char *out, size_t cap) {
  size_t o = 0;
  out[0] = '\0';
  for (int i = from; i < to && i < r.depth && o + 24 < cap; ++i) {
    o += std::snprintf(out + o, cap - o, "%s0x%08lx:0x%08lx", i == from ? "" : " ",
                       static_cast<unsigned long>(r.bt_pc[i]),
                       static_cast<unsigned long>(r.bt_sp[i]));
  }
  return o;
}

void log_record(const Record &r) {
  char sha[17];
  sha_hex(r.elf_sha, sha);
  const bool same = std::memcmp(r.elf_sha, s_elf_sha, sizeof(s_elf_sha)) == 0;
  ESP_LOGE(TAG, "panic #%lu at uptime %lu.%03lu s, core %u, task \"%s\": %s (%s)",
           static_cast<unsigned long>(r.seq),
           static_cast<unsigned long>(r.uptime_ms / 1000),
           static_cast<unsigned long>(r.uptime_ms % 1000), r.core, r.task,
           r.reason, exception_name(r.exception));
  if (r.details[0] != '\0') ESP_LOGE(TAG, "  %s", r.details);
  ESP_LOGE(TAG, "  pc 0x%08lx addr 0x%08lx cause %lu sp 0x%08lx ra 0x%08lx",
           static_cast<unsigned long>(r.pc), static_cast<unsigned long>(r.addr),
           static_cast<unsigned long>(r.cause), static_cast<unsigned long>(r.sp),
           static_cast<unsigned long>(r.ra));
  char line[200];
  for (int i = 0; i < r.depth; i += 6) {
    format_bt(r, i, i + 6, line, sizeof(line));
    ESP_LOGE(TAG, "  %s %s%s", i == 0 ? "Backtrace:" : "          ", line,
             (i + 6 >= r.depth)
                 ? ((r.flags & F_CORRUPTED)   ? " |<-CORRUPTED"
                    : (r.flags & F_TRUNCATED) ? " |<-CONTINUES"
                                              : "")
                 : "");
  }
  ESP_LOGE(TAG, "  elf %s%s%s", sha, same ? " (this image)" : " (a different image)",
           (r.flags & F_HEURISTIC) ? "; backtrace is a stack scan" : "");
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
const char *g_cd_region = "none";  // "table" | "runtime" | "none"
const esp_partition_t *g_cd_part = nullptr;

constexpr uint32_t kMinRuntimeRegion = 64 * 1024;
constexpr uint32_t kMaxRuntimeRegion = 256 * 1024;

void ensure_coredump_partition() {
  g_cd_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (g_cd_part != nullptr) {
    g_cd_region = "table";
    return;
  }
  // The table predates the coredump partition. Use the flash after the last
  // partition, if there is enough of it. esp_partition_register_external()
  // rejects any overlap with the running table, and the bound is the size
  // in the image header, so nothing in use can be hit.
  uint32_t end = 0;
  for (esp_partition_iterator_t it = esp_partition_find(
           ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
       it != nullptr; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    if (p->flash_chip != esp_flash_default_chip) continue;
    end = std::max<uint32_t>(end, p->address + p->size);
  }
  end = (end + 0xFFF) & ~0xFFFu;
  uint32_t chip = 0;
  if (esp_flash_get_size(esp_flash_default_chip, &chip) != ESP_OK || chip <= end ||
      chip - end < kMinRuntimeRegion) {
    ESP_LOGW(TAG, "no coredump partition and no free flash after 0x%lx (chip %lu B); "
             "RTC capture only", static_cast<unsigned long>(end),
             static_cast<unsigned long>(chip));
    return;
  }
  const uint32_t size = std::min(chip - end, kMaxRuntimeRegion);
  esp_err_t err = esp_partition_register_external(
      esp_flash_default_chip, end, size, "coredump", ESP_PARTITION_TYPE_DATA,
      ESP_PARTITION_SUBTYPE_DATA_COREDUMP, &g_cd_part);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "registering 0x%lx+0x%lx as coredump failed: %s",
             static_cast<unsigned long>(end), static_cast<unsigned long>(size),
             esp_err_to_name(err));
    g_cd_part = nullptr;
    return;
  }
  esp_core_dump_init();  // re-read the partition into the panic-time config
  g_cd_region = "runtime";
  ESP_LOGI(TAG, "partition table has no coredump partition; using free flash "
           "0x%lx+0x%lx", static_cast<unsigned long>(end),
           static_cast<unsigned long>(size));
}

// size of a stored, checksum-valid core dump, else 0
size_t coredump_size(const char **state) {
  *state = "none";
  if (g_cd_part == nullptr) return 0;
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) != ESP_OK) return 0;
  if (esp_core_dump_image_check() != ESP_OK) {
    *state = "bad_checksum";
    return 0;
  }
  *state = "ok";
  return size;
}
#endif

#if CONFIG_NBP_WIFI
// Append a JSON string literal (with quotes) to out.
size_t json_str(char *out, size_t cap, size_t o, const char *s) {
  if (o + 2 >= cap) return o;
  out[o++] = '"';
  for (; *s != '\0' && o + 8 < cap; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = static_cast<char>(c);
    } else if (c < 0x20) {
      o += std::snprintf(out + o, cap - o, "\\u%04x", c);
    } else {
      out[o++] = static_cast<char>(c);
    }
  }
  out[o++] = '"';
  out[o] = '\0';
  return o;
}

#define APPEND(...)                                                   \
  do {                                                                \
    if (o < cap) o += std::snprintf(buf + o, cap - o, __VA_ARGS__);   \
  } while (0)

esp_err_t crash_get(httpd_req_t *req) {
  constexpr size_t cap = 4096;
  char *buf = static_cast<char *>(std::malloc(cap));
  if (buf == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    return ESP_FAIL;
  }
  size_t o = 0;
  char sha[17];
  sha_hex(s_elf_sha, sha);
  const bool valid = store_valid();
  APPEND("{\"running_elf\":\"%s\",\"count\":%lu,\"records\":[", sha,
         static_cast<unsigned long>(valid ? s_store.count : 0));
  if (valid) {
    bool first = true;
    // newest first
    for (uint32_t k = 0; k < kSlots && k < s_store.count; ++k) {
      const Record &r = s_store.slot[(s_store.count - 1 - k) % kSlots];
      if (r.seq == 0) continue;
      char rsha[17];
      sha_hex(r.elf_sha, rsha);
      APPEND("%s{\"seq\":%lu,\"uptime_ms\":%lu,\"core\":%u,\"exception\":\"%s\","
             "\"task\":",
             first ? "" : ",", static_cast<unsigned long>(r.seq),
             static_cast<unsigned long>(r.uptime_ms), r.core,
             exception_name(r.exception));
      first = false;
      o = json_str(buf, cap, o, r.task);
      APPEND(",\"reason\":");
      o = json_str(buf, cap, o, r.reason);
      APPEND(",\"details\":");
      o = json_str(buf, cap, o, r.details);
      APPEND(",\"pc\":\"0x%08lx\",\"addr\":\"0x%08lx\",\"cause\":%lu,"
             "\"sp\":\"0x%08lx\",\"ra\":\"0x%08lx\",\"elf\":\"%s\","
             "\"elf_matches_running\":%s,\"corrupted\":%s,\"truncated\":%s,"
             "\"stack_scan\":%s,\"backtrace\":\"",
             static_cast<unsigned long>(r.pc), static_cast<unsigned long>(r.addr),
             static_cast<unsigned long>(r.cause), static_cast<unsigned long>(r.sp),
             static_cast<unsigned long>(r.ra), rsha,
             std::memcmp(r.elf_sha, s_elf_sha, 8) == 0 ? "true" : "false",
             (r.flags & F_CORRUPTED) ? "true" : "false",
             (r.flags & F_TRUNCATED) ? "true" : "false",
             (r.flags & F_HEURISTIC) ? "true" : "false");
      if (o < cap) o += format_bt(r, 0, kFrames, buf + o, cap - o);
      APPEND("\"}");
    }
  }
  APPEND("]");
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  const char *state = "none";
  const size_t cd = coredump_size(&state);
  APPEND(",\"coredump\":{\"region\":\"%s\",\"part_addr\":\"0x%lx\",\"part_size\":%lu,"
         "\"state\":\"%s\",\"size\":%u}",
         g_cd_region,
         static_cast<unsigned long>(g_cd_part ? g_cd_part->address : 0),
         static_cast<unsigned long>(g_cd_part ? g_cd_part->size : 0), state,
         static_cast<unsigned>(cd));
#else
  APPEND(",\"coredump\":null");
#endif
  APPEND("}\n");
  if (o >= cap) o = cap - 1;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, static_cast<ssize_t>(o));
  std::free(buf);
  return ESP_OK;
}
#undef APPEND

bool query_is(httpd_req_t *req, const char *key, const char *want, char *val,
              size_t cap) {
  char q[96];
  val[0] = '\0';
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
  if (httpd_query_key_value(q, key, val, cap) != ESP_OK) return false;
  return want == nullptr || std::strcmp(val, want) == 0;
}

esp_err_t crash_post(httpd_req_t *req) {
  char v[16];
  if (!query_is(req, "erase", "1", v, sizeof(v))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use POST /crash?erase=1");
    return ESP_FAIL;
  }
  store_reset();
  const char *cd = "n/a";
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  if (g_cd_part != nullptr) {
    cd = esp_core_dump_image_erase() == ESP_OK ? "erased" : "erase failed";
  }
#endif
  ESP_LOGW(TAG, "crash records cleared; coredump %s", cd);
  char out[64];
  std::snprintf(out, sizeof(out), "crash records cleared; coredump %s\n", cd);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, out);
  return ESP_OK;
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
esp_err_t coredump_get(httpd_req_t *req) {
  const char *state = "none";
  const size_t size = coredump_size(&state);
  if (size == 0) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    char out[48];
    std::snprintf(out, sizeof(out), "no core dump (%s)\n", state);
    httpd_resp_sendstr(req, out);
    return ESP_OK;
  }
  constexpr size_t kChunk = 2048;
  char *chunk = static_cast<char *>(std::malloc(kChunk));
  if (chunk == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
  esp_err_t err = ESP_OK;
  for (size_t off = 0; off < size && err == ESP_OK; off += kChunk) {
    const size_t n = std::min(kChunk, size - off);
    err = esp_partition_read(g_cd_part, off, chunk, n);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, chunk, static_cast<ssize_t>(n));
  }
  std::free(chunk);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "/coredump aborted: %s", esp_err_to_name(err));
    return ESP_FAIL;  // httpd closes the socket; the client sees a short read
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}
#endif

#if CONFIG_NBP_CRASH_TEST
// volatile so the compiler can't see the null and replace the store with a
// trap: the point is a real StoreProhibited with a real backtrace.
volatile uintptr_t g_crash_addr = 0;

__attribute__((noinline)) void crash_test_null() {
  *reinterpret_cast<volatile uint32_t *>(g_crash_addr) = 0xDEADBEEF;
}

void crash_test_task(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(500));  // let the HTTP reply leave first
  if (reinterpret_cast<uintptr_t>(arg) == 1) {
    abort();
  }
  crash_test_null();
  vTaskDelete(nullptr);
}

esp_err_t crash_test_post(httpd_req_t *req) {
  char kind[12];
  char confirm[8];
  query_is(req, "kind", nullptr, kind, sizeof(kind));
  if (!query_is(req, "confirm", "yes", confirm, sizeof(confirm)) ||
      (std::strcmp(kind, "null") != 0 && std::strcmp(kind, "abort") != 0)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "POST /crash-test?kind=null|abort&confirm=yes");
    return ESP_FAIL;
  }
  const uintptr_t k = std::strcmp(kind, "abort") == 0 ? 1 : 0;
  ESP_LOGW(TAG, "crash test requested (%s); panicking in 500 ms", kind);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, "panicking in 500 ms\n");
  if (xTaskCreate(&crash_test_task, "crash_test", 3072, reinterpret_cast<void *>(k), 5,
                  nullptr) != pdPASS) {
    ESP_LOGE(TAG, "crash test: xTaskCreate failed");
  }
  return ESP_OK;
}
#endif
#endif  // CONFIG_NBP_WIFI

}  // namespace

void init() {
  const esp_app_desc_t *d = esp_app_get_description();
  std::memcpy(s_elf_sha, d->app_elf_sha256, sizeof(s_elf_sha));

  if (!store_valid()) {
    store_reset();  // power-on or never written: RTC RAM is garbage
  } else if (s_store.count != s_store.reported) {
    const uint32_t fresh = s_store.count - s_store.reported;
    ESP_LOGE(TAG, "%lu panic(s) since the last boot report (%lu since power-on); "
             "GET /crash for details",
             static_cast<unsigned long>(fresh), static_cast<unsigned long>(s_store.count));
    // oldest first, at most kSlots
    const uint32_t n = std::min<uint32_t>(fresh, kSlots);
    for (uint32_t k = n; k > 0; --k) {
      log_record(s_store.slot[(s_store.count - k) % kSlots]);
    }
    s_store.reported = s_store.count;
    s_store.crc = store_crc();
  }

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  ensure_coredump_partition();
  const char *state = "none";
  const size_t cd = coredump_size(&state);
  if (cd != 0) {
    ESP_LOGW(TAG, "core dump in flash: %u bytes (GET /coredump, POST /crash?erase=1)",
             static_cast<unsigned>(cd));
  } else if (std::strcmp(state, "none") != 0) {
    ESP_LOGW(TAG, "core dump in flash is unreadable (%s)", state);
  }
#endif
}

void register_endpoints(httpd_handle_t srv) {
#if CONFIG_NBP_WIFI
  if (srv == nullptr) return;
  httpd_uri_t get = {};  // zero the optional websocket fields
  get.uri = "/crash";
  get.method = HTTP_GET;
  get.handler = &crash_get;
  httpd_uri_t post = {};  // zero the optional websocket fields
  post.uri = "/crash";
  post.method = HTTP_POST;
  post.handler = &crash_post;
  if (httpd_register_uri_handler(srv, &get) != ESP_OK ||
      httpd_register_uri_handler(srv, &post) != ESP_OK) {
    ESP_LOGE(TAG, "failed to register /crash");
  }
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  httpd_uri_t cd = {};  // zero the optional websocket fields
  cd.uri = "/coredump";
  cd.method = HTTP_GET;
  cd.handler = &coredump_get;
  if (httpd_register_uri_handler(srv, &cd) != ESP_OK) {
    ESP_LOGE(TAG, "failed to register /coredump");
  }
#endif
#if CONFIG_NBP_CRASH_TEST
  httpd_uri_t test = {};  // zero the optional websocket fields
  test.uri = "/crash-test";
  test.method = HTTP_POST;
  test.handler = &crash_test_post;
  if (httpd_register_uri_handler(srv, &test) != ESP_OK) {
    ESP_LOGE(TAG, "failed to register /crash-test");
  }
#endif
#else
  (void)srv;
#endif
}

}  // namespace crash_capture

extern "C" void __wrap_esp_panic_handler(panic_info_t *info) {
  crash_capture::capture(info);
  __real_esp_panic_handler(info);
}

#else  // !CONFIG_NBP_CRASH_CAPTURE

namespace crash_capture {
void init() {}
void register_endpoints(httpd_handle_t) {}
}  // namespace crash_capture

#endif
