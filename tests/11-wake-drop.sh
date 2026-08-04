#!/usr/bin/env bash
# DESC: lost-wakeup fault injection - drop 50%/90% of futex wakes in rb_signal; the RB_WAIT_RECHECK_NS net must heal every one
# EST_TIME: 2min
# TIMEOUT: 900
. "$(dirname "$0")/common.sh"

# Inject a deterministic wake-dropper into a COPY of ring_buf.c, right
# before the FUTEX_WAKE in rb_signal (first rb_futex(door, FUTEX_WAKE...):
# rb_wake() further down is left untouched).  The peer's sleep flag is
# already cleared at that point - exactly the lost-wakeup scenario the
# 1 ms park cap exists for.  RB_WAIT_RECHECK_NS is lowered to 50 us so the
# healed stalls do not dominate the runtime; RB_WAIT_SPIN=0 parks on every
# miss, maximising exposure (353k parks per 200k msgs in the study).
MUT="$BUILD/11-ring_buf_drop.c"
awk '{
    if (!done && $0 ~ /rb_futex\(door, FUTEX_WAKE/) {
        print "        { static _Atomic unsigned rb_dropc;";
        print "          if ((atomic_fetch_add_explicit(&rb_dropc, 1u,";
        print "               memory_order_relaxed) % RB_DROP_MOD) != 0)";
        print "              return; }";
        done = 1;
    }
    print
}' "$RB_SRC" > "$MUT"
grep -q rb_dropc "$MUT" || { fail "injection point not found in ring_buf.c"; finish; }

FLAGS="$CFLAGS_DEF -DRB_WAIT_SPIN=0 -DRB_WAIT_RECHECK_NS=50000"

for mod in 2 10; do
    pct=$(( (mod - 1) * 100 / mod ))
    step "drop ${pct}% of wakeups (RB_DROP_MOD=$mod)"
    for var in "" "-DRB_INT_INDEXED"; do
        tag=${var:+idx}
        tag=${tag:-line}
        build_bin "$BUILD/11-drop$mod-$tag" gcc \
            "$FLAGS -DRB_DROP_MOD=$mod $var" \
            "$TDIR/03-func-wait.c" "$MUT" || continue
        run_check "wake-drop $pct% $tag heals" \
            timeout 300 "$BUILD/11-drop$mod-$tag" 100000 quick
    done
done

finish
