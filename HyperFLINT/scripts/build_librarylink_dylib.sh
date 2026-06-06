#!/usr/bin/env bash
# =============================================================================
# build_librarylink_dylib.sh
#
# Build AND gate the HyperFLINT Mathematica LibraryLink shared library with:
#   * FIX-A : FLINT/GMP/MPFR statically force-loaded into the dylib, so dyld
#             never resolves FLINT against Wolfram's bundled libflint.* (the
#             three-way FLINT/GMP/MPFR collision -> SIGSEGV).  Gate: zero
#             undefined FLINT symbols in the built library.
#   * FIX-B : OpenMP via the `hyperflint_nomp_sd` static lib (schedule(static),
#             no `___kmpc_dispatch_deinit`), resolved against the host's libomp
#             at load.  Gate (macOS): no `___kmpc_dispatch_deinit`, exactly one
#             `___kmpc_fork_call`.
#
# This is the SINGLE canonical, self-gating producer of the dylib for both
# local macOS builds and
# the CI cross-build (inside the official wolframresearch/wolframengine image,
# which supplies WolframLibrary.h via WOLFRAM_INC; the headers are never
# redistributed, only the compiled dylib ships).
#
# Usage:
#   build_librarylink_dylib.sh [--arch <dist-arch>] [--src <HyperFLINT dir>]
#                              [--build-dir <dir>] [--gate-only <dylib>]
#
#   --arch       dist-dir name (macos-arm64 | macos-x86_64 | linux-x86_64 ...).
#                Default: derived from `uname`.  Used only for messages here;
#                staging into dist/<arch>/ is a separate step.
#   --gate-only  skip the build; run the symbol gates on an existing library.
#
# Env overrides (defaults target macos-arm64 on this machine):
#   HF_FLINT_A   static libflint.a      (default ~/hf-tuned/flint/lib/libflint.a)
#   HF_MPFR_A    static libmpfr.a       (default ~/hf-tuned/mpfr/lib/libmpfr.a)
#   HF_GMP_A     static libgmp.a        (default ~/hf-tuned/gmp/lib/libgmp.a)
#   WOLFRAM_INC  dir with WolframLibrary.h (default: let CMake auto-find; REQUIRED
#                in CI where there is no local Mathematica)
#   LIBOMP_DYLIB libomp shared lib      (macOS default /opt/homebrew/opt/libomp/lib/libomp.dylib)
#   LIBOMP_INC   libomp include dir     (macOS default /opt/homebrew/opt/libomp/include)
#   CXX_TUNE     extra compiler flags   (default empty = portable; e.g. "-mcpu=apple-m4")
#   JOBS         parallel build jobs    (default: nproc/sysctl)
#
# Exit status: 0 iff the library built (or was supplied) AND all hard gates pass.
# =============================================================================
set -euo pipefail

# ---- resolve repo paths -----------------------------------------------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
HF_ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"   # .../HyperFLINT

# ---- OS detection -----------------------------------------------------------
UNAME_S="$(uname -s)"
case "${UNAME_S}" in
  Darwin) HOST_OS=macos; LIB_EXT=dylib; DEFAULT_ARCH="macos-$(uname -m | sed 's/^x86_64$/x86_64/;s/^arm64$/arm64/')" ;;
  Linux)  HOST_OS=linux; LIB_EXT=so;    DEFAULT_ARCH="linux-$(uname -m | sed 's/^aarch64$/arm64/')" ;;
  *)      echo "ERROR: unsupported OS ${UNAME_S}" >&2; exit 2 ;;
esac

# ---- argument parsing -------------------------------------------------------
ARCH="${DEFAULT_ARCH}"
SRC_DIR="${HF_ROOT}"
BUILD_DIR=""
GATE_ONLY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --arch)       ARCH="$2"; shift 2 ;;
    --src)        SRC_DIR="$2"; shift 2 ;;
    --build-dir)  BUILD_DIR="$2"; shift 2 ;;
    --gate-only)  GATE_ONLY="$2"; shift 2 ;;
    -h|--help)    sed -n '2,45p' "$0"; exit 0 ;;
    *) echo "ERROR: unknown arg $1" >&2; exit 2 ;;
  esac
