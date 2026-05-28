# nimble-ble-proxy — project notes

## Runtime / hardware gotchas

- **WiFi power-save causes instability — keep it OFF.** Running with
  `WIFI_PS_MAX_MODEM` (listen_interval > 0) plus esp_pm light sleep made
  the ESP32-S3 intermittently unreachable over HTTP/the api_server and
  *silenced the USB-Serial/JTAG console* in steady state (serial only
  emits during the post-reset boot window). Default is now power-save OFF
  (`DEFAULT_WIFI_LI = 0` in both `components/wifi_sta/wifi_sta.cpp` and
  `components/api_server/stats.cpp`, kept in sync). Opt back in per-device
  via `POST /wifips?li=N` (1..10) only if power draw matters more than
  responsiveness. `li=0` → `WIFI_PS_NONE`, which also keeps esp_pm from
  light-sleeping while WiFi is associated.

- **Debugging the device:** if serial is silent, the device is almost
  certainly in light sleep (power-save on) — disable it live with
  `POST /wifips?li=0` and `POST /cpufreq?mhz=240&ls=0`, or rely on the
  `/log` web-console endpoint while keeping the page polling. Config
  endpoints (`/wifips`, `/cpufreq`, `/txpower`, …) only apply via `POST`;
  a bare GET just reads.

## There are multiple ESP32 boards on this LAN — verify which one you're on

- `mDNS nimble-proxy.local` resolves to **192.168.1.231** (MAC `94:A9:90:08:BA:3C`),
  which is NOT the board flashed over USB. The USB board (`/dev/cu.usbmodem*`,
  MAC `9C:13:9E:F4:04:98`) gets a different DHCP IP (e.g. 192.168.1.125).
  There's also `esp32-repeater.lan` (192.168.1.173). Flashing the USB board
  and then testing `nimble-proxy.local` silently tests the *wrong* device.
- **Before testing a flashed change, confirm the IP belongs to the board you
  flashed.** Quick checks: `arp -a | grep 9c:13:9e` to find the USB board's IP;
  or read `app_init: Compile time` from the serial boot log; or query the
  api_server over TCP (`:6053`, Hello+DeviceInfo) — `DeviceInfoResponse` carries
  the MAC and compile time. A stale compile time = you're on the wrong/old board.
- `idf.py flash`'s RTS hard-reset over USB-Serial/JTAG often does NOT reboot the
  app (it keeps running the old image). After flashing, reboot explicitly via
  `POST /reboot` (or an esptool reset) and re-check the compile time.

## WebSocket bridge (NBP_WS_PROXY)

- Gated off by default. When on, `GET ws://<host>/api` tunnels the plaintext
  aioesphomeapi protocol to the local api_server over a 127.0.0.1:6053 loopback.
  A browser JS client must treat inbound WS data as a **continuous ESPHome byte
  stream** (device→browser is chunked at arbitrary boundaries; one WS frame ≠
  one ESPHome message).

## ESP-IDF workflow (for the agent)

- `idf.py build` and `idf.py flash` terminate and return — run them as
  normal Bash calls. No MCP server needed.
- `idf.py monitor` is the only blocking command (streams serial forever,
  never returns). Don't run it foreground and wait. Instead use
  `run_in_background: true`, or bound it (`timeout 10 idf.py monitor`).
- **Prefer the `/log` web-console endpoint over serial monitor** for
  observing the device: it's pollable over HTTP, survives light sleep,
  and avoids USB port contention. Serial only emits in the post-reset
  boot window anyway (see power-save note above).
- Skip the ESP-IDF serial-monitor MCP server — it only wraps the blocking
  `monitor` case, which the options above already cover.
