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
#include "http_guard.h"

#include "sdkconfig.h"

#if CONFIG_NBP_CRASH_CAPTURE

#if CONFIG_ESP_PANIC_HANDLER_IRAM
// That option keeps IDF's panic handler in IRAM so it works with the flash
// cache off. __wrap_esp_panic_handler and everything it calls run from flash
// and would fault there, so the wrapper would defeat the option.
#error "NBP_CRASH_CAPTURE runs from flash; disable ESP_PANIC_HANDLER_IRAM or NBP_CRASH_CAPTURE"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdarg>
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
#include "esp_random.h"
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
constexpr uint8_t F_NESTED = 8;     // a second panic hit while this one was
                                    // captured or handled (record may be
                                    // partial; the core dump, if any, is of
                                    // the second fault)

struct Record {
  uint32_t seq;        // panic number since the store was initialised
  uint32_t uptime_ms;  // time since boot when it panicked
  uint32_t nonce;      // random; also written into the core dump (CoreTag)
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
  uint32_t version;   // kVersion: layout number and sizeof(Store)
  uint32_t count;     // panics recorded since the store was initialised
  uint32_t reported;  // `count` value already logged at boot
  Record slot[kSlots];
  uint32_t crc;  // over everything above
};

constexpr uint32_t kMagic = 0x4E425043u;  // "NBPC"
// Any change to Record/Store must bump kLayout; the size is folded in too so
// a field change that forgets the bump still reads as a different layout.
constexpr uint32_t kLayout = 2;
static_assert(sizeof(Store) < 0x10000, "Store size must fit the version field");
constexpr uint32_t kVersion = (kLayout << 16) | static_cast<uint32_t>(sizeof(Store));

RTC_NOINIT_ATTR Store s_store;

// Copied into every core dump (COREDUMP_DRAM_ATTR sections always are), so a
// decoder can tell which record a core belongs to. Same PC and task is not
// enough: the same bug crashes at the same place every time. Reloaded from
// the image at boot, so a panic that skipped capture() leaves nonce 0.
struct CoreTag {
  char magic[8];  // "NBPCTAG1"
  uint32_t seq;
  uint32_t uptime_ms;
  uint32_t nonce;
};
COREDUMP_DRAM_ATTR volatile CoreTag g_core_tag = {{'N', 'B', 'P', 'C', 'T', 'A', 'G', '1'}, 0, 0, 0};

// Panic-time state. The capture path runs from flash like IDF's own
// esp_panic_handler: the port-level handler re-enables the flash cache first
// (panic_enable_cache), so nothing here needs IRAM. With
// CONFIG_ESP_PANIC_HANDLER_IRAM that is no longer true (see the #error).
//
// The record is built in s_scratch (DRAM) and only then committed to the RTC
// store, so a fault while walking the stack never leaves the store half
// written. commit() is idempotent: a nested entry (fault inside capture or
// inside IDF's handler/core dump, or the other core panicking while this one
// was stalled mid-capture) re-runs or finishes it, so the CRC always matches
// before IDF resets the chip.
enum Phase : uint32_t { kIdle = 0, kBuilding = 1, kCommitting = 2, kDone = 3 };
Record s_scratch;
volatile uint32_t s_owner = 0;  // 0, or 1 + the core that is capturing
volatile uint32_t s_phase = kIdle;
volatile uint32_t s_nest = 0;
bool s_base_valid = false;  // store was valid when this capture started
uint32_t s_new_count = 0;   // count to commit

// Boot-time view, set in init().
esp_reset_reason_t s_reset = ESP_RST_UNKNOWN;
const char *s_store_state = "ok";  // ok | uninitialised | old_layout | corrupt
bool s_store_was_valid = false;
bool s_crash_without_record = false;

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

