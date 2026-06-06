// Period-tuples Phase 2 (spec 2026-06-04 §2.2/§2.3): scratch-ring mint.
//
// Under HF_PERIOD_TUPLES the main PolyCtx is SLIM (kinematic vars only).
// Boundary-period mints (break_up_contour sites) still need the exact
// production reduction semantics of zero_inf_period / zero_one_period,
// which operate in a ring containing the mzv atoms. This module keeps an
// immortal, private, atoms-only scratch ctx (no kinematic vars; mint
// polynomials there are tiny fmpq-coefficient objects), runs the
// UNCHANGED periods.cpp machinery in it, then decomposes the resulting
// Rat term-by-term into SymCoef monomials whose period content lives in
// SymMonomial::period_powers (PeriodTable ids named by atom name).
// Reduction timing and output are bit-equal to production arm-1/3; only
// the STORAGE of the result changes. Densification-neutral by
// construction (the tst3 basis-ctx failure mode cannot occur).

#pragma once

#include "hyperflint/core/symcoef.hpp"
#include "hyperflint/reduce/mzv_reduce.hpp"
#include "hyperflint/symbols/word.hpp"

namespace hyperflint {

// True iff HF_PERIOD_TUPLES=1 (read once). Aborts the process with a
// diagnostic if combined with HF_USE_SCALAR_REP=1 or HF_USE_BASIS_CTX=1
// (three representations of the same atoms cannot coexist).
bool period_tuples_enabled();

// Mint the boundary period of `w` (letters must be numeric/constant; the
// caller's site gating guarantees this) as a SymCoef over `slim`:
// zero_one=false -> zero_inf_period semantics; true -> zero_one_period.
SymCoef mint_period_sym(const PolyCtx& slim, const Word& w,
                        const MzvReductionTable& table, bool zero_one);

}  // namespace hyperflint
