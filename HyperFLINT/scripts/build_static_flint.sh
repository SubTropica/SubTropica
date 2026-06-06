#!/usr/bin/env bash
# Fetch FLINT 3.4.0 from upstream and build a static libflint.a for the
# current arch. Used by CI and by source-builders, since Homebrew's flint
# ships only a shared dylib (no static archive) and the HyperFLINT
# LibraryLink dylib must static-link FLINT to avoid a dyld symbol-resolution
# race that crashes the Mathematica kernel on heavy fixtures.
#
# Requires brew gmp + mpfr (static archives) already installed.
#
# Usage: build_static_flint.sh <install-prefix> [<brew-prefix>]
#   install-prefix : where libflint.a + headers land (e.g. $PWD/flint-static)
#   brew-prefix    : Homebrew root for gmp/mpfr (default: $(brew --prefix))
set -euo pipefail

FLINT_VERSION="3.4.0"
PREFIX="${1:?usage: build_static_flint.sh <install-prefix> [<brew-prefix>]}"
BREW_PREFIX="${2:-$(brew --prefix)}"
WORK="$(mktemp -d)"

echo "[flint] version ${FLINT_VERSION}; install -> ${PREFIX}; brew -> ${BREW_PREFIX}"
cd "${WORK}"
curl -sSL "https://github.com/flintlib/flint/releases/download/v${FLINT_VERSION}/flint-${FLINT_VERSION}.tar.gz" \
  -o flint.tar.gz
tar xzf flint.tar.gz
cd "flint-${FLINT_VERSION}"

./configure \
  --prefix="${PREFIX}" \
  --enable-static --disable-shared \
  --with-gmp="${BREW_PREFIX}" \
  --with-mpfr="${BREW_PREFIX}" \
  CFLAGS="-O2 -fvisibility=hidden"
# WANT_LTO=1 makes FLINT archive individual .o files rather than
# per-module merged .o files.  This is required for static linking
# into the CLI binary on macOS: FLINT's merged objects contain
# cross-module references to file-scoped (static) functions that the
# Mach-O linker cannot resolve when -force_load is used, but are
# fine when each object is archived individually (the linker only
# pulls in the .o files it actually needs, keeping all same-TU
# static symbols in scope).
make -j"$(sysctl -n hw.ncpu)" WANT_LTO=1
make install WANT_LTO=1

echo "[flint] built: ${PREFIX}/lib/libflint.a"
ls -l "${PREFIX}/lib/libflint.a"
