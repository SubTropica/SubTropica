// Word-level regularization primitives.
//
// These are the wordlist combinators used by TransformWord /
// TransformShuffle to express "the Hlog's head/tail letter is
// pathological and must be absorbed via shuffle regularization".
// All operate purely at the word-algebra level (no period evaluation).
//
//   regzero_word(word)
//     Shuffle-regularize a single Word whose trailing letters are all
//     `0` (the only "forbidden" final letter for Hlog; Hlog[., {..,0}]
//     is logarithmically divergent and must be exchanged for a
//     shuffle product).  Mirrors HyperIntica.wl:2484.
//
//   reg0(wordlist)
//     Apply regzero_word to every Word in a Wordlist and CollectWords
//     the result.  Mirrors HyperIntica.wl:2472.
//
//   reg_head(wordlist, letter, substitute)
//     Strip any leading run of `letter` from each Word via shuffle
//     regularization, replacing it with `substitute` (default 0).
//     Mirrors HyperIntica.wl:2522.
//
//   reg_tail(wordlist, letter, substitute)
//     Symmetric to reg_head, operating on the trailing run.
//     Mirrors HyperIntica.wl:2499.
//
// Implementation notes:
// * regzero_word's "trailing zeros" predicate is letter.is_zero() on
//   the canonical Rat form.
// * reg_head / reg_tail take the "letter to strip" as a Rat; the
//   predicate is equality under Rat::equal.

#pragma once

#include "hyperflint/core/rat.hpp"
#include "hyperflint/symbols/word.hpp"

namespace hyperflint {

// Note: the Word-only overload returns an empty wordlist for an empty
// input word, since the "1" coefficient needs a PolyCtx that the empty
// word cannot supply. Callers that handle the empty-word identity case
// must use the (ctx, w) overload.
Wordlist regzero_word(const Word& w);
Wordlist regzero_word_in_ctx(const PolyCtx& ctx, const Word& w);
Wordlist reg0(const Wordlist& wl);

Wordlist reg_head(const Wordlist& wl,
                  const Letter& letter,
                  const Letter& substitute);
Wordlist reg_tail(const Wordlist& wl,
                  const Letter& letter,
                  const Letter& substitute);

// ShuffleSymbolic lives with TransformShuffle in Phase 5c, where the
// Multiword regulator type it operates on is defined.

}  // namespace hyperflint
