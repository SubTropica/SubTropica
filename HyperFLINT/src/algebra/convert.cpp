// Endpoint-conversion implementations, mirroring HyperIntica.wl.

#include "hyperflint/algebra/convert.hpp"
#include "hyperflint/algebra/shuffle.hpp"

#include <sstream>
#include <stdexcept>

namespace hyperflint {

namespace {

// Local Rat helpers that read the letter "-1" / "0" / "1" structurally.
bool letter_equals(const Rat& l, const std::string& s) {
    return l.to_string() == s;
}

Rat make_rat(const PolyCtx& ctx, const std::string& expr) {
    return Rat::parse(ctx, expr);
}

// Prepend a letter to every word in v, returning new wordlist.
// prepend == "left" convention; concat_mul with a = [{1, {l}}] emulates it.
Wordlist prepend_letter(const Wordlist& v, const Letter& l) {
    Wordlist out;
    out.terms.reserve(v.size());
    for (auto& t : v.terms) {
        Word nw;
        nw.letters.reserve(t.word.size() + 1);
        nw.letters.push_back(l);
        for (auto& ll : t.word.letters) nw.letters.push_back(ll);
        out.terms.push_back(WordlistTerm{t.coef, std::move(nw)});
    }
    return out;
}

// Append a letter to every word in v.
Wordlist append_letter(const Wordlist& v, const Letter& l) {
    Wordlist out;
    out.terms.reserve(v.size());
    for (auto& t : v.terms) {
        Word nw = t.word;
        nw.letters.push_back(l);
        out.terms.push_back(WordlistTerm{t.coef, std::move(nw)});
    }
    return out;
}

// Negate coefs of every term (coef -> -coef), returning new wordlist.
Wordlist negate_coefs(const Wordlist& v) {
    Wordlist out;
    out.terms.reserve(v.size());
    for (auto& t : v.terms) {
        out.terms.push_back(WordlistTerm{-t.coef, t.word});
    }
    return out;
}

// Concatenate two wordlists by union (NOT the shuffle-algebra concat).
Wordlist wl_union(const Wordlist& a, const Wordlist& b) {
    Wordlist out = a;
    out.terms.insert(out.terms.end(), b.terms.begin(), b.terms.end());
    return out;
}

}  // namespace

Wordlist convert_zero_one(const Wordlist& wl) {
    if (wl.empty()) return wl;
    const PolyCtx& ctx = wl.terms.front().coef.ctx();
    Letter zero = Rat::zero_of(ctx);

    Wordlist out;
    for (const auto& term : wl.terms) {
        // Mirror the Mma inner loop: maintain v as a growing wordlist
        // of (coef, partial_word).  Start with the input coefficient
        // and empty accumulated word.
        Wordlist v;
        v.terms.push_back(WordlistTerm{term.coef, Word{}});
        for (const auto& wi : term.word.letters) {
            if (letter_equals(wi, "-1")) {
                // v <- prepend(0, v)
                v = prepend_letter(v, zero);
            } else {
                // v <- concat_mul([{1,{0}},{-1,{1/(1+wi)}}], v)
                // Equivalent to: (prepend 0 to v)  +  (negate-coef and
                // prepend 1/(1+wi) to v).
                Rat one_over = Rat::one_of(ctx) /
                              (Rat::one_of(ctx) + wi);
                Letter l_new = one_over;
                Wordlist left  = prepend_letter(v, zero);
                Wordlist right = negate_coefs(prepend_letter(v, l_new));
                v = wl_union(left, right);
            }
        }
        // Accumulate.
        out.terms.insert(out.terms.end(), v.terms.begin(), v.terms.end());
    }
    return collect_words(out);
}

Wordlist convert_1inf_to_01(const Wordlist& wl) {
    if (wl.empty()) return wl;
    const PolyCtx& ctx = wl.terms.front().coef.ctx();
    Letter zero = Rat::zero_of(ctx);

    Wordlist out;
    for (const auto& term : wl.terms) {
        Wordlist v;
        v.terms.push_back(WordlistTerm{term.coef, Word{}});
        for (const auto& wi : term.word.letters) {
            if (letter_equals(wi, "0")) {
                // v <- prepend 0 to v (structural -- the Mma code does
                // Prepend on the WORD; here Prepend means prepend via
                // the inner accumulation).
                v = prepend_letter(v, zero);
            } else {
                // v <- split: (prepend 0, v) union (negate, prepend 1/wi, v)
                Letter l_new = Rat::one_of(ctx) / wi;
                Wordlist left  = prepend_letter(v, zero);
                Wordlist right = negate_coefs(prepend_letter(v, l_new));
                v = wl_union(left, right);
            }
        }
        out.terms.insert(out.terms.end(), v.terms.begin(), v.terms.end());
    }
    return collect_words(out);
}

Wordlist convert_ab_to_zero_inf(const PolyCtx& ctx,
                                 const Wordlist& wl,
                                 const Rat& A,
                                 const Rat& B) {
    Wordlist out;
    Letter minus_one{Poly::from_int(ctx, -1)};
    for (const auto& term : wl.terms) {
        // Local accumulator starts with {(coef, [])}. Mma builds via
        // Append rather than Prepend, so we mirror that (we build v
        // as a list of (coef, word) where word is the "built-up" tail).
        Wordlist v;
        v.terms.push_back(WordlistTerm{term.coef, Word{}});

        for (const auto& letter : term.word.letters) {
            Wordlist next;
            Rat diff_B_letter = B - letter;
            bool letter_is_B = diff_B_letter.is_zero();
            for (const auto& entry : v.terms) {
                if (letter_is_B) {
                    // letter == B: (-coef, word ++ [-1])
                    Word appended = entry.word;
                    appended.letters.push_back(minus_one);
                    next.terms.push_back(
                        WordlistTerm{-entry.coef, std::move(appended)});
                } else {
                    // two branches
                    Word app1 = entry.word;
                    app1.letters.push_back(minus_one);
                    next.terms.push_back(
                        WordlistTerm{-entry.coef, std::move(app1)});

                    Rat new_letter = (letter - A) / diff_B_letter;
                    Word app2 = entry.word;
                    app2.letters.push_back(new_letter);
                    next.terms.push_back(
                        WordlistTerm{entry.coef, std::move(app2)});
                }
            }
            v = std::move(next);
        }

        for (auto& t : v.terms) out.terms.push_back(std::move(t));
    }
    return collect_words(out);
}

}  // namespace hyperflint
