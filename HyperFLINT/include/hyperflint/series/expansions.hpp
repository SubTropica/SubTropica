// Word-level series expansions at 0 and at infinity.
//
//   ExpandZeroWord(word, minOrder)
//     Formal series of Hlog[arg, word] at arg -> 0 up to O(arg^minOrder).
//
//   ExpandInfWord(word, minOrder)
//     Formal series of Hlog[arg, word] at arg -> infinity up to O(1/arg^minOrder).
//
// Returns a "SeriesTable" table[k][m] holding the coefficient of
// Log[arg]^k * arg^m.  The table is ragged: intermediate log powers
// with no coefficients yet are empty inner vectors; trailing empty
// log rows are dropped.
//
// Mirrors HyperIntica.wl:2391 (ExpandZeroWord) and 2437 (ExpandInfWord).
// The recursion is the standard Panzer / Brown formula: integrate each
// contribution of the tail expansion against dt/(t - word[0]).

#pragma once

#include "hyperflint/core/rat.hpp"
#include "hyperflint/symbols/word.hpp"

#include <vector>

namespace hyperflint {

using SeriesTable = std::vector<std::vector<Rat>>;

// Expand Hlog[arg, word] around arg = 0 up to polynomial order
// `min_order` (inclusive). min_order < 0 returns an empty table.
//
// The Word-only overload returns an EMPTY table for an empty input
// word, since the constant "1" term needs a PolyCtx the empty word
// cannot supply. Callers that handle the empty-word identity case must
// use the (ctx, word, min_order) overload.
SeriesTable expand_zero_word(const Word& word, long min_order);
SeriesTable expand_zero_word_in_ctx(const PolyCtx& ctx,
                                     const Word& word, long min_order);

// Expand Hlog[arg, word] around arg = Infinity up to 1/arg^min_order.
SeriesTable expand_inf_word(const Word& word, long min_order);
SeriesTable expand_inf_word_in_ctx(const PolyCtx& ctx,
                                    const Word& word, long min_order);

}  // namespace hyperflint
