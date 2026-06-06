#!/usr/bin/env bash
# hf-abi-break-detection ctest driver (§T10, iter-57).
#
# Purpose: extract the publicly exported `hf_*` C-ABI symbol surface
# from libhyperflint.a and diff it against the checked-in golden file.
# A diff != 0 flags an unintended ABI surface change (symbol removed,
# renamed, or added without updating the golden + the c_abi.h header).
#
# Why grep -a rather than `nm`:
#   - Portability across BSD nm / GNU nm / llvm-nm flag dialects.
#   - The .a archive is a concatenation of object files; symbol names
#     appear verbatim as NUL-terminated strings in the symbol tables,
#     so `grep -a -o '_hf_[a-zA-Z0-9_]*'` reliably enumerates them.
#   - macOS Mach-O convention prepends `_` to C symbols at the byte
#     level; on ELF the prefix is absent. The leading-underscore
#     stripping below normalizes both to bare C identifiers, then
#     diffs against the unprefixed golden.
#
# Check semantics: one-way containment. Every symbol listed in the
# golden file MUST exist as a linkable symbol in the .a archive. The
# converse is NOT enforced: the archive may legitimately export extra
# `hf_*` symbols (LibraryLink entry points like `hf_version`,
# `hf_step_trace`, allocator hooks `hf_rec1_*`, `hf_probe_*`, etc.)
# that are not part of the *documented* public C ABI surface declared
# in include/hyperflint/c_abi.h. The golden file pins what downstream
# consumers (Julia HJ.jl, Mma LibraryLink, Python cffi) must be able
# to dlopen + dlsym; accidentally exposing additional internal symbols
# is a code-review concern, not an ABI-break (caller never linked
# against them).
#
# A missing golden symbol = ABI break = FAIL.
# A novel extra symbol = silent PASS here (use the c_abi.h grep
# downstream in §A4 if/when explicit surface enumeration is needed).
#
# Usage: check_abi_symbols.sh <path/to/libhyperflint.a> <path/to/symbols_golden.txt>

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <libhyperflint.a> <symbols_golden.txt>" >&2
    exit 2
fi

lib_archive="$1"
golden_file="$2"

if [ ! -f "$lib_archive" ]; then
    echo "hf-abi-break-detection: archive not found: $lib_archive" >&2
    exit 3
fi
if [ ! -f "$golden_file" ]; then
    echo "hf-abi-break-detection: golden file not found: $golden_file" >&2
    exit 3
fi

# Extract every byte-substring of the form `_?hf_<ident>` from the
# archive. The `_?` accepts both Mach-O (underscored) and ELF (bare)
# C-symbol conventions. The sed normalization strips the optional
# leading underscore so the same golden file works on both platforms.
extracted=$(
    LC_ALL=C grep -a -o -E '\b_?hf_[a-zA-Z][a-zA-Z0-9_]*' "$lib_archive" \
        | sed -E 's/^_//' \
        | LC_ALL=C sort -u
)

# Normalize golden: strip optional leading underscore + blank lines.
golden=$(
    sed -E 's/^_//;/^$/d' "$golden_file" | LC_ALL=C sort -u
)

# Containment check: every golden symbol must appear in extracted.
# `comm -23 golden extracted` yields lines in golden NOT in extracted;
# nonempty result = missing public ABI symbol = FAIL.
missing=$(LC_ALL=C comm -23 <(echo "$golden") <(echo "$extracted"))

if [ -z "$missing" ]; then
    n=$(echo "$golden" | wc -l | tr -d ' ')
    echo "hf-abi-break-detection: PASS ($n public C-ABI symbols present in archive)"
    exit 0
fi

echo "hf-abi-break-detection: FAIL — public ABI symbol(s) missing from archive." >&2
echo "--- missing (golden \\ extracted) ---" >&2
echo "$missing" >&2
echo "--- golden (expected to be a subset of extracted) ---" >&2
echo "$golden" >&2
echo "--- extracted (libhyperflint.a hf_* surface, all) ---" >&2
echo "$extracted" >&2
exit 1
