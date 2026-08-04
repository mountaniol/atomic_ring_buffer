#!/usr/bin/env bash
# DESC: ThreadSanitizer over all four modes (int single, burst, ptr, wait), both variants - must be race-free
# EST_TIME: 1min
# TIMEOUT: 600
. "$(dirname "$0")/common.sh"

# clang TSan: unaffected by the Ubuntu 24.04 vm.mmap_rnd_bits pitfall
TSAN="-fsanitize=thread -O2 -g -Wall -Wextra -std=c11"
export TSAN_OPTIONS="$(tsan_opts)"

for var in "" "-DRB_INT_INDEXED"; do
    tag=${var:+idx}
    tag=${tag:-line}
    step "variant: $tag"
    build_bin "$BUILD/06-tsan-$tag" clang "$TSAN $var" \
        "$TDIR/06-tsan-modes.c" "$RB_SRC" || continue
    out="$BUILD/06-tsan-$tag.out.txt"
    timeout 240 "$BUILD/06-tsan-$tag" 100000 > "$out" 2>&1
    rc=$?
    cat "$out"
    warns=$(grep -c "WARNING: ThreadSanitizer" "$out")
    if [ "$rc" -eq 0 ] && [ "$warns" -eq 0 ]; then
        ok "tsan $tag clean"
    else
        fail "tsan $tag: rc=$rc, $warns TSan warnings"
    fi
done

finish
