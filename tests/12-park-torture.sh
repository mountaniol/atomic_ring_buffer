#!/usr/bin/env bash
# DESC: park-path torture - RB_WAIT_SPIN=0 (park on every miss), same-CPU and two-core regimes, futex syscall census
# EST_TIME: 2min
# TIMEOUT: 900
. "$(dirname "$0")/common.sh"

FLAGS="$CFLAGS_DEF -DRB_WAIT_SPIN=0"
CPU=$(( $(nproc) - 1 ))

for var in "" "-DRB_INT_INDEXED"; do
    tag=${var:+idx}
    tag=${tag:-line}
    build_bin "$BUILD/12-spin0-$tag" gcc "$FLAGS $var" \
        "$TDIR/03-func-wait.c" "$RB_SRC" || continue

    step "[$tag] both threads on one logical CPU ($CPU), SPIN=0"
    run_check "same-cpu $tag" \
        timeout 300 taskset -c "$CPU" "$BUILD/12-spin0-$tag" 150000 quick

    step "[$tag] two cores, SPIN=0"
    run_check "two-core $tag" timeout 300 "$BUILD/12-spin0-$tag" 400000 quick
done

step "futex syscall census (informational, strace -c, line variant)"
if command -v strace >/dev/null; then
    strace -f -c -e trace=futex -o "$BUILD/12-strace.txt" \
        taskset -c "$CPU" "$BUILD/12-spin0-line" 30000 quick > /dev/null 2>&1
    cat "$BUILD/12-strace.txt"
else
    echo "strace not installed, census skipped"
fi

finish
