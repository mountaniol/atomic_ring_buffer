#!/usr/bin/env bash
# DESC: GenMC (RC11) exhaustive weak-memory check of the core protocol, both variants + mutant control
# EST_TIME: 30s
# TIMEOUT: 1800
. "$(dirname "$0")/common.sh"

GENMC=$(command -v genmc || true)
[ -z "$GENMC" ] && [ -x "$HOME/tools/genmc/build/bin/genmc" ] \
    && GENMC="$HOME/tools/genmc/build/bin/genmc"
[ -z "$GENMC" ] && skip_test "genmc not installed - run deploy-tests-ubuntu.sh"

run_genmc() {  # <tag> <extra cc flags...>; echoes result, returns genmc rc
    local tag="$1"
    shift
    local out="$BUILD/16-genmc-$tag.txt"
    timeout 600 "$GENMC" --unroll=10 -- -I"$ROOT" "$@" > "$out" 2>&1
    local rc=$?
    tail -4 "$out"
    return $rc
}

for var in "" "-DRB_INT_INDEXED"; do
    tag=${var:+idx}
    tag=${tag:-line}
    step "genmc RC11, variant $tag (must pass)"
    if run_genmc "$tag" $var "$TDIR/16-genmc-harness.c" \
       && grep -q "No errors were detected" "$BUILD/16-genmc-$tag.txt"; then
        ok "genmc $tag"
    else
        fail "genmc $tag"
    fi
done

step "mutant control: one seq release downgraded to relaxed (must FAIL)"
# same recomputed-line technique as the TSan mutation gate (test 07)
L=$(grep -n 'atomic_store_explicit(&ln->seq, off + 1, memory_order_release)' \
    "$RB_SRC" | head -1 | cut -d: -f1)
if [ -z "$L" ]; then
    fail "cannot locate the publish edge in ring_buf.c - pattern stale?"
else
    sed "${L}s/memory_order_release/memory_order_relaxed/" "$RB_SRC" \
        > "$BUILD/16-mut-lib.c"
    sed 's|\.\./ring_buf\.c|16-mut-lib.c|' "$TDIR/16-genmc-harness.c" \
        > "$BUILD/16-harness-mut.c"
    if run_genmc "mut" "$BUILD/16-harness-mut.c" \
       && grep -q "No errors were detected" "$BUILD/16-genmc-mut.txt"; then
        fail "genmc missed the planted relaxed-publish bug - harness invalid"
    else
        ok "genmc kills the planted mutant"
    fi
fi

finish
