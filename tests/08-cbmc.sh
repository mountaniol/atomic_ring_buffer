#!/usr/bin/env bash
# DESC: CBMC bounded verification - contracts+FIFO, burst n=0..8 exhaustive, batch size, wait API; both variants
# EST_TIME: 12min
# TIMEOUT: 3600
. "$(dirname "$0")/common.sh"

command -v cbmc >/dev/null || skip_test "cbmc not installed - run deploy-tests-ubuntu.sh"

# Owner rule: CBMC always capped (ulimit -v 4G, nice 19) and strictly
# sequential - run_cbmc in common.sh enforces this.
CHECKS="--bounds-check --pointer-check"

for var in "" "-DRB_INT_INDEXED"; do
    tag=${var:+idx}
    tag=${tag:-line}

    step "[$tag] contracts + single push/pull FIFO laps"
    run_cbmc "single $tag" "$TDIR/08-cbmc-single.c" "$RB_SRC" -I"$ROOT" \
        $var $CHECKS --unwind 20 --unwinding-assertions

    step "[$tag] burst push/pull, concrete n = 0..8"
    # -Dmemcpy: word-wise verification memcpy (see 08-cbmc-burst.c header)
    for n in 0 1 2 3 4 5 6 7 8; do
        run_cbmc "burst $tag n=$n" "$TDIR/08-cbmc-burst.c" "$RB_SRC" \
            -I"$ROOT" $var -DN_CONST=$n -Dmemcpy=rb_cbmc_memcpy \
            $CHECKS --unwind 10 --unwinding-assertions
    done

    step "[$tag] rb_batch_size (nondet clock)"
    run_cbmc "bsize $tag" "$TDIR/08-cbmc-bsize.c" "$RB_SRC" -I"$ROOT" \
        $var $CHECKS --unwind 6

    step "[$tag] wait API (nondet clock + futex results)"
    run_cbmc "wait $tag" "$TDIR/08-cbmc-wait.c" "$RB_SRC" -I"$ROOT" \
        $var -DRB_WAIT_SPIN=2 $CHECKS --unwind 14
done

finish
