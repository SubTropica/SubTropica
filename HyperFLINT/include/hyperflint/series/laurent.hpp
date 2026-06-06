// Phase 5e-i: Laurent series expansion of a Rat at var = 0.
//
// series_expansion(f, var, max_order)
//   = sum_{k = pole_degree(f, var)}^{max_order} c_k * var^k
//
// Mirrors HyperIntica.wl:3405 (SeriesExpansion) and HyperInt.mpl:774
// (seriesExpansion). Used by IntegrationStep (Phase 5e) for both the
// zero and infinity expansions of primitive-coefficient rational
// functions.
//
// Algorithm (Cauchy-product recurrence):
//   Let  f = num / den, with pole_degree = nmin(num) - nmin(den) in var.
//   Strip var^nmin from num, var^dmin from den -> p', q' with
//   p'(0), q'(0) != 0.  Solve  p' = q' * sum_j c_j var^j  for c_j:
//
//     c_0 = p'_0 / q'_0
//     c_j = ( p'_j - sum_{i=1..j} q'_i * c_{j-i} ) / q'_0   (j >= 1)
//
//   up to j = max_order - pole_degree, then assemble
//
//     result = sum_j c_j * var^(pole_degree + j).
//
// The returned Rat has an exact (not approximate) Laurent tail.

#pragma once

#include "hyperflint/core/rat.hpp"

#include <cstddef>

namespace hyperflint {

Rat series_expansion(const PolyCtx& ctx,
                     const Rat& f,
                     size_t var_idx,
                     long max_order);

// Substitute var_idx -> 1/var_idx  (i.e., f(1/var)).
// Implementation: multiply num & den by var^N where N = max(deg num,
// deg den) in var, then reverse the var-grading. The Rat ctor
// canonicalizes any common-var factors out afterward.
Rat substitute_var_reciprocal(const PolyCtx& ctx, const Rat& f, size_t var_idx);

// Coefficient of var^0 in the Laurent expansion of `r` at var = 0.
// Two accepted input shapes:
//   (a) `r.den()` does not vanish at var=0 (no pole): returns
//       num(var=0) / den(var=0) directly. Required for the
//       integration_step trivial-convergence branch, whose input
//       after `substitute_var_reciprocal` is a general rational
//       function in var.
//   (b) `r.den()` is a pure var-monomial (a power of var possibly
//       times a var-free factor): the legacy shape produced by
//       series_expansion. Returns num.coeff(var^k) / den.coeff(var^k).
// Throws if the denominator vanishes at var=0 AND is non-monomial in
// var — in that case the caller must route through series_expansion.
Rat rat_var0_coefficient(const PolyCtx& ctx, const Rat& r, size_t var_idx);

}  // namespace hyperflint
