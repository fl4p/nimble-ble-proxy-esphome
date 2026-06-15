---
name: device-curl-post
description: "Config endpoints on the .231 proxy (wifips, txpower, passkey, etc.) only apply via curl -X POST; a bare curl is a GET that silently reads"
metadata: 
  type: feedback
---

When changing device config on the nimble-ble-proxy over HTTP (e.g. `192.168.1.231/wifips?li=0`), curl MUST use `-X POST`. A bare `curl "http://.../wifips?li=0"` is a GET.

**Why:** Endpoints like `/wifips` register separate GET and POST handlers at the same URI (stats.cpp ~1401-1410). GET (`wifips_get`) only returns the current state JSON (e.g. `{"li":0}`) and ignores the query param; only POST (`wifips_post` → `handle_wifips_set`) actually applies + persists to NVS. I once spent ~20 min thinking bare-GET calls were applying `li=0` when they were no-op reads, and even sent the user to power-cycle on that false premise.

**How to apply:** For any device config change via curl, use `-X POST` (the dashboard JS does `POST /wifips?li=`). Use a bare GET only when you intend to read current state. Same pattern likely applies to other config endpoints (txpower, passkey, hostname). Related: [[project_clone_targets]].
