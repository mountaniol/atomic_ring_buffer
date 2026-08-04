#!/usr/bin/env bash
# DESC: AFL++ smoke fuzz - 45s per variant on the persistent harness; any crash or hang is an error
# EST_TIME: 3min
# TIMEOUT: 900
. "$(dirname "$0")/common.sh"

command -v afl-fuzz >/dev/null || skip_test "afl-fuzz not installed - run deploy-tests-ubuntu.sh"
[ -d "$ROOT/afl_input" ] || skip_test "afl_input/ corpus missing"

step "build the AFL harnesses (Makefile-afl)"
run_check "make -f Makefile-afl" make -C "$ROOT" -f Makefile-afl -s

# No sudo here: skip the governor tweak and tolerate the apport core_pattern
export AFL_NO_UI=1 AFL_SKIP_CPUFREQ=1 AFL_FAST_CAL=1
export AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1

for bin in ring_buf_afl.out ring_buf_afl_idx.out; do
    [ -x "$ROOT/$bin" ] || { fail "$bin not built"; continue; }
    tag=${bin%.out}
    out="$BUILD/afl-smoke-$tag"
    rm -rf "$out"
    step "fuzz $bin for 45s"
    timeout 200 afl-fuzz -V 45 -i "$ROOT/afl_input" -o "$out" \
        -- "$ROOT/$bin" > "$BUILD/afl-$tag.log" 2>&1
    stats="$out/default/fuzzer_stats"
    if [ ! -f "$stats" ]; then
        tail -20 "$BUILD/afl-$tag.log"
        fail "$bin: afl-fuzz did not produce stats"
        continue
    fi
    execs=$(sed -n 's/^execs_done *: *//p' "$stats")
    crashes=$(sed -n 's/^saved_crashes *: *//p' "$stats")
    hangs=$(sed -n 's/^saved_hangs *: *//p' "$stats")
    echo "$bin: execs=$execs crashes=$crashes hangs=$hangs"
    if [ "${execs:-0}" -gt 100 ] && [ "${crashes:-1}" -eq 0 ] \
       && [ "${hangs:-1}" -eq 0 ]; then
        ok "$bin smoke clean"
    else
        fail "$bin: execs=$execs crashes=$crashes hangs=$hangs"
    fi
done

finish
