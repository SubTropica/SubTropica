// Word-level regularization primitives.
// Ports HyperIntica.wl:2472-2542 (Reg0, RegzeroWord, RegTail, RegHead).

#include "hyperflint/integrator/regularize.hpp"
#include "hyperflint/algebra/shuffle.hpp"

namespace hyperflint {

namespace {

// Rat representing the integer `k` in the given context.
Rat rat_int(const PolyCtx& ctx, long k) {
    return Rat{Poly::from_int(ctx, k)};
}

// n! as a Rat, using 64-bit integer arithmetic; we never need more
// than a few tens of factorials here (word length bounded by depth).
Rat factorial_rat(const PolyCtx& ctx, long n) {
    long f = 1;
    for (long k = 2; k <= n; ++k) f *= k;
    return rat_int(ctx, f);
}

Rat rat_one (const PolyCtx& ctx) { return Rat{Poly::one_of(ctx)}; }

Rat neg_one_pow(const PolyCtx& ctx, long n) {
    Rat o = rat_one(ctx);
    return (n % 2 == 0) ? o : -o;
}

}  // namespace

Wordlist regzero_word(const Word& w) {
    // Empty: identity {1, {}}. The caller must use the (ctx, w) overload
    // when the empty case is reachable — without a ctx we cannot
    // construct the "1" coefficient. Returning an empty wordlist matches
    // what the (Word) overload did historically when the empty path
    // wasn't actually reached at runtime (the CLI used the wordlist-
    // ranged reg0 path), but we leave a clear signal here for any new
    // caller that hits the empty case.
    if (w.empty()) return Wordlist{};
    return regzero_word_in_ctx(w[0].ctx(), w);
}

Wordlist regzero_word_in_ctx(const PolyCtx& ctx, const Word& w) {
    if (w.empty()) {
        Wordlist out;
        out.terms.push_back({rat_one(ctx), Word{}});
        return out;
    }

    const long len = (long)w.size();

    // Count trailing zero letters.
    long n = 0;
    for (long i = len - 1; i >= 0; --i) {
        if (w[(size_t)i].is_zero()) ++n;
        else break;
    }

    // Entire word is zeros: Hlog[., {0,...,0}] = (log arg)^n/n!, which
    // the regularizer throws away -> empty wordlist.
    if (n == len) return Wordlist{};

    // No trailing zeros: unchanged.
    if (n == 0) {
        Wordlist out;
        out.terms.push_back({rat_one(ctx), w});
        return out;
    }

    // Non-empty trailing run.  Shuffle-regularize:
    //   word = prefix + anchor + [0,...,0 (n times)]
    // -> for each interleaving of n zeros with prefix (signed (-1)^n),
    //    append the anchor letter.
    Wordlist zeros_wl;
    {
        Word zeros;
        zeros.letters = std::vector<Letter>((size_t)n,
                                             Rat::zero_of(ctx));
        zeros_wl.terms.push_back({neg_one_pow(ctx, n), zeros});
    }
    Wordlist prefix_wl;
    {
        Word prefix;
        prefix.letters.assign(w.letters.begin(),
                              w.letters.begin() + (len - n - 1));
        prefix_wl.terms.push_back({rat_one(ctx), prefix});
    }
    Wordlist sh = shuffle_product(zeros_wl, prefix_wl);

    const Letter& anchor = w[(size_t)(len - n - 1)];
    Wordlist result;
    for (const auto& t : sh.terms) {
        Word nw = t.word;
        nw.letters.push_back(anchor);
        result.terms.push_back({t.coef, std::move(nw)});
    }
    return result;
}

Wordlist reg0(const Wordlist& wl) {
    Wordlist result;
    for (const auto& t : wl.terms) {
        // Use the term's coef ctx; it always exists. This avoids the
        // dangling-stack-ctx bug when t.word is empty.
        Wordlist sub = regzero_word_in_ctx(t.coef.ctx(), t.word);
        for (const auto& s : sub.terms) {
            result.terms.push_back({s.coef * t.coef, s.word});
        }
    }
    return collect_words(result);
}

// Helper for reg_head / reg_tail: emit one summand of the regularized
// expansion into `res`, given the index ii and the direction (head vs
// tail controls how we lay out the ConcatMul).
//
// For reg_tail:   (shuffle {(-1)^ii, [letter]*ii} × {coef, prefix})
//                 ·_concat· {prefac, [anchor]}
// For reg_head:   {prefac, [anchor]}
//                 ·_concat· (shuffle {(-1)^ii, [letter]*ii} × {coef, suffix})
namespace {

enum class RegSide { kHead, kTail };

void emit_partial_regularization(Wordlist& res,
                                 RegSide side,
                                 const Letter& letter,
                                 const Letter& substitute,
                                 long n,
                                 long ii,
                                 const Letter& anchor,
                                 const Rat& term_coef,
                                 const Word& stripped,
                                 const PolyCtx& ctx) {
    Rat prefac = (ii == n) ? rat_one(ctx)
                           : substitute.pow(n - ii)
                             / factorial_rat(ctx, n - ii);
    Rat sign = neg_one_pow(ctx, ii);

    // shuffle ((-1)^ii, [letter]*ii)  with  (coef, stripped)
    Wordlist A;
    {
        Word ls;
        ls.letters = std::vector<Letter>((size_t)ii, letter);
        A.terms.push_back({sign, ls});
    }
    Wordlist B;
    B.terms.push_back({term_coef, stripped});
    Wordlist AB = shuffle_product(A, B);

    // (prefac, [anchor])
    Wordlist C;
    {
        Word aw;
        aw.letters.push_back(anchor);
        C.terms.push_back({prefac, aw});
    }

    Wordlist combined = (side == RegSide::kTail)
        ? concat_mul(AB, C)
        : concat_mul(C, AB);

    for (const auto& t : combined.terms) res.terms.push_back(t);
}

}  // namespace

Wordlist reg_tail(const Wordlist& wl,
                  const Letter& letter,
                  const Letter& substitute) {
    Wordlist res;
    for (const auto& w : wl.terms) {
        const Word& wrd = w.word;
        const long wlen = (long)wrd.size();
        if (wlen == 0) { res.terms.push_back(w); continue; }
        const PolyCtx& ctx = wrd[0].ctx();

        // Count trailing `letter` letters.
        long n = 0;
        for (long i = wlen - 1; i >= 0; --i) {
            if (wrd[(size_t)i].equal(letter)) ++n;
            else break;
        }

        if (n == wlen) {
            // Entire word is `letter`: collapse to (coef * substitute^n/n!, {}).
            Rat prefac = (n == 0) ? rat_one(ctx)
                                  : substitute.pow(n) / factorial_rat(ctx, n);
            res.terms.push_back({w.coef * prefac, Word{}});
            continue;
        }

        const Letter& anchor = wrd[(size_t)(wlen - n - 1)];
        Word stripped;
        stripped.letters.assign(wrd.letters.begin(),
                                wrd.letters.begin() + (wlen - n - 1));

        for (long ii = 0; ii <= n; ++ii) {
            emit_partial_regularization(res, RegSide::kTail,
                                        letter, substitute,
                                        n, ii, anchor,
                                        w.coef, stripped, ctx);
        }
    }
    return collect_words(res);
}

Wordlist reg_head(const Wordlist& wl,
                  const Letter& letter,
                  const Letter& substitute) {
    Wordlist res;
    for (const auto& w : wl.terms) {
        const Word& wrd = w.word;
        const long wlen = (long)wrd.size();
        if (wlen == 0) { res.terms.push_back(w); continue; }
        const PolyCtx& ctx = wrd[0].ctx();

        // Count leading `letter` letters.
        long n = 0;
        for (long i = 0; i < wlen; ++i) {
            if (wrd[(size_t)i].equal(letter)) ++n;
            else break;
        }

        if (n == wlen) {
            Rat prefac = (n == 0) ? rat_one(ctx)
                                  : substitute.pow(n) / factorial_rat(ctx, n);
            res.terms.push_back({w.coef * prefac, Word{}});
            continue;
        }

        const Letter& anchor = wrd[(size_t)n];
        Word stripped;
        stripped.letters.assign(wrd.letters.begin() + (n + 1),
                                wrd.letters.end());

        for (long ii = 0; ii <= n; ++ii) {
            emit_partial_regularization(res, RegSide::kHead,
                                        letter, substitute,
                                        n, ii, anchor,
                                        w.coef, stripped, ctx);
        }
    }
    return collect_words(res);
}

}  // namespace hyperflint
