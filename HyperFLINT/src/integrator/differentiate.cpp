// Phase 5b: DifferentiateWordlist — mirrors HyperIntica.wl:5499.

#include "hyperflint/integrator/differentiate.hpp"

#include "hyperflint/algebra/shuffle.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"

namespace hyperflint {

Wordlist differentiate_wordlist(const Wordlist& wl, size_t var_idx) {
    Wordlist out;
    if (wl.terms.empty()) return out;

    const PolyCtx& ctx = wl.terms.front().coef.ctx();
    // "var" as a Rat (the integration/anchor variable).  Used to build
    //   1 / (var - word[0])
    // for the Hlog chain-rule contribution.
    Rat var_rat{Poly::gen(ctx, var_idx)};

    for (const auto& term : wl.terms) {
        const Rat&  coef = term.coef;
        const Word& word = term.word;

        // (1) D[coef, var] * Hlog[var, word].
        Rat dcoef = coef.derivative(var_idx);
        if (!dcoef.is_zero()) {
            out.terms.push_back(WordlistTerm{std::move(dcoef), word});
        }

        // (2) coef / (var - word[0]) * Hlog[var, rest(word)]. Skipped
        //     when the word is empty (Hlog[var,{}] = 1 is already
        //     handled by step 1 via D[coef, var]).
        if (word.empty()) continue;

        Rat denom = var_rat - word[0];
        // Guard: if var - a reduces to zero (pathological: word[0] = var
        // itself, which HyperIntica would not produce), skip — the
        // corresponding Hlog is ill-defined.
        if (denom.is_zero()) continue;

        Rat chain_coef = coef / denom;
        Word rest;
        rest.letters.assign(word.letters.begin() + 1, word.letters.end());
        out.terms.push_back(WordlistTerm{std::move(chain_coef),
                                          std::move(rest)});
    }

    return collect_words(out);
}

}  // namespace hyperflint
