*this document is an LLM generated placeholder*

# havan-poller

Keeps a history of the nimble-proxy's health in InfluxDB. The device's own
counters live in RAM and are lost on a reboot.

`nbp_poll.py` runs on havan. Every 30 s it fetches `GET /stats.json` and
`GET /wifidisc` from the proxy (5 s timeout, no keep-alive) and sends line
protocol over UDP to the `influxdb-udp-relay` add-on at `127.0.0.1:8086`, which
forwards to db `ha_van`. It only reads; it never calls other endpoints.

Measurement `nimble_proxy`, tag `host=nimble-proxy-231`, ms timestamp. Fields:

- every numeric/bool scalar of `/stats.json` (strings and arrays are skipped);
- from `/wifidisc`: `uptime_s`, `sta_down_ms`, `total`, and the newest event's
  `last_evt_t_s`, `last_evt_reason`, `last_evt_if` (string);
- `ok` (both requests succeeded), `ok_stats`, `ok_wd`, `latency_ms`,
  `latency_wd_ms` (float).

- from `/wifihist` (added 2026-11-17): the newest liveness-probe sample as
  `sta_rssi` (proxy → router RSSI), `ap_rssi` (weakest SoftAP client),
  `wd_rtt_ms` (the proxy's own TCP-connect RTT to the router), `wd_probe_ok`
  (0 = that probe failed), `sta_count` (SoftAP clients); plus `ok_wh`,
  `latency_wh_ms`.

Measurement `nimble_proxy_wifi` (same tag) holds **every** `/wifihist` sample.
The device keeps ~1 h of them (120 × 30 s), so after an outage the samples
recorded while the poller could not reach it are backfilled. New samples are
found by matching the overlap with the previous fetch. The device stores no
per-sample time, so the timestamps are **estimates**: new samples are spread
evenly between the last one sent and now. That is accurate to the extent that
probe cycles are evenly paced (30 s plus up to ~8 s of tick and connect
timeouts on a bad link).

Measurement `nimble_proxy_reboot` gets one point per detected reboot (uptime
from `/wifidisc` went backwards): `prev_uptime_s`, `uptime_s`, `reset_reason`
and `crash_count` from `/crash`, `saved_log`, `saved_crash`. The new boot's
`/log` (whose 4 KB ring still holds the previous boot's tail for a few
minutes) and `/crash` are saved to `~/nbp-reboots/<UTC stamp>/` on havan
(`NBP_REBOOT_DIR`). A failed snapshot is retried for 10 cycles. A reboot
between two polls that both fail is not detected until the next answered
`/wifidisc`. A reboot that completes faster than the uptime it replaces is
always seen, because uptime only grows within a boot.

When the device does not answer, a point with `ok=0` and the latencies is still
sent. Integers carry the `i` suffix; `temp_c` and the latencies are always
float. Fields are split into several lines with the same timestamp when one
line would exceed 1400 bytes (the relay reads 2 KB datagrams).

Environment overrides: `NBP_HOST`, `NBP_RELAY_HOST`, `NBP_RELAY_PORT`,
`NBP_PERIOD_S`, `NBP_TIMEOUT_S`, `NBP_TAG_HOST`. `nbp_poll.py --print` prints
one point instead of sending.

## Install on havan

```bash
scp nbp_poll.py havan:bin/
scp nbp-poll.service havan:/tmp/
ssh havan 'sudo install -m644 /tmp/nbp-poll.service /etc/systemd/system/ &&
  sudo systemctl daemon-reload && sudo systemctl enable --now nbp-poll'
journalctl -u nbp-poll -f   # fetch errors only
```

Query: `SELECT * FROM nimble_proxy WHERE time > now()-1h`.
