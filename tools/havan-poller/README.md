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
