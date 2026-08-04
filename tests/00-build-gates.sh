#!/usr/bin/env bash
# DESC: clean build of the repo + warning-free compile of both library variants (gcc and clang, -Werror)
# EST_TIME: 20s
# TIMEOUT: 180
. "$(dirname "$0")/common.sh"

step "root make (library + benchmark binaries)"
run_check "root make" make -C "$ROOT" -s

step "library compiles warning-free with -Werror (both variants, gcc+clang)"
for cc in gcc clang; do
    for var in "" "-DRB_INT_INDEXED"; do
        tag="$cc ${var:-line}"
        echo "-- $tag"
        if $cc $CFLAGS_DEF -Werror $var -I"$ROOT" -c "$RB_SRC" \
             -o "$BUILD/rb-gate.o" 2>&1; then
            ok "compile $tag"
        else
            fail "compile $tag"
        fi
    done
done
rm -f "$BUILD/rb-gate.o"

step "functional test binaries (tests/Makefile)"
run_check "tests make" make -C "$TDIR" -s

finish
