// Phase 5c: TransformWord / TransformShuffle (+ helpers).
//
// Each TransformWord / TransformShuffle output is a list of pairs
//   { shuffle_part (Wordlist),  regulator_part (Regulator) },
// where the regulator_part is a Q-linear combination of "regulator
// keys" -- an empty key, a single Word, or a sorted list of Words
// (representing a product of periods). Mirrors HyperIntica.wl:
//
//   TransformShuffle       line 2555
//   TransformWord          line 2658
//   ShuffleSymbolic        line 2371
//   ReglimWord             line 5237   (Phase-5c stub; see notes)
//
// -----------------------------------------------------------------------------
// Types
// -----------------------------------------------------------------------------
// RegKey      : std::vector<Word>              -- [], {W}, or {W1,W2,...}
// RegTerm     : { Rat coef; RegKey key; }      -- one term of a regulator
// Regulator   : std::vector<RegTerm>           -- Q-linear combo of RegKeys
// TransformPair   : { Wordlist shuffle; Regulator regulator; }
// TransformResult : std::vector<TransformPair>
//
// -----------------------------------------------------------------------------
// reglim_word (Phase 5c stub)
// -----------------------------------------------------------------------------
// The full ReglimWord regularizes trailing-zero letters and evaluates
// positive-real-axis poles via BreakUpContour; those branches pull in
// Phase 6 (periods, MZV). For Phase 5c we implement the shape-preserving
// part only:
//
//   word = []                           : Regulator {{1, {}}}
//   word contains var                   : {}                 (defer)
//   FreeQ[word, var], all letters 0/-1  : {}                 (period = 0)
//   FreeQ[word, var], otherwise         : Regulator {{1, [word]}}
//
// The TransformWord port treats the stub's {} as "no regulator contribution"
// and the {{1,[word]}} case as "record the var-free subword as a symbolic
// period". No correctness is lost for the Phase 5c fixture scope (words
// whose var-free subwords never hit the deferred branches).
//
// -----------------------------------------------------------------------------
// shuffle_symbolic
// -----------------------------------------------------------------------------
// Mma:  i[[2]] may be {}, a Word, or a list of Words. We normalize all
// three to a RegKey (possibly empty), concatenate, sort by content-key,
// and accumulate.

#pragma once

#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/sym_coef_split.hpp"  // SymCoefSplit (for RegulatorSplit; B1.b)
#include "hyperflint/core/symcoef.hpp"        // SymCoef (for RegulatorSym)
#include "hyperflint/reduce/mzv_reduce.hpp"   // MzvReductionTable
#include "hyperflint/symbols/word.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hyperflint {

class ZWTable;  // fwd-decl; full def at hyperflint/core/zw_table.hpp.

using RegKey = std::vector<Word>;

struct RegTerm {
    Rat    coef;
    RegKey key;
};

using Regulator = std::vector<RegTerm>;

// SymCoef-valued analog of Regulator. Bug #6 lift
// (docs/next_session_bug6_plan.md): the per-step regulator chain emits
// SymCoef coefficients so `reglim_word`'s positive-letter branch can
// carry `I*Pi*delta[var]` residues that `Rat` cannot hold.
struct RegTermSym { SymCoef coef; RegKey key; };
using RegulatorSym = std::vector<RegTermSym>;

// SymCoefSplit-valued analog of RegulatorSym. Phase-B B1.b
// (notes/hf_mzv_rewrite_design_2026-05-05/b1_scoping_memo.md): the
// per-step regulator chain in `transform_word_impl` / `transform_shuffle`
// dispatches on `runtime::scalar_rep_enabled()`; under HF_USE_SCALAR_REP=1
// the working type becomes `RegulatorSplit`, then is converted back to
// `RegulatorSym` at the function's API boundary via
// `SymCoefSplit::as_rat()`. SymCoefSplit must be a complete type here
// (value member of RegTermSplit), so this header pulls
// `core/sym_coef_split.hpp` directly (added above). The transitive
// include chain delta over the iter-10 baseline is only `rat_split.hpp`
// + `zw_table.hpp`, both already pulled in by sym_coef_split's TU and
// already part of the HF translation-unit graph for any caller of
// transform.cpp.
struct RegTermSplit { SymCoefSplit coef; RegKey key; };
using RegulatorSplit = std::vector<RegTermSplit>;

