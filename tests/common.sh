# Shared helpers for the tests/ suite.  Source this from every NN-*.sh.
# Contract of a test script:
#   - writes a detailed log to stdout/stderr (the runner redirects to a file)
#   - prints "ERRORS: <n>" as the last line and exits 0 (success) / 1 (errors)
#   - prints "SKIP: <reason>" and exits 77 when the test cannot run here

set -u

TDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$TDIR/.." && pwd)"
BUILD="$TDIR/build"
mkdir -p "$BUILD"

# Default library build flags (mirror of the root Makefile)
CFLAGS_DEF="-Wall -Wextra -std=c11 -O3 -march=native -flto -funroll-loops -fomit-frame-pointer"
RB_SRC="$ROOT/ring_buf.c"

ERRORS=0

step() { echo; echo "== $*"; }
ok()   { echo "ok: $*"; }
fail() { echo "FAIL: $*"; ERRORS=$((ERRORS + 1)); }

skip_test() { echo "SKIP: $*"; echo "ERRORS: 0"; exit 77; }

finish() {
    echo
    echo "ERRORS: $ERRORS"
    [ "$ERRORS" -eq 0 ] && exit 0 || exit 1
}

# build_bin <out> <cc> "<flags>" <src>...   (flags word-split on purpose;
# extra libraries can be passed via BB_LIBS, e.g. BB_LIBS=-lm)
build_bin() {
    local out="$1" cc="$2" flags="$3"
    shift 3
    echo "-- build: $cc $flags $* -> ${out#$TDIR/}"
    # shellcheck disable=SC2086
    if ! $cc $flags -I"$ROOT" -o "$out" "$@" -pthread ${BB_LIBS:-} 2>&1; then
        fail "build $(basename "$out")"
        return 1
    fi
}

# run_check <description> <cmd>...  — one counted check per invocation
run_check() {
    local desc="$1"
    shift
    echo "-- run: $*"
    "$@"
    local rc=$?
    if [ $rc -eq 0 ]; then
        ok "$desc"
    else
        fail "$desc (rc=$rc)"
    fi
    return 0
}

# TSan options: symbolization is OFF.  Measured on this machine: the first
# race report wedges the whole process inside SymbolizerProcess (with the
# addr2line fallback AND with an explicit llvm-symbolizer-18), so reports
# print raw addresses instead.  To symbolize offline:
#   llvm-symbolizer-18 --obj=<binary> <offset>
tsan_opts() { echo "halt_on_error=0 exitcode=66 symbolize=0"; }

# run_cbmc <log-tag> <cbmc-args>... — resource-capped, sequential (owner rule:
# an uncapped CBMC once swap-froze this machine)
run_cbmc() {
    local tag="$1"
    shift
    echo "-- cbmc [$tag]: cbmc $*"
    local out
    out="$( (ulimit -v 4194304; exec nice -n 19 timeout 600 cbmc "$@") 2>&1 )"
    local rc=$?
    echo "$out" | tail -20
    if echo "$out" | grep -q "VERIFICATION SUCCESSFUL"; then
        ok "cbmc $tag"
        return 0
    fi
    if [ $rc -eq 124 ]; then
        fail "cbmc $tag (timeout)"
    else
        fail "cbmc $tag"
    fi
    return 1
}
