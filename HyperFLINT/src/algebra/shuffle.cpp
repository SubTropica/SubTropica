// Shuffle algebra implementation.

#include "hyperflint/algebra/shuffle.hpp"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace hyperflint {

namespace {

// Recursive shuffle. No memoization for now -- worst case blows up as
// C(m+n, m); caller should keep words short. Mirrors
// HyperIntica.wl:2349-2357.
Wordlist shuffle_words_impl(const Word& v, const Word& w) {
    Wordlist out;
    if (v.empty()) {
        if (w.empty()) {
            // sh(empty, empty) = [empty word] with coefficient 1
            // so that the unit of the shuffle algebra is preserved.
            // Use the ambient ctx from the first non-empty letter; in
            // the all-empty case the caller must seed.
            return out;  // caller sees empty -- shuffle_words handles
        }
        out.terms.push_back(
            WordlistTerm{Rat::one_of(w[0].ctx()), w});
        return out;
    }
    if (w.empty()) {
        out.terms.push_back(
            WordlistTerm{Rat::one_of(v[0].ctx()), v});
        return out;
    }
    // sh(v, w) = v[0] :: sh(tail(v), w)  +  w[0] :: sh(v, tail(w))
    Word vt; vt.letters.assign(v.letters.begin() + 1, v.letters.end());
    Word wt; wt.letters.assign(w.letters.begin() + 1, w.letters.end());
    Wordlist left  = shuffle_words_impl(vt, w);
    Wordlist right = shuffle_words_impl(v, wt);
    out.terms.reserve(left.terms.size() + right.terms.size());
    for (auto& t : left.terms) {
        Word nw;
        nw.letters.reserve(t.word.size() + 1);
        nw.letters.push_back(v[0]);
        for (auto& l : t.word.letters) nw.letters.push_back(l);
        out.terms.push_back(WordlistTerm{t.coef, std::move(nw)});
    }
    for (auto& t : right.terms) {
        Word nw;
        nw.letters.reserve(t.word.size() + 1);
        nw.letters.push_back(w[0]);
        for (auto& l : t.word.letters) nw.letters.push_back(l);
        out.terms.push_back(WordlistTerm{t.coef, std::move(nw)});
    }
    return out;
}

}  // namespace

Wordlist shuffle_words(const Word& v, const Word& w) {
    // Edge case: both empty -> one copy of the empty word with coef 1.
    // We need a PolyCtx to build the "1" coef, but if both words are
    // empty we have none. Caller avoids this case; if it happens we
    // return an empty wordlist (the convention used below).
    if (v.empty() && w.empty()) {
        return Wordlist{};
    }
    return collect_words(shuffle_words_impl(v, w));
}

Wordlist shuffle_product(const Wordlist& a, const Wordlist& b) {
    Wordlist out;
    for (auto& ti : a.terms) {
        for (auto& tj : b.terms) {
            if (ti.word.empty() && tj.word.empty()) {
                // Empty-empty shuffle is the unit: a single empty-word
                // term with the product of the coefficients. shuffle_words
                // can't synthesize the unit "1" coef without a ctx; we
                // inject it here at the wordlist level (where ti.coef
                // already carries the ctx).
                out.terms.push_back(
                    WordlistTerm{ti.coef * tj.coef, Word{}});
                continue;
            }
            Wordlist sh = shuffle_words(ti.word, tj.word);
            Rat c = ti.coef * tj.coef;
            for (auto& t : sh.terms) {
                out.terms.push_back(
                    WordlistTerm{c * t.coef, std::move(t.word)});
            }
        }
    }
    return collect_words(out);
}

Wordlist concat_mul(const Wordlist& a, const Wordlist& b) {
    Wordlist out;
    for (auto& ti : a.terms) {
        for (auto& tj : b.terms) {
            Word joined;
            joined.letters.reserve(ti.word.size() + tj.word.size());
            for (auto& l : ti.word.letters) joined.letters.push_back(l);
            for (auto& l : tj.word.letters) joined.letters.push_back(l);
            out.terms.push_back(
                WordlistTerm{ti.coef * tj.coef, std::move(joined)});
        }
    }
    return collect_words(out);
}

Wordlist scalar_mul_wordlist(const Wordlist& wl, const Rat& s) {
    Wordlist out;
    out.terms.reserve(wl.terms.size());
    for (const auto& t : wl.terms) {
        out.terms.push_back(WordlistTerm{t.coef * s, t.word});
    }
    return out;
}

Wordlist collect_words(const Wordlist& wl) {
    // Preserve first-occurrence order: use a vector of keys + a map.
    std::vector<std::string> order;
    std::unordered_map<std::string, size_t> idx;
    std::vector<WordlistTerm> kept;

    for (const auto& t : wl.terms) {
        std::string k = t.word.content_key();
        auto it = idx.find(k);
        if (it == idx.end()) {
            idx[k] = kept.size();
            kept.push_back(t);
            order.push_back(k);
        } else {
            kept[it->second].coef = kept[it->second].coef + t.coef;
        }
    }
    Wordlist out;
    for (auto& t : kept) {
        if (!t.coef.is_zero()) out.terms.push_back(std::move(t));
    }
    return out;
}

Word reg_head(const Word& w, const std::string& reg_letter) {
    size_t i = 0;
    while (i < w.size() && w[i].to_string() == reg_letter) ++i;
    Word out;
    out.letters.assign(w.letters.begin() + i, w.letters.end());
    return out;
}

Word reg_tail(const Word& w, const std::string& reg_letter) {
    size_t n = w.size();
    while (n > 0 && w[n - 1].to_string() == reg_letter) --n;
    Word out;
    out.letters.assign(w.letters.begin(), w.letters.begin() + n);
    return out;
}

}  // namespace hyperflint
