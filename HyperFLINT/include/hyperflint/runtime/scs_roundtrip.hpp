// Phase-B B1.c / B2+: shared SymCoef <-> SymCoefSplit round-trip helper
// for the per-call-site as_rat() boundary verifier (design v2 §3.5a).
//
// Each Phase-B B-commit that flips a per-step regulator-producing
// function to dispatch on `runtime::scalar_rep_enabled()` calls this
// helper at the function-exit boundary. Under v1 (HF_USE_SCALAR_REP=1),
// every per-pair `RegulatorSym` is round-tripped through
// `SymCoefSplit::from_rat -> as_rat`. Under HF_RAT_SPLIT_VERIFY=1, the
// helper additionally re-splits the as_rat output and checks that
// `equals_canonical` (B1.a) holds. Mismatch emits a JSON FAIL line on
// stderr (`{"hf_scs_roundtrip_verify":"FAIL","site":"<tag>"}`) and
// aborts.
//
// The helper maintains atomic counters across all call sites (B1.c,
// B2, ..., B7) and emits a single at-exit line of the form
// `hf_scs_roundtrip_verify: enabled=N calls=N terms_checked=N` so each
// per-commit gate run can assert positive proof of coverage parallel to
// `hf_rat_split_verify:` (Phase-A precondition (1)).
//
// Wide-vs-narrow ctx: at B1 the W-side variable set is empty by
// hypothesis (b1_scoping_memo.md R2; design v2 §4.4a Note 2), so passing
// `N = F = ctx` makes split puts every monomial at e_W = 0 and as_rat
// reconstitutes the wide-ctx Rat byte-exact. B2 is the first commit
// where W-side content can arrive (positive-letter Wm/Wp lift in
// `break_up_contour_sym`); for Smirnov fixtures the positive-letter
// branch never fires (linear-in-letters), so the B1 round-trip-trivial
// hypothesis still holds. The signature already takes `F` by const ref
// so a future B-commit that needs N != F is a localised change to that
// helper's caller list.

#pragma once

#include "hyperflint/core/rat.hpp"                // Rat
#include "hyperflint/core/sym_coef_split.hpp"     // SymCoefSplit
#include "hyperflint/core/zw_table.hpp"           // ZWTable
#include "hyperflint/integrator/transform.hpp"    // RegulatorSym

#include <memory>

namespace hyperflint::runtime {

// Round-trip every coef in `r` through SymCoefSplit::from_rat -> as_rat,
// using `F` as the wide ctx and (at B1/B2 on Smirnov) also as the
// narrow ctx N. `site_tag` is propagated to FAIL diagnostics so RCA
// can pinpoint the offending caller.
hyperflint::RegulatorSym roundtrip_regulator_through_scs(
    const hyperflint::RegulatorSym& r,
    const hyperflint::PolyCtx& F,
    std::shared_ptr<hyperflint::ZWTable> zw_tab,
    const char* site_tag);

// B4 sibling: round-trip a single Rat through SymCoef::from_rat ->
// SymCoefSplit::from_rat -> SymCoefSplit::as_rat -> SymCoef::as_rat.
// Used at the linear_factors post-LR-cache-lookup `as_rat` boundary
// (design v2 §3.5a B4 row): every cached LinearFactor.pole crosses the
// storage boundary, so each pole is round-tripped at the cache exit.
// Same counters (`g_calls`, `g_terms_checked`) and same at-exit emission
// (`hf_scs_roundtrip_verify:`) as the RegulatorSym sibling.
//
// Verifier-only contract identical to the RegulatorSym sibling:
//   - Under HF_USE_SCALAR_REP=1 the round-trip is performed
//     unconditionally (no-op on identity at B-stages on Smirnov);
//   - Under HF_RAT_SPLIT_VERIFY=1 (additionally) the as_rat output
//     is re-split and `equals_canonical` checked. Mismatch emits
//     `{"hf_scs_roundtrip_verify":"FAIL","site":"<tag>"}` and aborts.
hyperflint::Rat roundtrip_rat_through_scs(
    const hyperflint::Rat& r,
    const hyperflint::PolyCtx& F,
    std::shared_ptr<hyperflint::ZWTable> zw_tab,
    const char* site_tag);

}  // namespace hyperflint::runtime
