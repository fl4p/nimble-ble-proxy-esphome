// Liveness watchdog: a supervisor task that probes upstream LAN reachability
// and recovers the device after sustained failure. Recovers a total
// network wedge (WiFi/coex livelock under heavy forwarded throughput) where
// tasks stay alive — so the IDF Task-WDT never fires — but the IP path is dead.
// A cycle is good if the STA gateway (TCP :80) or the optional extra target
// answers within 1 s, or an upstream API session that has been open for at
// least max(60 s, 2 × interval_s), and whose sends are being ACKed, sent data
// recently. `threshold` bad cycles among the last `threshold + 2` first force
// a WiFi re-association, and if that recurs within 16 cycles, a reboot
// (liveness_policy.h). An esp_timer heartbeat reboots if the task itself
// stops ticking (blocked in lwIP). Session age matters: a client
// that keeps reconnecting is a symptom of a bad uplink, and its handshakes
// must not hold off the reboot. Reboots are armed only after a probe target
// answered once this boot.

#pragma once

#include <cstddef>
#include <cstdint>

namespace liveness_wdt {

// Spawn the probe task. Loads enabled/interval/threshold from NVS first.
// Idempotent — a second call is a no-op. No-op when CONFIG_NBP_LIVENESS_WDT=n.
void start();

// Where to read the last receive time (esp_timer µs, 0 = none) of a live API
// session at least `min_age_us` old. Recent traffic from such a session counts
// as a passed cycle. Call before start().
void set_client_rx_source(int64_t (*stable_rx_us)(int64_t min_age_us));

struct State {
  bool enabled;
  uint32_t interval_s;     // seconds between probe cycles
  uint32_t threshold;      // bad cycles (in the window) that trigger an action
  uint32_t failures;       // bad cycles in the current window
  uint32_t window;         // window size in cycles (threshold << backoff, + 2)
  uint32_t reconnects;     // forced WiFi re-associations this boot
  uint32_t slow_ms;        // a probe slower than this counts as bad
  bool armed;              // a probe target answered this boot; reboots possible
  uint32_t last_ok_age_s;  // seconds since the last reachable cycle (UINT32_MAX if never)
};

State get_state();

// Update runtime params and persist to NVS. Pass -1 for any field to leave it
// unchanged. Returns nullptr on success or a static error string.
const char *set_params(int enabled, int interval_s, int threshold);

// Force the failure counter to the threshold so the next tick reboots — proves
// the recovery path end-to-end without having to wedge the network.
void force_fail_test();

// One WiFi-quality sample, recorded once per probe cycle (cadence = interval_s,
// so the ring spans one hour at the 30 s default).
struct WifiSample {
  int8_t sta_rssi;    // uplink: STA→home-AP RSSI, dBm; 0 = unknown
  int8_t ap_rssi;     // downlink: weakest associated client, dBm; 0 = none
  uint16_t rtt_ms;    // uplink reachability RTT; 0xFFFF = probe failed (loss)
  uint8_t sta_count;  // associated clients
};

// Copy the most-recent samples, oldest→newest, into out[] (up to `max`).
// Returns the number written; *interval_s (if non-null) gets the sample cadence.
size_t get_history(WifiSample *out, size_t max, uint32_t *interval_s);

}  // namespace liveness_wdt
