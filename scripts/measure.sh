#!/usr/bin/env bash
# Task 6: time a 1 MiB transfer through the relay at --delay 50, three runs for
# each window/loss combination, checking every copy with cmp.
#
#   make release && scripts/measure.sh [port]
#
# Prints one line per run and a Markdown table of the means at the end.
set -u
cd "$(dirname "$0")/.."

PORT=${1:-$((20000 + RANDOM % 40000))}
APP=./build/release/myapp
RUNS=3
WORK=$(mktemp -d)
trap 'kill $RELAY 2>/dev/null; rm -rf "$WORK"' EXIT

[[ -x $APP ]] || { echo "build first: make release" >&2; exit 1; }
head -c 1048576 /dev/urandom > "$WORK/1mib.bin"

python3 scripts/cs425_relay.py --port "$PORT" --delay 50 > "$WORK/relay.log" 2>&1 &
RELAY=$!
sleep 0.5

TIMEFORMAT=%R
TABLE=""
n=0
for combo in "1 0" "16 0" "1 0.05" "16 0.05"; do
    read -r W L <<< "$combo"
    total=0
    for run in $(seq 1 $RUNS); do
        n=$((n + 1))
        S="m-$$-$n"
        rm -f "$WORK/out.bin"
        $APP recv -s "$S" -p "$PORT" 127.0.0.1 "$WORK/out.bin" 2> /dev/null &
        R=$!
        sleep 0.2
        secs=$( { time $APP send -s "$S" -p "$PORT" -w "$W" -l "$L" 127.0.0.1 "$WORK/1mib.bin" 2> "$WORK/send.err"; } 2>&1 )
        wait $R
        if cmp -s "$WORK/1mib.bin" "$WORK/out.bin"; then ok=identical; else ok=DIFFERENT; fi
        echo "window $W loss $L run $run: ${secs}s $ok ($(cat "$WORK/send.err"))"
        total=$(echo "$total + $secs" | bc -l)
    done
    mean=$(echo "$total / $RUNS" | bc -l)
    TABLE+=$(printf '| %s | %s | 0 | 0 | %.2f | %.1f |' "$W" "$L" "$mean" "$(echo "1024 / $mean" | bc -l)")$'\n'
done

echo
echo "| Window | Loss | Corrupt | Dup | Mean time (s) | Throughput (KiB/s) |"
echo "|-------:|-----:|--------:|----:|--------------:|-------------------:|"
printf '%s' "$TABLE"