done
: "${BUILD_DIR:=${SRC_DIR}/build-librarylink-${ARCH}}"
: "${JOBS:=$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu || echo 4 )}"

LIB_BASENAME="libhyperflint_librarylink.${LIB_EXT}"

# =============================================================================
# gate_macos <dylib> : the three Mach-O / Wolfram-libomp gates (hard fail).
# =============================================================================
gate_macos() {
  local dy="$1" g1 g2 g3 rc=0
  g1=$(nm -u "$dy" 2>/dev/null | grep -c 'fmpz' || true)
  g2=$(nm -u "$dy" 2>/dev/null | grep -c '___kmpc_dispatch_deinit' || true)
  g3=$(nm -u "$dy" 2>/dev/null | grep -c '___kmpc_fork_call' || true)
  echo "  GATE 1  undefined FLINT (fmpz)       = ${g1}  (want 0)  $([ "$g1" -eq 0 ] && echo PASS || { echo FAIL; rc=1; })"
  echo "  GATE 2  ___kmpc_dispatch_deinit      = ${g2}  (want 0)  $([ "$g2" -eq 0 ] && echo PASS || { echo FAIL; rc=1; })"
  echo "  GATE 3  ___kmpc_fork_call            = ${g3}  (want 1)  $([ "$g3" -eq 1 ] && echo PASS || { echo FAIL; rc=1; })"
  if nm -u "$dy" 2>/dev/null | grep -iqE 'flint|fmpq|mpfr|__gmp'; then
    echo "  WARN: other undefined FLINT/GMP/MPFR symbols present:"; nm -u "$dy" | grep -iE 'flint|fmpq|mpfr|__gmp' | head -5
    rc=1
  fi
  return $rc
}

# =============================================================================
# gate_linux <so> : ELF gates.  FLINT-undefined==0 is a HARD gate; the OpenMP
# teardown-symbol situation is macOS/Wolfram-libomp specific, so on Linux we
# only REPORT the OMP symbols (warn) pending the Linux FIX-B re-validation
# against the libomp the Linux Wolfram Engine ships (see PLAN Part B caveats).
# =============================================================================
gate_linux() {
  local so="$1" g1 rc=0
  g1=$(nm -D --undefined-only "$so" 2>/dev/null | grep -cE 'fmpz|fmpq|\bflint' || true)
  echo "  GATE 1  undefined FLINT (fmpz/fmpq/flint) = ${g1}  (want 0)  $([ "$g1" -eq 0 ] && echo PASS || { echo FAIL; rc=1; })"
  echo "  REPORT  undefined OpenMP symbols (verify vs Linux Wolfram libomp before trusting):"
  nm -D --undefined-only "$so" 2>/dev/null | grep -iE 'GOMP|kmpc|omp_' | sed 's/^/          /' | head -12 || echo "          (none)"
  echo "  (Linux FIX-B is NOT yet validated; re-test a smoke STIntegrate on the Linux runner.)"
  return $rc
}

run_gates() {
  local lib="$1"
  echo "=== GATES on ${lib} (host ${HOST_OS}) ==="
  # `file` is informational and may be absent in minimal CI images; never let
  # its absence (or a SIGPIPE under pipefail) abort the gate.
  { file "$lib" 2>/dev/null || true; } | sed 's/^/  /' || true
  if [ "${HOST_OS}" = macos ]; then gate_macos "$lib"; else gate_linux "$lib"; fi
}

# ---- gate-only short-circuit ------------------------------------------------
if [ -n "${GATE_ONLY}" ]; then
  [ -f "${GATE_ONLY}" ] || { echo "ERROR: --gate-only file not found: ${GATE_ONLY}" >&2; exit 2; }
  run_gates "${GATE_ONLY}"; exit $?
fi

# ---- static-archive defaults (macOS local; CI passes its own via env) -------
if [ "${HOST_OS}" = macos ]; then
  : "${HF_FLINT_A:=${HOME}/hf-tuned/flint/lib/libflint.a}"
  : "${HF_MPFR_A:=${HOME}/hf-tuned/mpfr/lib/libmpfr.a}"
  : "${HF_GMP_A:=${HOME}/hf-tuned/gmp/lib/libgmp.a}"
  : "${LIBOMP_DYLIB:=/opt/homebrew/opt/libomp/lib/libomp.dylib}"
  : "${LIBOMP_INC:=/opt/homebrew/opt/libomp/include}"
