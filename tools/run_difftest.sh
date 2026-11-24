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

if [ -f "$MM_BIN" ] && [ -x ./difftest ]; then
    echo "Manic Miner (Bug-Byte):"
    check demo 1400
    check finale 400
    for seed in 1 2 3; do check "play:$seed" 6000; done
fi

# Manic Miner 128: same, plus the AY music compared tick by tick.
export MM_TAP="${MM_TAP:-$PWD/ManicMiner128.tap}"
check128() {
    local script=$1 passes=$2
    ./difftest128 ref  "$script" "$passes" "$tmp/ay_ref.txt"  > "$tmp/ref.txt"
    ./difftest128 port "$script" "$passes" "$tmp/ay_port.txt" > "$tmp/port.txt"
    local first
    first=$(diff <(cut -d' ' -f1,2 "$tmp/ref.txt") <(cut -d' ' -f1,2 "$tmp/port.txt") | sed -n 2p || true)
    local caverns
    caverns=$(cut -d' ' -f4 "$tmp/ref.txt" | sort -un | wc -l)
    local ay
    ay=$(python3 -c "
import difflib
a=open('$tmp/ay_ref.txt').read().split(); b=open('$tmp/ay_port.txt').read().split()
sm=difflib.SequenceMatcher(None,a,b,autojunk=False)
print('%.1f' % (100.0*sum(i2-i1 for t,i1,i2,j1,j2 in sm.get_opcodes() if t=='equal')/max(1,len(a))))" 2>/dev/null || echo "?")
    if [ -z "$first" ]; then
        printf 'PASS  %-10s %6d passes, %2d caverns, AY ticks identical: %s%%\n' "$script" "$passes" "$caverns" "$ay"
    else
        echo "FAIL  $script: first divergence at pass $(echo "$first" | cut -d' ' -f2)"
        fail=1
    fi
}
if [ -f "$MM_TAP" ] && [ -x ./difftest128 ]; then
    echo "Manic Miner 128:"
    check128 demo 1400
    for seed in 1 2 3; do check128 "play:$seed" 6000; done
fi
exit $fail