// Wordlist with SymCoef coefficients — used by the SymCoef-valued
// BreakUpContour path.
struct WordlistSymTerm { SymCoef coef; Word word; };
struct WordlistSym     { std::vector<WordlistSymTerm> terms; };

struct TransformPair {
    Wordlist  shuffle;
    Regulator regulator;
};

using TransformResult = std::vector<TransformPair>;

// SymCoef-valued transform-pair. The shuffle part stays Wordlist
// (shuffle coefficients are rational polynomials in the Schwinger
// parameters — no transcendental residues enter there); the regulator
// lifts to RegulatorSym.
struct TransformPairSym {
    Wordlist     shuffle;
    RegulatorSym regulator;
};

using TransformResultSym = std::vector<TransformPairSym>;

// Canonicalize a RegKey: sort its Words by content_key (strict weak,
// stable on ties).  Returns a fresh copy.
RegKey canonicalize_regkey(const RegKey& k);

// String hash for a RegKey: concatenated per-Word content_keys, with a
// nested separator that differs from Word::content_key's.  Suitable as
// an unordered_map<std::string, ...> key.
std::string regkey_content_key(const RegKey& k);

// 2026-04-26 (a-prime lever): 128-bit structural hash over the
// canonicalized RegKey content. Folds `Word::struct_hash()` per-word
// with a word-separator sentinel. Replaces `regkey_content_key()` on
// the `PolesBucket::bump` and post-OMP `flat` map paths, where the
// string-build dominated `loop_residual_s`.
//
// Caller contract: the input must already be canonicalised (sorted)
// — `canonicalize_regkey()` is unchanged and still produces the
// deterministic ordering. The structural hash assumes the sort has
// happened so two equivalent canonical RegKeys hash identically.
std::pair<uint64_t, uint64_t> regkey_struct_hash(const RegKey& k);

// Collect duplicate RegKeys in a Regulator, summing their coefs and
// dropping zero-coef terms. First-occurrence insertion order preserved.
Regulator collect_regulator(const Regulator& r);

// SymCoef-valued analog of collect_regulator: collect duplicate
// RegKeys, sum SymCoef coefs, drop zero-coef terms, preserve insertion
// order.
RegulatorSym collect_regulator_sym(const RegulatorSym& r);

// ShuffleSymbolic(A, B): each term is { coef, Sort[Join[A_key, B_key]] }
// with coefficients multiplied and collected.
Regulator shuffle_symbolic(const Regulator& a, const Regulator& b);

// SymCoef-valued analog: multiplies SymCoef coefficients per pair, joins
// RegKeys and canonicalizes, accumulates.
RegulatorSym shuffle_symbolic_sym(const RegulatorSym& a,
                                   const RegulatorSym& b);

// Regularized-limit word. See header comment above.
// The ctx is required to construct the "1" coefficient for the empty
// word and for the generic {{1, word}} output; it must match the ctx
// of any letters in `word`. Bug #6 lift: returns RegulatorSym so the
// positive-letter branch can emit `I*Pi*delta[var]` residues.
//
// C0c.1 iter-66 (path 1a sub-iter 3 — ABI cascade): mandatory
// `std::shared_ptr<ZWTable> zw_tab` parameter (Option A; ratified at
// iter-50 MEMO §6 Q1 + iter-51 §6.5; per iter-63 audit MEMO §5.3 +
// §6.2). Threaded into the v1 SymCoef <-> SymCoefSplit round-trip
// lambda (`apply_v1_roundtrip`) at transform.cpp so the persistent
// table allocated by the outermost driver (hyperflint_sym /
// hyper_int.cpp:463-466 or CLI handle_reglim_word /
// bridge/cli/main.cpp:3097) is reused across nested calls. Mirrors
// `transform_word` / `transform_shuffle` signatures (iter-52b cascade)
// and sites 2/3/5 lambda kills (iter-64/65). Note: the local
// `bcs_zw_tab_local` allocation in reglim_word's positive-letter
// branch (transform.cpp:601-616) is preserved by iter-66; replacing
// it with the threaded `zw_tab` is an optional fold deferred to
// iter-67+ (out-of-scope per iter-63 audit MEMO §5.3 task list).
RegulatorSym reglim_word(const PolyCtx& ctx,
                          const Word& word,
                          size_t var_idx,
                          const MzvReductionTable& table,
                          std::shared_ptr<ZWTable> zw_tab);

