// Phase 7-i: algebraic letter pairs (Wm/Wp).
//
// HyperIntica's algebraic-letter machinery (HyperIntica.wl:2911-3155)
// allows degree-2 irreducible polynomial factors to be split into a
// pair of inert atoms `Wm[i]` and `Wp[i]` representing the two roots
// (-b ∓ √disc)/(2a) of the polynomial `lc·x² + b·x + c`. Downstream
// passes (PartialFractions, IntegrateII, Vieta simplification) treat
// the atoms symbolically; only the user-facing back-substitution
// rules (`get_back_sub_rules`) ever materialize `√disc` numerically.
//
// HF stores each allocated pair's metadata in a process-global
// `AlgebraicLetterTable` singleton. Atoms `Wm_<i>` and `Wp_<i>` (as
// well as the compound `WmOverWp_<i>`) are pre-allocated as PolyCtx
// variable names by `build_algebraic_letter_var_list`, mirroring how
// `build_mzv_var_list` pre-loads MZV basis variables.
//
// HyperInt.mpl does not have algebraic-letter support; cross-
// validation is Mma ↔ HF only. The Maple runner emits `skip` for
// every algebraic-letter op.

#pragma once

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace hyperflint {

// One entry of the algebraic-letter table: the per-pair metadata that
// HyperIntica's `$HyperAlgebraicLetterTable[idx]` records.
struct AlgebraicLetterEntry {
    long  idx;             // 1-based, matches Mma's `$HyperAlgebraicLetterCounter`
    Poly  polynomial;      // the degree-2 factor as it came from factor()
    long  var_idx;         // PolyCtx index of the special variable in `polynomial`
    Rat   lc;              // leading coefficient in `var_idx`
    Rat   sum_value;       // -b/lc      = Wm + Wp  (Vieta sum)
    Rat   product_value;   //  c/lc      = Wm · Wp  (Vieta product)
    Poly  discriminant;    // b² - 4·lc·c

    // Convenience accessor: the atom name "Wm_<i>" / "Wp_<i>".
    std::string wm_name() const;
    std::string wp_name() const;
    std::string wm_over_wp_name() const;
};

// Process-global registry of allocated algebraic-letter pairs.
//
// Lifetime: persists across CLI calls within a single process. Tests
// that need a clean slate (e.g. fixtures that assert "i = 1") must
// call `clear()` at setup. The CLI's `algebraic_letters_clear` op
// exposes this for the cross-validation harness.
//
// Thread-safety: all public methods hold an internal std::mutex.
// `allocate` is also content-deduplicating — a second call with a
// poly equal to one already in the table returns the existing
// index rather than creating a new entry. This preserves serial-
// equivalent output (the al_alloc regression fixtures assert
// `idx == 1` for the first allocation) even under concurrent
// callers from the OpenMP integration-step loop.
class AlgebraicLetterTable {
public:
    static AlgebraicLetterTable& global();

    // Allocate a fresh entry or return the index of an existing
    // entry whose polynomial equals `polynomial` (by content). The
    // PolyCtx-bound storage uses the `polynomial`'s ctx (so the
    // caller must have already extended ctx to include the
    // `Wm_<idx>` / `Wp_<idx>` / `WmOverWp_<idx>` variable names —
    // typically via `build_algebraic_letter_var_list`).
    long allocate(Poly polynomial, long var_idx);

    const AlgebraicLetterEntry& at(long idx) const;
    long size() const;
    void clear();
    std::vector<long> indices() const;

private:
    AlgebraicLetterTable() = default;
    // std::deque preserves reference/pointer stability on push_back,
    // so `at()` can return a const reference safely even when
    // concurrent `allocate` calls grow the storage. std::vector would
    // invalidate on reallocation.
    std::deque<AlgebraicLetterEntry> entries_;
    // Maps canonical poly string -> existing idx, for content-based
    // dedup under concurrent callers.
    std::unordered_map<std::string, long> content_index_;
};

// Pre-allocate the `Wm_<i>` / `Wp_<i>` / `WmOverWp_<i>` symbol names
// for i = 1..pool_size, appending them to `vars` in a deterministic
// order. The default pool_size is 16 (Phase-8 STBenchmark Long peaks
// at ~6 simultaneous pairs; 16 is a 2× safety margin).
//
// Returns the augmented variable list; the caller constructs a
// `PolyCtx` from it.
std::vector<std::string> build_algebraic_letter_var_list(
    const std::vector<std::string>& vars,
    long pool_size = 16);

// Convenience: combine the user-vars + MZV basis + algebraic-letter
// atom pool into one variable list. Equivalent to
// build_algebraic_letter_var_list(build_mzv_var_list(table, vars)).
// Most CLI handlers in Phase 7+ use this rather than the two
// separate builders.
struct MzvReductionTable;   // forward decl
std::vector<std::string> build_full_var_list(
    const MzvReductionTable& table,
    const std::vector<std::string>& vars,
    long pool_size = 16);

// Phase 7-iv: combine Wm_i / Wp_i ratios into the compound atom
// WmOverWp_i. For each pair `i`, substitutes `Wm_i = WmOverWp_i · Wp_i`
// into r and lets the Rat normalization cancel any common Wp_i factors
// between numerator and denominator. Accepted only if the substitution
// shrinks the total Wm/Wp/WmOverWp atom count — otherwise the pair
// passes through unchanged (the substitution can introduce more atoms
// than it cancels when num doesn't have enough Wm_i to match den's
// Wp_i factor).
//
// This is HF's analogue of Mma's stCombineWmWpRatios (a SubTropica-
// side symbol-level pass). HyperIntica itself does not actively
// introduce WmOverWp atoms; the function exists so HF callers that
// want WmOverWp-flavored output can opt in.
Rat combine_wm_wp_ratios(const Rat& r);

// Phase 7-v: back-substitution to evaluate Wm_i / Wp_i at concrete
// roots. For each pair `i` allocated in the AlgebraicLetterTable,
// substitutes:
//
//   Wm_i  ->  sum_value/2 - sqrt_disc_<i>/(2 · lc)
//   Wp_i  ->  sum_value/2 + sqrt_disc_<i>/(2 · lc)
//
// where `sqrt_disc_<i>` is a pre-allocated PolyCtx symbol representing
// √(discriminant_i). The user can then substitute `sqrt_disc_<i>`
// with a numeric value (e.g., `sqrt(5)` for the discriminant of
// `x² - 5`) at evaluation time.
//
// Mirrors HyperIntica's `GetAlgebraicBackSubRules[]` (line 3018) +
// the user-facing rule application.
Rat back_substitute(const Rat& r);

// Phase 7-iii: Vieta simplifier.
//
// Reduce `r` to use each `Wm_i` and `Wp_i` only at degree ≤ 1, then
// collapse any `Wm_i · Wp_i` monomial via the Vieta product. Per pair
// `i` allocated in the AlgebraicLetterTable, this applies:
//
//   Wm_i^n  ->  sum_i · Wm_i^(n-1) - product_i · Wm_i^(n-2)   (n ≥ 2)
//   Wp_i^n  ->  sum_i · Wp_i^(n-1) - product_i · Wp_i^(n-2)   (n ≥ 2)
//   Wm_i · Wp_i  ->  product_i
//
// Both `Wm_i` and `Wp_i` are kept in the result (Mma's normal form);
// `Wp_i` is NOT eliminated via the sum substitution.
//
// Mirrors HyperIntica.wl:3066-3120 (`SimplifyWithVieta`). The Mma
// version gives up if the denominator contains any `Wm_i` or `Wp_i`;
// HF does the same — when the denominator carries an algebraic atom,
// the input Rat is returned unchanged with no error. The CLI op
// `simplify_with_vieta` reports `denominator_has_atoms: true` in
// that branch so the caller can see why nothing happened.
Rat simplify_with_vieta(const Rat& r);

}  // namespace hyperflint
