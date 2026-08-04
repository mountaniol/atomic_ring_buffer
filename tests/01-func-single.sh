#!/usr/bin/env bash
# DESC: single-message API functional test (contracts, wrap laps, 2-thread FIFO), both variants
# EST_TIME: 15s
# TIMEOUT: 300
. "$(dirname "$0")/common.sh"

make -C "$TDIR" -s "build/01-func-single-line" "build/01-func-single-idx" \
    || { fail "build"; finish; }

step "line format"
run_check "01 line" "$BUILD/01-func-single-line"

step "RB_INT_INDEXED"
run_check "01 idx" "$BUILD/01-func-single-idx"

finish