// Canonicalize a Regulator: collect duplicate RegKeys (via
// collect_regulator) and then sort by RegKey content_key so the
// resulting vector compares byte-exact across equivalent inputs.
Regulator canonicalize_regulator(const Regulator& r);

// SymCoef-valued analog of canonicalize_regulator. Implemented in
// break_up_contour.cpp (where RegulatorSym originally lived) —
// the declaration is mirrored here so the per-step regulator chain
// in transform.cpp can use it without taking a dependency on
// break_up_contour.hpp (which would create a circular include via
// break_up_contour.hpp -> transform.hpp).
RegulatorSym canonicalize_regulator_sym(const RegulatorSym& r);

// ---- Phase-B B1.b: RegulatorSplit helpers (dead code at B1.b) ----
//
// Siblings of the `_sym` helpers above, with `SymCoef` replaced by
// `SymCoefSplit`. No call site dispatches to these at B1.b; B1.c flips
// `transform_word_impl` and `transform_shuffle` to call them under
// `runtime::scalar_rep_enabled()`, then converts back at the function
// boundary via `SymCoefSplit::as_rat()`. Bodies live in transform.cpp.
//
// All four take a same-shape `RegulatorSplit` and assume every
// `RegTermSplit::coef` shares wide_ctx / narrow_ctx / ZWTable instance
// (shared_ptr equality), which is the natural runtime invariant within
// a single `transform_word_impl` / `transform_shuffle` call. A mismatch
// throws via the underlying `SymCoefSplit::add` / `SymCoefSplit::mul`.

// Collect duplicate RegKeys, sum SymCoefSplit coefs, drop zero-coef
// terms, preserve first-occurrence insertion order.
RegulatorSplit collect_regulator_split(const RegulatorSplit& r);

// Cartesian product of regulator entries; per pair, multiply
// SymCoefSplit coefs and join+canonicalize RegKeys; collect.
RegulatorSplit shuffle_symbolic_split(const RegulatorSplit& a,
                                      const RegulatorSplit& b);

// Collect duplicate RegKeys (as collect_regulator_split) and then
// stable-sort by RegKey content_key for byte-stable output across
// equivalent inputs.
RegulatorSplit canonicalize_regulator_split(const RegulatorSplit& r);

// Scale every term's SymCoefSplit coef by a Rat scalar. Returns a fresh
// RegulatorSplit (input unchanged). Each `coef` must be constructible
// against the same narrow_ctx as `s`'s ambient wide_ctx; the underlying
// `SymCoefSplit::mul_rat` handles the wide-ctx -> narrow split + ZW
// product.
RegulatorSplit scalar_mul_regulator_split(const RegulatorSplit& r,
                                          const Rat& s);

// Content hash of a Regulator. The input is canonicalized internally,
// so equivalent (but unsorted / dup-RegKey) inputs hash identically.
std::string regulator_content_key(const Regulator& r);

// SymCoef-valued analog of regulator_content_key.
std::string regulator_sym_content_key(const RegulatorSym& r);

// ---- Phase 5c-ii: TransformWord ----
//
// Port of HyperIntica.wl:2658. For the empty word, returns the base-case
// pair list `{ {shuffle={{1,{}}}, regulator={{1,{}}}} }`. For words whose
// last letter is 0, throws (mirrors HyperIntica's `$Failed` sentinel).
//
// The Phase 5c stub of reglim_word means TransformWord's trailing-zero
// regularization branch (the `Length[sub] > 0 && !FreeQ[word, var]`
// pre-population) never fires — but all the pole-extraction logic
// still runs, so fixtures that don't require the Phase 6 period-eval
// path are handled exactly.
class TransformFailed : public std::runtime_error {
public:
    TransformFailed() : std::runtime_error("TransformWord: $Failed") {}
};

