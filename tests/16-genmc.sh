#!/usr/bin/env bash
# DESC: GenMC (RC11) exhaustive weak-memory check of the core protocol, both variants
# EST_TIME: 2min
# TIMEOUT: 1800
. "$(dirname "$0")/common.sh"

GENMC=$(command -v genmc || true)
[ -z "$GENMC" ] && [ -x "$HOME/tools/genmc/build/genmc" ] \
    && GENMC="$HOME/tools/genmc/build/genmc"
[ -z "$GENMC" ] && skip_test "genmc not installed - run deploy-tests-ubuntu.sh --with-genmc"

for var in "" "-DRB_INT_INDEXED"; do
    tag=${var:+idx}
    tag=${tag:-line}
    step "genmc RC11, variant $tag"
    out="$BUILD/16-genmc-$tag.txt"
    if timeout 600 "$GENMC" -unroll=10 -- -I"$ROOT" $var \
         "$TDIR/16-genmc-harness.c" > "$out" 2>&1; then
        tail -5 "$out"
        ok "genmc $tag"
    else
        tail -30 "$out"
        fail "genmc $tag"
    fi
done

finish
