// Phase 5c: TransformWord / TransformShuffle impl.
//
// This file implements:
//   * canonicalize_regkey, regkey_content_key, collect_regulator
//   * shuffle_symbolic
//   * reglim_word (stub; see header)
//
// transform_word and transform_shuffle land in 5c-ii/5c-iii.

#include "hyperflint/integrator/transform.hpp"

#include "hyperflint/reduce/period_scratch.hpp"  // period-tuples Phase 2

#include "hyperflint/algebra/linear_factors.hpp"
#include "hyperflint/algebra/shuffle.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/zw_table.hpp"               // ZWTable (B1.c v1 round-trip)
#include "hyperflint/instrumentation/dag_hashcons_probe.hpp"  // §A.1 iter-50: op_call emit at function entry
#include "hyperflint/integrator/regularize.hpp"     // regzero_word_in_ctx (Bug #6 trailing-letter port)
#include "hyperflint/reduce/break_up_contour.hpp"   // break_up_contour (Bug #6 Fragment P1)
#include "hyperflint/runtime/scalar_rep.hpp"         // runtime::scalar_rep_enabled (B1.c dispatch)
#include "hyperflint/runtime/scs_roundtrip.hpp"      // runtime::roundtrip_regulator_through_scs (B1.c/B2 verifier site)
// iter-62-β.2: §E Step E.2-impl-2 transform_shuffle outer-cache wrap.
#include "hyperflint/core/operator_memo.hpp"
#include "hyperflint/core/canonical_signature.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>      // std::abort (iter-44 require_persistent assertion)
#include <iostream>     // std::cerr (iter-44 require_persistent assertion)
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace hyperflint {

// ---------------------------------------------------------------- RegKey --
//
// Words within a RegKey are sorted by their content_key. The Mma source
// says `Sort[Join[iW, jW]]`, which uses Mma's canonical order on lists
// of rationals/letters; for cross-validation we need the *same* ordering
// on both sides. Sorting by the flattened content_key gives a strict
// deterministic order that is consistent across all HyperFLINT calls --
// but Mma may order differently. The comparator in compare.py handles
// this by multisetting the RegKey across the whole Regulator, so only
// the multiset of {coef, RegKey} pairs matters, not the internal
// ordering of words within a RegKey.

RegKey canonicalize_regkey(const RegKey& k) {
    // Drop empty words first: Hlog[var, {}] = 1 is the identity, so an
    // empty word inside a RegKey should be absorbed (RegKey `{{}}` means
    // "product of Hlog[{}]" = 1, equivalent to the empty RegKey `{}`).
    RegKey out;
    out.reserve(k.size());
    for (const auto& w : k) {
        if (!w.empty()) out.push_back(w);
    }
    std::stable_sort(out.begin(), out.end(),
        [](const Word& a, const Word& b) {
            return a.content_key() < b.content_key();
        });
    return out;
}

std::string regkey_content_key(const RegKey& k) {
    // Use ASCII 0x02 as the word-to-word separator so it cannot
    // collide with Word::content_key's 0x01 letter separator.
    std::string out;
    for (const auto& w : k) {
        out += w.content_key();
        out += '\x02';
    }
    return out;
}

// 2026-04-26 (a-prime lever): 128-bit structural hash for use in the
// PolesBucket and post-OMP `flat` map indexes. Word::struct_hash()
// memoizes per Word, so a RegKey that has flowed through a copy
// (canonicalize_regkey returns a sorted copy) only pays the per-Word
// hash cost on its first lookup.
std::pair<uint64_t, uint64_t> regkey_struct_hash(const RegKey& k) {
    auto seeds = poly_struct_hash_seed();
    uint64_t h1 = seeds.first;
    uint64_t h2 = seeds.second;
    // Top-of-RegKey sentinel + length-disambiguator (so a 0-word
    // RegKey hash differs from a single-empty-word RegKey).
    poly_struct_hash_mix(h1, h2, 0xfffcfffcfffcfffcULL);
    poly_struct_hash_mix(h1, h2, static_cast<uint64_t>(k.size()));
    for (const auto& w : k) {
        const auto wh = w.struct_hash();
        poly_struct_hash_mix(h1, h2, wh.first);
        poly_struct_hash_mix(h1, h2, wh.second);
        // Word-separator sentinel.
        poly_struct_hash_mix(h1, h2, 0xfffcffffffffffffULL);
    }
    return {h1, h2};
}

// ---- collect_regulator: group by canonicalized RegKey ----

Regulator collect_regulator(const Regulator& r) {
    std::vector<std::string> order;
    std::unordered_map<std::string, size_t> idx;
    std::vector<RegTerm> kept;

    for (const auto& t : r) {
        RegKey canon = canonicalize_regkey(t.key);
        std::string k = regkey_content_key(canon);
        auto it = idx.find(k);
        if (it == idx.end()) {
            idx[k] = kept.size();
            kept.push_back(RegTerm{t.coef, std::move(canon)});
            order.push_back(k);
        } else {
            kept[it->second].coef = kept[it->second].coef + t.coef;
        }
    }
    Regulator out;
    out.reserve(kept.size());
    for (auto& t : kept) {
        if (!t.coef.is_zero()) out.push_back(std::move(t));
    }
    return out;
}

// --------------------------------------------------- shuffle_symbolic --

Regulator shuffle_symbolic(const Regulator& a, const Regulator& b) {
    Regulator out;
    out.reserve(a.size() * b.size());
    for (const auto& ti : a) {
        for (const auto& tj : b) {
            RegKey joined;
            joined.reserve(ti.key.size() + tj.key.size());
            for (const auto& w : ti.key) joined.push_back(w);
            for (const auto& w : tj.key) joined.push_back(w);
            out.push_back(RegTerm{ti.coef * tj.coef,
                                   canonicalize_regkey(joined)});
        }
    }
    return collect_regulator(out);
}

// --------------------------- collect_regulator_sym / shuffle_symbolic_sym --

RegulatorSym collect_regulator_sym(const RegulatorSym& r) {
    std::unordered_map<std::string, size_t> idx;
    std::vector<RegTermSym> kept;
    for (const auto& t : r) {
        RegKey canon = canonicalize_regkey(t.key);
        std::string k = regkey_content_key(canon);
        auto it = idx.find(k);
        if (it == idx.end()) {
            idx[k] = kept.size();
            kept.push_back(RegTermSym{t.coef, std::move(canon)});
        } else {
            kept[it->second].coef = kept[it->second].coef + t.coef;
        }
    }
    RegulatorSym out;
    out.reserve(kept.size());
    for (auto& t : kept) {
        if (!t.coef.is_zero()) out.push_back(std::move(t));
    }
    return out;
}

RegulatorSym shuffle_symbolic_sym(const RegulatorSym& a,
                                    const RegulatorSym& b) {
    RegulatorSym out;
    out.reserve(a.size() * b.size());
    for (const auto& ti : a) {
        for (const auto& tj : b) {
            RegKey joined;
            joined.reserve(ti.key.size() + tj.key.size());
            for (const auto& w : ti.key) joined.push_back(w);
            for (const auto& w : tj.key) joined.push_back(w);
            out.push_back(RegTermSym{ti.coef.mul(tj.coef),
                                      canonicalize_regkey(joined)});
        }
    }
    return collect_regulator_sym(out);
}

// ----- collect_regulator_split / shuffle_symbolic_split / -----
// ----- canonicalize_regulator_split / scalar_mul_regulator_split -----
//
// Phase-B B1.b: SymCoefSplit-valued siblings of the `_sym` helpers
// above. Dead code at B1.b (no live call site dispatches here yet);
// B1.c flips `transform_word_impl` and `transform_shuffle` to call
// these under `runtime::scalar_rep_enabled()`. Mirrors the `_sym`
// shape line-for-line, with `SymCoef::operator+` -> `SymCoefSplit::add`,
// `SymCoef::mul` -> `SymCoefSplit::mul`, and `SymCoef::mul_rat` ->
// `SymCoefSplit::mul_rat`. Same RegKey content_key ordering.

RegulatorSplit collect_regulator_split(const RegulatorSplit& r) {
    std::unordered_map<std::string, size_t> idx;
    std::vector<RegTermSplit> kept;
    for (const auto& t : r) {
        RegKey canon = canonicalize_regkey(t.key);
        std::string k = regkey_content_key(canon);
        auto it = idx.find(k);
        if (it == idx.end()) {
            idx[k] = kept.size();
            kept.push_back(RegTermSplit{t.coef, std::move(canon)});
        } else {
            kept[it->second].coef = kept[it->second].coef.add(t.coef);
        }
    }
    RegulatorSplit out;
    out.reserve(kept.size());
    for (auto& t : kept) {
        if (!t.coef.is_zero()) out.push_back(std::move(t));
    }
    return out;
}

RegulatorSplit shuffle_symbolic_split(const RegulatorSplit& a,
                                      const RegulatorSplit& b) {
    RegulatorSplit out;
    out.reserve(a.size() * b.size());
    for (const auto& ti : a) {
        for (const auto& tj : b) {
            RegKey joined;
            joined.reserve(ti.key.size() + tj.key.size());
            for (const auto& w : ti.key) joined.push_back(w);
            for (const auto& w : tj.key) joined.push_back(w);
            out.push_back(RegTermSplit{ti.coef.mul(tj.coef),
                                       canonicalize_regkey(joined)});
        }
    }
    return collect_regulator_split(out);
}

RegulatorSplit canonicalize_regulator_split(const RegulatorSplit& r) {
    RegulatorSplit out = collect_regulator_split(r);
    std::stable_sort(out.begin(), out.end(),
        [](const RegTermSplit& a, const RegTermSplit& b) {
            return regkey_content_key(a.key) < regkey_content_key(b.key);
        });
    return out;
}

RegulatorSplit scalar_mul_regulator_split(const RegulatorSplit& r,
                                          const Rat& s) {
    RegulatorSplit out;
    out.reserve(r.size());
    for (const auto& t : r) {
        out.push_back(RegTermSplit{t.coef.mul_rat(s), t.key});
    }
    return out;
}

// ---- Phase-B B1.c / B2: SymCoef <-> SymCoefSplit round-trip at function exit --
//
// `transform_word_impl` and `transform_shuffle` dispatch on
// `runtime::scalar_rep_enabled()` (HF_USE_SCALAR_REP env-gate, B1.b).
// Under v1, every produced `RegulatorSym` is round-tripped through
// `SymCoefSplit::from_rat` -> `as_rat` at the function-exit boundary
// via the shared helper `runtime::roundtrip_regulator_through_scs`
// (originally B1.c, promoted to a runtime/ utility at B2 so
// `break_up_contour_sym` and the rest of the Phase-B fan-out share
// one set of counters and one at-exit line).
//
// Verifier site (design v2 §3.5a "post-`as_rat` boundary"). When
// HF_RAT_SPLIT_VERIFY=1, every per-term as_rat() output is fed back
// into `SymCoefSplit::from_rat` and `equals_canonical` (B1.a) checks
// that the round-trip preserved canonical form. On mismatch the helper
// emits a JSON FAIL line on stderr and aborts; bit-identity drift is
// surfaced by the gate runner regardless of this site, but the
// canonical-form check pinpoints the offending term in stderr for
// faster RCA.

// ---------------------------------------------------------- reglim_word --

namespace {

bool word_depends_on_var(const Word& word, size_t var_idx) {
    for (const auto& letter : word.letters) {
        // A letter is "var-dependent" if its derivative w.r.t. var_idx
        // is nonzero. Canonical-Rat form means no hidden cancellation.
        if (!letter.derivative(var_idx).is_zero()) return true;
    }
    return false;
}

// Return true if every letter in `word` canonicalizes to the same
// rational constant `rhs_str`. Used for the "all 0" and "all -1" checks.
bool all_letters_equal_str(const Word& word, const std::string& rhs_str) {
    if (word.empty()) return false;
    for (const auto& letter : word.letters) {
        if (letter.to_string() != rhs_str) return false;
    }
    return true;
}

// True if every letter in `word` is one of "0", "-1", "-2".  Mirrors
// HyperIntica's  SubsetQ[{-2, -1, 0}, Union[word]]  check, which routes
// such words through BreakUpContour or ZeroInfPeriod symbolically
// (Phase 6).  We use this to *defer* the Phase-5c stub so those cases
// don't produce mismatching outputs against the reference backends.
bool all_letters_in_minus2_minus1_zero(const Word& word) {
    if (word.empty()) return false;
    for (const auto& letter : word.letters) {
        const std::string s = letter.to_string();
        if (s != "0" && s != "-1" && s != "-2") return false;
    }
    return true;
}

}  // namespace

RegulatorSym reglim_word(const PolyCtx& ctx,
                          const Word& word,
                          size_t var_idx,
                          const MzvReductionTable& table,
                          std::shared_ptr<ZWTable> zw_tab) {
    const Rat rat_one{Poly::one_of(ctx)};

    // Phase-B B3: SymCoef <-> SymCoefSplit round-trip applied at each
    // function-exit point under runtime::scalar_rep_enabled() (the
    // HF_USE_SCALAR_REP env-gate). Recursive reglim_word calls already
    // round-trip their own outputs; applying again at the outer layer is
    // canonically a no-op at B1 (W-side empty hypothesis,
    // b1_scoping_memo.md R2 + design v2 §4.4a Note 2). Smirnov fixtures
    // never exercise the positive-letter branch (linear-in-letters), so
    // the bit-identity gate at B3 stays in the round-trip-trivial
    // regime. Mirrors break_up_contour_sym's B2 dispatch pattern.
    //
    // C0c.1 iter-66 (path 1a sub-iter 3 — ABI cascade): lambda kill.
    // The persistent ZWTable is threaded by the caller through
    // reglim_word's `zw_tab` parameter (signature widened in iter-66
    // per iter-63 audit MEMO §5.3); the `[&]` reference capture brings
    // it into scope. The threaded table reaches reglim_word along one
    // of three caller paths:
    //   (i)  CLI direct entry — `handle_reglim_word`
    //        (bridge/cli/main.cpp:3097) allocates a fresh transient
    //        `_lf_zw` per iter-52 C0c.1 Increment β pattern.
    //   (ii) integration-pipeline transitive thread — hyperflint_sym
    //        driver allocates once at hyper_int.cpp:463-466, propagates
    //        through integration_step.cpp:1614 `zw_for_this_thread`
    //        (per-thread per iter-58 Option A), into transform_shuffle
    //        / transform_word / transform_word_impl, and reaches
    //        reglim_word at transform.cpp:768 (transform_word_impl
    //        pre-population step).
    //   (iii) recursive reglim_word — the body's own free-var-period
    //        and trailing-zero branches call back at transform.cpp:437
    //        and transform.cpp:452 with the same threaded `zw_tab`,
    //        preserving handle reuse across recursion depth.
    // The prior local `auto zw_tab = make_shared<ZWTable>(ctx);`
    // shadowed the outer parameter and discarded handle reuse across
    // recursive reglim_word calls. Same pattern as iter-64 site 2 at
    // transform.cpp:917, iter-65 site 3 at transform.cpp:1013, and
    // iter-65 site 5 at primitive.cpp:296.
    //
    // Iter-66 advisory-2 fold (carry-forward from iter-65 reviewer
    // agentId a55d7855e2140cfd6 advisory-2): defense-in-depth
    // HF_SCALAR_REP_REQUIRE_PERSISTENT abort guard mirrors site 4
    // pattern at break_up_contour.cpp:305 and sites 2/3/5/7 at
    // transform.cpp:929/1003 / primitive.cpp:298 /
    // integration_step.cpp:1003. Site-1's separate
    // `bcs_zw_tab_local` allocation at transform.cpp:601-616
    // (positive-letter branch into break_up_contour_sym) is NOT
    // touched by iter-66 — that allocation is for the bcs callee,
    // whereas this lambda is the function-exit round-trip; folding
    // bcs onto the threaded `zw_tab` is deferred to iter-67+ per
    // iter-63 audit MEMO §5.3 task list.
    auto apply_v1_roundtrip = [&](RegulatorSym&& r,
                                    const char* tag) -> RegulatorSym {
        if (!runtime::scalar_rep_enabled()) return std::move(r);
        if (runtime::require_persistent_enabled() && !zw_tab) {
            std::cerr << "[HF_SCALAR_REP_REQUIRE_PERSISTENT=1]"
                << " reglim_word apply_v1_roundtrip:"
                << " zw_tab is null."
                << " Caller did not thread the persistent ZWTable"
                << " from hyperflint_sym (hyper_int.cpp:463-466),"
                << " CLI (bridge/cli/main.cpp:3097), or"
                << " transform_word_impl (transform.cpp:768)."
                << " Migrate per design v2 sec 3.6a."
                << std::endl;
            std::abort();
        }
        return runtime::roundtrip_regulator_through_scs(r, ctx, zw_tab, tag);
    };

    // Helper: evaluate `scaled` (or `word`) as a zero-infinity period
    // via break_up_contour with empty onAxis. Mirrors HyperIntica.wl:
    //   BreakUpContour[{{1, word}}, {}]  //. mzvAllReductions
    // which routes to ZeroInfPeriodEval for words with letters in
    // {-2, -1, 0}, yielding mzv[...] constants that the `table`
    // reduces to the basis (mzv_2 -> Pi^2/6 -> 6*mzv_2 stays as
    // mzv_2 in HF's PolyCtx form). The returned Rat is a polynomial
    // in the PolyCtx's mzv_* variables.
    auto evaluate_period = [&](const Word& w) -> RegulatorSym {
        // Period-tuples Phase 2: this is a production mint site (the
        // Rat-valued break_up_contour folds the period into a Rat over
        // atom vars, impossible under the slim ctx). Under the flag the
        // {-2,-1,0}-letter word mints directly via the scratch ring
        // into period_powers; the partition logic below only ever sees
        // such words here (callers gate on all_letters_in_minus2_...).
        if (period_tuples_enabled()) {
            RegulatorSym out;
            SymCoef minted =
                mint_period_sym(ctx, w, table, /*zero_one=*/false);
            if (!minted.is_zero())
                out.push_back(RegTermSym{std::move(minted), RegKey{}});
            return out;
        }
        Wordlist seed;
        seed.terms.push_back(WordlistTerm{rat_one, w});
        Regulator buc = break_up_contour(ctx, seed, {}, table);
        RegulatorSym out;
        for (auto& t : buc) {
            if (t.coef.is_zero()) continue;
            out.push_back(RegTermSym{SymCoef::from_rat(t.coef),
                                       std::move(t.key)});
        }
        return out;
    };

    // Empty word: symbolic period is 1 (represented as the empty RegKey
    // with coef 1). Matches HyperIntica:5237 and HyperInt.mpl:323.
    if (word.empty()) {
        RegulatorSym out;
        out.push_back(RegTermSym{SymCoef::from_rat(rat_one), RegKey{}});
        return apply_v1_roundtrip(std::move(out), "reglim_word/empty_word");
    }

    if (!word_depends_on_var(word, var_idx)) {
        // FreeQ[word, var] — period branch.
        if (all_letters_equal_str(word, "0") || all_letters_equal_str(word, "-1")) {
            // Period of an all-0 or all-(-1) word vanishes.
            return apply_v1_roundtrip(RegulatorSym{},
                                        "reglim_word/free_var_zeros");
        }
        if (all_letters_in_minus2_minus1_zero(word)) {
            // Bug #6 Fragment P1: evaluate via break_up_contour (empty
            // onAxis). Mirrors HyperIntica.wl:5483-5493 which routes
            // through BreakUpContour[{{1, word}}, {}] and reduces mzv
            // constants via mzvAllReductions.
            return apply_v1_roundtrip(evaluate_period(word),
                                        "reglim_word/free_var_period");
        }
        // Generic: symbolic "period" is the word itself.
        RegulatorSym out;
        out.push_back(RegTermSym{SymCoef::from_rat(rat_one), RegKey{word}});
        return apply_v1_roundtrip(std::move(out),
                                    "reglim_word/free_var_symbolic");
    }

    // Word depends on var. Port HyperIntica.wl:5241-5486.
    //
    // Mma's `PoleDegree[f, var]` is the leading Laurent order at
    // var=0 (positive for zeros, negative for poles —
    // `HYleadingLaurentOrder`). HF's `Rat::pole_degree` uses the
    // *same* convention (nmin - dmin). Mma's PoleDegree[0, var]
    // is Infinity (HyperIntica.wl:2859); HF's pole_degree on the
    // zero Rat returns LONG_MAX, which we preserve — it's
    // guaranteed to exceed every finite zeroOrder, matching Mma's
    // Infinity semantics under `zeroOrders[i] > minOrder` checks.
    // An earlier version of this code clamped LONG_MAX → 0; that
    // silently shifted `minOrder` and made the trailing-letter
    // branch fire when Mma's main-logic would have fired (bug #6
    // final sign flip).
    const long n = static_cast<long>(word.size());
    std::vector<long> zeroOrders;
    zeroOrders.reserve(n);
    for (const auto& letter : word.letters) {
        long hf_pd = letter.pole_degree(var_idx);
        zeroOrders.push_back(hf_pd);   // LONG_MAX stands for +Infinity
    }
    long minOrder = zeroOrders[0];
    for (long v : zeroOrders) if (v < minOrder) minOrder = v;

    // Trailing letters with zeroOrder > minOrder — port of
    // HyperIntica.wl:5287-5311. When any trailing block has larger
    // zeroOrder than the minimum, the word decomposes as
    //   ReglimWord[word, var] = sum_{ii=0..n}
    //     shuffle_symbolic(
    //         {sum_sc  sc.coef * ReglimWord[sc.word, var]
    //                 : sc in RegzeroWord[ word[0..w-n-1] ++ [0]*ii ] },
    //         ReglimWord[ word[w-n+ii..w-1], var ])
    // with `n` = count of trailing letters whose zeroOrder > minOrder.
    long trailing = 0;
    while (trailing < n && zeroOrders[n - 1 - trailing] > minOrder) ++trailing;
    if (trailing > 0) {
        const long head_len = n - trailing;    // = w - n in Mma
        Word head_base;
        head_base.letters.assign(word.letters.begin(),
                                  word.letters.begin() + head_len);

        RegulatorSym result;
        for (long ii = 0; ii <= trailing; ++ii) {
            // head_ii = head_base ++ [0]*ii
            Word head_ii = head_base;
            for (long k = 0; k < ii; ++k) {
                head_ii.letters.push_back(Rat::zero_of(ctx));
            }
            // RegzeroWord[head_ii] — shuffle-regularize trailing zeros.
            Wordlist regzero_head = regzero_word_in_ctx(ctx, head_ii);

            // For each (sc.coef, sc.word) in regzero_head:
            //   temp += sc.coef * ReglimWord[sc.word, var]
            RegulatorSym temp_reg;
            for (const auto& sc : regzero_head.terms) {
                RegulatorSym sub = reglim_word(ctx, sc.word, var_idx, table, zw_tab);
                for (const auto& ts : sub) {
                    temp_reg.push_back(RegTermSym{ts.coef.mul_rat(sc.coef),
                                                   ts.key});
                }
            }
            temp_reg = canonicalize_regulator_sym(temp_reg);

            // Tail: word[head_len + ii .. n-1].
            Word tail;
            if (head_len + ii < n) {
                tail.letters.assign(word.letters.begin() + head_len + ii,
                                    word.letters.end());
            }
            RegulatorSym tail_reg =
                reglim_word(ctx, tail, var_idx, table, zw_tab);

            // Combine by shuffling regulator keys and multiplying
            // SymCoef coefs pairwise.
            RegulatorSym combined = shuffle_symbolic_sym(temp_reg, tail_reg);
            for (auto& c : combined) {
                result.push_back(std::move(c));
            }
        }
        return apply_v1_roundtrip(canonicalize_regulator_sym(result),
                                    "reglim_word/trailing_zero");
    }

    // Compute scaled word: letter[i] -> 0 if zeroOrder[i] > minOrder,
    // else RatResidue(letter[i], var) if letter depends on var,
    // else letter[i] as-is.
    Word scaled;
    scaled.letters.reserve(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) {
        const Rat& letter = word[static_cast<size_t>(i)];
        if (zeroOrders[i] > minOrder) {
            scaled.letters.push_back(Rat::zero_of(ctx));
        } else if (letter.derivative(var_idx).is_zero()) {
            scaled.letters.push_back(letter);
        } else {
            scaled.letters.push_back(letter.rat_residue(var_idx));
        }
    }

    if (all_letters_equal_str(scaled, "0") || all_letters_equal_str(scaled, "-1")) {
        // All-0 or all-(-1) scaled word: period vanishes.
        return apply_v1_roundtrip(RegulatorSym{},
                                    "reglim_word/scaled_zeros");
    }
    if (all_letters_in_minus2_minus1_zero(scaled)) {
        // Bug #6 Fragment P1: evaluate the var-dep scaled word as a
        // zero-infinity period. Mirrors HyperIntica.wl:5483-5493.
        return apply_v1_roundtrip(evaluate_period(scaled),
                                    "reglim_word/scaled_period");
    }

    // Bug #6 Fragment P2: positive-letter contour deformation.
    // Mirrors HyperIntica.wl:5322-5468. Scan `scaled` for literal
    // positive-integer letters; if any are found, build `on_axis` with
    // delta[var] factors carrying signs determined by the next-to-
    // leading term of (original-letter - positive-letter) at
    // vars=1, then call break_up_contour_sym.
    auto is_pos_int_literal = [](const std::string& s) {
        if (s.empty() || s[0] == '-' || s == "0") return false;
        for (char c : s) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    };

    std::vector<Rat> pos_letters;
    std::vector<long> pos_vals;
    std::vector<size_t> pos_first_idx;
    for (long i = 0; i < n; ++i) {
        const std::string s = scaled[static_cast<size_t>(i)].to_string();
        if (!is_pos_int_literal(s)) continue;
        long v = std::stol(s);
        bool seen = false;
        for (long u : pos_vals) if (u == v) { seen = true; break; }
        if (!seen) {
            pos_vals.push_back(v);
            pos_letters.push_back(scaled[static_cast<size_t>(i)]);
            pos_first_idx.push_back(static_cast<size_t>(i));
        }
    }

    if (!pos_letters.empty()) {
        // Sort by letter value, ascending (BreakUpContour uses
        // on_axis[0].letter as `smallest` for the recursion).
        std::vector<size_t> perm(pos_letters.size());
        for (size_t k = 0; k < perm.size(); ++k) perm[k] = k;
        std::sort(perm.begin(), perm.end(),
            [&](size_t a, size_t b) { return pos_vals[a] < pos_vals[b]; });

        // Build vars-at-1 valuation for sign extraction. We only use
        // this to take Sign[Re[nextCoef]] per HyperIntica.wl's Case 4.
        const size_t nvars = ctx.vars().size();
        std::vector<std::string> ones(nvars, "1");

        auto rat_sign_at_ones = [&](const Rat& r) -> int {
            try {
                std::string s = r.evaluate_all(ones);
                if (s.empty()) return 0;
                if (s == "0") return 0;
                if (s[0] == '-') return -1;
                return 1;
            } catch (const std::exception&) {
                return 0;
            }
        };

        const std::string& var_name = ctx.vars()[var_idx];
        SymCoef delta_v = SymCoef::delta_factor(ctx, var_name);

        std::vector<OnAxisSymEntry> on_axis;
        for (size_t k : perm) {
            const Rat& letter_rat = pos_letters[k];
            SymCoef origin(ctx);
            if (minOrder > 0) {
                origin = delta_v;
            } else if (minOrder < 0) {
                origin = SymCoef::from_rat(-rat_one).mul(delta_v);
            } else {
                // minOrder == 0: compute Sign[Re[nextCoef]] via eval
                // at vars=1, per HyperIntica "Case 4" (the common case
                // when `nextCoef` is a rational function of real
                // Schwinger parameters).
                //
                //   nextCoef = RatResidue[word[idx] * var^(-minOrder)
                //                          - letter, var]
                // With minOrder == 0, var^0 = 1, so
                //   nextCoef = RatResidue[word[idx] - letter, var].
                const Word& orig_word = word;
                Rat diff = orig_word[pos_first_idx[k]] - letter_rat;
                Rat next_coef = diff.rat_residue(var_idx);
                int s = rat_sign_at_ones(next_coef);
                if (s > 0) {
                    origin = delta_v;
                } else if (s < 0) {
                    origin = SymCoef::from_rat(-rat_one).mul(delta_v);
                } else {
                    // Re is zero or non-numeric at vars=1 — fall back
                    // to +delta[var]. Matches HyperIntica's final
                    // default in Case 4.
                    origin = delta_v;
                }
            }
            on_axis.push_back(OnAxisSymEntry{letter_rat, origin});
        }

        WordlistSym seed;
        seed.terms.push_back(WordlistSymTerm{SymCoef::from_rat(rat_one),
                                               scaled});
        // C0b.4 (iter-42): break_up_contour_sym now takes a mandatory
        // `std::shared_ptr<ZWTable>` parameter. `reglim_word` (site 1)
        // does not yet receive a `zw_tab` from its caller (deferred to
        // iter-43+ when the rest of the integration-step API thread-
        // through lands). For now, allocate a per-call ZWTable at this
        // callsite when the env-gate is on; under default-OFF the gate
        // short-circuits inside the lambda before any `zw_tab` use, so
        // a null shared_ptr is safe. Pre-iter-42 break_up_contour_sym's
        // body lambda already allocated a per-call table here, so this
        // preserves byte-identity. Replace this allocation with the
        // reglim_word-supplied table at iter-43 (C0b.1).
        std::shared_ptr<ZWTable> bcs_zw_tab_local;
        if (runtime::scalar_rep_enabled()) {
            // Iter-44 (2026-05-09): HF_SCALAR_REP_REQUIRE_PERSISTENT=1
            // assertion (Concern-2 mitigation). See scalar_rep.hpp.
            if (runtime::require_persistent_enabled()) {
                std::cerr << "[HF_SCALAR_REP_REQUIRE_PERSISTENT=1]"
                    << " transform.cpp:reglim_word/positive_letter"
                    << " (around line 600): allocating per-call"
                    << " ZWTable. Migrate to a caller-supplied"
                    << " persistent table threaded from"
                    << " hyperflint_sym per design v2 sec 3.6a."
                    << std::endl;
                std::abort();
            }
            bcs_zw_tab_local = std::make_shared<ZWTable>(ctx);
        }
        return apply_v1_roundtrip(
            break_up_contour_sym(ctx, seed, on_axis, table,
                                   bcs_zw_tab_local),
            "reglim_word/positive_letter");
    }

    RegulatorSym out;
    out.push_back(RegTermSym{SymCoef::from_rat(rat_one), RegKey{scaled}});
    return apply_v1_roundtrip(std::move(out),
                                "reglim_word/scaled_symbolic");
}

// ---------------------------------------------- combine_shuffle_keys --

RegKey combine_shuffle_keys(const RegKey& a, const RegKey& b) {
    RegKey out;
    out.reserve(a.size() + b.size());
    for (const auto& w : a) out.push_back(w);
    for (const auto& w : b) out.push_back(w);
    return canonicalize_regkey(out);
}

// --------------------------------------------- canonicalize_regulator --

Regulator canonicalize_regulator(const Regulator& r) {
    Regulator collected = collect_regulator(r);
    std::stable_sort(collected.begin(), collected.end(),
        [](const RegTerm& a, const RegTerm& b) {
            return regkey_content_key(a.key) < regkey_content_key(b.key);
        });
    return collected;
}

std::string regulator_content_key(const Regulator& r) {
    Regulator canon = canonicalize_regulator(r);
    std::ostringstream o;
    for (const auto& t : canon) {
        // \x03 separates coef from key, \x04 separates terms.
        o << t.coef.to_string() << '\x03'
          << regkey_content_key(t.key) << '\x04';
    }
    return o.str();
}

std::string regulator_sym_content_key(const RegulatorSym& r) {
    RegulatorSym canon = canonicalize_regulator_sym(r);
    std::ostringstream o;
    for (const auto& t : canon) {
        o << t.coef.to_string() << '\x03'
          << regkey_content_key(t.key) << '\x04';
    }
    return o.str();
}

// --------------------------------------------------- transform_word --

namespace {

using TransformCache = std::unordered_map<std::string, TransformResultSym>;

std::string transform_cache_key(const Word& word, size_t var_idx) {
    std::ostringstream o;
    o << var_idx << '|' << word.content_key();
    return o.str();
}

// Sub-sub-helper: accumulate (coef, new_word) into the resultTable row
// associated with `regulator_key`. Creates the row if absent. Bug #6
// lift: the regulator is RegulatorSym (SymCoef-valued) but the inner
// shuffle coefficients stay Rat-valued — shuffle expansion never
// introduces transcendental residues.
struct ResultRow {
    RegulatorSym regulator;
    std::vector<std::string> word_order;
    std::unordered_map<std::string, size_t> word_index;
    std::vector<WordlistTerm> terms;
};

void bump_result(
    std::vector<ResultRow>& rows,
    std::unordered_map<std::string, size_t>& row_index,
    const RegulatorSym& regulator_key,
    const Word& new_word,
    const Rat& coef)
{
    std::string rkey = regulator_sym_content_key(regulator_key);
    auto rit = row_index.find(rkey);
    size_t ri;
    if (rit == row_index.end()) {
        ri = rows.size();
        row_index[rkey] = ri;
        rows.push_back(ResultRow{canonicalize_regulator_sym(regulator_key),
                                 {}, {}, {}});
    } else {
        ri = rit->second;
    }
    ResultRow& row = rows[ri];
    std::string wkey = new_word.content_key();
    auto wit = row.word_index.find(wkey);
    if (wit == row.word_index.end()) {
        row.word_index[wkey] = row.terms.size();
        row.word_order.push_back(wkey);
        row.terms.push_back(WordlistTerm{coef, new_word});
    } else {
        row.terms[wit->second].coef = row.terms[wit->second].coef + coef;
    }
}

TransformResultSym transform_word_impl(const PolyCtx& ctx,
                                        const Word& word,
                                        size_t var_idx,
                                        const MzvReductionTable& table,
                                        TransformCache& cache,
                                        std::shared_ptr<ZWTable> zw_tab,
                                        bool introduce_algebraic_letters);

// Predicate: word[-1] is literal zero.
bool trailing_zero(const Word& w) {
    return !w.empty() && w[w.size() - 1].is_zero();
}

// Core of Phase 5c-ii, factored for readability.
TransformResultSym transform_word_impl(const PolyCtx& ctx,
                                        const Word& word,
                                        size_t var_idx,
                                        const MzvReductionTable& table,
                                        TransformCache& cache,
                                        std::shared_ptr<ZWTable> zw_tab,
                                        bool introduce_algebraic_letters) {
    std::string ck = transform_cache_key(word, var_idx);
    auto it = cache.find(ck);
    if (it != cache.end()) return it->second;

    const Rat rat_one{Poly::one_of(ctx)};

    // Base case: empty word.
    if (word.empty()) {
        TransformResultSym out;
        TransformPairSym p;
        p.shuffle.terms.push_back(
            WordlistTerm{rat_one, Word{}});
        p.regulator.push_back(
            RegTermSym{SymCoef::from_rat(rat_one), RegKey{}});
        out.push_back(std::move(p));
        cache[ck] = out;
        return out;
    }

    // ReglimWord-driven pre-population (Phase 5c stub skips the
    // var-dependent trailing-zero branch, so this effectively only
    // matters for FreeQ[word, var] below).
    //
    // C0c.1 iter-66 (path 1a sub-iter 3 — ABI cascade): reglim_word's
    // signature widened to take a `std::shared_ptr<ZWTable> zw_tab`
    // (per iter-63 audit MEMO §5.3); thread `transform_word_impl`'s
    // outer `zw_tab` parameter through.
    RegulatorSym sub = reglim_word(ctx, word, var_idx, table, zw_tab);

    bool word_has_var = false;
    for (const auto& l : word.letters) {
        if (!l.derivative(var_idx).is_zero()) { word_has_var = true; break; }
    }

    std::vector<ResultRow> rows;
    std::unordered_map<std::string, size_t> row_index;

    if (!sub.empty() && word_has_var) {
        // Pre-populate: resultTable[sub][{}] = 1.
        bump_result(rows, row_index, sub, Word{}, rat_one);
    }

    if (!word_has_var) {
        TransformResultSym out;
        if (!sub.empty()) {
            TransformPairSym p;
            p.shuffle.terms.push_back(
                WordlistTerm{rat_one, Word{}});
            p.regulator = canonicalize_regulator_sym(sub);
            out.push_back(std::move(p));
        }
        cache[ck] = out;
        return out;
    }

    // A trailing zero with a var-dependent word is $Failed.
    if (trailing_zero(word)) {
        throw TransformFailed{};
    }

    const size_t n = word.size();

    // Main loop.  Iterate over positions i = 0..n-1 (Mma's 1..Length[word]).
    for (size_t i = 0; i < n; ++i) {
        // Mma guard: if i === Length[word] && i > 1 && word[i-1] === 0 then break.
        // 1-indexed -> 0-indexed: i == n-1 && n >= 2 && word[n-2] is zero.
        if (i == n - 1 && n >= 2 && word[n - 2].is_zero()) break;

        // p = word[i] - word[i+1]  (if i < n-1)  or word[i].
        Rat p = (i < n - 1) ? (word[i] - word[i + 1]) : Rat{word[i]};
        if (p.is_zero()) continue;

        // LinearFactors[p, var] on the full Rat p = num/den: a
        // denominator-factor (lc·var + c0)^k contributes a pole at
        // -c0/lc with multiplicity -k, matching Mma's FactorList on
        // rationals (which reports denominator factors with negative
        // exponents; HyperIntica.wl:2894+). Letters produced by
        // earlier integration steps routinely carry the current
        // integration variable in their denominator (e.g. a letter
        // `c / (t5 + 1)` after step 1 becomes a pole at t5=-1 when
        // step 2 integrates t5). Factoring only `p.num()` silently
        // drops those contributions.
        // Iter-52 C0c.1 Increment β: zw_tab is the threaded-through param
        // (replaces the iter-52a Increment α caller-side transient).
        // Production calls thread the persistent driver-entry ZWTable
        // from hyper_int.cpp:~463 through transform_shuffle ->
        // transform_word -> transform_word_impl. CLI/test entry points
        // construct fresh transients per public-API call.
        LinearFactorization lf_num = linear_factors(p.num(), var_idx,
                                                     zw_tab,
                                                     introduce_algebraic_letters);
        LinearFactorization lf_den = linear_factors(p.den(), var_idx,
                                                     zw_tab,
                                                     introduce_algebraic_letters);
        std::vector<LinearFactor> facts = lf_num.linear;
        for (auto& f : lf_den.linear) {
            facts.push_back(LinearFactor{-f.multiplicity, std::move(f.pole)});
        }
        if (facts.empty()) continue;

        // First recursive call: drop position i+1 (i < n-1 only).
        if (i < n - 1) {
            Word sub_word;
            sub_word.letters.reserve(n - 1);
            for (size_t k = 0; k <= i; ++k) sub_word.letters.push_back(word[k]);
            for (size_t k = i + 2; k < n; ++k) sub_word.letters.push_back(word[k]);
            if (!sub_word.empty() && !trailing_zero(sub_word)) {
                TransformResultSym subResult =
                    transform_word_impl(ctx, sub_word, var_idx, table, cache,
                                        zw_tab,
                                        introduce_algebraic_letters);
                for (const auto& s : subResult) {
                    for (const auto& term : s.shuffle.terms) {
                        for (const auto& fact : facts) {
                            Word new_word;
                            new_word.letters.reserve(term.word.size() + 1);
                            new_word.letters.push_back(fact.pole);
                            for (const auto& l : term.word.letters)
                                new_word.letters.push_back(l);
                            Rat mult_rat = Rat::from_int(ctx, fact.multiplicity);
                            Rat coef = mult_rat * term.coef;
                            bump_result(rows, row_index, s.regulator,
                                        new_word, coef);
                        }
                    }
                }
            }
        }

        // Second recursive call: drop position i.
        {
            Word sub_word;
            sub_word.letters.reserve(n - 1);
            for (size_t k = 0; k < i; ++k)     sub_word.letters.push_back(word[k]);
            for (size_t k = i + 1; k < n; ++k) sub_word.letters.push_back(word[k]);
            if (sub_word.empty() || !trailing_zero(sub_word)) {
                TransformResultSym subResult =
                    transform_word_impl(ctx, sub_word, var_idx, table, cache,
                                        zw_tab,
                                        introduce_algebraic_letters);
                for (const auto& s : subResult) {
                    for (const auto& term : s.shuffle.terms) {
                        for (const auto& fact : facts) {
                            Word new_word;
                            new_word.letters.reserve(term.word.size() + 1);
                            new_word.letters.push_back(fact.pole);
                            for (const auto& l : term.word.letters)
                                new_word.letters.push_back(l);
                            Rat mult_rat = Rat::from_int(ctx, -fact.multiplicity);
                            Rat coef = mult_rat * term.coef;
                            bump_result(rows, row_index, s.regulator,
                                        new_word, coef);
                        }
                    }
                }
            }
        }
    }

    // Build finalResult from rows.
    TransformResultSym out;
    for (auto& row : rows) {
        Wordlist wl;
        for (auto& t : row.terms) {
            if (!t.coef.is_zero()) wl.terms.push_back(std::move(t));
        }
        if (!wl.terms.empty()) {
            out.push_back(TransformPairSym{std::move(wl),
                                             std::move(row.regulator)});
        }
    }
    // Phase B B1.c: v1 SymCoef <-> SymCoefSplit round-trip at function
    // exit. Cache stores the post-as_rat output so cache hits also see
    // the v1 representation. At B1 the W-side variable set is empty (N
    // = F = ctx); B2+ widens N as Wm/Wp letters lift to the W-side.
    // C0c.1 iter-64: lambda kill. The persistent ZWTable is threaded by
    // the caller through the `zw_tab` parameter (allocated once per
    // hyperflint_sym driver entry in hyper_int.cpp:463-466, once per CLI
    // invocation in bridge/cli/main.cpp:1752, or per-thread by
    // integration_step.cpp's outer parallel-for per iter-58 Option A);
    // the prior local `auto zw_tab = make_shared<ZWTable>(ctx);` shadowed
    // the outer parameter and discarded handle reuse across nested calls.
    //
    // Iter-65 advisory-2 fold (from iter-64 reviewer agentId
    // a8d6453b4307f6d1f): defense-in-depth
    // HF_SCALAR_REP_REQUIRE_PERSISTENT abort guard mirrors site 4
    // pattern at break_up_contour.cpp:305 and sites 1/7 at
    // transform.cpp:605 / integration_step.cpp:1003. Site 2 is open
    // code (not in a lambda), but the null-check semantics match
    // sites 3/5 (also iter-65 lambda-kill targets).
    if (runtime::scalar_rep_enabled()) {
        if (runtime::require_persistent_enabled() && !zw_tab) {
            std::cerr << "[HF_SCALAR_REP_REQUIRE_PERSISTENT=1]"
                << " transform_word_impl exit (around line 922):"
                << " zw_tab is null."
                << " Caller did not thread the persistent ZWTable"
                << " from hyperflint_sym (hyper_int.cpp:463-466)"
                << " or CLI (bridge/cli/main.cpp:1752)."
                << " Migrate per design v2 sec 3.6a."
                << std::endl;
            std::abort();
        }
        for (auto& pair : out) {
            pair.regulator = runtime::roundtrip_regulator_through_scs(
                pair.regulator, ctx, zw_tab,
                "transform_word_impl");
        }
    }
    cache[ck] = out;
    return out;
}

}  // namespace

TransformResultSym transform_word(const PolyCtx& ctx,
                                   const Word& word,
                                   size_t var_idx,
                                   const MzvReductionTable& table,
                                   std::shared_ptr<ZWTable> zw_tab,
                                   bool introduce_algebraic_letters) {
    TransformCache cache;
    return transform_word_impl(ctx, word, var_idx, table, cache,
                                zw_tab,
                                introduce_algebraic_letters);
}

// ------------------------------------------------- transform_shuffle --

namespace {

// True iff `word` is non-empty and every letter equals word[0]
// (a "logarithm power" Log^n / n!).
bool is_log_power(const Word& word) {
    if (word.empty()) return false;
    std::string first = word[0].to_string();
    for (size_t i = 1; i < word.size(); ++i) {
        if (word[i].to_string() != first) return false;
    }
    return true;
}

Rat factorial_rat(const PolyCtx& ctx, size_t n) {
    long acc = 1;
    for (size_t i = 2; i <= n; ++i) acc *= static_cast<long>(i);
    return Rat::from_int(ctx, acc);
}

// Scale every term in a RegulatorSym by a Rat scalar. Returns a fresh
// RegulatorSym (input unchanged).
RegulatorSym scalar_mul_regulator_sym(const RegulatorSym& r, const Rat& s) {
    RegulatorSym out;
    out.reserve(r.size());
    for (const auto& t : r) {
        out.push_back(RegTermSym{t.coef.mul_rat(s), t.key});
    }
    return out;
}

}  // namespace

// iter-62-β.2: forward-decl the global cache singleton (definition in
// operator_memo.cpp). operator_memo.hpp does not expose this accessor
// because TransformResultSym is `std::vector<TransformPairSym>` (an alias
// to a vector of an integrator-layer struct), which is awkward to
// forward-declare in the public header without including the full
// integrator/transform.hpp from operator_memo.hpp's clients. The forward
// decl below stays local to this TU; rebuilt callers in the test
// scaffold use the same pattern.
OperatorMemo<canonical_signature::TransformShuffleKey,
             TransformResultSym>&
g_transform_shuffle_cache();

// iter-62-β.2: renamed pre-iter-62 body to `transform_shuffle_impl`,
// made static (file-local). The public `transform_shuffle` (defined
// just past this body) is the §E operator-memo wrap; on cache MISS
// it delegates here. All external callers (hyper_int.cpp,
// integration_step.cpp, bridge/cli/main.cpp) go through the public
// wrap automatically.
static TransformResultSym transform_shuffle_impl(
                                      const PolyCtx& ctx,
                                      const std::vector<Word>& wordlist,
                                      size_t var_idx,
                                      const MzvReductionTable& table,
                                      std::shared_ptr<ZWTable> zw_tab,
                                      bool introduce_algebraic_letters) {
    // HF FF Phase 5 §A.1 iter-50: op_call emit at transform_shuffle entry
    // (§3.1 op #5). Per design memo §3.1 location-typo correction (handoff
    // iter-50-γ.5): the actual file is src/integrator/transform.cpp, not
    // src/algebra/transform.cpp. Arity = wordlist size (clamped to 255 for
    // uint8_t); input hash combines var_idx, the flag, and the wordlist
    // size as a coarse signature. The full per-Word canonical signature is
    // out-of-scope for iter-50 MVP (Word lives in regularize.hpp; would
    // require pulling that header into the probe; the aggregator's
    // op_dup_rate column treats coarse-signature duplicates as a lower
    // bound). OFF-path fast-guard via `hf_probe_active` branch.
    if (hf_probe_active) {
        uint64_t ih = kFnv1a64OffsetBasis;
        ih = hf_probe_fnv1a64_mix_u64(ih, (uint64_t)wordlist.size());
        ih = hf_probe_fnv1a64_mix_u64(ih, (uint64_t)var_idx);
        ih = hf_probe_fnv1a64_mix_u64(ih,
                (uint64_t)(introduce_algebraic_letters ? 1u : 0u));
        const uint8_t arity = (uint8_t)std::min<size_t>(wordlist.size(), 255u);
        hf_probe_emit_op_call("transform_shuffle", ih, arity);
    }
    const Rat rat_one{Poly::one_of(ctx)};

    // Phase B B1.c: v1 SymCoef <-> SymCoefSplit round-trip applied at
    // every function-exit point. Reused for the three return paths
    // below (empty case, repeated-log-power collation, main loop).
    // Recursive transform_shuffle / transform_word calls already
    // round-trip their own outputs; applying again at this layer is
    // canonically a no-op at B1 (W-side empty hypothesis) and the
    // bit-identity gate catches any drift.
    //
    // C0c.1 iter-65 (path 1a sub-iter 2): lambda kill. The persistent
    // ZWTable is threaded by the caller through transform_shuffle's
    // `zw_tab` parameter (signature widened in iter-52b cascade); the
    // `[&]` reference capture brings it into scope. Outer callers
    // allocate once per hyperflint_sym driver entry
    // (hyper_int.cpp:463-466), once per CLI invocation
    // (bridge/cli/main.cpp:3055), once per integration_step's serial
    // post-OMP divergence-check pass (integration_step.cpp:1140), or
    // per-thread via integration_step's outer parallel-for per
    // iter-58 Option A (integration_step.cpp:1614,
    // `zw_for_this_thread`). The prior local
    // `auto zw_tab = make_shared<ZWTable>(ctx);` shadowed the outer
    // parameter and discarded handle reuse across nested calls. Same
    // pattern as iter-64 site 2 at transform.cpp:917 and iter-65
    // site 5 at primitive.cpp:296.
    //
    // Iter-65 advisory-2 fold (from iter-64 reviewer agentId
    // a8d6453b4307f6d1f): defense-in-depth
    // HF_SCALAR_REP_REQUIRE_PERSISTENT abort guard mirrors site 4
    // pattern at break_up_contour.cpp:305 and sites 1/7 at
    // transform.cpp:605 / integration_step.cpp:1003.
    auto apply_v1_roundtrip = [&](TransformResultSym& out) {
        if (!runtime::scalar_rep_enabled()) return;
        if (runtime::require_persistent_enabled() && !zw_tab) {
            std::cerr << "[HF_SCALAR_REP_REQUIRE_PERSISTENT=1]"
                << " transform_shuffle apply_v1_roundtrip:"
                << " zw_tab is null."
                << " Caller did not thread the persistent ZWTable"
                << " from hyperflint_sym (hyper_int.cpp:463-466),"
                << " CLI (bridge/cli/main.cpp:3055), or"
                << " integration_step (1140/1614)."
                << " Migrate per design v2 sec 3.6a."
                << std::endl;
            std::abort();
        }
        for (auto& pair : out) {
            pair.regulator = runtime::roundtrip_regulator_through_scs(
                pair.regulator, ctx, zw_tab,
                "transform_shuffle");
        }
    };

    // Base case: empty input.
    if (wordlist.empty()) {
        TransformResultSym out;
        TransformPairSym p;
        p.shuffle.terms.push_back(
            WordlistTerm{rat_one, Word{}});
        p.regulator.push_back(
            RegTermSym{SymCoef::from_rat(rat_one), RegKey{}});
        out.push_back(std::move(p));
        apply_v1_roundtrip(out);
        return out;
    }

    // ---- Collate repeated-log-power words ----
    // Words of the form [l, l, ..., l] are log^n factors; we merge
    // across occurrences of the same letter and apply the usual
    // combinatorial factor 1/n! per merged word, then n_total!
    // to account for the final single shuffle.
    std::unordered_map<std::string, Letter> letter_store;
    std::vector<std::string> letter_order;
    std::unordered_map<std::string, size_t> log_count;
    std::vector<Word> combined;
    Rat temp_factor{Poly::one_of(ctx)};
    bool repeated = false;

    for (const auto& w : wordlist) {
        if (w.empty()) {
            // Match HyperIntica.wl:2571-2574 — empty words in the
            // shuffle product list represent Hlog[var, {}] = 1
            // (identity) and are skipped. HyperIntica prints an
            // "Error: ..." message but Continue[]s; we silently skip
            // (forensics available by rebuilding with a cerr line).
            continue;
        }
        if (is_log_power(w)) {
            const std::string key = w[0].to_string();
            // 1/n! factor
            temp_factor = temp_factor / factorial_rat(ctx, w.size());
            auto it = log_count.find(key);
            if (it == log_count.end()) {
                log_count[key] = w.size();
                letter_store.emplace(key, w[0]);
                letter_order.push_back(key);
            } else {
                it->second += w.size();
                repeated = true;
            }
        } else {
            combined.push_back(w);
        }
    }

    if (repeated) {
        // Rebuild the combined list with merged log-power words.
        for (const std::string& key : letter_order) {
            size_t n = log_count[key];
            Word merged;
            merged.letters.reserve(n);
            const Letter& l = letter_store.at(key);
            for (size_t i = 0; i < n; ++i) merged.letters.push_back(l);
            combined.push_back(std::move(merged));
        }
        // Multiply tempFactor by Product[n!] over the merged counts.
        for (const auto& kv : log_count) {
            temp_factor = temp_factor * factorial_rat(ctx, kv.second);
        }
        // Recurse on the collated list.
        TransformResultSym sub = transform_shuffle(ctx, combined, var_idx,
                                                     table,
                                                     zw_tab,
                                                     introduce_algebraic_letters);
        // Scale the regulator of each pair by temp_factor.
        TransformResultSym out;
        out.reserve(sub.size());
        for (const auto& pair : sub) {
            TransformPairSym np;
            np.shuffle   = pair.shuffle;
            np.regulator = scalar_mul_regulator_sym(pair.regulator,
                                                      temp_factor);
            out.push_back(std::move(np));
        }
        apply_v1_roundtrip(out);
        return out;
    }

    // ---- Main accumulator loop ----
    std::vector<TransformPairSym> acc;
    {
        TransformPairSym seed;
        seed.shuffle.terms.push_back(
            WordlistTerm{rat_one, Word{}});
        seed.regulator.push_back(
            RegTermSym{SymCoef::from_rat(rat_one), RegKey{}});
        acc.push_back(std::move(seed));
    }

    for (const auto& w : wordlist) {
        TransformResultSym sub = transform_word(ctx, w, var_idx, table,
                                                  zw_tab,
                                                  introduce_algebraic_letters);
        if (sub.empty()) {
            // count = 0; Break -> empty overall.
            return {};
        }
        std::vector<TransformPairSym> next;
        next.reserve(acc.size() * sub.size());
        for (const auto& a : acc) {
            for (const auto& s : sub) {
                TransformPairSym np;
                np.shuffle   = shuffle_product(a.shuffle, s.shuffle);
                np.shuffle   = collect_words(np.shuffle);
                np.regulator = shuffle_symbolic_sym(a.regulator, s.regulator);
                next.push_back(std::move(np));
            }
        }
        acc = std::move(next);
    }

    TransformResultSym out{std::move(acc)};
    apply_v1_roundtrip(out);
    return out;
}

// HF FF Phase 5 §E Step E.2-impl-2 (iter-62-β.2).
//
// Public `transform_shuffle` entry-point with operator-memoization wrap.
// On cache HIT, returns a deep-copy of the cached TransformResultSym and
// fires the (no-op) `counter_replay::transform_shuffle_on_hit()` shim
// for diagnostic-counter discipline per §iter-59-fold-REQ-5 §4.4-bis
// row 5. On cache MISS, delegates to `transform_shuffle_impl` (the
// pre-iter-62 body) and inserts the result.
//
// FOLD-ER3 SCALAR_REP=1 disposition (defense-in-depth):
// `operator_memo::transform_shuffle_enabled()` already returns false
// under HF_USE_SCALAR_REP=1, so the cache is never consulted on the
// SCALAR-rep path. The explicit `runtime::scalar_rep_enabled()` check
// here is the implementation_memo §3.5 + FOLD-ER3 defense-in-depth
// pattern — double-protection in case the predicate's env-gate parse
// has a future bug.
//
// Master-switch fast path: when HF_OPERATOR_MEMO=0 (default), the wrap
// is a single load-from-cached-bool + branch + tail-call; canonical
// output is byte-identical to pre-iter-62 (byte-id smoke gate at
// iter-62-ε / iter-63 §7).
TransformResultSym transform_shuffle(const PolyCtx& ctx,
                                      const std::vector<Word>& wordlist,
                                      size_t var_idx,
                                      const MzvReductionTable& table,
                                      std::shared_ptr<ZWTable> zw_tab,
                                      bool introduce_algebraic_letters) {
    // Defense-in-depth: explicit SCALAR_REP=1 bypass per FOLD-ER3.
    if (runtime::scalar_rep_enabled()) {
        return transform_shuffle_impl(
            ctx, wordlist, var_idx, table, zw_tab,
            introduce_algebraic_letters);
    }
    if (!operator_memo::transform_shuffle_enabled()) {
        return transform_shuffle_impl(
            ctx, wordlist, var_idx, table, zw_tab,
            introduce_algebraic_letters);
    }

    canonical_signature::TransformShuffleKey key =
        canonical_signature::make_transform_shuffle_key(
            wordlist,
            var_idx,
            ctx,
            zw_tab.get(),
            introduce_algebraic_letters);
    const std::uint64_t key_hash =
        canonical_signature::hash_transform_shuffle_key(key);

    // iter-70 REC-2: try_lookup returns
    // std::shared_ptr<const TransformResultSym> (ref-counted COW handle).
    // On HIT, `return *cached_sp` invokes TransformResultSym's copy ctor
    // at the caller-by-value boundary; cache-side lock-held window
    // shrinks to O(1) ref-count++.
    auto cached_sp = g_transform_shuffle_cache().try_lookup(key, key_hash);
    if (cached_sp) {
        counter_replay::transform_shuffle_on_hit();
        return *cached_sp;
    }

    TransformResultSym result = transform_shuffle_impl(
        ctx, wordlist, var_idx, table, zw_tab,
        introduce_algebraic_letters);
    g_transform_shuffle_cache().insert(
        std::move(key), key_hash, TransformResultSym(result));
    return result;
}

}  // namespace hyperflint
