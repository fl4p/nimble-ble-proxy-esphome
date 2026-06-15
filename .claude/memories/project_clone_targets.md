---
name: clone-targets
description: BLE clone test targets and their PSK PINs (the global /passkey applies to whichever target the clone is currently bonding with)
metadata: 
  type: project
---

The proxy's BLE clone supervisor pairs upstream with whatever target MAC is configured in `/clone`. There's a single global passkey (default 123456, NVS-persisted via `/passkey?val=...`). When switching the clone target between devices that require different PINs, the passkey has to be updated too — there's no per-target passkey today.

**Why:** SMP "DisplayOnly" pairing injects exactly one passkey from `connection::get_passkey()` regardless of which peer is asking.

**How to apply:**
- SmartShunt (Victron, e.g. `E0:E5:16:A0:5A:C8`) → PIN `123456` (factory default; matches the firmware default).
- fugu-flat (`70:04:1D:A4:EA:36`) → PIN `654321`.

Before switching the clone target, also POST `/passkey?val=NNNNNN` if the new target needs a different PIN, otherwise `secureConnection` will fail with rc=1284 → cascading `writeValue rc:261` (Insufficient Authentication) and `upstream writeValue failed` warnings.
