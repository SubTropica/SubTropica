// Phase 6d: BreakUpContour.
//
// HyperIntica.wl:5136. Given a wordlist (list of {coef, word}) and an
// "onAxis" list describing positive letters that lie on the integration
// contour, produce a Regulator (list of {coef, RegKey}) that encodes
// the integral split by contour deformation.
//
// Two flavors:
//   * `break_up_contour`     : Phase 6d-ii base case (onAxis empty)
//                              with Rat-valued coefficients. Throws if
//                              onAxis is non-empty.
//   * `break_up_contour_sym` : Phase 6d-v-ii recursive case with
//                              SymCoef-valued coefficients. Handles the
//                              full positive-letter contour deformation
//                              via RegTail with substitute
//                              `Pi*I*imPart - Log[smallest]`.
//
// The plain `break_up_contour` remains for callers that don't need the
// symbolic-residue path. `break_up_contour_sym` is what
// integration_step's positive-letter continuation calls (Phase
// 6d-v-iii).

#pragma once

#include "hyperflint/core/symcoef.hpp"
#include "hyperflint/core/zw_table.hpp"          // ZWTable (C0b.4 iter-42 zw_tab parameter)
#include "hyperflint/integrator/transform.hpp"   // Regulator, RegKey
#include "hyperflint/reduce/mzv_reduce.hpp"
#include "hyperflint/symbols/word.hpp"

#include <cstddef>
#include <memory>                                 // std::shared_ptr (C0b.4 iter-42)
#include <vector>

namespace hyperflint {

// Plain on-axis entry (legacy; used by the Phase 6d-ii base case only).
struct OnAxisEntry {
    Rat letter;
    long im_side;   // +1 or -1
};

Regulator break_up_contour(const PolyCtx& ctx,
                            const Wordlist& wordlist,
                            const std::vector<OnAxisEntry>& on_axis,
                            const MzvReductionTable& table);

// ---- Phase 6d-v-ii: SymCoef-valued recursive case ----
//
// RegTermSym, RegulatorSym, WordlistSym, WordlistSymTerm moved to
// transform.hpp (Bug #6 lift) so the per-step regulator-producing
// chain can share them. OnAxisSymEntry stays here — it's specific to
// BreakUpContour.

// On-axis entry for the SymCoef path: a positive letter and the
// SymCoef-valued imaginary part (typically delta[var]).
struct OnAxisSymEntry {
    Rat     letter;
    SymCoef im_part;
};

// Promote a plain Wordlist (Rat coefs) to a WordlistSym (SymCoef coefs
// via SymCoef::from_rat per term). Used when integration_step calls the
// SymCoef path on a Rat-coef input.
WordlistSym to_wordlist_sym(const Wordlist& wl);

// canonicalize_regulator_sym: declared in transform.hpp (it's a
// per-step regulator helper used by transform_word / transform_shuffle
// too).

// Phase 6d-v-ii recursive BreakUpContour.
//
// Input: a SymCoef-valued wordlist and an on_axis list (positive
// letters with their delta[var] sides). Output: a RegulatorSym.
//
// Base case (on_axis empty): partition wordlist into period-evaluable
// vs not — exactly the Phase 6d-ii base case lifted to SymCoef.
//
// Recursive case (on_axis non-empty):
//   smallest = on_axis[0].letter
//   imPart   = on_axis[0].im_part
//   newAxis  = [(l - smallest, ip) for (l, ip) in on_axis[1:]]
//   substitute = Pi*I*imPart - Log[smallest]   (SymCoef)
//   For each word in wordlist, for each split i in 0..len(word):
//     tail = word[i+1..] (or {} if i == len)
//     temp = (smallest == 1 && tail numeric)
//              ? ZeroOnePeriod[tail]                  -> Rat
//              : ConvertABtoZeroInf(RegHead({{1,tail}},smallest), 0, smallest)
//     head = [letter - smallest for letter in word[0..i]]
//     brk  = break_up_contour_sym(reg_tail_sym({{coef, head}}, 0, substitute),
//                                  newAxis)
//     Combine via shuffle: result[Sort[Join[bW, tW]]] += b.coef * t.coef
//
// C0b.4 (iter-42, 2026-05-09): mandatory `std::shared_ptr<ZWTable> zw_tab`
// parameter. Per iter-40 `api_design.md` §3.1 + §3.2 (mandatory, no
// default-`nullptr`). Caller owns the persistent ZWTable; the lambda body
// inside `break_up_contour_sym` short-circuits on `runtime::scalar_rep_enabled()`
// before any `zw_tab` use, so a null shared_ptr is safe at default-OFF
// (`HF_USE_SCALAR_REP` unset) and is the canonical iter-42 transitional
// state at intermediate callsites that have not yet received the persistent
// table from `hyperflint_sym` (iter-43+ closes this loop). Per design v2 §3.6a
// the ZWTable is non-thread-safe; the sole multi-threaded callsite of
// `break_up_contour_sym` (`integration_step.cpp::close_positive_letters_in_regulator_sym`,
// line 983, inside an OMP `parallel for`) allocates a per-iteration table at
// the callsite, so each OMP worker holds its own table for the duration of
// one outer term. Recursive calls inside the body share the caller's table.
RegulatorSym break_up_contour_sym(const PolyCtx& ctx,
                                    const WordlistSym& wordlist,
                                    const std::vector<OnAxisSymEntry>& on_axis,
                                    const MzvReductionTable& table,
                                    std::shared_ptr<ZWTable> zw_tab);

}  // namespace hyperflint
