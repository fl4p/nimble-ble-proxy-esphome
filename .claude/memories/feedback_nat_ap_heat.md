---
name: nat-ap-heat
description: "NAT SoftAP (CONFIG_NBP_NAT_ROUTER, APSTA) runs the WiFi radio at ~100% duty → device runs ~20-25°C hotter; mitigate with WiFi PS + lower TX power"
metadata: 
  type: feedback
---

With the NAT router SoftAP active (CONFIG_NBP_NAT_ROUTER, APSTA mode), the device runs markedly hotter — observed jump from ~35°C (STA-only) to ~70°C, settling ~60°C after mitigation.

**Why:** A SoftAP must beacon continuously, so the WiFi radio can't modem-sleep — the PHY runs at ~100% duty regardless of WiFi PS on the STA side. During an episode the on-chip sensor read 60.6°C while cpu0~11% / cpu1~3% (near-idle), confirming the heat is RF, not a software busy loop.

**How to apply:** Don't treat ~50-60°C in NAT/AP mode as a bug or a crash-loop symptom. The user's validated mitigation: lower WiFi TX power (dashboard POST /txpower, e.g. 20→11 dBm) + lower BLE TX (→0) + WiFi PS on (POST /wifips li=3). The SoftAP itself is now runtime-toggleable — `POST /nat?enabled=0` (dashboard "SoftAP enabled" checkbox) tears it down to STA-only and persists the choice in NVS; that's the live knob to shed the AP's duty without reflashing (port maps are kept and re-applied on re-enable). Even with the AP off the device won't reach the old ~35°C because the BLE scanner keeps the radio/CPU busy. Note the AP only beacons after the STA gets an IP (nat_router::start runs after wifi_sta), so it's absent during STA association-retry windows. Relates to [[feedback_wifi_ps_instability]].
