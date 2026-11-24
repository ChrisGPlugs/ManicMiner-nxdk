#!/usr/bin/env bash
# Compare the C port with the original Z80 code, pass by pass.
# Needs ./difftest (make -f Makefile.host difftest) and ManicMiner.bin.
set -euo pipefail
cd "$(dirname "$0")/.."
export MM_BIN="${MM_BIN:-$PWD/ManicMiner.bin}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

check() {
    local script=$1 passes=$2
    ./difftest ref  "$script" "$passes" > "$tmp/ref.txt"
    ./difftest port "$script" "$passes" > "$tmp/port.txt"
    local first
    first=$(diff <(cut -d' ' -f1,2 "$tmp/ref.txt") <(cut -d' ' -f1,2 "$tmp/port.txt") | sed -n 2p || true)
    local caverns
    caverns=$(cut -d' ' -f4 "$tmp/ref.txt" | sort -un | wc -l)
    local tr tp
    tr=$(awk 'NR>1{s+=$3} END{print s}' "$tmp/ref.txt")
    tp=$(awk 'NR>1{s+=$3} END{print s}' "$tmp/port.txt")
    if [ -z "$first" ]; then
        printf 'PASS  %-10s %6d passes, %2d caverns, timing %+.3f%%\n' "$script" "$passes" "$caverns" \
            "$(awk -v a="$tp" -v b="$tr" 'BEGIN{print (a-b)*100/b}')"
    else
        echo "FAIL  $script: first divergence at pass $(echo "$first" | cut -d' ' -f2)"
        fail=1
    fi
}

check demo 1400
check finale 400
for seed in 1 2 3; do check "play:$seed" 6000; done
exit $fail
