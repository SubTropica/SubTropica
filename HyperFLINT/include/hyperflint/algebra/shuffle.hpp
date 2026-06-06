// Shuffle algebra on Words and Wordlists.
//
// Mirrors HyperIntica.wl:2349-2378 (ShuffleWords, ShuffleProduct,
// ConcatMul) and the CollectWords companion.
//
// The shuffle product of two words v, w is the sum of all interleavings
// that preserve the relative order within each:
//   sh(v, w) = sum over shuffles s of (v,w)  1 * s
// Recurrence:
//   sh({}, w) = [w]
//   sh(v, {}) = [v]
//   sh(v, w)  = v[0] :: sh(v[1:], w)  +  w[0] :: sh(v, w[1:])

#pragma once

#include "hyperflint/symbols/word.hpp"

namespace hyperflint {

// Shuffle two plain Words. Returns a unit-coefficient Wordlist.
Wordlist shuffle_words(const Word& v, const Word& w);

// Shuffle two Wordlists (Q-linear combinations of Words), collecting
// duplicate Words by summing their coefficients.
Wordlist shuffle_product(const Wordlist& a, const Wordlist& b);

// Concatenation of two Wordlists: for each pair of terms, join words
// and multiply coefs. Collect duplicates.
Wordlist concat_mul(const Wordlist& a, const Wordlist& b);

// Merge identical words in a Wordlist, summing their coefs, dropping
// terms with zero coef. Preserves insertion order of first occurrence.
Wordlist collect_words(const Wordlist& wl);

// Scale every term's coef by `s`. Returns a fresh Wordlist.
Wordlist scalar_mul_wordlist(const Wordlist& wl, const Rat& s);

// Drop leading / trailing "regularizer" letters. HyperIntica
// regularizes Hlog by stripping leading/trailing zero letters;
// parameterize the "what is a regularizer" predicate via an
// explicit letter string. The default reg_letter is "0".
Word reg_head(const Word& w, const std::string& reg_letter = "0");
Word reg_tail(const Word& w, const std::string& reg_letter = "0");

}  // namespace hyperflint
