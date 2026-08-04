#!/usr/bin/env bash
# DESC: burst API + rb_batch_size functional test, both variants
# EST_TIME: 15s
# TIMEOUT: 300
. "$(dirname "$0")/common.sh"

make -C "$TDIR" -s "build/02-func-burst-line" "build/02-func-burst-idx" \
    || { fail "build"; finish; }

step "line format"
run_check "02 line" "$BUILD/02-func-burst-line"

step "RB_INT_INDEXED"
run_check "02 idx" "$BUILD/02-func-burst-idx"

finish
