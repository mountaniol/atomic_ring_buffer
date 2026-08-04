#!/usr/bin/env bash
# DESC: functional tests 01-03 under AddressSanitizer + UBSan, both variants
# EST_TIME: 2min
# TIMEOUT: 600
. "$(dirname "$0")/common.sh"

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -O2 -g -Wall -Wextra -std=c11"
export ASAN_OPTIONS="allocator_may_return_null=1"

for var in "" "-DRB_INT_INDEXED"; do
    tag=${var:+idx}
    tag=${tag:-line}
    step "variant: $tag"
    for t in 01-func-single 02-func-burst 03-func-wait; do
        build_bin "$BUILD/$t-asan-$tag" gcc "$SAN $var" \
            "$TDIR/$t.c" "$RB_SRC" || continue
        # reduced N; "quick" skips timing-sensitive sections
        run_check "$t $tag asan+ubsan" \
            timeout 240 "$BUILD/$t-asan-$tag" 300000 quick
    done
done

finish
