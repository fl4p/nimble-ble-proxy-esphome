---
name: wifi-ps-instability
description: CPU light-sleep (not WiFi power-save) was the real cause of the ESP32-S3 hanging/going unreachable; both light-sleep and WiFi PS now default OFF
metadata: 
  type: feedback
---

Both CPU light-sleep and WiFi power-save default OFF on this device. Keep them off unless deliberately testing power/thermal.

**Why:** the device kept going unreachable (HTTP + api_server dead, serial silent except the post-reset boot window). A 2026-05-28 controlled test isolated the cause: **CPU light-sleep is the real culprit, not WiFi PS.** With WiFi PS off (`li=0`/`PS_NONE`) the device still hung — stability poll ~3/36 reachable, dropouts of minutes ending in a watchdog reboot. After also disabling light-sleep (`ls=false`) the same poll was 48/48. So the earlier belief that "li=0 → PS_NONE also blocks esp_pm light sleep" is **wrong in practice here** — light-sleep needs its own `ls=0`. (`PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y` is the suspected buggy mechanism.)

**How to apply:** source defaults are now both OFF — `DEFAULT_WIFI_LI = 0` (stats.cpp + wifi_sta.cpp, keep in sync) and `g_light_sleep = false` (stats.cpp). NVS keys `wifi_li` / `cpu_ls` override per-device and persist across reboot. To recover a dozing device live, POST (not GET — see [[device-curl-post]]) both `/wifips?li=0` and `/cpufreq?mhz=240&ls=0`; values land in NVS so a later watchdog reboot stays fixed. Re-enabling light-sleep (`POST /cpufreq?ls=1`) brings back the hang risk but saves thermals. See also [[silent-device-recovery]].
