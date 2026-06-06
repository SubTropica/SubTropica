// rat_split: wide-ctx Rat <-> SymMonomialSplit-list adapter.
//
// Phase-A commit (2) introduces the round-trip primitive that the
// HF MZV-as-scalar rewrite design v2 §3.5 calls
// `SymCoefSplit::from_rat_split` / `SymCoefSplit::as_rat`. We expose
// it here as free functions so that `SymCoef` does not have to take
// `<core/zw_table.hpp>` as a transitive include — the verifier wired
// in commit (3) calls a thin `SymCoef::verify_rat_split_roundtrip`
// member that delegates to these.
//
// The decomposition is:
//   r_F  =  num_F(N, W)  /  den_F(N, W)
//        =  ( sum_{e_W ∈ supp_W(num_F)}  q_{e_W}(N) · W^{e_W} ) / den_F(N, W)
//
// where `q_{e_W}(N)` is a polynomial purely in the narrow-ctx
// variables (Feynman + MZV basis), and `W^{e_W}` is the W-only
// monomial with exponent vector `e_W` over Mandelstam, mass, Wm/Wp.
//
// `split_rat_by_w_monomial` produces one `SymMonomialSplit` per
// distinct `e_W`. All such splits share a single `den_zw` handle
// (the wide-ctx denominator, interned whole).
//
// `recombine_rat_split` is the inverse: it transplants each
// `num_N` from the narrow ctx into the wide ctx, multiplies by
// `ZWTable::get(num_zw)`, accumulates over all splits, and divides
// by `ZWTable::get(den_zw)`.

#pragma once

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/zw_table.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace hyperflint {

// One leaf of a SymCoefSplit decomposition. Carries the narrow-ctx
// numerator polynomial, two ZWTable handles indexing into the
// wide-ctx side-table, and the transcendental power-flags that the
// design v2 §3.4 specifies:
//
//   num_zw       — points at the W-only monomial (single term in W)
//                  that this split represents from the source Rat's
//                  numerator.
//   den_zw       — points at the source Rat's denominator (wide-ctx
//                  Poly, kept whole; sign-canonicalized at intern
//                  time).
//   pi_power     — exponent on the literal Pi factor; ≥ 0.
//   i_power      — exponent on the imaginary unit; canonical {0, 1}
//                  modulo I^2 = -1, sign folded into num_N.
//   log_powers   — Log[n]^k indexed by integer n > 0.
//   delta_powers — delta[var]^k indexed by var name; canonical
//                  {0, 1} modulo delta^2 = +1.
//
// Commit (2) used this struct without transcendentals (all defaults).
// Commit (5) adds the trans-flags so SymCoefSplit (the SymCoef-shaped
// container) can store its terms_ as a vector<SymMonomialSplit>
// directly, mirroring SymCoef's terms_ shape but with the prefactor
// factored into (num_N, num_zw, den_zw).
struct SymMonomialSplit {
    Poly                         num_N;
    ZWHandle                     num_zw    = ZW_ONE;
    ZWHandle                     den_zw    = ZW_ONE;
    int                          pi_power  = 0;
    int                          i_power   = 0;
    std::map<long, int>          log_powers;
    std::map<std::string, int>   delta_powers;
};

// Split a wide-ctx Rat into a list of SymMonomialSplits, binning the
// numerator's monomials by their W-exponent.
//
// Parameters:
//   r_F            : source Rat over the wide ctx F = N ⊕ W.
//   N              : narrow ctx (Feynman + MZV basis).
//   tab            : ZWTable carrying the shared W-side polynomials.
//                    The resulting splits' num_zw / den_zw handles
//                    index into this table.
//   F_to_N_idx     : per-variable mapping from F's variable indices
//                    to N's. `F_to_N_idx[i] == SIZE_MAX` marks a
//                    W-side variable (Mandelstam / mass / Wm / Wp);
//                    those are NOT permitted to appear in any
//                    SymMonomialSplit's num_N. The vector must have
//                    length == F.vars().size().
//
// Returns: vector with one SymMonomialSplit per distinct W-monomial
// of the numerator. The denominator is interned WHOLE (signature
// kept) and shared across all returned splits via `den_zw`.
//
// Throws std::runtime_error if r_F is degenerate (zero den) or if
// the F→N mapping rejects a numerator monomial that touches a
// W-variable's slot in N (a bug in the caller's index map).
std::vector<SymMonomialSplit>
split_rat_by_w_monomial(const Rat& r_F,
                        const PolyCtx& N,
                        ZWTable& tab,
                        const std::vector<size_t>& F_to_N_idx);

// Inverse: recombine a SymMonomialSplit list back into a wide-ctx
// Rat over F. Bit-identical to the source Rat under the round-trip
// invariant (i.e. `recombine_rat_split(split_rat_by_w_monomial(r))
// == r`).
//
// Parameters:
//   parts          : split list as produced by split_rat_by_w_monomial.
//   F              : wide ctx (the destination ring).
//   tab            : ZWTable carrying the W-side polynomials.
//   N_to_F_idx     : per-variable mapping from N's variable indices
//                    to F's. Length == N.vars().size(); each entry
//                    must be a valid F variable index.
Rat recombine_rat_split(const std::vector<SymMonomialSplit>& parts,
                        const PolyCtx& F,
                        ZWTable& tab,
                        const std::vector<size_t>& N_to_F_idx);

// Build the F→N and N→F index maps from two PolyCtx instances by
// matching variable names. Variables in F but not in N map to
// SIZE_MAX in F_to_N_idx (those are W-side vars). Throws if any N
// variable does not have a corresponding F variable.
struct FNIndexMaps {
    std::vector<size_t> F_to_N_idx;  // size F.nvars; SIZE_MAX = W-side
    std::vector<size_t> N_to_F_idx;  // size N.nvars; every entry valid
};
FNIndexMaps build_fn_index_maps(const PolyCtx& F, const PolyCtx& N);

}  // namespace hyperflint
