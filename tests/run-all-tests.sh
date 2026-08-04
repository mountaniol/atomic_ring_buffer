#!/usr/bin/env bash
# Runs every tests/NN-*.sh in order.  Per test: full output goes to
# logs/NN-name.log, the screen gets one progress line.  A test may rebuild
# ring_buf.c with its own flags (sanitizers, mutants, RB_WAIT_SPIN, ...).
#
# Usage: ./run-all-tests.sh [NN ...]   (e.g. ./run-all-tests.sh 07 08)

cd "$(dirname "$0")" || exit 1
mkdir -p logs

if [ $# -gt 0 ]; then
    tests=()
    for p in "$@"; do
        for t in "$p"-*.sh; do
            [ -f "$t" ] && tests+=("$t")
        done
    done
else
    tests=([0-9][0-9]-*.sh)
fi

N=${#tests[@]}
if [ "$N" -eq 0 ]; then
    echo "no tests matched"
    exit 1
fi

summary="logs/summary.txt"
: > "$summary"
echo "test run: $(date '+%Y-%m-%d %H:%M:%S'), host $(hostname)" >> "$summary"

i=0 failed=0 skipped=0
total_start=$(date +%s)

for t in "${tests[@]}"; do
    i=$((i + 1))
    name="${t%.sh}"
    est=$(sed -n 's/^# EST_TIME: //p' "$t" | head -1)
    to=$(sed -n 's/^# TIMEOUT: //p' "$t" | head -1)
    [ -z "$to" ] && to=600
    log="logs/$name.log"

    printf "running test %02d of %d: %-24s estimated time %-6s ... " \
           "$i" "$N" "$name" "${est:-?}"

    start=$(date +%s)
    timeout --kill-after=30 "$to" bash "$t" > "$log" 2>&1
    rc=$?
    dur=$(( $(date +%s) - start ))

    errs=$(sed -n 's/^ERRORS: //p' "$log" | tail -1)
    if [ "$rc" -eq 77 ]; then
        reason=$(sed -n 's/^SKIP: //p' "$log" | head -1)
        line="SKIPPED (${reason:-no reason given})"
        skipped=$((skipped + 1))
    elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        line="DONE: Error (timeout after ${to}s)"
        failed=$((failed + 1))
    elif [ "$rc" -eq 0 ]; then
        line="DONE: Success (${dur}s)"
    else
        line="DONE: Error (${errs:-?} errors found, ${dur}s)"
        failed=$((failed + 1))
    fi
    echo "$line"
    printf "%-24s %s\n" "$name" "$line" >> "$summary"
done

total_dur=$(( $(date +%s) - total_start ))
echo
echo "$N tests: $((N - failed - skipped)) passed, $failed failed, $skipped skipped (${total_dur}s total)"
echo "logs: $(pwd)/logs/  (summary: $summary)"
echo "total: $((N - failed - skipped)) passed, $failed failed, $skipped skipped, ${total_dur}s" >> "$summary"

[ "$failed" -eq 0 ]
