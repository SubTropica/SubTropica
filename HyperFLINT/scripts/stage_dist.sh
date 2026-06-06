#!/usr/bin/env bash
# Stage HyperFLINT artifacts into HyperFLINT/dist/<arch>/ as a fully
# self-contained bundle (CLI + LibraryLink dylib + libomp).  FLINT, GMP,
# and MPFR are statically linked into the binaries; only libomp is bundled
# as a dylib.  The result is what stDiscoverHyperFlint /
# stHyperFlintLibraryPathCandidates probe in SubTropica.wl, so
# collaborators get a working HF on `git pull` (with Git LFS) without
# running cmake themselves.
#
# Layout produced (under $OUT, default HyperFLINT/dist/macos-arm64):
#
#   hyperflint                              <-- CLI binary
#   libhyperflint_librarylink.dylib         <-- LibraryLink entry point
#   libomp.dylib                            <-- only dylib dep (CLI)
#
# Each binary's LC_LOAD_DYLIB entries pointing to ${BREW_PREFIX}/... are
# rewritten to @loader_path/<basename> so dyld resolves them out of the
# stage directory.  Each dylib's LC_ID_DYLIB becomes @rpath/<basename>.
# Everything is ad-hoc codesigned so it runs on the build machine and
# typical user machines (collaborators may need
# `xattr -d com.apple.quarantine <file>` if they download via a browser
# -- git checkouts skip the quarantine bit).
#
# The LibraryLink dylib does NOT bundle libomp because it uses
# dynamic_lookup against Wolfram's libomp at runtime (see CMakeLists);
# only the CLI binary links libomp directly.
#
# Usage: stage_dist.sh [<build-dir>] [<arch>]
#   build-dir    defaults to HyperFLINT/build-release
#   arch         defaults to macos-arm64
#   BREW_PREFIX  env var, defaults to /opt/homebrew
set -euo pipefail

HF_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-${HF_ROOT}/build-release}"
ARCH="${2:-macos-arm64}"
BREW_PREFIX="${BREW_PREFIX:-/opt/homebrew}"
OUT_DIR="${HF_ROOT}/dist/${ARCH}"

CLI="${BUILD_DIR}/hyperflint"
LL="${BUILD_DIR}/libhyperflint_librarylink.dylib"

if [[ ! -f "${CLI}" ]]; then
    echo "ERROR: ${CLI} not found." >&2
    echo "       Build first:  cmake --build ${BUILD_DIR} -j" >&2
    exit 1
fi
# The LibraryLink dylib is OPTIONAL. It requires WolframLibrary.h, which only
# ships with a Mathematica / Wolfram Engine install, so CI runners (which have
# neither) produce a CLI-only bundle. When the dylib is absent, stage the CLI +
# libomp only; the HF backend then uses the CLI subprocess transport.
HAVE_LL=1
if [[ ! -f "${LL}" ]]; then
    echo "[stage] NOTE: ${LL##*/} not found -> staging CLI-only bundle (no in-process LibraryLink dylib)." >&2
    HAVE_LL=0
fi

mkdir -p "${OUT_DIR}"
rm -rf "${OUT_DIR}/integrands"
# Remove stale bundled dylibs that are no longer part of the dist
# (FLINT/GMP/MPFR are now statically linked; only libomp is bundled).
rm -f "${OUT_DIR}"/libflint*.dylib \
      "${OUT_DIR}"/libgmp*.dylib \
      "${OUT_DIR}"/libmpfr*.dylib
echo "[stage] staging into ${OUT_DIR}"