// Phase 7-vi-a: `introduce_algebraic_letters`, when true, propagates
// through to the inner linear_factors call (transform.cpp:342) so that
// deg-2 irreducible numerators introduce Wm/Wp atom pairs. Default
// false preserves existing behavior.
//
// Iter-52 C0c.1 Increment β: mandatory `std::shared_ptr<ZWTable> zw_tab`
// parameter (Option A; ratified at iter-50 MEMO §6 Q1 + iter-51 §6.5).
// Threaded through to the inner `linear_factors` call (replacing the
// iter-52a caller-side transient inside transform_word_impl).
TransformResultSym transform_word(const PolyCtx& ctx,
                                   const Word& word,
                                   size_t var_idx,
                                   const MzvReductionTable& table,
                                   std::shared_ptr<ZWTable> zw_tab,
                                   bool introduce_algebraic_letters = false);

// ---- Phase 5e-i: Shuffle-key combinators used by IntegrationStep ----

// Normalize a RegKey: sort its words by content_key. Alias for
// canonicalize_regkey, matching HyperIntica's name NormalizeShuffleKey.
inline RegKey normalize_shuffle_key(const RegKey& k) {
    return canonicalize_regkey(k);
}

// Concatenate two RegKeys and normalize the result.  Mirrors
// HyperIntica's CombineShuffleKeys (line 2790): `Sort[Join[aW, bW]]`
// with the same empty / single-Word / multi-Word convention.
RegKey combine_shuffle_keys(const RegKey& a, const RegKey& b);

// ---- Phase 5e-i: ShuffleList — input type for IntegrationStep ----
//
// Each entry is  coef * Hlog_{shuffle}(var, ...)  where `shuffle` is a
// list of Words meant to be shuffle-multiplied inside Hlog. The coef
// can be a rational function of var and the remaining variables.
struct ShuffleEntry {
    Rat coef;
    std::vector<Word> shuffle;
};

using ShuffleList = std::vector<ShuffleEntry>;

// ---- Phase 6d-vi: ShuffleListSym — SymCoef-valued input for
// intermediate steps that carry symbolic residues (I*Pi*delta[var])
// emitted by Fragment P2's positive-letter closure. For tst0-style
// integrands where Fragment P2 only fires at the final step, every
// ShuffleEntrySym has a Rat-pure coef and is equivalent to
// ShuffleEntry. For tst1-style integrands where an intermediate
// step triggers Fragment P2 (via reglim_word's positive-letter
// branch), the coef carries the residue as a symbolic factor and
// propagates through subsequent integration_step calls as an outer
// scalar on the RegulatorSym contributions.
struct ShuffleEntrySym {
    SymCoef coef;
    std::vector<Word> shuffle;
};

using ShuffleListSym = std::vector<ShuffleEntrySym>;

// ---- Phase 5c-iii: TransformShuffle ----
//
// Input: a list of Words (interpreted as a shuffle product).
// Output: same shape as transform_word.
// Port of HyperIntica.wl:2555. Throws TransformFailed on a trailing-zero
// sub-word (mirrors HyperIntica's $Failed sentinel).
// Phase 7-vi-a: same `introduce_algebraic_letters` flag as
// transform_word; propagates into every recursive transform_word call.
//
// Iter-52 C0c.1 Increment β: same mandatory `std::shared_ptr<ZWTable>`
// parameter as transform_word.
TransformResultSym transform_shuffle(const PolyCtx& ctx,
                                      const std::vector<Word>& wordlist,
                                      size_t var_idx,
                                      const MzvReductionTable& table,
                                      std::shared_ptr<ZWTable> zw_tab,
                                      bool introduce_algebraic_letters = false);

}  // namespace hyperflint
