// Heap-exhaustion handling: make an allocation failure an error the caller
// handles, never an abort, wherever our code allocates.
//
// Why this exists. The firmware builds without C++ exceptions
// (CONFIG_COMPILER_CXX_EXCEPTIONS=n). libstdc++'s operator new then cannot
// throw std::bad_alloc: the throw lands in IDF's __wrap___cxa_allocate_
// exception stub and abort()s. That is what killed the field proxy on
// 2026-11-12 (std::make_unique in gatt_discovery, heap_min 244 B). The
// `if (p == nullptr)` after a throwing new is dead code.
//
// Worse, `new (std::nothrow)` aborts too. Since GCC 9, libstdc++ implements
// the nothrow form as `try { return ::operator new(n); } catch (...) {
// return nullptr; }`, and with the throw stubbed to abort() the catch is
// never reached (verified in the 0.2.3 ELF: _ZnwjRKSt9nothrow_t calls
// _Znwj). So this component replaces the global allocation functions
// (oom_guard.cpp, linked WHOLE_ARCHIVE, checked after every link by
// check_new_override.py):
//
//   - nothrow new / new[]: malloc, nullptr on failure. Honest nothrow.
//   - throwing new / new[] (used by NimBLE-cpp and libstdc++ containers,
//     which we cannot change): on failure, wait for the heap to recover
//     (CONFIG_NBP_OOM_NEW_WAIT_MS, task context only), then abort with
//     "OOM: operator new(N) ..." so the crash record names the cause. It
//     must not return nullptr: callers of the throwing form never check,
//     and GCC assumes the result is non-null.
//
// Our own code therefore allocates only through the helpers below (or
// malloc/heap_caps_malloc) and handles nullptr.
//
// Counting: init() registers a heap_caps failed-allocation hook, so count()
// is every failed heap allocation on the device (ours, lwIP, WiFi, NimBLE),
// including ones the caller handles. /stats.json reports it as "oom".

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

namespace oom_guard {

// Register the heap failed-allocation hook and the periodic OOM logger.
// Call once, early in app_main. Allocation failures before init() are not
// counted (the operator new override works regardless).
void init();

// Failed heap allocations since boot (every heap_caps_* / malloc caller).
uint32_t count();

// Size and allocator function of the most recent failure (0 / nullptr if
// none). The name is the heap function that failed (e.g.
// "heap_caps_malloc"), not the caller.
size_t last_size();
const char *last_function();

// Throwing operator new calls that failed, waited, and then succeeded.
uint32_t new_recovered();

// new (std::nothrow) T(args...). nullptr on OOM. With the operator new
// override in this component this really is nothrow.
template <typename T, typename... Args>
T *make(Args &&...args) {
  static_assert(alignof(T) <= alignof(std::max_align_t),
                "over-aligned new uses the aligned operator new, which is "
                "not overridden");
  return new (std::nothrow) T(std::forward<Args>(args)...);
}

// std::make_unique<T>() without the abort. Value-initialises like
// std::make_unique. Check the result for nullptr.
template <typename T>
std::unique_ptr<T> make_unique() {
  return std::unique_ptr<T>(make<T>());
}

}  // namespace oom_guard
