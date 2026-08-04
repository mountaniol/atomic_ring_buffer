#!/usr/bin/env bash
# DESC: codegen gates - hot paths must have zero mfence/lock/xchg (x86) and LDAR/STLR without dmb or ldapr (aarch64 cross)
# EST_TIME: 20s
# TIMEOUT: 300
. "$(dirname "$0")/common.sh"

# extract one function's disassembly from objdump output
xfunc() { awk -v fn="<$2>:" '$0 ~ fn {f=1} f && /^$/ {f=0} f' "$1"; }

HOT="rb_push_int rb_pull_int rb_push_int_burst rb_pull_int_burst rb_push_ptr rb_pull_ptr"

step "x86_64: hot functions free of mfence/lock/xchg (linked LTO binaries)"
for tag in line idx; do
    var=""
    [ "$tag" = idx ] && var="-DRB_INT_INDEXED"
    # anchor binary: address-taken functions survive LTO as standalone code
    build_bin "$BUILD/13-audit-$tag" gcc "$CFLAGS_DEF $var" \
        "$TDIR/13-asm-audit.c" "$RB_SRC" || continue
    dis="$BUILD/13-x86-$tag.dis"
    objdump -d "$BUILD/13-audit-$tag" > "$dis"
    for fn in $HOT; do
        body=$(xfunc "$dis" "$fn")
        if [ -z "$body" ]; then
            fail "x86 $tag: $fn not found in binary"
            continue
        fi
        bad=$(echo "$body" | grep -E 'mfence|lock |xchg' \
                           | grep -v 'xchg   %ax,%ax')
        if [ -z "$bad" ]; then
            ok "x86 $tag $fn: clean"
        else
            echo "$bad"
            fail "x86 $tag $fn: barrier/RMW on the hot path"
        fi
    done
done

step "x86_64: wait path MUST carry its seq_cst RMWs/fence"
for tag in line idx; do
    dis="$BUILD/13-x86-$tag.dis"
    # rb_signal/rb_park are static and typically inlined into the _wait pair
    body=$( { xfunc "$dis" rb_push_wait; xfunc "$dis" rb_pull_wait; \
              xfunc "$dis" rb_wake; } )
    if echo "$body" | grep -qE 'mfence|lock |xchg' ; then
        ok "x86 $tag wait path: RMW/fence present"
    else
        fail "x86 $tag wait path: no RMW/fence found - protocol weakened?"
    fi
done

step "aarch64 cross codegen (clang -S)"
A64=""
for extra in "" "-isystem /usr/aarch64-linux-gnu/include"; do
    if clang --target=aarch64-linux-gnu -O2 -std=c11 $extra -I"$ROOT" \
             -S "$RB_SRC" -o "$BUILD/13-a64-line.s" 2>/dev/null; then
        A64="$extra"
        break
    fi
done
if ! [ -s "$BUILD/13-a64-line.s" ]; then
    echo "WARN: aarch64 cross-compile unavailable (headers missing) - ARM leg skipped"
    echo "      run deploy-tests-ubuntu.sh to install cross headers"
else
    [ -n "$A64" ] && echo "note: cross headers via '$A64' (codegen audit only)"
    clang --target=aarch64-linux-gnu -O2 -std=c11 $A64 -I"$ROOT" \
          -DRB_INT_INDEXED -S "$RB_SRC" -o "$BUILD/13-a64-idx.s" 2>/dev/null
    for tag in line idx; do
        s="$BUILD/13-a64-$tag.s"
        [ -s "$s" ] || { fail "a64 $tag: no assembly"; continue; }
        # ldapr (RCpc) is forbidden everywhere: RCsc reasoning assumed
        if grep -qw ldapr "$s"; then
            grep -nw ldapr "$s" | head -5
            fail "a64 $tag: LDAPR emitted"
        else
            ok "a64 $tag: no ldapr"
        fi
        # hot pair: acquire/release must be LDAR/STLR, with no dmb
        for fn in rb_push_int rb_pull_int; do
            body=$(awk -v fn="^$fn:" '$0 ~ fn {f=1} /^\.Lfunc_end/ {f=0} f' "$s")
            if [ -z "$body" ]; then
                fail "a64 $tag: $fn not found"
                continue
            fi
            if echo "$body" | grep -qw ldar && echo "$body" | grep -qw stlr \
               && ! echo "$body" | grep -qw dmb; then
                ok "a64 $tag $fn: ldar+stlr, no dmb"
            else
                fail "a64 $tag $fn: expected ldar+stlr and no dmb"
            fi
        done
        # the futex-door fence must exist somewhere in the wait path
        if grep -qw 'dmb' "$s"; then
            ok "a64 $tag: wait-path dmb present"
        else
            fail "a64 $tag: no dmb anywhere - seq_cst fence lost?"
        fi
    done
fi

finish
