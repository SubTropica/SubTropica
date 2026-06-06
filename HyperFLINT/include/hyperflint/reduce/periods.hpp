// Phase 6b/6f: period evaluators.
//
// Input: a Word whose letters are literal integers (typically in the
// set {-1, 0, 1}). Output: a Rat in the transcendental basis
// (Log2, mzv_2, mzv_3, ...).
//
// `to_mzv(wl)` mirrors HyperIntica.wl:3537. Given a Wordlist where
// each word's letters are literal integers, produce the Rat
//       sum_i coef_i * sign_i * mzv[index_sequence_i]
// by walking each word backwards and condensing runs of 0 letters
// into the multiplicities of the non-0 "pole" letters, then applying
// the standard sign / index rearrangement.
//
// `zero_one_period(word)` mirrors HyperIntica.wl:2804-2824 restricted
// to the MZV branch (SubsetQ[{-1, 0, 1}, word]):
//   empty           -> 1
//   all zeros       -> 0
//   trailing zeros  -> regularize via reg0
//   leading 1's     -> regularize via reg_head
//   otherwise       -> to_mzv + apply_mzv_reductions
//
// Phase 6b does not handle letters outside {-1, 0, 1}; those return
// Hlog[1, word]-placeholder via the symbolic fallback.

#pragma once

#include "hyperflint/core/rat.hpp"
#include "hyperflint/integrator/transform.hpp"   // Regulator, RegKey
#include "hyperflint/reduce/mzv_reduce.hpp"
#include "hyperflint/symbols/word.hpp"

