#!/usr/bin/env bash
# DESC: valgrind memcheck (--fair-sched=yes, leak check) over the functional tests
# EST_TIME: 2min
# TIMEOUT: 900
. "$(dirname "$0")/common.sh"

command -v valgrind >/dev/null || skip_test "valgrind not installed"

make -C "$TDIR" -s || { fail "build"; finish; }

VG="valgrind --fair-sched=yes --error-exitcode=99 --leak-check=full"

vg_run() {  # <desc> <binary> <args>...
    local desc="$1"
    shift
    local out="$BUILD/10-vg.out.txt"
    echo "-- run: $VG $*"
    timeout 300 $VG "$@" > "$out" 2>&1
    local rc=$?
    tail -15 "$out"
    local lost
    lost=$(grep -c "definitely lost" "$out")
    if [ "$rc" -eq 0 ] && [ "$lost" -eq 0 ]; then
        ok "$desc"
    else
        fail "$desc (rc=$rc, definitely-lost lines=$lost)"
    fi
}

step "memcheck: single API (both variants)"
vg_run "memcheck 01 line" "$BUILD/01-func-single-line" 40000
vg_run "memcheck 01 idx" "$BUILD/01-func-single-idx" 40000

step "memcheck: burst API"
vg_run "memcheck 02 line" "$BUILD/02-func-burst-line" 150000 quick

step "memcheck: wait API (futex under valgrind)"
vg_run "memcheck 03 line" "$BUILD/03-func-wait-line" 30000 quick

finish
