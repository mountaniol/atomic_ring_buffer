#!/usr/bin/env bash
# Installs everything the tests/ suite needs on Ubuntu (22.04/24.04).
# Needs sudo for apt.  Idempotent: already-installed packages are skipped.
#
# Usage: ./deploy-tests-ubuntu.sh [--with-genmc]
#   --with-genmc   additionally builds GenMC v0.16.1 from source into
#                  ~/tools/genmc (needed only by test 16; slow, ~10 min)

set -u

WITH_GENMC=0
[ "${1:-}" = "--with-genmc" ] && WITH_GENMC=1

# Core toolchain + every analyzer/fuzzer the suite invokes
PKGS=(
    build-essential gcc clang clang-tidy clang-tools cppcheck
    cbmc z3 valgrind afl++ strace gdb llvm-18
    linux-tools-common "linux-tools-$(uname -r)"
    stress-ng binutils
    # aarch64 cross headers for the codegen gate (test 13)
    gcc-aarch64-linux-gnu libc6-dev-arm64-cross
    # GenMC build prerequisites (harmless if GenMC is never built)
    git cmake libedit-dev zlib1g-dev libffi-dev llvm-18-dev clang-18
)

echo "== checking packages"
missing=()
for p in "${PKGS[@]}"; do
    if dpkg -s "$p" >/dev/null 2>&1; then
        echo "   present: $p"
    else
        missing+=("$p")
    fi
done

if [ ${#missing[@]} -gt 0 ]; then
    echo "== installing: ${missing[*]}"
    sudo apt-get update
    # linux-tools for the exact kernel may not exist; tolerate single failures
    for p in "${missing[@]}"; do
        sudo apt-get install -y "$p" || echo "WARN: could not install $p"
    done
else
    echo "== all packages already installed"
fi

if [ "$WITH_GENMC" -eq 1 ]; then
    echo "== building GenMC (v0.16.1: last release with IMM enabled)"
    mkdir -p "$HOME/tools"
    if [ -x "$HOME/tools/genmc/build/genmc" ]; then
        echo "   already built: $HOME/tools/genmc/build/genmc"
    else
        git clone --branch v0.16.1 --depth 1 \
            https://github.com/MPI-SWS/genmc "$HOME/tools/genmc" &&
        cmake -S "$HOME/tools/genmc" -B "$HOME/tools/genmc/build" \
              -DCMAKE_BUILD_TYPE=Release &&
        make -C "$HOME/tools/genmc/build" -j"$(nproc)" ||
        echo "WARN: GenMC build failed - test 16 will be skipped"
    fi
    if [ -x "$HOME/tools/genmc/build/genmc" ]; then
        echo "   add to PATH: export PATH=\$HOME/tools/genmc/build:\$PATH"
    fi
fi

echo
echo "== verification"
for c in gcc clang clang-tidy cppcheck cbmc valgrind afl-fuzz strace stress-ng; do
    if command -v "$c" >/dev/null; then
        echo "   $c: $(command -v "$c")"
    else
        echo "   MISSING: $c"
    fi
done
command -v genmc >/dev/null && echo "   genmc: $(command -v genmc)" \
    || echo "   genmc: not on PATH (test 16 will be skipped)"
echo "done"
