#!/usr/bin/env bash
# DESC: integer-semantics audit - clang -fsanitize=integer at runtime + CBMC overflow checks on the estimator
# EST_TIME: 2min
# TIMEOUT: 900
# Regression gate for HARDENING.md #11 defect 2 (fixed 2026-08-03): the
# rb_batch_size() estimator must stay wrap-free for arbitrary clock values.
. "$(dirname "$0")/common.sh"

step "clang -fsanitize=integer over the burst/estimator workload"
SAN="-fsanitize=integer -fsanitize-recover=all -O1 -g -std=c11"
export UBSAN_OPTIONS="print_stacktrace=0"
for var in "" "-DRB_INT_INDEXED"; do
    tag=${var:+idx}
    tag=${tag:-line}
    build_bin "$BUILD/14-int-$tag" clang "$SAN $var" \
        "$TDIR/02-func-burst.c" "$RB_SRC" || continue
    out="$BUILD/14-int-$tag.out.txt"
    timeout 240 "$BUILD/14-int-$tag" 500000 quick > "$out" 2>&1
    rc=$?
    sites=$(grep "runtime error" "$out" \
            | grep -oE '[a-z_./]*ring_buf\.c:[0-9]+' | sort -u)
    nsites=$(echo -n "$sites" | grep -c .)
    grep "runtime error" "$out" | sort -u | head -10
    if [ "$rc" -eq 0 ] && [ "$nsites" -eq 0 ]; then
        ok "integer-san $tag clean"
    else
        fail "integer-san $tag: rc=$rc, $nsites site(s)"
    fi
done

step "CBMC overflow checks on rb_batch_size (nondet clock)"
for var in "" "-DRB_INT_INDEXED"; do
    tag=${var:+idx}
    tag=${tag:-line}
    run_cbmc "bsize-overflow $tag" "$TDIR/08-cbmc-bsize.c" "$RB_SRC" \
        -I"$ROOT" $var --signed-overflow-check --unsigned-overflow-check \
        --unwind 6
done

finish
