#!/usr/bin/env bash
# DESC: owner's acceptance benchmark (ring_buf_test.out, unchanged) - busy and wait modes, both variants
# EST_TIME: 30s
# TIMEOUT: 300
. "$(dirname "$0")/common.sh"

step "build the untouched benchmark (root make + indexed variant)"
run_check "root make" make -C "$ROOT" -s
BB_LIBS=-lm build_bin "$BUILD/ring_buf_test-idx.out" gcc \
    "$CFLAGS_DEF -DRB_INT_INDEXED" \
    "$ROOT/ring_buf_test_int.c" "$RB_SRC" || finish

# The acceptance check: 50M monotonically increasing messages, abort() on
# the first out-of-order value.  Success = clean exit.
step "line format, busy"
run_check "acceptance line busy" \
    "$ROOT/ring_buf_test.out" --cores --samples 50m

step "line format, wait (futex)"
run_check "acceptance line wait" \
    "$ROOT/ring_buf_test.out" --cores --samples 50m --wait

step "RB_INT_INDEXED, busy"
run_check "acceptance idx busy" \
    "$BUILD/ring_buf_test-idx.out" --cores --samples 50m

step "RB_INT_INDEXED, wait (futex)"
run_check "acceptance idx wait" \
    "$BUILD/ring_buf_test-idx.out" --cores --samples 50m --wait

finish