# Resolve a brew dylib through any symlinks to the real file.
# macOS lacks `readlink -f`, so loop manually.
resolve_real() {
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

OMP_SRC="$(resolve_real "${BREW_PREFIX}/opt/libomp/lib/libomp.dylib")"

OMP_BASE="$(basename "${OMP_SRC}")"

echo "[stage] libomp  : ${OMP_SRC}  -> ${OMP_BASE}"

cp -f "${CLI}" "${OUT_DIR}/hyperflint"
if [[ "${HAVE_LL}" -eq 1 ]]; then
    cp -f "${LL}"  "${OUT_DIR}/libhyperflint_librarylink.dylib"
else
    rm -f "${OUT_DIR}/libhyperflint_librarylink.dylib"
fi
cp -f "${OMP_SRC}"   "${OUT_DIR}/${OMP_BASE}"

chmod u+w "${OUT_DIR}"/* 2>/dev/null || true

# Rewrite a dylib: LC_ID_DYLIB to @rpath/, LC_LOAD_DYLIB pointing to
# ${BREW_PREFIX}/opt/libomp to @loader_path/<basename>.
rewrite_dylib() {
    local dylib="$1"
    local base
    base="$(basename "${dylib}")"
    install_name_tool -id "@rpath/${base}" "${dylib}"
    while read -r dep; do
        case "${dep}" in
            "${BREW_PREFIX}"/opt/libomp/*) target="${OMP_BASE}"   ;;
            *) continue ;;
        esac
        install_name_tool -change "${dep}" "@loader_path/${target}" "${dylib}"
    done < <(otool -L "${dylib}" | tail -n +2 | awk '{print $1}')
}

# Rewrite the CLI executable: LC_LOAD_DYLIB redirects, no LC_ID_DYLIB
# (executables don't carry one).
rewrite_executable() {
    local exe="$1"
    while read -r dep; do
        case "${dep}" in
            "${BREW_PREFIX}"/opt/libomp/*) target="${OMP_BASE}"   ;;
            *) continue ;;
        esac
        install_name_tool -change "${dep}" "@loader_path/${target}" "${exe}"
    done < <(otool -L "${exe}" | tail -n +2 | awk '{print $1}')
}

echo "[stage] rewriting ${OMP_BASE}"
rewrite_dylib "${OUT_DIR}/${OMP_BASE}"
if [[ "${HAVE_LL}" -eq 1 ]]; then
    echo "[stage] rewriting libhyperflint_librarylink.dylib"
    rewrite_dylib "${OUT_DIR}/libhyperflint_librarylink.dylib"
fi

echo "[stage] rewriting hyperflint (CLI)"
rewrite_executable "${OUT_DIR}/hyperflint"

# Ad-hoc codesign every binary so SIP / Gatekeeper accept them.
echo "[stage] ad-hoc codesigning"
for f in "${OUT_DIR}"/*.dylib "${OUT_DIR}/hyperflint"; do
    codesign --force --sign - "${f}"
done

# Drop a small VERSION file so collaborators (and reviewers) can see at
# a glance what HF/git SHA the bundle was built from.  Plain text so it
# stays in regular git, not LFS.
HF_GIT_SHA="$(git -C "${HF_ROOT}/.." rev-parse --short HEAD 2>/dev/null || echo unknown)"
HF_VERSION="$(grep -E '^[[:space:]]*\$SubTropicaVersion[[:space:]]*=' \
              "${HF_ROOT}/../SubTropica.wl" 2>/dev/null \
              | head -1 | sed -E 's/.*"([^"]+)".*/\1/' || echo unknown)"
{
    echo "HyperFLINT prebuilt distribution"
    echo "================================"
    echo "Built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Arch: ${ARCH}"
    echo "SubTropica.wl version: ${HF_VERSION}"
    echo "Git SHA at build: ${HF_GIT_SHA}"
} > "${OUT_DIR}/VERSION"

# Verification: dump load tables and CLI version so reviewers can see
# the bundle is self-contained (no /opt/homebrew/ paths remaining).
echo
echo "[stage] verification:"
for f in "${OUT_DIR}"/*.dylib "${OUT_DIR}/hyperflint"; do
    echo "  ${f##*/}:"
    otool -L "${f}" | tail -n +2 | sed 's/^/    /'
done
echo
echo "[stage] CLI smoke test:"
"${OUT_DIR}/hyperflint" --help 2>&1 | head -3 | sed 's/^/    /' || \
    echo "    (--help not implemented; binary launches OK if no error above)"

echo
echo "[stage] done.  ${OUT_DIR} is ready to commit."
echo "       LFS picks up *.dylib + hyperflint automatically (.gitattributes)."
