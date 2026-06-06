#!/usr/bin/env bash
# Bundle the LibraryLink dylib + its FLINT/MPFR/GMP runtime deps into a
# single directory whose dylibs reference each other via @loader_path,
# so the bundle can be dropped into any macOS Mma installation without
# requiring brew on the user's machine.
#
# Layout produced (under $OUT):
#   libhyperflint_librarylink.dylib   <-- entry point, the thing Mma loads
#   libflint.22.0.dylib
#   libmpfr.6.dylib
#   libgmp.10.dylib
#
# Each dylib's LC_ID_DYLIB becomes `@rpath/libfoo.dylib`, and every
# LC_LOAD_DYLIB pointing to /opt/homebrew/... is rewritten to
# @loader_path/libfoo.dylib so dyld resolves them out of the bundle.
# Each is then ad-hoc codesigned.
#
# Usage:  bundle_dylibs.sh [<build-dir>] [<out-dir>]
#   build-dir defaults to HyperFLINT/build-release
#   out-dir   defaults to <build-dir>/bundle
set -euo pipefail

HF_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-${HF_ROOT}/build-release}"
OUT_DIR="${2:-${BUILD_DIR}/bundle}"

if [[ ! -f "${BUILD_DIR}/libhyperflint_librarylink.dylib" ]]; then
    echo "ERROR: ${BUILD_DIR}/libhyperflint_librarylink.dylib not found." >&2
    echo "       Build first: cmake --build ${BUILD_DIR}" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"
echo "[bundle] staging into ${OUT_DIR}"

# ---- Resolve the brew dylibs (follow symlinks to real files) ----
resolve_real() {
    # macOS lacks `readlink -f`; loop until a real file.
    local p="$1"
    while [[ -L "${p}" ]]; do
        local target
        target="$(readlink "${p}")"
        if [[ "${target}" != /* ]]; then
            target="$(dirname "${p}")/${target}"
        fi
        p="${target}"
    done
    echo "${p}"
}

FLINT_SRC="$(resolve_real /opt/homebrew/opt/flint/lib/libflint.dylib)"
MPFR_SRC="$(resolve_real /opt/homebrew/opt/mpfr/lib/libmpfr.dylib)"
GMP_SRC="$(resolve_real /opt/homebrew/opt/gmp/lib/libgmp.dylib)"

FLINT_BASE="$(basename "${FLINT_SRC}")"
MPFR_BASE="$(basename "${MPFR_SRC}")"
GMP_BASE="$(basename "${GMP_SRC}")"

echo "[bundle] flint: ${FLINT_SRC}  -> ${FLINT_BASE}"
echo "[bundle] mpfr : ${MPFR_SRC}  -> ${MPFR_BASE}"
echo "[bundle] gmp  : ${GMP_SRC}  -> ${GMP_BASE}"

cp -f "${BUILD_DIR}/libhyperflint_librarylink.dylib" "${OUT_DIR}/"
cp -f "${FLINT_SRC}" "${OUT_DIR}/${FLINT_BASE}"
cp -f "${MPFR_SRC}"  "${OUT_DIR}/${MPFR_BASE}"
cp -f "${GMP_SRC}"   "${OUT_DIR}/${GMP_BASE}"

chmod u+w "${OUT_DIR}"/*.dylib

# ---- Rewrite load commands ----
# For each dylib in the bundle:
#   * set LC_ID_DYLIB to @rpath/<basename>
#   * for each load command pointing to /opt/homebrew/{flint,mpfr,gmp}/...,
#     rewrite to @loader_path/<basename>
rewrite_one() {
    local dylib="$1"
    local base
    base="$(basename "${dylib}")"
    install_name_tool -id "@rpath/${base}" "${dylib}"

    # Iterate over any /opt/homebrew/.../libX.dylib load commands.
    while read -r dep; do
        case "${dep}" in
            /opt/homebrew/opt/flint/*) target="${FLINT_BASE}" ;;
            /opt/homebrew/opt/mpfr/*)  target="${MPFR_BASE}"  ;;
            /opt/homebrew/opt/gmp/*)   target="${GMP_BASE}"   ;;
            *) continue ;;
        esac
        install_name_tool -change "${dep}" "@loader_path/${target}" "${dylib}"
    done < <(otool -L "${dylib}" | tail -n +2 | awk '{print $1}')
}

for f in "${OUT_DIR}/libhyperflint_librarylink.dylib" \
         "${OUT_DIR}/${FLINT_BASE}" \
         "${OUT_DIR}/${MPFR_BASE}" \
         "${OUT_DIR}/${GMP_BASE}"; do
    echo "[bundle] rewriting ${f##*/}"
    rewrite_one "${f}"
done

# ---- Codesign (ad-hoc) ----
# Without an Apple Developer ID, ad-hoc signing (`codesign -s -`) lets
# the binary run on the build machine and most user machines.  Users
# downloading the bundle through a browser may need
# `xattr -d com.apple.quarantine <file>` to clear Gatekeeper's
# quarantine bit.  A real release should re-sign with a Developer ID.
echo "[bundle] ad-hoc codesigning"
for f in "${OUT_DIR}"/*.dylib; do
    codesign --force --sign - "${f}"
done

# ---- Self-check: dump the rewritten load tables ----
echo "[bundle] verification (otool -L on each output dylib):"
for f in "${OUT_DIR}"/*.dylib; do
    echo "  ${f##*/}:"
    otool -L "${f}" | tail -n +2 | sed 's/^/    /'
done

echo "[bundle] done.  Drop ${OUT_DIR} into the SubTropica paclet under"
echo "          HyperFLINT/build-release/  to ship a self-contained HF."
