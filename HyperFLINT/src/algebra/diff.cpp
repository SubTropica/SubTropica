// Differentiation implementation.  Mirrors HyperIntica.wl:3949-3982.

#include "hyperflint/algebra/diff.hpp"

#include <algorithm>
#include <stdexcept>

namespace hyperflint {

namespace {

// Build Hlog[z, subword] symbol, dropping the (n=0) empty-word case
// into an implicit 1 by the caller -- we track a coef and return
// either an HlogTerm with the symbol or absorb the 1 into the coef.
HlogTerm with_word(const Rat& coef, const Rat& z, const Word& word) {
    return HlogTerm{coef, Hlog{z, word}};
}

// Remove index i (0-based) from a word, returning the shorter word.
Word delete_at(const Word& w, size_t i) {
    Word out;
    out.letters.reserve(w.size() - 1);
    for (size_t k = 0; k < w.size(); ++k) if (k != i) out.letters.push_back(w[k]);
    return out;
}

// Simple addition of a {coef * Hlog[z, wrd]} into an accumulator.
// The empty-word case produces a coefficient-only contribution
// (HyperIntica's Hlog[_, {}] -> 1), which we encode as
// Hlog{z, word = empty} and leave to the caller's flattening.
void add_hlog(HlogList& out, const Rat& coef, const Rat& z, const Word& wrd) {
    if (coef.is_zero()) return;
    out.push_back(with_word(coef, z, wrd));
}

// Derivative of each letter in a word, returned as a vector parallel
// to `word`. The i-th entry is D[letter_i, var].
std::vector<Rat> diff_word_letters(const Word& word, size_t var_idx) {
    std::vector<Rat> out;
    out.reserve(word.size());
    for (const auto& l : word.letters) {
        out.push_back(l.derivative(var_idx));
    }
    return out;
}

}  // namespace

HlogList diff_hlog(const Rat& z, const Word& word, size_t var_idx) {
    HlogList out;
    const size_t n = word.size();
    if (n == 0) return out;

    const PolyCtx& ctx = z.ctx();
    Rat zero_rat{Poly::zero_of(ctx)};

    // Precompute letter derivatives.
    std::vector<Rat> dwords = diff_word_letters(word, var_idx);
    Rat dz = z.derivative(var_idx);

    // (1) Contribution from the upper limit derivative:
    //        D[z, var] / (z - w[0]) * Hlog[z, rest(word)]
    {
        Rat denom = z - word[0];
        if (!denom.is_zero() && !dz.is_zero()) {
            Rat coef = dz / denom;
            Word rest;
            rest.letters.assign(word.letters.begin() + 1, word.letters.end());
            add_hlog(out, coef, z, rest);
        }
    }

    // (2) Contributions from differences of neighboring letters:
    //        D[Log[w[i] - w[i+1]], var] * (
    //          Hlog[z, delete(word, i+1)] - Hlog[z, delete(word, i)] )
    //        = (d(w[i] - w[i+1])/dvar) / (w[i] - w[i+1]) * ( ... )
    for (size_t i = 0; i + 1 < n; ++i) {
        Rat f = word[i] - word[i + 1];
        if (f.is_zero()) continue;
        Rat df = dwords[i] - dwords[i + 1];
        if (df.is_zero()) continue;
        Rat coef = df / f;
        Word w_no_i_plus_1 = delete_at(word, i + 1);
        Word w_no_i        = delete_at(word, i);
        add_hlog(out,  coef, z, w_no_i_plus_1);
        add_hlog(out, -coef, z, w_no_i);
    }

    // (3) Contribution from last letter (if nonzero):
    //        - D[Log[w[n-1]], var] * Hlog[z, most(word)]
    if (!word[n - 1].is_zero()) {
        Rat dlast = dwords[n - 1];
        if (!dlast.is_zero()) {
            Rat coef = dlast / word[n - 1];
            Word most_w;
            most_w.letters.assign(word.letters.begin(),
                                   word.letters.begin() + (n - 1));
            add_hlog(out, -coef, z, most_w);
        }
    }

    // (4) Contribution from first letter (arg != Infinity assumed here):
    //        - D[w[0], var] / (z - w[0]) * Hlog[z, rest(word)]
    {
        Rat denom = z - word[0];
        Rat dfirst = dwords[0];
        if (!denom.is_zero() && !dfirst.is_zero()) {
            Rat coef = dfirst / denom;
            Word rest;
            rest.letters.assign(word.letters.begin() + 1, word.letters.end());
            add_hlog(out, -coef, z, rest);
        }
    }

    return out;
}

// Lightweight DiffMpl port. HyperIntica.wl:3985.
//
// For each index j with dz_j = D[z_j, var] != 0, build a term. The
// branch depends on (ns[j], length of ns, position j):
//   - ns[j] > 1             : Mpl[ns with j-th decremented, zs] / z_j
//   - ns == {1}             : 1/(1 - z_1)
//   - j == last && ns[j]==1 : Mpl[most(ns), most(zs) with last elem replaced by zs[-2]*zs[-1]] / (1 - zs[n-1])
//   - j == 0    && ns[j]==1 : Mpl[rest(ns), rest(zs) with first elem replaced by zs[0]*zs[1]] / (zs[0]*(zs[0]-1))
//                             - Mpl[rest(ns), rest(zs)] / (zs[0] - 1)
//   - middle    && ns[j]==1 : Mpl[delete(ns,j), delete(zs,j) with elem j replaced by zs[j]*zs[j+1]]
//                              / (zs[j]*(zs[j]-1))
//                             - Mpl[delete(ns,j), delete(zs,j)] / (zs[j] - 1)
MplList diff_mpl(const std::vector<long>& ns,
                 const std::vector<Rat>& zs,
                 size_t var_idx) {
    MplList out;
    const size_t n = ns.size();
    if (n == 0) return out;
    const PolyCtx& ctx = zs[0].ctx();

    auto one_minus = [&](const Rat& x) {
        return Rat::one_of(ctx) - x;
    };

    for (size_t j = 0; j < n; ++j) {
        Rat dzj = zs[j].derivative(var_idx);
        if (dzj.is_zero()) continue;

        if (ns[j] > 1) {
            // Mpl[ns with ns[j]->ns[j]-1, zs] / zs[j]
            std::vector<long> ns2 = ns;
            ns2[j] -= 1;
            Rat coef = dzj / zs[j];
            out.push_back(MplTerm{coef, Mpl{ns2, zs}});
            continue;
        }
        if (ns[j] < 1) {
            throw std::runtime_error(
                "diff_mpl: non-positive integer index not supported");
        }

        // ns[j] == 1 branch
        if (n == 1) {
            // ns == {1}: derivative is dz_1 / (1 - z_1), no Mpl factor.
            // Represent as "const-Mpl" with indices {} and args {} and
            // coef = dzj / (1 - zs[0]).  Our Mpl struct supports empty.
            Rat coef = dzj / one_minus(zs[0]);
            out.push_back(MplTerm{coef, Mpl{std::vector<long>{}, std::vector<Rat>{}}});
            continue;
        }

        if (j == n - 1) {
            // last: Mpl[most(ns), most(zs) with last -> zs[-2]*zs[-1]] / (1 - zs[n-1])
            std::vector<long> ns2(ns.begin(), ns.end() - 1);
            std::vector<Rat>  zs2(zs.begin(),  zs.end()  - 1);
            zs2.back() = zs[n - 2] * zs[n - 1];
            Rat coef = dzj / one_minus(zs[n - 1]);
            out.push_back(MplTerm{coef, Mpl{ns2, zs2}});
            continue;
        }

        if (j == 0) {
            // first: Mpl[rest(ns), rest(zs) with first -> zs[0]*zs[1]] / (z0*(z0-1))
            //      - Mpl[rest(ns), rest(zs)] / (z0 - 1)
            std::vector<long> ns2(ns.begin() + 1, ns.end());
            std::vector<Rat>  zs2(zs.begin()  + 1, zs.end());
            std::vector<Rat>  zs3 = zs2;
            zs3.front() = zs[0] * zs[1];
            Rat d1 = dzj / (zs[0] * (zs[0] - Rat::one_of(ctx)));
            Rat d2 = dzj / (zs[0] - Rat::one_of(ctx));
            out.push_back(MplTerm{ d1, Mpl{ns2, zs3}});
            out.push_back(MplTerm{-d2, Mpl{ns2, zs2}});
            continue;
        }

        // middle, ns[j] == 1.  Mirror of HyperIntica.wl:4010-4011.
        // The first Mpl merges zs[j]*zs[j+1] into position j of the
        // j-deleted list.  The second Mpl merges zs[j-1]*zs[j] into
        // position j-1 of the j-deleted list.
        {
            std::vector<long> ns2; ns2.reserve(n - 1);
            for (size_t k = 0; k < n; ++k) if (k != j) ns2.push_back(ns[k]);
            std::vector<Rat>  zs2; zs2.reserve(n - 1);
            for (size_t k = 0; k < n; ++k) if (k != j) zs2.push_back(zs[k]);
            // After deleting index j, the entries at original positions
            // (j-1, j+1) become (j-1, j) in the shortened list.
            std::vector<Rat>  zs3 = zs2;     // first Mpl: merge right
            zs3[j] = zs[j] * zs[j + 1];
            std::vector<Rat>  zs4 = zs2;     // second Mpl: merge left
            zs4[j - 1] = zs[j - 1] * zs[j];
            Rat d1 = dzj / (zs[j] * (zs[j] - Rat::one_of(ctx)));
            Rat d2 = dzj / (zs[j] - Rat::one_of(ctx));
            out.push_back(MplTerm{ d1, Mpl{ns2, zs3}});
            out.push_back(MplTerm{-d2, Mpl{ns2, zs4}});
        }
    }

    return out;
}

}  // namespace hyperflint
