# Thermal investigation — why the board runs warm, and which levers help

Device: ESP32-S3 nimble-ble-proxy at **192.168.1.125** (MAC `9C:13:9E:F4:04:98`).
Steady-state package temp (on-die sensor, `/stats.json` `temp_c`) sits around
**56–58 °C** while running the normal workload: WiFi STA + NimBLE scanner +
clone (proxying a notify stream) + dashboard httpd.

This is **within spec** — the S3 junction limit is ~85–125 °C — so it's a
warm-to-touch / longevity question, not an imminent-failure one.

## TL;DR

- No single dramatic lever, but two that **stack**: the heat is dominated by
  the *always-on* WiFi + BLE radios and the CPU clock.
- **CPU frequency is the biggest lever: ~3 °C** cooler at 80 MHz vs 160 MHz
  (measured). Trade-off: less headroom — the clone can briefly peg a core, so
  80 MHz risks dropped notifies under load.
- **WiFi TX power: ~1.7 °C** from 20 dBm → 13 dBm (measured). Free on a LAN.
- **Combined (80 MHz + 13 dBm) ≈ 4–5 °C** off the top — *in theory*. In
  practice the device sits **low-to-mid 50s °C regardless** (see caveat).

> **Reproducibility caveat.** The on-die temp sensor quantizes to ~1 °C steps
> and was observed wandering across 3 steps (51.6→54.6 °C) at *constant*
> settings — so measurement noise (~±1.5 °C, plus warm-up/ambient drift) is
> comparable to the per-lever deltas below. The back-to-back A/Bs are the most
> trustworthy comparisons, but a follow-up at the "cool combo" (80 MHz +
> 13 dBm) did **not** reproduce a clear drop — it read the same 52–55 °C band.
> Treat the numbers below as indicative, not precise; don't expect a dramatic
> change from any of these knobs.
- **Light-sleep / CPU power-down is a dead end** on this device: NimBLE is
  always active, and the SoC refuses light-sleep while Bluetooth is enabled
  (boot log: *"light sleep mode will not be able to apply when bluetooth is
  enabled"*). So the light-sleep toggle — and the new `pdcpu` (power-down CPU)
  toggle — change nothing thermally here.

## Method

- Each setting is **soaked ~2.7 min** (8 samples × 20 s) so the package
  reaches thermal equilibrium before reading; the last ~3 samples are
  averaged. Single instantaneous readings are unreliable — they're confounded
  by post-reboot warm-up and by clone notify bursts that briefly peg a core.
- The on-die temp sensor reads in ~1 °C steps, so deltas of 1–2 °C are only a
  couple of sensor steps — treat them as approximate.

## Results

### 1. Light-sleep + power-down-CPU (`/cpufreq?ls=`, `?pdcpu=`)

Added a runtime `pdcpu` toggle (`esp_sleep_pd_config(ESP_PD_DOMAIN_CPU, …)`,
NVS key `cpu_pd`, dashboard checkbox) to allow re-enabling light-sleep *without*
the `PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` path that previously hung the device.

- `ls=on` + `pdcpu=off`: **stable** (no hang over the soak) but **0 thermal
  change** — temp held flat at 57.6 °C.
- Reason: light-sleep never actually engages because BLE is always active.
- Conclusion: the toggle is correct and harmless, but **moot** while the proxy
  runs BLE. Only useful on a build with BLE disabled.

### 2. WiFi TX power (`/txpower?wifi=`) — controlled A/B, CPU idle (~2 %)

| WiFi TX | Equilibrium temp |
|--------:|-----------------:|
| 20 dBm  | ~58.3 °C |
| 13 dBm  | ~56.6 °C |

**Δ ≈ 1.7 °C for a 7 dBm drop.** Real but small. 13 dBm is plenty for a home
LAN, so this is a free ~1.7 °C. (An earlier *uncontrolled* 21→14 reading
suggested ~3 °C, but that was confounded by reboot warm-up — the soaked number
is the trustworthy one.)

### 3. CPU frequency (`/cpufreq?mhz=`) — controlled A/B, CPU idle (~2 %)

| CPU freq | Equilibrium temp |
|---------:|-----------------:|
| 160 MHz  | ~54.6 °C |
|  80 MHz  | ~51.6 °C |

**Δ ≈ 3.0 °C** — 80 MHz is the single biggest lever found, bigger than TX
power. (TX power was held constant across the A/B, so the delta is purely the
clock. The first 160 MHz pass was discarded: the board had just been
power-cycled and was still warming from cold — its rising 43→50 °C trace was
warm-up, not equilibrium. The 54.6 °C above is a clean *warm* re-measurement.)

Caveat: cpu0 was idle (~2 %) during the test. The clone's notify proxying can
briefly spike a core to ~100 %; at 80 MHz that headroom is halved, so under a
sustained notify burst 80 MHz could drop notifies. Safe for light workloads;
keep 160 MHz if you want margin.

## Gotchas hit during this investigation

- **Multiple boards on the LAN.** `mDNS nimble-proxy.local` → **192.168.1.231**
  (a *different* board, older firmware). The USB-flashed board is
  **192.168.1.125**. Always confirm the MAC/compile-time before trusting a
  reading.
- **Concurrent reflashing** of `.125` by another session repeatedly invalidated
  measurements (the board reboots offline for the whole soak window).
- **Serial is unreliable** in steady state (USB-Serial/JTAG only emits in the
  post-reset boot window). Prefer the `/log` + `/stats.json` HTTP endpoints.
- **Don't reset repeatedly** — the AP rate-limits re-association, and the board
  can drop off WiFi entirely for a minute or more.