#if NBP_CRASH_SELFTEST
// QEMU-only fault injection (components/crash_capture/CMakeLists.txt,
// -DNBP_CRASH_SELFTEST=1). Never set in a firmware build.
volatile uintptr_t g_st_null = 0;
volatile uint32_t g_st_fault_phase = 0;  // fault once inside this phase
volatile bool g_st_skip = false;         // panic without capturing
#define ST_FAULT(p)                                                   \
  do {                                                                \
    if (g_st_fault_phase == (p)) {                                    \
      g_st_fault_phase = 0;                                           \
      *reinterpret_cast<volatile uint32_t *>(g_st_null) = 0xBADC0DE;  \
    }                                                                 \
  } while (0)
#else
#define ST_FAULT(p) \
  do {              \
  } while (0)
#endif

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

// A record as read back from RTC memory: strings terminated, depth clamped.
// RTC memory is not trusted beyond the CRC (a CRC only proves the bytes are
// the ones written, not that the writer filled every field).
Record sane(const Record &in) {
  Record r = in;
  r.task[sizeof(r.task) - 1] = '\0';
  r.reason[sizeof(r.reason) - 1] = '\0';
  r.details[sizeof(r.details) - 1] = '\0';
  if (r.depth > kFrames) r.depth = kFrames;
  return r;
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

// Write s_scratch into the store. Idempotent, so a nested entry can re-run
// it; the CRC is the last write.
void commit() {
  if (!s_base_valid) {
    std::memset(&s_store, 0, sizeof(s_store));
    s_store.magic = kMagic;
    s_store.version = kVersion;
  }
  s_store.slot[(s_new_count - 1) % kSlots] = s_scratch;
  s_store.count = s_new_count;
  ST_FAULT(kCommitting);
  s_store.crc = store_crc();
}

// Second entry while a capture is in progress or done: leave the store valid.
void recover() {
  s_nest = s_nest + 1;
  if (s_nest > 2) return;  // the repair itself keeps faulting
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  switch (s_phase) {
    case kBuilding:  // keep what was captured so far
      s_scratch.flags |= F_NESTED;
      s_phase = kCommitting;
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
      commit();
      s_phase = kDone;
      break;
    case kCommitting:  // s_scratch is complete; finish writing it
      s_scratch.flags |= F_NESTED;
      commit();
      s_phase = kDone;
      break;
    case kDone:  // panic inside IDF's handler (e.g. the core dump)
      if (store_valid() && s_store.count != 0) {
        s_store.slot[(s_store.count - 1) % kSlots].flags |= F_NESTED;
        s_store.crc = store_crc();
      }
      break;
    default:  // between taking ownership and starting; nothing written
      break;
  }
}

void capture(const panic_info_t *info) {
#if NBP_CRASH_SELFTEST
  if (g_st_skip) return;
#endif
  const uint32_t me = static_cast<uint32_t>(esp_cpu_get_core_id()) + 1;
  if (!esp_cpu_compare_and_set(&s_owner, 0, me)) {
    // Nested panic on this core, or the other core panicked while this one
    // was stalled mid-capture (IDF stalls the other core before calling
    // esp_panic_handler, so the owner cannot still be running).
    recover();
    return;
  }
  Record &r = s_scratch;
  std::memset(&r, 0, sizeof(r));
  s_base_valid = store_valid();
  s_new_count = (s_base_valid ? s_store.count : 0) + 1;
  r.seq = s_new_count;
  r.uptime_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  r.nonce = esp_random() | 1u;  // never 0 (0 = "no capture")
  g_core_tag.seq = r.seq;
  g_core_tag.uptime_ms = r.uptime_ms;
  g_core_tag.nonce = r.nonce;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  s_phase = kBuilding;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  r.core = static_cast<uint8_t>(info->core);
  // Read the SHA here, not at init(): a panic before init() still names its
  // image. esp_app_desc lives in flash .rodata; the cache is on.
  std::memcpy(r.elf_sha, esp_app_get_description()->app_elf_sha256, sizeof(r.elf_sha));
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
  ST_FAULT(kBuilding);
  if (info->frame != nullptr) capture_frame(r, info->frame);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  s_phase = kCommitting;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  commit();
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  s_phase = kDone;
}

const char *reset_name(esp_reset_reason_t r) {
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
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
  }
}

