#!/usr/bin/env bash
# DESC: gcc -fanalyzer over every .c file + curated clang-tidy and cppcheck over the library
# EST_TIME: 1min
# TIMEOUT: 600
. "$(dirname "$0")/common.sh"

step "gcc -fanalyzer: library, both variants (must be 0 findings)"
for var in "" "-DRB_INT_INDEXED"; do
    tag=${var:+idx}
    tag=${tag:-line}
    out=$(gcc -fanalyzer $CFLAGS_DEF $var -I"$ROOT" -c "$RB_SRC" \
              -o /dev/null 2>&1)
    n=$(echo "$out" | grep -c "warning:")
    echo "$out" | grep "warning:" | head -20
    if [ "$n" -eq 0 ]; then
        ok "fanalyzer ring_buf.c $tag"
    else
        fail "fanalyzer ring_buf.c $tag: $n findings"
    fi
done

step "gcc -fanalyzer: harnesses and test sources"
for f in "$ROOT"/ring_buf_test_int.c "$ROOT"/ring_buf_ping_pong.c \
         "$ROOT"/count_minimal_latency.c "$ROOT"/v3_ab_bench.c \
         "$TDIR"/0[1236]-*.c; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    out=$(gcc -fanalyzer $CFLAGS_DEF -I"$ROOT" -c "$f" -o /dev/null 2>&1)
    if [ $? -ne 0 ] && ! echo "$out" | grep -q "warning:"; then
        echo "WARN: $base does not compile standalone, skipped"
        continue
    fi
    n=$(echo "$out" | grep -c "warning:")
    if [ "$n" -eq 0 ]; then
        ok "fanalyzer $base"
    else
        echo "$out" | grep "warning:" | head -10
        known=""
        [ "$base" = "ring_buf_test_int.c" ] && known=" [KNOWN defect 1]"
        fail "fanalyzer $base: $n findings$known"
    fi
done

step "clang-tidy (curated bugprone/concurrency/cert) on the library"
# excluded: err33-c (unchecked printf-family), easily-swappable (API shape),
# reserved-identifier+dcl37/51 (feature-test macros ARE defined by user code)
TIDY_CHECKS='-*,bugprone-*,concurrency-*,cert-*,-cert-err33-c,-bugprone-easily-swappable-parameters,-bugprone-reserved-identifier,-cert-dcl37-c,-cert-dcl51-cpp'
for var in "" "-DRB_INT_INDEXED"; do
    tag=${var:+idx}
    tag=${tag:-line}
    out=$(clang-tidy --quiet --checks="$TIDY_CHECKS" "$RB_SRC" -- \
              $CFLAGS_DEF $var -I"$ROOT" 2>/dev/null)
    n=$(echo "$out" | grep -cE 'ring_buf\.[ch]:[0-9]+:[0-9]+: warning')
    echo "$out" | grep -E 'ring_buf\.[ch]:[0-9]+:[0-9]+: warning' | head -10
    if [ "$n" -eq 0 ]; then
        ok "clang-tidy $tag"
    else
        fail "clang-tidy $tag: $n findings"
    fi
done

step "cppcheck (warning+portability) on the library"
out=$(cppcheck --enable=warning,portability --quiet -I"$ROOT" "$RB_SRC" 2>&1)
n=$(echo "$out" | grep -cE '\((error|warning|portability)\)')
echo "$out" | head -10
if [ "$n" -eq 0 ]; then
    ok "cppcheck"
else
    fail "cppcheck: $n findings"
fi

finish
