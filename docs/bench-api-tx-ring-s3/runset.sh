#!/bin/bash
# runset.sh <tag> <phases...>  phases: A AB ABgs
S=/private/tmp/claude-503/-Users-fab-dev-ha-nimble-ble-proxy/208f29f7-44f3-4c64-8580-d1f6919f4bd3/scratchpad
H=192.168.1.125; tag=$1; shift; cd $S
P="--host $H --peer 80:A9:97:1D:E2:C9 --read-handle 30 --notify-handle 27"
reboot_wait() {
  curl -s -m5 -X POST http://$H/reboot >/dev/null; sleep 8
  for i in $(seq 1 60); do curl -s -m2 http://$H/stats.json >/dev/null && break; sleep 2; done
  sleep 20
}
for ph in "$@"; do
  reboot_wait
  s0=$(python3 -c "import json;print(json.load(open('periph.json'))['sent'])")
  case $ph in
    A)    python3 apibench.py runs/$tag-A $P --dur 60 --no-b > runs/$tag-A.out 2>&1 ;;
    AB)   python3 apibench.py runs/$tag-AB $P --dur 180 --b-start 15 > runs/$tag-AB.out 2>&1 ;;
    ABf)  python3 apibench.py runs/$tag-ABf $P --dur 180 --b-start 15 --b-flood 600 > runs/$tag-ABf.out 2>&1 ;;
    ABgs) python3 apibench.py runs/$tag-ABgs $P --dur 120 --b-start 15 --gs-every 10 > runs/$tag-ABgs.out 2>&1 ;;
    D*)   curl -s -m5 -X POST "http://$H/wifips?benchdelay=${ph#D}" >/dev/null
          python3 apibench.py runs/$tag-$ph $P --dur 60 --no-b > runs/$tag-$ph.out 2>&1 ;;
  esac
  s1=$(python3 -c "import json;print(json.load(open('periph.json'))['sent'])")
  echo "{\"periph_sent\": $((s1-s0))}" > runs/$tag-$ph/periph.json
  curl -s -m5 "http://$H/log" > runs/$tag-$ph.log
done
echo done