// Resets that mean "the firmware crashed": each should have left a record.
bool is_crash_reset(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
         r == ESP_RST_WDT || r == ESP_RST_CPU_LOCKUP;
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

const uint8_t *running_elf_sha() { return esp_app_get_description()->app_elf_sha256; }

// "0xPC:0xSP 0xPC:0xSP …" (the format IDF prints and idf.py monitor decodes).
// `r` must be sane() (depth <= kFrames).
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

void log_record(const Record &raw) {
  const Record r = sane(raw);
  char sha[17];
  sha_hex(r.elf_sha, sha);
  const bool same = std::memcmp(r.elf_sha, running_elf_sha(), sizeof(r.elf_sha)) == 0;
  ESP_LOGE(TAG, "panic #%lu at uptime %lu.%03lu s, core %u, task \"%s\": %s (%s)%s",
           static_cast<unsigned long>(r.seq),
           static_cast<unsigned long>(r.uptime_ms / 1000),
           static_cast<unsigned long>(r.uptime_ms % 1000), r.core, r.task,
           r.reason, exception_name(r.exception),
           (r.flags & F_NESTED) ? " [nested panic during capture/handler]" : "");
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

// Size of a stored, checksum-valid core dump, else 0. *state is
//   none          no region, or the region is blank/erased
//   ok            a core of `size` bytes with a valid checksum
//   invalid       the header is unreadable/implausible (*err says why)
//   bad_checksum  the header is plausible but the image checksum fails
// Only "none" means there is nothing there; the other two are evidence of a
// core that could not be written or has been overwritten. GET
// /coredump?raw=1 serves the region regardless.
size_t coredump_size(const char **state, esp_err_t *err) {
  *state = "none";
  *err = ESP_OK;
  if (g_cd_part == nullptr) return 0;
  size_t addr = 0, size = 0;
  const esp_err_t e = esp_core_dump_image_get(&addr, &size);
  if (e == ESP_ERR_NOT_FOUND) return 0;
  if (e != ESP_OK) {
    *state = "invalid";
    *err = e;
    return 0;
  }
  const esp_err_t c = esp_core_dump_image_check();
  if (c != ESP_OK) {
    *state = "bad_checksum";
    *err = c;
    return 0;
  }
  *state = "ok";
  return size;
}
#endif

#if CONFIG_NBP_WIFI || NBP_CRASH_SELFTEST
// Streams JSON through a small buffer to a sink (an httpd chunk, or stdout
// for the QEMU self-test), so the size of the reply is not capped. A piece
// that does not fit its format buffer is a bug: it sets err and the handler
// fails the request instead of sending a truncated document.
struct JsonOut {
  esp_err_t (*sink)(void *ctx, const char *p, size_t n);
  void *ctx;
  esp_err_t err;
  size_t n;
  char buf[512];

  void flush() {
    if (n != 0 && err == ESP_OK) err = sink(ctx, buf, n);
    n = 0;
  }
  void put(const char *s, size_t len) {
    while (len != 0 && err == ESP_OK) {
      if (n == sizeof(buf)) flush();
      const size_t k = std::min(len, sizeof(buf) - n);
      std::memcpy(buf + n, s, k);
      n += k;
      s += k;
      len -= k;
    }
  }
  __attribute__((format(printf, 2, 3))) void printf(const char *fmt, ...) {
    char tmp[192];
    va_list ap;
    va_start(ap, fmt);
    const int k = std::vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (k < 0 || static_cast<size_t>(k) >= sizeof(tmp)) {
      err = ESP_ERR_INVALID_SIZE;
      return;
    }
    put(tmp, static_cast<size_t>(k));
  }
  // JSON string literal, quotes included; `s` must be NUL-terminated.
  void str(const char *s) {
    put("\"", 1);
    for (; *s != '\0'; ++s) {
      const unsigned char c = static_cast<unsigned char>(*s);
      if (c == '"' || c == '\\') {
        const char e[2] = {'\\', static_cast<char>(c)};
        put(e, 2);
      } else if (c < 0x20) {
        printf("\\u%04x", c);
      } else {
        put(s, 1);
      }
    }
    put("\"", 1);
  }
};

void write_crash_json(JsonOut &o) {
  char sha[17];
  sha_hex(running_elf_sha(), sha);
  const bool valid = store_valid();
  o.printf("{\"running_elf\":\"%s\",\"last_reset_reason\":\"%s\",\"store_was_valid\":%s,"
           "\"store_state\":\"%s\",\"crash_without_record\":%s,\"count\":%lu,\"records\":[",
           sha, reset_name(s_reset), s_store_was_valid ? "true" : "false", s_store_state,
           s_crash_without_record ? "true" : "false",
           static_cast<unsigned long>(valid ? s_store.count : 0));
  if (valid) {
    bool first = true;
    // newest first
    for (uint32_t k = 0; k < kSlots && k < s_store.count; ++k) {
      const Record r = sane(s_store.slot[(s_store.count - 1 - k) % kSlots]);
      if (r.seq == 0) continue;
      char rsha[17];
      sha_hex(r.elf_sha, rsha);
      o.printf("%s{\"seq\":%lu,\"uptime_ms\":%lu,\"nonce\":\"%08lx\",\"core\":%u,"
               "\"exception\":\"%s\",\"task\":",
               first ? "" : ",", static_cast<unsigned long>(r.seq),
               static_cast<unsigned long>(r.uptime_ms), static_cast<unsigned long>(r.nonce),
               r.core, exception_name(r.exception));
      first = false;
      o.str(r.task);
      o.printf(",\"reason\":");
      o.str(r.reason);
      o.printf(",\"details\":");
      o.str(r.details);
      o.printf(",\"pc\":\"0x%08lx\",\"addr\":\"0x%08lx\",\"cause\":%lu,"
               "\"sp\":\"0x%08lx\",\"ra\":\"0x%08lx\",\"elf\":\"%s\",",
               static_cast<unsigned long>(r.pc), static_cast<unsigned long>(r.addr),
               static_cast<unsigned long>(r.cause), static_cast<unsigned long>(r.sp),
               static_cast<unsigned long>(r.ra), rsha);
      o.printf("\"elf_matches_running\":%s,\"corrupted\":%s,\"truncated\":%s,"
               "\"stack_scan\":%s,\"nested\":%s,\"backtrace\":\"",
               std::memcmp(r.elf_sha, running_elf_sha(), 8) == 0 ? "true" : "false",
               (r.flags & F_CORRUPTED) ? "true" : "false",
               (r.flags & F_TRUNCATED) ? "true" : "false",
               (r.flags & F_HEURISTIC) ? "true" : "false",
               (r.flags & F_NESTED) ? "true" : "false");
      for (int i = 0; i < r.depth; ++i) {
        o.printf("%s0x%08lx:0x%08lx", i == 0 ? "" : " ",
                 static_cast<unsigned long>(r.bt_pc[i]),
                 static_cast<unsigned long>(r.bt_sp[i]));
      }
      o.printf("\"}");
    }
  }
  o.printf("]");
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  const char *state = "none";
  esp_err_t err = ESP_OK;
  const size_t cd = coredump_size(&state, &err);
  o.printf(",\"coredump\":{\"region\":\"%s\",\"part_addr\":\"0x%lx\",\"part_size\":%lu,"
           "\"state\":\"%s\",\"size\":%u",
           g_cd_region, static_cast<unsigned long>(g_cd_part ? g_cd_part->address : 0),
           static_cast<unsigned long>(g_cd_part ? g_cd_part->size : 0), state,
           static_cast<unsigned>(cd));
  if (err != ESP_OK) o.printf(",\"error\":\"%s\"", esp_err_to_name(err));
  o.printf("}");
#else
  o.printf(",\"coredump\":null");
#endif
  o.printf("}\n");
  o.flush();
}
#endif  // CONFIG_NBP_WIFI || NBP_CRASH_SELFTEST

#if CONFIG_NBP_WIFI
esp_err_t httpd_sink(void *ctx, const char *p, size_t n) {
  return httpd_resp_send_chunk(static_cast<httpd_req_t *>(ctx), p, static_cast<ssize_t>(n));
}

esp_err_t crash_get(httpd_req_t *req) {
  auto *o = static_cast<JsonOut *>(std::malloc(sizeof(JsonOut)));
  if (o == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    return ESP_FAIL;
  }
  o->sink = &httpd_sink;
  o->ctx = req;
  o->err = ESP_OK;
  o->n = 0;
  httpd_resp_set_type(req, "application/json");
  write_crash_json(*o);
  const esp_err_t err = o->err;
  std::free(o);
  if (err != ESP_OK) {
    // Headers and part of the body are out; returning an error makes httpd
    // close the socket, so the client sees an unterminated chunked reply
    // (an error), never a short document with 200.
    ESP_LOGE(TAG, "GET /crash failed: %s", esp_err_to_name(err));
    return ESP_FAIL;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

bool query_is(httpd_req_t *req, const char *key, const char *want, char *val,
              size_t cap) {
  char q[96];
  val[0] = '\0';
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
  if (httpd_query_key_value(q, key, val, cap) != ESP_OK) return false;
  return want == nullptr || std::strcmp(val, want) == 0;
}

// Destructive endpoints need `X-Crash-Confirm: yes`. A custom header is not
// CORS-safelisted, so a page on another origin cannot send it without a
// preflight, which this server never answers: a drive-by fetch() or form
// cannot erase the evidence or panic the device.
constexpr const char *kConfirmHdr = "X-Crash-Confirm";

bool confirmed(httpd_req_t *req) {
  char v[8];
  return httpd_req_get_hdr_value_str(req, kConfirmHdr, v, sizeof(v)) == ESP_OK &&
         std::strcmp(v, "yes") == 0;
}

esp_err_t crash_post(httpd_req_t *req) {
  char v[16];
  if (!query_is(req, "erase", "1", v, sizeof(v)) || !confirmed(req)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "use POST /crash?erase=1 with header X-Crash-Confirm: yes");
    return ESP_FAIL;
  }
  store_reset();
  s_crash_without_record = false;
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
// GET /coredump: the stored core if its checksum is valid, else 404.
// GET /coredump?raw=1: the region as it is, whatever its state, so a core
// that fails the checksum (or a region of garbage) can still be inspected.
// Size: the header's size if plausible, else the whole region.
esp_err_t coredump_get(httpd_req_t *req) {
  char v[8];
  const bool raw = query_is(req, "raw", "1", v, sizeof(v));
  const char *state = "none";
  esp_err_t cerr = ESP_OK;
  size_t size = coredump_size(&state, &cerr);
  if (size == 0 && raw && g_cd_part != nullptr) {
    size_t addr = 0, hsize = 0;
    size = (esp_core_dump_image_get(&addr, &hsize) == ESP_OK && hsize != 0 &&
            hsize <= g_cd_part->size)
               ? hsize
               : g_cd_part->size;
  }
  if (size == 0) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    char out[96];
    std::snprintf(out, sizeof(out), "no core dump (%s%s%s); ?raw=1 serves the region\n",
                  state, cerr != ESP_OK ? ": " : "",
                  cerr != ESP_OK ? esp_err_to_name(cerr) : "");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
  }
  // Up to the whole 128 KiB region: refuse while the heap is low, and keep
  // at most two segments queued in lwIP while it streams.
  if (!http_guard::bulk_ok(req)) return ESP_OK;
  constexpr size_t kChunk = 2048;
  char *chunk = static_cast<char *>(std::malloc(kChunk));
  if (chunk == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "X-Coredump-State", state);
  httpd_resp_set_hdr(req, "Content-Disposition",
                     raw ? "attachment; filename=\"coredump-raw.bin\""
                         : "attachment; filename=\"coredump.bin\"");
  esp_err_t err = ESP_OK;
  for (size_t off = 0; off < size && err == ESP_OK; off += kChunk) {
    const size_t n = std::min(kChunk, size - off);
    err = esp_partition_read(g_cd_part, off, chunk, n);
    if (err == ESP_OK) err = http_guard::send_chunk_paced(req, chunk, n);
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
  query_is(req, "kind", nullptr, kind, sizeof(kind));
  if (!confirmed(req) ||
      (std::strcmp(kind, "null") != 0 && std::strcmp(kind, "abort") != 0)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "POST /crash-test?kind=null|abort with header X-Crash-Confirm: yes");
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

#if NBP_CRASH_SELFTEST
// QEMU self-test, one step per boot (RTC_NOINIT step counter). Each boot
// prints the /crash JSON between markers and checks what the previous step
// left behind. Steps: 0 plain panic; 1 fault while committing; 2 fault while
// building; 3 corrupt the store's CRC and restart; 4 panic with capture
// skipped; 5 done (idle).
RTC_NOINIT_ATTR uint32_t s_st_magic;
RTC_NOINIT_ATTR uint32_t s_st_step;

esp_err_t stdout_sink(void *, const char *p, size_t n) {
  std::printf("%.*s", static_cast<int>(n), p);
  return ESP_OK;
}

__attribute__((noinline)) void st_panic() {
  *reinterpret_cast<volatile uint32_t *>(g_st_null) = 0xDEADBEEF;
}

void st_check(uint32_t step, bool ok, const char *what) {
  ESP_LOGE(TAG, "SELFTEST step %lu: %s: %s", static_cast<unsigned long>(step),
           ok ? "PASS" : "FAIL", what);
}

void selftest(uint32_t fresh) {
  if (s_st_magic != 0x53545354u) {
    s_st_magic = 0x53545354u;
    s_st_step = 0;
  }
  const uint32_t step = s_st_step++;
  static JsonOut o;
  o.sink = &stdout_sink;
  o.ctx = nullptr;
  o.err = ESP_OK;
  o.n = 0;
  std::printf("SELFTEST-JSON-BEGIN %lu\n", static_cast<unsigned long>(step));
  write_crash_json(o);
  std::printf("SELFTEST-JSON-END %lu\n", static_cast<unsigned long>(step));
  st_check(step, o.err == ESP_OK, "JSON written");
  // Time for the host to snapshot the flash (this step's core) before the
  // next panic overwrites it.
  vTaskDelay(pdMS_TO_TICKS(4000));
  const bool valid = store_valid();
  const Record newest = sane(s_store.slot[(s_store.count + kSlots - 1) % kSlots]);
  switch (step) {
    case 0:
      ESP_LOGE(TAG, "SELFTEST step 0: plain panic");
      st_panic();
      break;
    case 1:
      st_check(1, s_store_was_valid && s_store.count == 1 && fresh == 1 &&
                      newest.seq == 1 && !(newest.flags & F_NESTED) &&
                      std::strcmp(newest.reason, "StoreProhibited") == 0,
               "record #1 from a plain panic");
      ESP_LOGE(TAG, "SELFTEST step 1: panic, fault while committing");
      g_st_fault_phase = kCommitting;
      st_panic();
      break;
    case 2:
      st_check(2, s_store_was_valid && valid && s_store.count == 2 && fresh == 1 &&
                      newest.seq == 2 && (newest.flags & F_NESTED) && newest.depth > 0 &&
                      s_store.slot[0].seq == 1,
               "commit-phase fault: #1 kept, #2 complete and flagged nested");
      ESP_LOGE(TAG, "SELFTEST step 2: panic, fault while building");
      g_st_fault_phase = kBuilding;
      st_panic();
      break;
    case 3:
      st_check(3, s_store_was_valid && valid && s_store.count == 3 && fresh == 1 &&
                      newest.seq == 3 && (newest.flags & F_NESTED) &&
                      std::strcmp(newest.task, "main") == 0,
               "build-phase fault: #1 #2 kept, partial #3 flagged nested");
      ESP_LOGE(TAG, "SELFTEST step 3: corrupt the store CRC, restart");
      s_store.crc ^= 1;
      esp_restart();
      break;
    case 4:
      st_check(4, !s_store_was_valid && std::strcmp(s_store_state, "corrupt") == 0 &&
                      valid && s_store.count == 0,
               "corrupt store reported, then reset");
      ESP_LOGE(TAG, "SELFTEST step 4: panic without capture");
      g_st_skip = true;
      st_panic();
      break;
    case 5:
      st_check(5, s_reset == ESP_RST_PANIC && s_crash_without_record && s_store.count == 0,
               "crash reset without a record is reported");
      ESP_LOGE(TAG, "SELFTEST done");
      break;
    default:
      break;
  }
}
#endif

}  // namespace

void init() {
  s_reset = esp_reset_reason();
  s_store_was_valid = store_valid();
  uint32_t fresh = 0;
  if (!s_store_was_valid) {
    if (s_store.magic == kMagic && s_store.version != kVersion) {
      s_store_state = "old_layout";
      ESP_LOGW(TAG, "crash store has layout 0x%08lx, this image uses 0x%08lx (firmware "
               "update); its records are dropped",
               static_cast<unsigned long>(s_store.version),
               static_cast<unsigned long>(kVersion));
    } else if (s_store.magic == kMagic) {
      s_store_state = "corrupt";
      ESP_LOGE(TAG, "crash store in RTC memory FAILED its CRC (a panic while it was "
               "written?); up to %d panic record(s) are LOST",
               kSlots);
    } else {
      s_store_state = "uninitialised";
      if (s_reset != ESP_RST_POWERON) {
        ESP_LOGW(TAG, "crash store in RTC memory was not initialised after a %s reset",
                 reset_name(s_reset));
      }
    }
    store_reset();
  } else if (s_store.count != s_store.reported) {
    fresh = s_store.count - s_store.reported;
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
  if (is_crash_reset(s_reset) && fresh == 0) {
    s_crash_without_record = true;
    ESP_LOGE(TAG, "crash reset (%s), no record captured%s", reset_name(s_reset),
             s_store_was_valid ? "" : " (the crash store was not valid)");
  }

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  ensure_coredump_partition();
  const char *state = "none";
  esp_err_t err = ESP_OK;
  const size_t cd = coredump_size(&state, &err);
  if (cd != 0) {
    ESP_LOGW(TAG, "core dump in flash: %u bytes (GET /coredump, POST /crash?erase=1)",
             static_cast<unsigned>(cd));
  } else if (std::strcmp(state, "invalid") == 0) {
    // Usually leftover data from an older image in the region (a fresh
    // flash does not erase it), not a crash: then crash_without_record is
    // false in GET /crash. Only the raw region would tell, and it is the
    // whole partition, so it is not offered here as a first step.
    ESP_LOGI(TAG, "core dump region holds no readable core (%s); likely "
             "old flash content, not a crash. POST /crash?erase=1 clears it",
             esp_err_to_name(err));
  } else if (std::strcmp(state, "none") != 0) {
    ESP_LOGW(TAG, "core dump in flash fails its checksum (%s); GET "
             "/coredump?raw=1 serves the region (%u KiB)", state,
             static_cast<unsigned>(g_cd_part != nullptr ? g_cd_part->size / 1024 : 0));
  }
#endif
#if NBP_CRASH_SELFTEST
  selftest(fresh);
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
