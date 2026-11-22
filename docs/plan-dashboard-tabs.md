# Plan: group the dashboard into tabs

Status: implemented and deployed to .231, 2026-11-22. Scope: `web/index.html` only (embedded into the
firmware by `components/api_server/CMakeLists.txt`). No firmware endpoint changes.

## Tabs

| Tab | Content (existing element ids) | Shown when |
|---|---|---|
| **Status** (default) | `#chart4` WiFi link | always |
| **Switches** | the switch row now in `#swgroup` (state badge, turn on/off, at-boot policy, info line), moved out of `#tunables` | `/stats.json` has `sw_on` |
| **Settings** | `#tunables`: CPU, WiFi, BLE, device rows (incl. reboot, name, BLE PIN), then `#chart2` CPU/temp right below them (shows the thermal effect of the settings) | always |
| **BLE** | `#chart` BLE activity, then the `#devices` table, `#bondpanel` | BLE build (`.ble-only`) |
| **NAT router / AP** | everything for the repeater in one place, top to bottom: access point (SoftAP on/off, SSID, password), `#chart3` throughput chart, connected clients, static leases, port maps | NAT build (`g_natCap`) |

`#controls` (Connect button, status) and the footer (`#fwinfo`, OTA hint) stay
outside the tabs. The **device log** (`#console`) is not a tab: it sits below
the tab panes and stays visible whichever tab is active. Its height stays
300 px so a tall tab pushes it down rather than squeezing it.

### Switches tab layout

One card per output, so a second channel later is one more card rather than
a redesign. Today there is a single card for GPIO14 (inverter remote contact):

```
 Status | Switches | Settings | BLE | NAT router / AP
 ──────────────────────────────────────────────
 ┌ inverter (GPIO14) ─────────────────────────┐
 │  ● OFF        [ turn on ]  [ turn off ]    │
 │  at boot: [last ▾]                         │
 │  since 12 min · 0 changes since boot       │
 └────────────────────────────────────────────┘
 ── device log ───────────────────────────────
 │ … always visible below the active tab …
```

Confirm dialogs on on/off remain. The card label is fixed text ("inverter")
for now; a per-output name would need a firmware field (out of scope).

## Page layout (fixed)

- Header (Connect line + tab bar) stays at the top; only the active tab
  scrolls.
- Content is a centered column, max 960 px; backgrounds span the window.
- Device log docked at the bottom: click its header to collapse/expand
  (collapsed shows a count of new lines, yellow if warnings), drag its top
  edge to resize; height and collapsed state kept in `localStorage`.

## Mechanics

- **Markup:** a `<nav id=tabs role=tablist>` of `<button role=tab data-tab=…>`
  after `#controls`; each group wrapped in `<section class=tabpane
  id=tab-…>`. Only the active pane is shown (`.hidden`).
- **Active tab** is kept in `location.hash` (`#switches`, `#ble`, …), so a
  reload or bookmark opens the same tab; unknown or unavailable hash →
  Status. Arrow keys move between tabs (role=tab basics).
- **Capability gating:** a tab button stays hidden until its capability is
  known true (`applyCaps()`: `sw_on` → Switches, `g_ble` → BLE, `g_natCap`
  → NAT). If the hash names a tab that turns out unavailable, fall back to
  Status. The existing `.ble-only` / `.nat-only` hiding stays as is.
- **Chart sizing:** uPlot gets width 0 (negative from `chartW`) inside a
  hidden pane. Factor the three repeated `setSize` blocks (lines ~561, ~599,
  resize listener) into one `resizeCharts()` that skips charts whose pane is
  hidden, and call it on every tab switch (Status, Settings, BLE and NAT router / AP all hold charts). `#chart4` is included
  (today it is sized only at creation). Data keeps accumulating while hidden
  because `tickStats` keeps running.
- **Polling while hidden:**
  - `tickStats` (charts, caps), `tickLog`: keep running always. The log is
    always on screen, and the charts are history.
  - `tickDevices` (+ the piggybacked `/clone` read): only while BLE is the
    active tab, immediate tick on switching to it. `/devices` is one of the
    `bulk_ok()`-gated heavy endpoints, so this cuts load on a heap-bound device.
  - `loadSwitch`: only while Switches is active (immediate on show);
    `loadNat` only while NAT is active; `loadBond` only while BLE is active.
  - `tickWifiHist`: unchanged (15 s, feeds a Status chart).
- **BLE transport (Web Bluetooth):** same page, same tabs. NAT and Switches
  stay hidden there because `/switch` and `/nat` are not routed over
  `ble_httpd`. That already follows from the caps once the Switches tab also
  requires `TRANSPORT !== 'ble'`.
- **Narrow screens:** the tab bar scrolls horizontally (`overflow-x:auto`,
  no wrap).

## Steps

1. Factor `resizeCharts()`; no visible change. Check on device.
2. Add tab bar + panes, move existing blocks into panes, device log below
   the panes, hash routing.
3. Move the switch row into the Switches tab as a card; caps gating.
4. Gate the per-tab polls on the active tab.
5. Build `build-psram-sw` (psram + switch fragments), OTA to .231, archive
   the ELF.

## Verification

- `node --check` on the extracted inline script.
- Headless Playwright against .231: every visible tab renders, charts have
  width > 0 after switching to Status from another tab, the device log is
  visible on every tab and after a window
  resize while another tab is active; reload with `#switches` opens
  Switches; switch state matches `GET /switch`. No click on turn on/off.
- Caps: a page-side test that feeds `applyCaps()` a stats object without
  `sw_on`/`ap_rx` and with `ble:false` and checks that those tabs are hidden
  and a hash pointing at them falls back to Status.
- Network: with the Status tab active, the browser issues no `/devices`,
  `/nat`, `/bond`, `/switch` requests (Playwright request log, 10 s).

## Bond list names (BLE tab)

`GET /bond` returns MACs only. The auto-bond and stored-bond lists show a
name next to each MAC, resolved in the page:

1. the name from `/devices` (already polled while the BLE tab is open);
2. otherwise the last name the page saw for that MAC, kept in
   `localStorage` (`nbp.names`), shown greyed with "last seen" in the title;
3. otherwise "no name seen".

No firmware change. Gap: names only reach the cache while a page is open,
and a peer that puts its name only in the scan response shows none (on .231
fugu-flat `70:04:1D:A4:EA:36` has an empty name in `/devices`). A
device-side name per bond (NVS) would cover that; not planned.

## Open

- Tunables are kept together in one Settings tab. Alternative: move WiFi
  and BLE rows to subsystem tabs. Easy to change later, not done now.
