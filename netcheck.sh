#!/usr/bin/env bash
# netcheck.sh — ping the router + co-located BLE proxy and print a
# loss/latency table. Save a baseline (e.g. while the proxy is doing
# active BLE scanning), change one thing (switch to passive scan, move
# the boxes apart, power the proxy off), then re-run to see the
# before/after side by side.
#
#   ./netcheck.sh            measure now; show the baseline column if one is saved
#   ./netcheck.sh --save     measure now and store it as the baseline
#   COUNT=40 ./netcheck.sh   more ping samples per host (default 20)
#   HOSTS="router:192.168.1.173 proxy:192.168.1.231" ./netcheck.sh
set -uo pipefail
export LC_ALL=C  # ping prints "." decimals; make printf parse them regardless of locale

COUNT="${COUNT:-20}"
BASELINE_FILE="${BASELINE_FILE:-$(dirname "$0")/.netcheck-baseline}"
read -ra HOST_LIST <<<"${HOSTS:-router:192.168.1.173 proxy:192.168.1.231}"

SAVE=0
if [[ "${1:-}" == "--save" ]]; then SAVE=1; fi

# ping a host; echo "loss avg max stddev" (ms, rounded).
# loss="x" if no reply line at all; rtt fields="-" on 100% loss.
measure() {
  local ip="$1" out loss rtt avg max sd
  out="$(ping -c "$COUNT" "$ip" 2>/dev/null || true)"
  loss="$(printf '%s\n' "$out" | sed -n 's/.*received, \([0-9.]*\)% packet loss.*/\1/p')"
  rtt="$(printf '%s\n'  "$out" | sed -n 's#.*= [0-9.]*/\([0-9.]*\)/\([0-9.]*\)/\([0-9.]*\) ms#\1 \2 \3#p')"
  if [[ -z "$loss" ]]; then echo "x x x x"; return; fi
  if [[ -z "$rtt"  ]]; then echo "$loss - - -"; return; fi
  read -r avg max sd <<<"$rtt"
  printf '%s %.0f %.0f %.0f\n' "$loss" "$avg" "$max" "$sd"
}

# render "loss avg max sd" as one table cell.
cell() {
  local loss="$1" avg="$2" max="$3" sd="$4"
  if [[ "$loss" == "x" ]]; then printf 'unreachable'; return; fi
  if [[ "$avg"  == "-" ]]; then printf '%s%% loss (no rtt)' "$loss"; return; fi
  printf '%s%% loss · avg %s / max %s / σ%s ms' "$loss" "$avg" "$max" "$sd"
}

# echo a baseline line's "loss avg max sd" for the given ip, or nothing.
base_lookup() {
  local ip="$1" f_ip rest
  [[ -f "$BASELINE_FILE" ]] || return 0
  while read -r f_ip rest; do
    if [[ "$f_ip" == "$ip" ]]; then printf '%s' "$rest"; return 0; fi
  done < "$BASELINE_FILE"
}

dashes() { printf '%*s' "$1" '' | tr ' ' '-'; }

have_base=0
if [[ $SAVE -eq 0 && -s "$BASELINE_FILE" ]]; then have_base=1; fi

if [[ $have_base -eq 1 ]]; then
  printf '| %-12s | %-42s | %-42s |\n' "device" "baseline" "now"
  printf '|%s|%s|%s|\n' "$(dashes 14)" "$(dashes 44)" "$(dashes 44)"
else
  printf '| %-12s | %-42s |\n' "device" "now"
  printf '|%s|%s|\n' "$(dashes 14)" "$(dashes 44)"
fi

saved=""
for hp in "${HOST_LIST[@]}"; do
  label="${hp%%:*}"; ip="${hp##*:}"
  read -r n_loss n_avg n_max n_sd <<<"$(measure "$ip")"
  dev="$label .${ip##*.}"
  now_cell="$(cell "$n_loss" "$n_avg" "$n_max" "$n_sd")"
  if [[ $have_base -eq 1 ]]; then
    b="$(base_lookup "$ip")"
    if [[ -n "$b" ]]; then
      read -r b_loss b_avg b_max b_sd <<<"$b"
      base_cell="$(cell "$b_loss" "$b_avg" "$b_max" "$b_sd")"
    else
      base_cell="—"
    fi
    printf '| %-12s | %-42s | %-42s |\n' "$dev" "$base_cell" "$now_cell"
  else
    printf '| %-12s | %-42s |\n' "$dev" "$now_cell"
  fi
  saved+="$ip $n_loss $n_avg $n_max $n_sd"$'\n'
done

if [[ $SAVE -eq 1 ]]; then
  printf '%s' "$saved" > "$BASELINE_FILE"
  printf '\nbaseline saved to %s\n' "$BASELINE_FILE"
fi
