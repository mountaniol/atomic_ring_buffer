#!/usr/bin/env bash
# DESC: ordering mutation gate - every protocol release/acquire edge downgraded to relaxed must be caught by TSan
# EST_TIME: 4min
# TIMEOUT: 1200
. "$(dirname "$0")/common.sh"

# Protocol edges: acquire/release operations on seq (line format) or
# head/tail (indexed + ptr).  Recomputed from the source each run, so the
# gate survives line-number drift.  Futex-section edges (closed/doors) are
# excluded: they are seq_cst / healed-by-recheck by design.
mapfile -t CAND < <(grep -n 'memory_order_\(acquire\|release\)' "$RB_SRC" \
                    | grep -E '(->|&d->)?seq|->head|->tail' | cut -d: -f1)

IDX_START=$(grep -n '^#ifdef RB_INT_INDEXED' "$RB_SRC" | head -1 | cut -d: -f1)
IDX_ELSE=$(grep -n '^#else  /\* default' "$RB_SRC" | head -1 | cut -d: -f1)
IDX_END=$(grep -n '^#endif /\* RB_INT_INDEXED \*/' "$RB_SRC" | head -1 | cut -d: -f1)
if [ -z "$IDX_START" ] || [ -z "$IDX_ELSE" ] || [ -z "$IDX_END" ]; then
    fail "cannot locate RB_INT_INDEXED preprocessor ranges in ring_buf.c"
    finish
fi
echo "found ${#CAND[@]} protocol edges (idx section $IDX_START-$IDX_ELSE-$IDX_END)"
if [ "${#CAND[@]}" -lt 16 ]; then
    fail "expected >= 16 protocol edges, found ${#CAND[@]} - grep filter stale?"
fi

TSAN="-fsanitize=thread -O2 -g -std=c11"
export TSAN_OPTIONS="$(tsan_opts)"
N=50000

# tsan_run <binary> -> prints TSan warning count, returns run rc
tsan_run() {
    local out="$BUILD/07-mut.out.txt"
    timeout 90 "$1" $N > "$out" 2>&1
    local rc=$?
    grep -c "WARNING: ThreadSanitizer" "$out"
    return $rc
}

step "baseline: unmutated builds must be TSan-clean"
for var in "" "-DRB_INT_INDEXED"; do
    tag=${var:+idx}
    tag=${tag:-line}
    build_bin "$BUILD/07-base-$tag" clang "$TSAN $var" \
        "$TDIR/06-tsan-modes.c" "$RB_SRC" || continue
    w=$(tsan_run "$BUILD/07-base-$tag")
    rc=$?
    if [ "$rc" -eq 0 ] && [ "$w" -eq 0 ]; then
        ok "baseline $tag clean"
    else
        fail "baseline $tag dirty (rc=$rc, $w warnings) - gate invalid"
    fi
done

step "mutants: one edge at a time -> TSan must warn"
MUT="$BUILD/07-ring_buf_mut.c"
killed=0
for L in "${CAND[@]}"; do
    # pick the build variant that actually compiles this line
    if [ "$L" -lt "$IDX_START" ]; then
        var="" tag=line              # ptr ring: present in both, one is enough
    elif [ "$L" -lt "$IDX_ELSE" ]; then
        var="-DRB_INT_INDEXED" tag=idx
    elif [ "$L" -lt "$IDX_END" ]; then
        var="" tag=line
    else
        echo "-- line $L beyond protocol sections, skipping"
        continue
    fi

    src=$(sed -n "${L}p" "$RB_SRC" | sed 's/^ *//')
    sed "${L}s/memory_order_acquire/memory_order_relaxed/; \
         ${L}s/memory_order_release/memory_order_relaxed/" "$RB_SRC" > "$MUT"
    if cmp -s "$MUT" "$RB_SRC"; then
        fail "mutant line $L: sed changed nothing"
        continue
    fi
    if ! clang $TSAN $var -I"$ROOT" -o "$BUILD/07-mut" \
         "$TDIR/06-tsan-modes.c" "$MUT" -pthread 2> "$BUILD/07-mut.cc.txt"; then
        fail "mutant line $L ($tag): does not compile"
        continue
    fi
    w=$(tsan_run "$BUILD/07-mut")
    rc=$?
    if [ "$w" -gt 0 ]; then
        killed=$((killed + 1))
        echo "KILLED  line $L [$tag] ($w warnings): $src"
    else
        # one retry: race detection is scheduling-dependent
        w=$(tsan_run "$BUILD/07-mut")
        if [ "$w" -gt 0 ]; then
            killed=$((killed + 1))
            echo "KILLED  line $L [$tag] ($w warnings, retry): $src"
        else
            fail "SURVIVED line $L [$tag] (run rc=$rc): $src"
        fi
    fi
done
echo
echo "mutation score: $killed of ${#CAND[@]} edges killed by TSan"

finish
