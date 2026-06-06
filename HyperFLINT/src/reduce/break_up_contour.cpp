// Phase 6d-ii  + Phase 6d-v-ii: BreakUpContour.
//
//   * break_up_contour     — base case (onAxis empty), Rat coefficients.
//   * break_up_contour_sym — recursive positive-letter case, SymCoef
//                            coefficients.
//
// The recursive case mirrors HyperIntica.wl:5136-5230 line-by-line:
//   smallest = first(onAxis)
//   imPart   = its delta[var]
//   newAxis  = [(l - smallest, ip) for (l,ip) in rest(onAxis)]
//   substitute = Pi*I*imPart - Log[smallest]
//   For each word in input, for each split-i in 0..len(word):
//     tail = word[i+1..]   (or {} when i == len)
//     temp = (smallest == 1 && tail numeric) ? ZeroOnePeriod[tail]
//                                            : ConvertABtoZeroInf(RegHead(...), 0, smallest)
//     head = [letter - smallest for letter in word[0..i]]
//     brk  = break_up_contour_sym(reg_tail_sym({{coef, head}}, 0, substitute),
//                                  newAxis)
//     Combine via shuffle: result[Sort[Join[bW, tW]]] += b.coef * t.coef
//
// Recursion terminates because each call shrinks |onAxis| by 1.

#include "hyperflint/reduce/break_up_contour.hpp"

#include "hyperflint/algebra/convert.hpp"
#include "hyperflint/algebra/shuffle.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/zw_table.hpp"            // ZWTable (B2 v1 round-trip)
#include "hyperflint/integrator/regularize.hpp"
#include "hyperflint/reduce/period_scratch.hpp"  // period-tuples Phase 2
#include "hyperflint/reduce/periods.hpp"
#include "hyperflint/runtime/scalar_rep.hpp"        // runtime::scalar_rep_enabled (B2 dispatch)
#include "hyperflint/runtime/scs_roundtrip.hpp"     // runtime::roundtrip_regulator_through_scs (B2 verifier site)

