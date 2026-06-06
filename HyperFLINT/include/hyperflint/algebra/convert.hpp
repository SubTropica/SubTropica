// Endpoint / rescaling conversions on Wordlists.
//
//   convert_zero_one(wl)        -- translates letters from the
//     {-1, 0, +x}  alphabet (used for integration on [0, infinity))
//     to the {0, 1}  alphabet (used on [0, 1]), via the substitution
//     z -> z / (1 + z).  Exactly mirrors HyperIntica.wl:3494.
//
//   convert_1inf_to_01(wl)      -- converts integrations from [1, infinity]
//     to [0, 1] via z -> 1/z (plus word reversal).  Mirrors
//     HyperIntica.wl:3514.
//
// Both take and return Wordlists over the same PolyCtx as the input's
// Rat letters.

#pragma once

#include "hyperflint/symbols/word.hpp"

namespace hyperflint {

Wordlist convert_zero_one(const Wordlist& wl);
Wordlist convert_1inf_to_01(const Wordlist& wl);

// Port of HyperIntica.wl:3470 (ConvertABtoZeroInf). Convert an integral
// from [A, B] to [0, infinity). Each letter `w_i` expands as:
//   letter == B : (-coef, word ++ [-1])
//   otherwise   : (-coef, word ++ [-1])
//                 and
//                 ( coef, word ++ [(letter - A)/(B - letter)])
// Result is collect_words-normalized.
Wordlist convert_ab_to_zero_inf(const PolyCtx& ctx,
                                 const Wordlist& wl,
                                 const Rat& A,
                                 const Rat& B);

}  // namespace hyperflint
