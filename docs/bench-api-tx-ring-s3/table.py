#!/usr/bin/env python3
"""Print one markdown row per run directory given (summary.json + stats.jsonl)."""
import json, sys, os

hdr = ["run", "build", "A alive", "A notifies", "seq lost", "notify_rx Δ", "read n", "read p50/p95/max ms",
       "notify gap p99/max ms", "GS bursts (ms p50/max)", "B sessions/reaps (s)", "heap min seen", "heap_min",
       "largest min", "txq max", "ring max", "txq_drops", "txq_reaps", "ring_reaps", "frames/send",
       "enq_us_max", "api_tx stack free", "oom", "periph sent"]
print("| " + " | ".join(hdr) + " |")
print("|" + "---|" * len(hdr))
for d in sys.argv[1:]:
    s = json.load(open(os.path.join(d, "summary.json")))
    A, B, L = s["A"], s["B"], s["stats_last"]
    try: s["periph_sent"] = json.load(open(os.path.join(d, "periph.json")))["periph_sent"]
    except Exception: pass
    r = s["A_read_ms"]; g = s["A_gap_ms"]; gs = sorted(s["A_gs_ms"])
    fr = (L.get("api_tx_frames") / L["api_tx_sends"]) if L.get("api_tx_sends") else None
    row = [os.path.basename(d.rstrip("/")), (s.get("dev") or {}).get("compile"),
           "no: " + A["disconnected"] if A["disconnected"] else "yes",
           A["notifies"], A["seq_lost"], s.get("notify_rx_delta"),
           r["n"], f'{r["p50"]}/{r["p95"]}/{r["max"]}', f'{g["p99"]}/{g["max"]}',
           f'{len(gs)} ({gs[len(gs)//2] if gs else None}/{gs[-1] if gs else None})',
           f'{B["sessions"]}/{B["reaps"]} {B["reap_after_s"]}',
           s["heap_min_seen"], L.get("heap_min"), s["heap_largest_min_seen"], s["txq_max_seen"],
           s["ring_max_seen"], L.get("api_txq_drops"), L.get("api_txq_reaps"), L.get("api_ring_reaps"),
           f"{fr:.2f}" if fr else None, L.get("api_enq_us_max"), L.get("api_tx_stack_free"), L.get("oom"),
           s.get("periph_sent")]
    print("| " + " | ".join(str(x) for x in row) + " |")
