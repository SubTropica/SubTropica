// LR-order search: find the variable-integration order that makes an
// Euler integrand linearly reducible.  C++ port of SubTropica's
// STFasterFubini2 (SubTropica.wl:11050-11149) for the single-group,
// FindRoots=False, LeafCountLinear MVP scope.
//
// Phase β.2 scope:
//   - single polynomial group (SubTropica's "groupMembers == 1" case)
//   - Heuristic = LeafCountLinear
//   - FindRoots = False
//   - Proportionality dedup via canonical form (Phase-α.2 semantics,
//     stricter than Mma's PPQ; equivalent over ℚ)
//   - Subsets DP with bitmask-indexed memoization; O(2^n * n) states

#pragma once

#include "hyperflint/core/poly.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hyperflint {
namespace lr_search {

// Result shape.  `order.empty() && score == +inf` means NOLR (no
// linearly reducible order exists for this input).
//
// `root_polys` (Phase 7-vii): when the search accepted deg-2 steps
// (i.e., `allow_algebraic_letters` was true), this collects the
// distinct deg-2 polynomials that appeared as the "current poly in
// var" at the var-removal step.  These are the polys the integration
// stage will turn into Wm/Wp pairs (HF's `linear_factors` allocates
// AlgebraicLetterTable entries for them).  Mma's STFasterFubini2
// returns the same list as `result[[2]]` when FindRoots=True, and
// downstream STIntegrate consumers (STApplyRootFactoring) need it
// to record where to introduce algebraic letters.  Empty for the
// FindRoots=False / `allow_algebraic_letters=false` path.
struct LrResult {
    std::vector<size_t> order;       // xvar ORIGINAL-CTX indices
    double              score;
    std::vector<Poly>   root_polys;  // deg-2 polys accepted during walk
    bool nolr() const { return order.empty() && score >= 1e300; }
};

// STFubiniLR port: inner polynomial kernel.  Given a list of polys
// (intermediate state for some subset) and a pivot variable, return
// the list of factor bases of {lc(f, v) : f ∈ polys, deg(f,v) >= 0}
// ∪ {disc(f, v) : f ∈ polys, deg(f,v) >= 1}
// ∪ {res(f1, f2, v) : f1, f2 ∈ polys, deg(f1,v) ≥ 1, deg(f2,v) ≥ 1}
// after (a) factoring each via fmpq_mpoly_factor and (b) dedup via
// canonical-form equivalence (proportionality over ℚ).
std::vector<Poly> st_fubini_lr(const std::vector<Poly>& polys, size_t var_idx);

// Proportionality dedup: drop numeric / zero polys, group by canonical
// form, return one representative per equivalence class.
std::vector<Poly> dedup_proportional(const std::vector<Poly>& polys);

// N-way intersection under proportionality equivalence.  Picks
// representatives from the FIRST list.  Matches the semantics of Mma's
// `Intersection[..., SameTest -> ProportionalPolynomialsQLR]` up to
// representative choice (dedup result is the same equivalence set).
std::vector<Poly> intersect_proportional(
    const std::vector<std::vector<Poly>>& lists);

// Proxy for Mma's `LeafCount[list-of-polys]`.  Counts atoms in a
// hypothetical Mma-style tree: for each monomial, 1 for each variable
// with exponent >= 1, +1 for each variable with exponent >= 2, and
// +1 for a non-unit coefficient.  Summed over monomials across all
// polys in the list.  Within ~10% of Mma's `LeafCount` on typical
// Symanzik inputs; the exact value doesn't matter, only the relative
// ranking of candidate orders under MinimalBy does.
long leaf_count_proxy(const std::vector<Poly>& polys);

// Top-level entry point.  `group_polys` is the input list-of-lists:
// for the single-group MVP it has one element.  `xvar_indices` lists
// the integration-variable indices into the shared PolyCtx.
//
// `allow_algebraic_letters` (Phase 7-vii): when true, deg-2 polys are
// accepted at each LR step.  HF's integrator already allocates Wm/Wp
// at integration time via `linear_factors`, so the LR pass only needs
// to admit deg-2 as a valid step shape.  When false (the classic
// FindRoots=False semantics) only linear (deg ≤ 1) polys are accepted.
LrResult find_lr_orders(
    const std::vector<std::vector<Poly>>& group_polys,
    const std::vector<size_t>& xvar_indices,
    bool allow_algebraic_letters = false);

}  // namespace lr_search
}  // namespace hyperflint
