// Phase 3-b implementation — see convert_hlog.hpp for the 7-case map.

#include "hyperflint/convert/convert_hlog.hpp"

#include "hyperflint/algebra/shuffle.hpp"

#include <cassert>
#include <sstream>

namespace hyperflint {
namespace convert {

namespace {

// ---- Rat convenience helpers (ctx-bound) ----

Rat rat_literal(const PolyCtx& ctx, const std::string& s) {
    return Rat::parse(ctx, s);
}
Rat rat_one(const PolyCtx& ctx)  { return rat_literal(ctx, "1"); }
Rat rat_zero(const PolyCtx& ctx) { return rat_literal(ctx, "0"); }

// factorial as a Rat literal in ctx.
Rat rat_factorial(const PolyCtx& ctx, long n) {
    if (n <= 1) return rat_one(ctx);
    long acc = 1;
    for (long k = 2; k <= n; ++k) acc *= k;
    return rat_literal(ctx, std::to_string(acc));
}

// (-1)^ii as a Rat.
Rat rat_sign(const PolyCtx& ctx, long ii) {
    return (ii % 2 == 0) ? rat_one(ctx) : -rat_one(ctx);
}

// ---- Word <-> RegKey adapter ----

// Wrap a single Word as a 1-element RegKey (what Case 7's tail
// `Map[{#[[1]], {#[[2]]}}&, result]` does in HyperIntica.wl:3447).
RegKey word_as_regkey(const Word& w) {
    RegKey k;
    k.push_back(w);
    return k;
}

// ---- Log-power formulas (Cases 3 and 6) ----

// Emit Regulator{{(-1)^n / n!, [[letter, letter, ..., letter]]}} — a
// single Word of n copies of `letter`, wrapped as a RegKey.
Regulator
log_power(const PolyCtx& ctx, const Rat& letter, long n) {
    Rat coef = rat_sign(ctx, n) / rat_factorial(ctx, n);
    Word w;
    w.letters.assign(static_cast<size_t>(n), letter);
    Regulator out;
    out.push_back(RegTerm{std::move(coef), word_as_regkey(w)});
    return out;
}

// ---- Case 7: general convergent case ----
//
// Mirrors HyperIntica.wl:3443-3447. Builds a Wordlist via a left-to-
// right ConcatMul chain where each letter contributes either
// [{1, [-1]}]                                   (word[i] == 0)  or
// [{1, [-1]}, {-1, [var/word[i] - 1]}]          (word[i] != 0)
// prepended to the running `result`. Final wrap: each Wordlist
// entry's Word becomes a 1-element RegKey.
Regulator
case_general(const Rat& var, const std::vector<Rat>& word,
             const PolyCtx& ctx) {
    Wordlist result;
    result.terms.push_back(WordlistTerm{rat_one(ctx), Word{}});

    Rat minus_one = rat_literal(ctx, "-1");

    for (const Rat& wi : word) {
        Wordlist left;   // either [{1, [-1]}] or [{1, [-1]}, {-1, [var/wi - 1]}]
        left.terms.push_back(WordlistTerm{
            rat_one(ctx),
            Word{std::vector<Letter>{minus_one}}});
        if (!wi.is_zero()) {
            // var/wi - 1  (as a Rat in the caller-supplied PolyCtx).
            Rat letter = (var / wi) - rat_one(ctx);
            left.terms.push_back(WordlistTerm{
                -rat_one(ctx),
                Word{std::vector<Letter>{letter}}});
        }
        result = concat_mul(left, result);
    }

    // Wrap each Word into a RegKey of length 1.
    Regulator out;
    out.reserve(result.terms.size());
    for (auto& t : result.terms) {
        out.push_back(RegTerm{t.coef, word_as_regkey(t.word)});
    }
    return out;
}

// ---- Case 4 support: reg_tail_expr's inner shuffle ----
//
// Build the Mma `ShuffleProduct[{{(-1)^ii, ii_word}}, {{1, prefix}}]`
// → HF Wordlist. `ii_word` is `ii` copies of `letter_to_strip`;
// `prefix` is `word[0..anchor_pos - 1]`.
Wordlist
shuffle_ii_zeros_with_prefix(long ii,
                              const Rat& letter_to_strip,
                              const Word& prefix,
                              const PolyCtx& ctx) {
    Wordlist left;
    Word ii_word;
    for (long k = 0; k < ii; ++k) ii_word.letters.push_back(letter_to_strip);
    Rat sign = rat_sign(ctx, ii);
    left.terms.push_back(WordlistTerm{std::move(sign), std::move(ii_word)});
    Wordlist right;
    right.terms.push_back(WordlistTerm{rat_one(ctx), prefix});
    return shuffle_product(left, right);
}

}  // namespace

// ---- reg_tail_expr ----

std::vector<RegTailExprTerm>
reg_tail_expr(const Word& word,
               const Rat& letter_to_strip,
               const Expr& subst,
               const PolyCtx& ctx) {
    std::vector<RegTailExprTerm> out;

    // Empty word: {1, {}} — no stripping needed.
    if (word.letters.empty()) {
        out.push_back(RegTailExprTerm{
            Expr::leaf(rat_one(ctx)), word});
        return out;
    }

    // Count trailing occurrences of letter_to_strip.
    long wlen = static_cast<long>(word.letters.size());
    long n = 0;
    for (long i = wlen - 1; i >= 0; --i) {
        if (word.letters[i].equal(letter_to_strip)) ++n;
        else break;
    }

    // All letters equal letter_to_strip: whole-word collapse.
    if (n == wlen) {
        // Emit {subst^n / n!, {}}.
        Expr coef = [&]() {
            if (n == 0) return Expr::leaf(rat_one(ctx));
            Rat rat_coef = rat_one(ctx) / rat_factorial(ctx, n);
            std::vector<Expr> factors;
            factors.push_back(Expr::leaf(std::move(rat_coef)));
            factors.push_back(Expr::power(subst, n));
            return Expr::times(std::move(factors));
        }();
        out.push_back(RegTailExprTerm{std::move(coef), Word{}});
        return out;
    }

    // n < wlen. Anchor is the first non-stripped letter counting
    // from the right. After reg_tail_expr returns, every emitted
    // word ends with `anchor_letter` (non-stripped by construction),
    // guaranteeing the Case-4 infinite-recursion guard.
    long anchor_idx = wlen - n - 1;
    Word prefix;
    prefix.letters.assign(
        word.letters.begin(),
        word.letters.begin() + anchor_idx);
    const Rat& anchor_letter = word.letters[anchor_idx];

    for (long ii = 0; ii <= n; ++ii) {
        // Shuffle produces Wordlist entries with coef (-1)^ii × 1.
        Wordlist shuffled =
            shuffle_ii_zeros_with_prefix(ii, letter_to_strip, prefix, ctx);

        // Build the symbolic prefac = subst^(n-ii) / (n-ii)!  as an
        // Expr. When n == ii, prefac == 1 (Leaf).
        long k_pow = n - ii;
        Expr prefac = [&]() {
            if (k_pow == 0) return Expr::leaf(rat_one(ctx));
            Rat coef_rat = rat_one(ctx) / rat_factorial(ctx, k_pow);
            std::vector<Expr> pf_factors;
            pf_factors.push_back(Expr::leaf(std::move(coef_rat)));
            pf_factors.push_back(Expr::power(subst, k_pow));
            return Expr::times(std::move(pf_factors));
        }();

        for (const auto& st : shuffled.terms) {
            // Final coef is st.coef (Rat, already includes (-1)^ii)
            // times prefac (Expr). Combine as Times.
            std::vector<Expr> coef_factors;
            coef_factors.push_back(Expr::leaf(st.coef));
            coef_factors.push_back(prefac);
            Expr coef_expr = Expr::times(std::move(coef_factors));

            // word_w = shuffled_word ++ [anchor_letter]
            Word new_w = st.word;
            new_w.letters.push_back(anchor_letter);

            out.push_back(RegTailExprTerm{std::move(coef_expr), std::move(new_w)});
        }
    }

    // Regression guard for the Case-4 infinite-recursion invariant.
    //
    // The invariant holds compile-time by construction: every new_w is
    // `shuffled_word ++ [anchor_letter]`, and `anchor_letter` is picked
    // as the first (from right) letter that does NOT equal
    // letter_to_strip. So the assert is a tautology today — it can't
    // fire on any input. Kept because a future edit to the loop
    // structure (say, a different shuffle ordering or a wrong anchor
    // pick) would break the post-condition, and this assert surfaces
    // the break immediately rather than burning stack on re-entry.
    for (const auto& t : out) {
        assert(!t.w.letters.empty() &&
               "reg_tail_expr: non-collapse branch produced empty word");
        assert(!t.w.letters.back().equal(letter_to_strip) &&
               "reg_tail_expr: emitted word ends in stripped letter — "
               "Case-4 infinite-recursion guard violated");
    }

    return out;
}

// ---- Hlog 7-case dispatcher ----

Regulator
convert_to_hlog_reg_inf_hlog(const Rat& var,
                              const std::vector<Rat>& word,
                              const PolyCtx& ctx) {
    // Case 1: empty word.
    if (word.empty()) {
        Regulator r;
        r.push_back(RegTerm{rat_one(ctx), RegKey{}});
        return r;
    }

    // Case 2: var == 0. Divergent base; return empty Regulator.
    if (var.is_zero()) return Regulator{};

    // Case 3: all letters == 0.
    bool all_zero = true;
    for (const Rat& l : word) {
        if (!l.is_zero()) { all_zero = false; break; }
    }
    if (all_zero) {
        // var == 1: Log(1)^n / n! = 0. Return empty. (HyperIntica bug
        // upstream — see plan §2.2.1 "Known reference-backend
        // discrepancy". HF follows the Maple/physics convention.)
        if (var.equal(rat_one(ctx))) return Regulator{};
        // else: {{(-1)^n / n!, {{-var, -var, ..., -var}}}}
        return log_power(ctx, -var, static_cast<long>(word.size()));
    }

    // Case 4: trailing zero — reg_tail regularization.
    if (word.back().is_zero()) {
        // Build the substitute `subst = Hlog[var, {0}]`, or Leaf(0)
        // when var == 1 (short-circuit to avoid infinite recursion
        // via the Case-4 path re-encountering Hlog[1, {0}]).
        Expr subst = [&]() {
            if (var.equal(rat_one(ctx))) return Expr::leaf(rat_zero(ctx));
            std::vector<Rat> zero_word;
            zero_word.push_back(rat_zero(ctx));
            return Expr::hlog(var, std::move(zero_word));
        }();

        Word word_as_word;
        word_as_word.letters = word;   // Word is just a vector<Rat>
        auto terms = reg_tail_expr(word_as_word, rat_zero(ctx), subst, ctx);

        // Build sum_i coef_i * Hlog[var, w_i] and recurse.
        std::vector<Expr> summands;
        summands.reserve(terms.size());
        for (auto& t : terms) {
            std::vector<Rat> letters = t.w.letters;
            Expr hlog_term = Expr::hlog(var, std::move(letters));
            std::vector<Expr> factors;
            factors.push_back(std::move(t.coef));
            factors.push_back(std::move(hlog_term));
            summands.push_back(Expr::times(std::move(factors)));
        }
        Expr sum = (summands.size() == 1)
            ? std::move(summands[0])
            : Expr::plus(std::move(summands));
        return convert_to_hlog_reg_inf(sum, ctx);
    }

    // Case 5: word[0] == var. Divergent log; throw.
    if (word.front().equal(var)) {
        throw ConvertFailed(
            "Hlog[var, [var, ...]] is divergent "
            "($Failed in Mma; no regularization available)");
    }

    // Case 6: all letters == word[0] (same non-zero letter).
    bool all_same = true;
    for (const Rat& l : word) {
        if (!l.equal(word.front())) { all_same = false; break; }
    }
    if (all_same) {
        // Return {{(-1)^n/n!, {[var/a - 1, ..., var/a - 1]}}}.
        Rat letter = (var / word.front()) - rat_one(ctx);
        return log_power(ctx, std::move(letter),
                          static_cast<long>(word.size()));
    }

    // Case 7: general convergent case.
    return case_general(var, word, ctx);
}

// ---- Top-level driver (minimal, Phase-3-b scope) ----

namespace {

// Utility: is a Regulator entry's key empty (i.e. no RegKey words)?
bool regkey_is_empty(const RegKey& k) { return k.empty(); }

// Union two Regulators by merging duplicate keys via
// canonicalize_regulator (declared in transform.hpp).
Regulator merge_regulators(const Regulator& a, const Regulator& b) {
    Regulator out = a;
    for (const auto& t : b) out.push_back(t);
    return canonicalize_regulator(out);
}

// Scalar multiply a Regulator by a Rat constant.
Regulator scalar_mul_regulator_rat(const Regulator& r, const Rat& s) {
    Regulator out;
    out.reserve(r.size());
    for (const auto& t : r) {
        out.push_back(RegTerm{t.coef * s, t.key});
    }
    return out;
}

}  // namespace

Regulator
convert_to_hlog_reg_inf(const Expr& e, const PolyCtx& ctx) {
    switch (e.kind()) {
        case ExprKind::Leaf: {
            Regulator out;
            if (!e.leaf_rat().is_zero()) {
                out.push_back(RegTerm{e.leaf_rat(), RegKey{}});
            }
            return out;
        }
        case ExprKind::Hlog:
            return convert_to_hlog_reg_inf_hlog(
                e.hlog_var(), e.hlog_word(), ctx);
        case ExprKind::Plus: {
            Regulator acc;
            for (size_t i = 0; i < e.num_children(); ++i) {
                Regulator sub = convert_to_hlog_reg_inf(e.child(i), ctx);
                acc = merge_regulators(acc, sub);
            }
            return acc;
        }
        case ExprKind::Times: {
            // Fold via shuffle_symbolic starting from identity.
            Regulator acc;
            acc.push_back(RegTerm{rat_one(ctx), RegKey{}});
            for (size_t i = 0; i < e.num_children(); ++i) {
                Regulator sub = convert_to_hlog_reg_inf(e.child(i), ctx);
                acc = shuffle_symbolic(acc, sub);
            }
            return canonicalize_regulator(acc);
        }
        case ExprKind::Power: {
            long n = e.power_n();
            if (n < 1) {
                throw ConvertFailed(
                    "Power node with non-positive exponent in finalized "
                    "AST (parser should have folded)");
            }
            Regulator base = convert_to_hlog_reg_inf(e.child(0), ctx);
            Regulator acc;
            acc.push_back(RegTerm{rat_one(ctx), RegKey{}});
            for (long k = 0; k < n; ++k) {
                acc = shuffle_symbolic(acc, base);
            }
            return canonicalize_regulator(acc);
        }
    }
    // Unreachable in C++17 exhaustive switch.
    return Regulator{};
}

}  // namespace convert
}  // namespace hyperflint
