#!/usr/bin/env bash
# DESC: futex wait API functional test (parking FIFO, timeouts, rb_wake), both variants
# EST_TIME: 30s
# TIMEOUT: 300
. "$(dirname "$0")/common.sh"

make -C "$TDIR" -s "build/03-func-wait-line" "build/03-func-wait-idx" \
    || { fail "build"; finish; }

step "line format"
run_check "03 line" "$BUILD/03-func-wait-line"

step "RB_INT_INDEXED"
run_check "03 idx" "$BUILD/03-func-wait-idx"

finish