namespace hyperflint {

// Forward declaration for the basis-ctx campaign cascade (PHASE_2, 2026-05-28).
// The MzvExpansionTable* parameter on the period functions below threads
// the eager-expansion table through to the mint site in `to_mzv_one_word`.
// When nullptr (default), the legacy path runs unchanged (Rat::parse +
// apply_mzv_reductions). When non-null, the mint site looks up the
// expanded basis Rat instead of allocating a Poly::gen for an LHS variable.
// See notes/hf_mzv_weight_cap_2026-05-28/design.md §5.3.
struct MzvExpansionTable;

// Convert a Wordlist (plain words with literal-integer letters) to a
// Rat-valued MZV expression. The ctx must contain every `mzv_<enc>`
// name that may appear during the conversion (use
// build_mzv_var_list).
//
// `expansion` (default nullptr): if non-null, mint-site lookups use the
// eager-expansion table instead of Rat::parse against wide-ctx LHS
// variables. PHASE_2 plumbing parameter; behavior unchanged at default.
Rat to_mzv(const PolyCtx& ctx, const Wordlist& wl,
           const MzvExpansionTable* expansion = nullptr);

// Evaluate ZeroOnePeriod[word] restricted to the MZV branch. Same ctx
// requirement as to_mzv. `expansion`: see to_mzv.
Rat zero_one_period(const PolyCtx& ctx,
                    const Word& word,
                    const MzvReductionTable& table,
                    const MzvExpansionTable* expansion = nullptr);

// Evaluate ZeroInfPeriod[word] (HyperIntica.wl:3572 ZeroInfPeriodEval)
// for letters in {-1, 0}:
//   empty           -> 1
//   all zeros       -> 0
//   all -1          -> 0     (trailing 0 via reg0 regularization)
//   letters == {-1} -> ConvertZeroOne -> ZeroOnePeriod
// Letters outside {-1, 0} throw (Phase 6d extension handles the
// rescale / two-letter / positive-axis branches via BreakUpContour).
//
// `expansion`: see to_mzv.
Rat zero_inf_period(const PolyCtx& ctx,
                    const Word& word,
                    const MzvReductionTable& table,
                    const MzvExpansionTable* expansion = nullptr);

// Phase 6f: EvaluatePeriods.
//
// Post-pass on a Regulator (typically the output of IntegrationStep /
// hyperflint): each entry {coef, key} is inspected; if the key's words
// can all be period-evaluated via zero_inf_period, the product of
// those values gets absorbed into the empty-key coefficient. Entries
// whose key can't be evaluated (parametric letters, positive letters
// out of Phase 6d scope, etc.) pass through unchanged.
//
// Mirrors HyperIntica.wl:2544 (EvaluatePeriods), restricted to the
// branches where zero_inf_period succeeds.
Regulator evaluate_periods(const PolyCtx& ctx,
                            const Regulator& r,
                            const MzvReductionTable& table);

// Phase 6d-v-v: FibrationBasis.
//
// Mirrors HyperIntica.wl:5024-5047 (FibrationBasis) +
// HyperIntica.wl:5051-5127 (FibrationBasisRecurse). See also
// HyperInt.mpl:1692-1744.
//
// The fibration basis is the canonical normal form for a multi-
// variable hyperlogarithm sum: every entry is expressed as a
// product of Hlogs, one per var, with Rat coefficients that have
// been reduced to periods (via zero_inf_period) at every leaf.
//
// Result shape: a flat list of (key, coef) pairs where the key is
// a vector<Word> of length `vars.size()`. `key[i]` is the word of
// `Hlog[vars[i], key[i]]` (empty means "factor is 1"). For the
// degenerate vars == [] case, the result has a single entry with
// an empty key (or no entries, if the input reduces to zero).
struct FibrationBasisResult {
    std::vector<std::string>                    vars;   // var names in order
    std::vector<std::pair<std::vector<Word>,     // key: one Word per var
                          Rat>>                  terms; // coef
};

FibrationBasisResult
fibration_basis(const PolyCtx&                   ctx,
                 const Regulator&                 input,
                 const std::vector<size_t>&       var_indices,
                 const MzvReductionTable&         table);

// Phase 6d-v-v-ii: SymCoef-valued variant. Accepts a RegulatorSym
// (the output type of hyperflint_sym / integration_step_sym), so
// Fragment-P2 residues (I*Pi*delta[x], Log[n]) propagate through
// unchanged. The Rat-valued `fibration_basis` above throws when it
// sees a non-Rat SymCoef; `_sym` handles them natively.
//
// `fibration_basis_sym` is the substrate for `test_zero_function_sym`
// below, which is in turn the substrate for the eventual
// divergence-detection pass (Phase 5e-iii / P1).
struct FibrationBasisResultSym {
    std::vector<std::string>                         vars;
    std::vector<std::pair<std::vector<Word>,
                          SymCoef>>                  terms;
};

FibrationBasisResultSym
fibration_basis_sym(const PolyCtx&                   ctx,
                     const RegulatorSym&              input,
                     const std::vector<size_t>&       var_indices,
                     const MzvReductionTable&         table);

// Phase 6e: TestZeroFunction (stub; Rat-valued).
//
// Returns a Rat that is 0 iff `r` reduces to the zero function under
// evaluate_periods. Non-empty keys remaining after evaluate_periods
// are assumed independent over the transcendental basis; the sum of
// surviving coefficients is returned. Zero iff all evaluable
// contributions cancel AND every remaining (un-evaluable) key has
// zero coefficient.
//
// This stub is **INCORRECT** in general: distinct fibration-basis
// keys are linearly independent over the transcendental basis, so
// summing coefs can cancel spuriously. Use `test_zero_function_sym`
// below when correctness matters (P1 divergence detection).
Rat test_zero_function(const PolyCtx& ctx,
                        const Regulator& r,
                        const MzvReductionTable& table);

// Phase 6d-v-v-ii: correct zero-test via fibration_basis_sym.
//
// `r` reduces to the zero function iff, after projecting to the
// fibration basis over all of `var_indices`, every term's SymCoef
// coefficient is the zero SymCoef. Distinct (var_indices-long
// vector<Word>) keys are linearly independent, so the per-term
// check is both necessary and sufficient.
//
// Mirrors HyperInt.mpl:1065 testZeroFunction's contract (Maple
// returns a residue that is simplify-to-zero iff the function is
// zero; we return `true`/`false` directly).
bool test_zero_function_sym(const PolyCtx&                  ctx,
                             const RegulatorSym&             r,
                             const std::vector<size_t>&      var_indices,
                             const MzvReductionTable&        table);

}  // namespace hyperflint
