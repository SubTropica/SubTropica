// Phase 5b: DifferentiateWordlist.
//
// A Wordlist {{coef_i, word_i}} is HyperIntica's symbolic-algebra form
// of
//     sum_i coef_i * Hlog[var, word_i].
//
// The anchor is implicit: it is the same variable the caller plans to
// differentiate against. So
//
//     d/dvar sum_i coef_i * Hlog[var, word_i]
//         = sum_i ( D[coef_i, var] * Hlog[var, word_i]
//                  + coef_i / (var - word_i[0]) * Hlog[var, rest(word_i)] )
//
// where a zero-length word contributes only the D[coef, var] piece
// (Hlog[var, {}] = 1).
//
// This matches HyperIntica.wl:5499 exactly.  IntegrateII (Phase 5d)
// is the inverse:
//     differentiate_wordlist(integrate_ii(wl, x), x) == wl
// after collect_words.
//
// Contract:
//   - Letters are Rat (Phase 3 convention; Phase 7 widens to
//     AlgebraicLetter but DifferentiateWordlist does not need to
//     change — the quotient  coef / (var - a)  works for any Letter
//     that supports Rat arithmetic).
//   - Coefficients are Rat in the same PolyCtx as the letters.
//   - The result is collect_words-normalized.

#pragma once

#include "hyperflint/symbols/word.hpp"

#include <cstddef>

namespace hyperflint {

Wordlist differentiate_wordlist(const Wordlist& wl, size_t var_idx);

}  // namespace hyperflint