fi
for a in "${HF_FLINT_A:-}" "${HF_MPFR_A:-}" "${HF_GMP_A:-}"; do
  [ -n "$a" ] && [ ! -f "$a" ] && { echo "ERROR: static archive missing: $a" >&2; exit 2; }
done

echo "=== build_librarylink_dylib  arch=${ARCH} host=${HOST_OS} jobs=${JOBS} ==="
echo "  src       = ${SRC_DIR}"
echo "  build dir = ${BUILD_DIR}"
echo "  FLINT.a   = ${HF_FLINT_A:-<cmake default>}"
echo "  wolfram   = ${WOLFRAM_INC:-<cmake auto-find>}"

# ---- stamp the dylib's hf_version to match $SubTropicaVersion ---------------
# SubTropica's loader REJECTS a dylib whose hf_version != $SubTropicaVersion
# (the runtime version gate), so the dylib must carry the package version, not
# the CMakeLists default. Parse it from <repo>/SubTropica.wl (the parent of
# --src), overridable via the HF_VERSION env var.
if [ -z "${HF_VERSION:-}" ]; then
  _stwl="$(dirname "${SRC_DIR}")/SubTropica.wl"
  [ -f "${_stwl}" ] && HF_VERSION=$(sed -n 's/.*\$SubTropicaVersion[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "${_stwl}" | head -1)
fi
echo "  HF_VERSION = ${HF_VERSION:-<cmake default>}"

# ---- assemble cmake args ----------------------------------------------------
CMAKE_ARGS=(
  -S "${SRC_DIR}" -B "${BUILD_DIR}"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_CXX_FLAGS="-O3 ${CXX_TUNE:-}"
  -DHF_OPENMP=ON -DHF_MIMALLOC=OFF
  -DHF_LIBRARYLINK=ON -DHF_LIBRARYLINK_STATIC_DEPS=ON
)
[ -n "${HF_VERSION:-}" ] && CMAKE_ARGS+=( -DHF_VERSION="${HF_VERSION}" )
[ -n "${HF_FLINT_A:-}" ] && CMAKE_ARGS+=( -DHF_FLINT_STATIC_ARCHIVE="${HF_FLINT_A}" )
[ -n "${HF_MPFR_A:-}" ]  && CMAKE_ARGS+=( -DHF_MPFR_STATIC_ARCHIVE="${HF_MPFR_A}" )
[ -n "${HF_GMP_A:-}" ]   && CMAKE_ARGS+=( -DHF_GMP_STATIC_ARCHIVE="${HF_GMP_A}" )
[ -n "${WOLFRAM_INC:-}" ] && CMAKE_ARGS+=( -DWOLFRAM_LIBRARY_INCLUDE_DIR="${WOLFRAM_INC}" )
if [ "${HOST_OS}" = macos ]; then
  CMAKE_ARGS+=(
    -DOpenMP_CXX_FLAGS="-Xclang -fopenmp" -DOpenMP_CXX_LIB_NAMES=libomp
    -DOpenMP_libomp_LIBRARY="${LIBOMP_DYLIB}" -DOpenMP_CXX_INCLUDE_DIR="${LIBOMP_INC}"
  )
fi

rm -rf "${BUILD_DIR}"
cmake "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --target hyperflint_librarylink -j "${JOBS}"

DY="$(find "${BUILD_DIR}" -name "${LIB_BASENAME}" | head -1)"
[ -n "${DY}" ] || { echo "ERROR: no ${LIB_BASENAME} produced" >&2; exit 1; }
echo "RESULT_DYLIB=${DY}  ($(wc -c < "${DY}") bytes)"

# ---- gate before declaring success -----------------------------------------
run_gates "${DY}"
GATE_RC=$?
if [ "${GATE_RC}" -ne 0 ]; then
  echo "=== GATES FAILED (rc=${GATE_RC}) — do NOT bundle this library ===" >&2
  exit "${GATE_RC}"
fi
echo "=== build + gates PASS: ${DY} ==="
