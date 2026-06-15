---
name: feedback-coex-wifi-priority
description: "WiFi/BLE coexistence — bias radio to WiFi when SoftAP is up to avoid SA-Query timeouts and client flapping"
metadata: 
  type: feedback
---

When both WiFi SoftAP (APSTA) and BLE scanning are active on the single radio, explicitly bias the coexistence arbiter toward WiFi to prevent the AP's SA-Query exchange from timing out.

**Why:** Under default `ESP_COEX_PREFER_BALANCE`, a duty-cycled BLE scan can steal enough airtime that the SoftAP misses PMF SA-Query responses → triggers the disassoc loop (`wifi: … STA not responded to 6 SA Query attempts`, reason=209, repeating) → every TCP connection fails. The device becomes unreachable even though the radio is nominally up. This cascades into heap exhaustion and coex-induced flapping.

**How to apply:**

- `nat_router::ap_up()` calls **`esp_coex_preference_set(ESP_COEX_PREFER_WIFI)`** (header `esp_coexist.h`) when the SoftAP comes up.
- `ap_down()` restores `ESP_COEX_PREFER_BALANCE` so STA-only / BLE-proxy-only builds regain BLE's fair share.
- This trades some BLE throughput for WiFi/AP stability — the right call for a repeater where BLE is best-effort.
- The `esp_coex` component must be in the callee's `REQUIRES` — it is **not** pulled in transitively by `esp_wifi`.
- `esp_coex_preference_set` is current in IDF 5.5 (not deprecated); SW coexist (`ESP_COEX_SW_COEXIST_ENABLE`) must be on (default).
- The BLE scan is **also** already duty-cut to ~3–50% via `proxy::SCAN_INTERVAL_MS`/`SCAN_WINDOW_MS` and the runtime `/duty` endpoint — coex preference tuning is orthogonal to that.

**Verification:** If the device starts dropping WiFi clients under heavy BLE scan load, check whether the coex preference was reset or the AP is coming up before the preference is set. The `/log` endpoint will show SA-Query warnings if the arbiter is starving the AP.
