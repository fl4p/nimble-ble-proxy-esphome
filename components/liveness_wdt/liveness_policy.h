// Liveness watchdog decision logic. Pure, no IDF dependencies, so it builds
// on the host (host_test/test_liveness_policy.cpp).
//
// Why a window and not a consecutive count (2026-11-17, .231): on a marginal
// uplink (-70 dBm, the AP dropping us for missing ACKs) 30-70 % of probe
// cycles failed and the ones that passed took up to 1.5 s (healthy: 7-32 ms).
// HTTP, the API sessions and ICMP were dead for 15-20 min at a time, but one
// lucky probe every few cycles reset "6 in a row", so recovery took ~20 min
// instead of ~3. Now a cycle is bad if the probe failed OR was slow, and the
// watchdog acts on `threshold` bad cycles among the last `threshold + 2`.
//
// Escalation: the first trigger forces a WiFi re-association (every recovery
// on 2026-11-17 came right after one; it costs a few seconds and keeps the
// BLE links). A trigger again within RECONNECT_MEMORY_CYCLES of that reboots.

#pragma once

#include <cstddef>
#include <cstdint>

namespace liveness_wdt {
namespace policy {

// A probe that answered slower than this counts as a bad cycle. Healthy on
// .231 is 7-32 ms; a stalled link answered in 700-1500 ms when it answered.
constexpr uint16_t SLOW_RTT_MS = 1000;
// Extra cycles in the window beyond the threshold: 6 bad of the last 8.
constexpr uint32_t WINDOW_SLACK = 2;
// Largest window (threshold << backoff can reach 60 << 5).
constexpr uint32_t MAX_WINDOW = 2048;
// After a forced reconnect, a trigger within this many connected cycles
// reboots instead of reconnecting again.
constexpr uint32_t RECONNECT_MEMORY_CYCLES = 16;

enum class Action : uint8_t { NONE, RECONNECT, REBOOT };

// rtt_ms is ignored when probe_ok is false. client_ok: an upstream API
// session vouched for the path this cycle (see stable_session.h).
inline bool cycle_bad(bool probe_ok, uint16_t rtt_ms, bool client_ok) {
  if (probe_ok && rtt_ms <= SLOW_RTT_MS) return false;
  return !client_ok;
}

class Window {
 public:
  // Size for a given effective threshold (threshold << backoff).
  static uint32_t size_for(uint32_t thr) {
    uint32_t w = thr + WINDOW_SLACK;
    return w > MAX_WINDOW ? MAX_WINDOW : w;
  }

  void reset() {
    n_ = 0;
    head_ = 0;
    bad_ = 0;
    for (auto &b : bits_) b = 0;
  }

  // Push one connected cycle's verdict into a window of `size` cycles.
  void push(bool bad, uint32_t size) {
    if (size == 0) size = 1;
    if (size > MAX_WINDOW) size = MAX_WINDOW;
    if (size != size_) {  // threshold changed: start over rather than mix sizes
      size_ = size;
      reset();
    }
    if (n_ == size_) {
      if (get(head_)) --bad_;
    } else {
      ++n_;
    }
    set(head_, bad);
    if (bad) ++bad_;
    head_ = (head_ + 1) % size_;
  }

  uint32_t bad() const { return bad_; }
  uint32_t filled() const { return n_; }
  uint32_t size() const { return size_; }

 private:
  bool get(uint32_t i) const { return (bits_[i / 8] >> (i % 8)) & 1u; }
  void set(uint32_t i, bool v) {
    if (v) bits_[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    else bits_[i / 8] &= static_cast<uint8_t>(~(1u << (i % 8)));
  }
  uint8_t bits_[MAX_WINDOW / 8] = {};
  uint32_t size_ = 0, n_ = 0, head_ = 0, bad_ = 0;
};

// The per-cycle state machine. Only connected cycles are fed to it; while
// the STA is disassociated the native reconnect path owns recovery, and the
// window keeps what it had (a link that flaps between bad associations
// still accumulates toward a trigger).
class Decider {
 public:
  // `thr` = effective threshold (configured << backoff). `armed` = a probe
  // target answered at least once (this boot or carried); an unarmed
  // watchdog records but never acts.
  Action on_cycle(bool bad, uint32_t thr, bool armed) {
    if (thr == 0) thr = 1;
    win_.push(bad, Window::size_for(thr));
    if (since_reconnect_ < UINT32_MAX) ++since_reconnect_;
    if (!armed || win_.bad() < thr) return Action::NONE;
    win_.reset();
    if (reconnected_ && since_reconnect_ <= RECONNECT_MEMORY_CYCLES) return Action::REBOOT;
    reconnected_ = true;
    since_reconnect_ = 0;
    ++reconnects_;
    return Action::RECONNECT;
  }

  uint32_t bad() const { return win_.bad(); }
  uint32_t window() const { return win_.size(); }
  uint32_t reconnects() const { return reconnects_; }

 private:
  Window win_;
  bool reconnected_ = false;
  uint32_t since_reconnect_ = UINT32_MAX;
  uint32_t reconnects_ = 0;
};

}  // namespace policy
}  // namespace liveness_wdt
