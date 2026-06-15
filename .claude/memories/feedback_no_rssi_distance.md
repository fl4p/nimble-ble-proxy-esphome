---
name: feedback-no-rssi-distance
description: "Don't surface RSSI-derived distance estimates in the dashboard UI — user considers them unreliable noise."
metadata: 
  type: feedback
---

Do not add "distance" / "≈Xm" / path-loss-derived columns or fields to the device dashboard from RSSI + TX power. User called this "complete bs" — the indoor multipath / antenna gain / orientation variance is too large for the number to be useful even with a `~` prefix.

**Why:** Free-space path loss assumes n=2 but indoors n is 2.5–4 and varies room-to-room; the resulting estimate can be off by 2–5×. The number looks authoritative on the dashboard but isn't actionable.

**How to apply:** When suggesting "what other data could we show for each scanned BLE device," skip distance-estimate proposals. RSSI itself is fine to display (raw dBm has known meaning). TX power capture may still be valuable as raw JSON metadata if a downstream consumer asks for it, but don't render it in the UI as a distance.