#include <algorithm>
#include <cstdlib>      // std::abort (iter-44 require_persistent assertion)
#include <iostream>     // std::cerr (iter-44 require_persistent assertion)
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace hyperflint {

namespace {

bool word_letters_in_minus2_minus1_zero(const Word& w) {
    for (const auto& l : w.letters) {
        const std::string s = l.to_string();
        if (s != "0" && s != "-1" && s != "-2") return false;
    }
    return true;
}

// True iff every letter of `w` parses as a literal integer (its
// canonical Rat string matches /-?\d+/).
bool word_all_integer_letters(const Word& w) {
    for (const auto& l : w.letters) {
        const std::string s = l.to_string();
        size_t i = 0;
        if (i < s.size() && s[i] == '-') ++i;
        if (i >= s.size()) return false;
        for (; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
        }
    }
    return true;
}

// True iff every letter is a "numeric constant" (rational literal —
// integer or fraction). Mirrors `AllTrue[Simplify /@ tail, NumericQ]`
// in the BreakUpContour Mma source.
bool word_all_numeric_letters(const Word& w) {
    for (const auto& l : w.letters) {
        if (!l.num().is_fmpq()) return false;
        if (!l.den().is_fmpq()) return false;
    }
    return true;
}

Rat rat_int(const PolyCtx& ctx, long k) {
    return Rat{Poly::from_int(ctx, k)};
}
Rat rat_one(const PolyCtx& ctx) { return Rat{Poly::one_of(ctx)}; }
Rat rat_zero(const PolyCtx& ctx) { return Rat{Poly::zero_of(ctx)}; }

Rat factorial_rat(const PolyCtx& ctx, long n) {
    long f = 1;
    for (long k = 2; k <= n; ++k) f *= k;
    return rat_int(ctx, f);
}

}  // namespace

// -------- Phase 6d-ii base case --------

Regulator break_up_contour(const PolyCtx& ctx,
                            const Wordlist& wordlist,
                            const std::vector<OnAxisEntry>& on_axis,
                            const MzvReductionTable& table) {
    if (!on_axis.empty()) {
        throw std::runtime_error(
            "break_up_contour: onAxis non-empty — use break_up_contour_sym");
    }
    Rat const_val = rat_zero(ctx);
    Regulator var_part;
    for (const auto& t : wordlist.terms) {
        if (word_letters_in_minus2_minus1_zero(t.word)) {
            Rat period = zero_inf_period(ctx, t.word, table);
            const_val = const_val + t.coef * period;
        } else {
            RegKey key;
            if (!t.word.empty()) key.push_back(t.word);
            var_part.push_back(RegTerm{t.coef, std::move(key)});
        }
    }
    Regulator out;
    if (!const_val.is_zero()) out.push_back(RegTerm{const_val, RegKey{}});
    for (auto& t : var_part) out.push_back(std::move(t));
    return canonicalize_regulator(out);
}

// -------- Phase 6d-v-ii: SymCoef-valued helpers --------

WordlistSym to_wordlist_sym(const Wordlist& wl) {
    WordlistSym out;
    out.terms.reserve(wl.terms.size());
    for (const auto& t : wl.terms) {
        out.terms.push_back(WordlistSymTerm{SymCoef::from_rat(t.coef), t.word});
    }
    return out;
}

namespace {

// Collect like-Word entries in a WordlistSym (sum SymCoef coefs, drop zero).
WordlistSym collect_wordlist_sym(const WordlistSym& wl) {
    std::unordered_map<std::string, size_t> idx;
    std::vector<WordlistSymTerm> kept;
    for (const auto& t : wl.terms) {
        std::string k = t.word.content_key();
        auto it = idx.find(k);
        if (it == idx.end()) {
            idx[k] = kept.size();
            kept.push_back(t);
        } else {
            kept[it->second].coef = kept[it->second].coef + t.coef;
        }
    }
    WordlistSym out;
    for (auto& t : kept) {
        if (!t.coef.is_zero()) out.terms.push_back(std::move(t));
    }
    return out;
}

// SymCoef-substitute RegTail. Mirrors HyperIntica.wl:2499 with the
// substitute as a SymCoef and the term coefficients as SymCoef.
WordlistSym reg_tail_sym(const PolyCtx& ctx,
                          const WordlistSym& wl,
                          const Letter& letter,
                          const SymCoef& substitute) {
    WordlistSym res;
    for (const auto& term : wl.terms) {
        const Word& wrd = term.word;
        const long wlen = (long)wrd.size();
        if (wlen == 0) { res.terms.push_back(term); continue; }

        long n = 0;
        for (long i = wlen - 1; i >= 0; --i) {
            if (wrd[(size_t)i].equal(letter)) ++n; else break;
        }

        if (n == wlen) {
            // Entire word is `letter`: coef * substitute^n / n!
            SymCoef prefac = SymCoef::from_rat(rat_one(ctx));
            for (long k = 0; k < n; ++k) prefac = prefac.mul(substitute);
            if (n > 0) prefac = prefac.div_rat(factorial_rat(ctx, n));
            res.terms.push_back(WordlistSymTerm{term.coef.mul(prefac), Word{}});
            continue;
        }

        const Letter& anchor = wrd[(size_t)(wlen - n - 1)];
        Word stripped;
        stripped.letters.assign(wrd.letters.begin(),
                                wrd.letters.begin() + (wlen - n - 1));

        for (long ii = 0; ii <= n; ++ii) {
            // sign · [letter]^ii  shuffled with  (1, stripped)  -> Rat-coef.
            Wordlist A;
            {
                Word ls;
                ls.letters = std::vector<Letter>((size_t)ii, letter);
                Rat sign = (ii % 2 == 0) ? rat_one(ctx) : -rat_one(ctx);
                A.terms.push_back({sign, ls});
            }
            Wordlist B;
            B.terms.push_back({rat_one(ctx), stripped});
            Wordlist AB = shuffle_product(A, B);

            // prefac is SymCoef: (ii == n) ? 1 : substitute^(n-ii) / (n-ii)!
            SymCoef prefac = SymCoef::from_rat(rat_one(ctx));
            if (ii != n) {
                for (long k = 0; k < n - ii; ++k) prefac = prefac.mul(substitute);
                prefac = prefac.div_rat(factorial_rat(ctx, n - ii));
            }

            // Combine: (term.coef * AB[k].coef * prefac, AB[k].word ++ [anchor])
            SymCoef combined_prefac = term.coef.mul(prefac);
            for (const auto& ab : AB.terms) {
                Word w = ab.word;
                w.letters.push_back(anchor);
                res.terms.push_back(
                    WordlistSymTerm{combined_prefac.mul_rat(ab.coef),
                                     std::move(w)});
            }
        }
    }
    return collect_wordlist_sym(res);
}

}  // namespace

// -------- canonicalize_regulator_sym --------

RegulatorSym canonicalize_regulator_sym(const RegulatorSym& r) {
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
    std::stable_sort(out.begin(), out.end(),
        [](const RegTermSym& a, const RegTermSym& b) {
            return regkey_content_key(a.key) < regkey_content_key(b.key);
        });
    return out;
}

// -------- Phase 6d-v-ii recursive case --------

RegulatorSym break_up_contour_sym(const PolyCtx& ctx,
                                    const WordlistSym& wordlist,
                                    const std::vector<OnAxisSymEntry>& on_axis,
                                    const MzvReductionTable& table,
                                    std::shared_ptr<ZWTable> zw_tab) {
    // Phase-B B2: SymCoef <-> SymCoefSplit round-trip applied at each
    // function-exit point under runtime::scalar_rep_enabled() (the
    // HF_USE_SCALAR_REP env-gate). Recursive break_up_contour_sym calls
    // already round-trip their own outputs; applying again at the outer
    // layer is canonically a no-op at B1 (W-side empty hypothesis,
    // b1_scoping_memo.md R2 + design v2 §4.4a Note 2). Smirnov fixtures
    // never exercise the positive-letter recursive branch (linear-in-
    // letters), so the bit-identity gate at B2 stays in the
    // round-trip-trivial regime.
    //
    // C0b.4 (iter-42, 2026-05-09): the per-call `make_shared<ZWTable>(ctx)`
    // inside this lambda has been hoisted out — `zw_tab` now arrives as a
    // mandatory function parameter (caller owns the table). When
    // `runtime::scalar_rep_enabled()` is false, the lambda short-circuits
    // before any `zw_tab` use, so a null shared_ptr is safe (and is the
    // current state at intermediate callsites that have not yet received
    // the persistent ZWTable from `hyperflint_sym`). When the env-gate is
    // on, callers (the three external callsites at `transform.cpp:589`,
    // `integration_step.cpp:983`, and the recursive call below) supply a
    // non-null shared_ptr; the lambda passes it straight to
    // `runtime::roundtrip_regulator_through_scs`. Per-call lifetime is
    // therefore preserved at iter-42 (each top-level callsite still
    // allocates one ZWTable per call), but the allocation site has moved
    // up to the caller — paving the way for iter-43+ to consolidate
    // sites 1-5 and the eventual driver-entry table from
    // `hyperflint_sym` (already allocated at iter-41) to traverse the
    // entire chain.
    // Iter-44 (2026-05-09): debug-build assertion gate
    // `HF_SCALAR_REP_REQUIRE_PERSISTENT=1` mitigates the silent-regression
    // trap identified by the iter-43 cold-start drift check (Concern 2):
    // a transitional callsite that fails to thread the persistent
    // `ZWTable` allocated at `hyperflint_sym` (iter-41) would silently
    // fall back to a per-call/null table without visible behavior change
    // under the bit-identity gate. Default-OFF preserves byte-identity;
    // turning the gate on aborts here at every callsite that has not yet
    // migrated. Vacates entirely once the C0c.1 cascade lands and every
    // caller supplies a non-null `zw_tab` (iter-46+).
    //
    // We use `std::abort()` (preceded by an explanatory `std::cerr` line)
    // rather than `throw std::runtime_error`: this lambda may execute
    // inside the `close_positive_letters_in_regulator_sym` OMP parallel
    // region (`integration_step.cpp:920+`), whose catch block at
    // `integration_step.cpp:1032` silently swallows `std::runtime_error`
    // when `HF_LF_TOLERATE_NARROW_CTX` is set. Aborting bypasses the
    // catch and produces an unambiguous failure.
    auto apply_v1_roundtrip = [&](RegulatorSym&& r,
                                    const char* tag) -> RegulatorSym {
        if (!runtime::scalar_rep_enabled()) return std::move(r);
        if (runtime::require_persistent_enabled() && !zw_tab) {
            std::cerr << "[HF_SCALAR_REP_REQUIRE_PERSISTENT=1]"
                << " break_up_contour_sym: zw_tab is null at tag="
                << tag
                << " (src/reduce/break_up_contour.cpp body lambda)."
                << " Caller did not thread the persistent ZWTable"
                << " allocated at hyperflint_sym (iter-41). Migrate"
                << " the offending callsite (transform.cpp:589,"
                << " integration_step.cpp:983, or"
                << " bridge/cli/main.cpp:2075) to forward the driver-"
                << "entry table per design v2 sec 3.6a." << std::endl;
            std::abort();
        }
        return runtime::roundtrip_regulator_through_scs(r, ctx, zw_tab, tag);
    };

    // Base case: no positive letters on axis. Same partition logic as
    // break_up_contour, lifted to SymCoef.
    if (on_axis.empty()) {
        SymCoef const_val(ctx);
        RegulatorSym var_part;
        for (const auto& t : wordlist.terms) {
            if (word_letters_in_minus2_minus1_zero(t.word)) {
                // Period-tuples Phase 2: same reduction, tuple storage
                // (mint_period_sym runs the identical machinery in the
                // atoms-only scratch ring and returns period_powers
                // monomials over the slim ctx).
                if (period_tuples_enabled()) {
                    const_val = const_val + t.coef.mul(
                        mint_period_sym(ctx, t.word, table,
                                        /*zero_one=*/false));
                } else {
                    Rat period = zero_inf_period(ctx, t.word, table);
                    const_val = const_val + t.coef.mul_rat(period);
                }
            } else {
                RegKey key;
                if (!t.word.empty()) key.push_back(t.word);
                var_part.push_back(RegTermSym{t.coef, std::move(key)});
            }
        }
        RegulatorSym out;
        if (!const_val.is_zero()) {
            out.push_back(RegTermSym{const_val, RegKey{}});
        }
        for (auto& t : var_part) out.push_back(std::move(t));
        return apply_v1_roundtrip(canonicalize_regulator_sym(out),
                                    "break_up_contour_sym/base");
    }

    // Recursive case.
    const Rat&     smallest = on_axis[0].letter;
    const SymCoef& im_part  = on_axis[0].im_part;
    std::vector<OnAxisSymEntry> new_axis;
    new_axis.reserve(on_axis.size() - 1);
    for (size_t k = 1; k < on_axis.size(); ++k) {
        new_axis.push_back(OnAxisSymEntry{on_axis[k].letter - smallest,
                                            on_axis[k].im_part});
    }

    // substitute = Pi * I * imPart - Log[smallest].
    // For Phase 6d-v-ii, we require `smallest` to be a positive integer
    // literal so we can build Log[n] as a SymCoef::log_factor. If
    // smallest is symbolic (out of scope), throw with a clear message.
    long smallest_long = 0;
    {
        if (smallest.den().to_string() != "1") {
            throw std::runtime_error(
                "break_up_contour_sym: non-integer smallest=" +
                smallest.to_string() + " (out of Phase 6d-v-ii scope)");
        }
        try { smallest_long = std::stol(smallest.num().to_string()); }
        catch (const std::exception&) {
            throw std::runtime_error(
                "break_up_contour_sym: symbolic smallest=" +
                smallest.to_string() + " (out of Phase 6d-v-ii scope)");
        }
        if (smallest_long <= 0) {
            throw std::runtime_error(
                "break_up_contour_sym: non-positive smallest=" +
                smallest.to_string());
        }
    }
    SymCoef substitute =
        SymCoef::pi_factor(ctx).mul(SymCoef::im_factor(ctx)).mul(im_part);
    if (smallest_long != 1) {
        // - Log[n] for n >= 2.
        substitute = substitute - SymCoef::log_factor(ctx, smallest_long);
    }
    // smallest == 1: Log[1] = 0, so substitute = Pi*I*imPart.

    // Accumulator: RegKey content_key -> (SymCoef coef, RegKey copy).
    std::unordered_map<std::string, size_t> acc_idx;
    std::vector<RegTermSym> acc;

    auto push_term = [&](const RegKey& key, const SymCoef& add_coef) {
        RegKey canon = canonicalize_regkey(key);
        std::string k = regkey_content_key(canon);
        auto it = acc_idx.find(k);
        if (it == acc_idx.end()) {
            acc_idx[k] = acc.size();
            acc.push_back(RegTermSym{add_coef, std::move(canon)});
        } else {
            acc[it->second].coef = acc[it->second].coef + add_coef;
        }
    };

    for (const auto& term : wordlist.terms) {
        const Word& word    = term.word;
        const SymCoef& coef = term.coef;
        const long wlen     = (long)word.size();

        for (long i = 0; i <= wlen; ++i) {
            // Mma split at i: head = word[1..i], tail = word[i+1..wlen]
            // (1-based). 0-based equivalent: head = word[0..i-1],
            // tail = word[i..wlen-1]. So `tail.begin() = word.begin()+i`.
            Word tailWord;
            if (i < wlen) {
                tailWord.letters.assign(word.letters.begin() + i,
                                        word.letters.end());
            }

            // temp: list of (SymCoef coef, RegKey key).
            RegulatorSym temp;
            const bool tail_numeric =
                tailWord.empty()
                || word_all_integer_letters(tailWord)
                || word_all_numeric_letters(tailWord);
            if (smallest_long == 1 && tail_numeric) {
                // ZeroOnePeriod evaluates to a Rat (mzv-basis); RegKey is empty.
                // Period-tuples Phase 2: tuple storage of the same value.
                if (period_tuples_enabled()) {
                    SymCoef zop = mint_period_sym(ctx, tailWord, table,
                                                  /*zero_one=*/true);
                    if (!zop.is_zero()) {
                        temp.push_back(RegTermSym{std::move(zop), RegKey{}});
                    }
                } else {
                    Rat zop = zero_one_period(ctx, tailWord, table);
                    if (!zop.is_zero()) {
                        temp.push_back(RegTermSym{SymCoef::from_rat(zop), RegKey{}});
                    }
                }
            } else {
                // Mma:  ConvertABtoZeroInf[RegHead[{{1, tailWord}}, smallest], 0, smallest]
                // RegHead signature: reg_head(wl, letter, substitute=0).
                // So letter == smallest, substitute defaults to 0.
                Wordlist seed;
                seed.terms.push_back({rat_one(ctx), tailWord});
                Wordlist rh = reg_head(seed, smallest, rat_zero(ctx));
                Wordlist conv = convert_ab_to_zero_inf(ctx, rh,
                                                       rat_zero(ctx), smallest);
                for (const auto& tt : conv.terms) {
                    RegKey key;
                    if (!tt.word.empty()) key.push_back(tt.word);
                    temp.push_back(RegTermSym{SymCoef::from_rat(tt.coef),
                                               std::move(key)});
                }
            }

            // Head: word[0..i-1] each shifted by -smallest.
            Word headWord;
            for (long j = 0; j < i; ++j) {
                headWord.letters.push_back(word[(size_t)j] - smallest);
            }

            // brk = break_up_contour_sym(reg_tail_sym({{coef, head}}, 0, substitute),
            //                              newAxis)
            WordlistSym head_seed;
            head_seed.terms.push_back(WordlistSymTerm{coef, headWord});
            WordlistSym head_regtailed =
                reg_tail_sym(ctx, head_seed, rat_zero(ctx), substitute);
            // C0b.4 (iter-42): forward the caller-owned `zw_tab` to the
            // recursive call so the entire recursion shares one table.
            RegulatorSym brk =
                break_up_contour_sym(ctx, head_regtailed, new_axis, table,
                                       zw_tab);

            // Shuffle-combine: result[Sort[Join[bW, tW]]] += b.coef * t.coef.
            for (const auto& b : brk) {
                for (const auto& t : temp) {
                    RegKey joined;
                    joined.reserve(b.key.size() + t.key.size());
                    for (const auto& w : b.key) joined.push_back(w);
                    for (const auto& w : t.key) joined.push_back(w);
                    push_term(joined, b.coef.mul(t.coef));
                }
            }
        }
    }

    return apply_v1_roundtrip(canonicalize_regulator_sym(acc),
                                "break_up_contour_sym/recursive");
}

}  // namespace hyperflint
