// Word and Wordlist: the symbolic-algebra layer under Hlog.
//
// A Word is an ordered list of letters. A letter is whatever a
// hyperlogarithm's singularity can be: rational function of the
// integration variables, "0", "1", or (Phase 7) an algebraic letter
// Wm[i]/Wp[i]. In Phase 3 we model letters as Rat; Phase 7 will
// widen to a variant.
//
// A Wordlist is a Q-rational-function linear combination of Words:
// list of (coef: Rat, word: Word). The coefficient ring is the same
// ambient Rat used everywhere else.

#pragma once

#include "hyperflint/algebra/poly_struct_hash.hpp"   // 2026-04-26 a-prime
#include "hyperflint/core/rat.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace hyperflint {

using Letter = Rat;

struct Word {
    std::vector<Letter> letters;
    // 2026-04-26 (a-prime lever): memoized 128-bit structural hash.
    // Mutable so `struct_hash()` can lazy-compute on a const Word.
    // Aggregate-init `Word{vec}` continues to compile because C++14+
    // default-member-initializers fill these fields automatically.
    // CALLER CONTRACT: do not mutate `letters` (e.g. via the non-const
    // operator[]) after the first `struct_hash()` call on this Word.
    mutable std::pair<uint64_t, uint64_t> cached_hash_{0, 0};
    mutable bool hash_cached_{false};

    const Letter& operator[](size_t i) const { return letters[i]; }
    Letter& operator[](size_t i) { return letters[i]; }
    size_t size() const { return letters.size(); }
    bool   empty() const { return letters.empty(); }

    bool equal(const Word& other) const {
        if (letters.size() != other.letters.size()) return false;
        for (size_t i = 0; i < letters.size(); ++i) {
            if (!letters[i].equal(other.letters[i])) return false;
        }
        return true;
    }

    std::string to_string() const {
        std::ostringstream o;
        o << "[";
        for (size_t i = 0; i < letters.size(); ++i) {
            if (i) o << ", ";
            o << letters[i].to_string();
        }
        o << "]";
        return o.str();
    }

    // Content-hash: concatenation of letter strings, suitable as a key
    // in a std::unordered_map<std::string, ...>.
    std::string content_key() const {
        std::ostringstream o;
        for (const auto& l : letters) {
            o << l.to_string();
            o << "\x01";   // non-printable separator to avoid collision
        }
        return o.str();
    }

    // 2026-04-26 (a-prime lever): 128-bit structural hash over
    // letters' (num, den) Polys. Memoized. Replaces the
    // ostringstream-based `content_key()` on the bump path.
    //
    // Sentinels distinct from poly_struct_hash_raw's internal
    // 0xfffe.../0xffff... so a Word hash cannot alias a raw-Poly hash
    // even on degenerate input.
    std::pair<uint64_t, uint64_t> struct_hash() const {
        if (hash_cached_) return cached_hash_;
        auto seeds = poly_struct_hash_seed();
        uint64_t h1 = seeds.first;
        uint64_t h2 = seeds.second;
        // Top-of-word sentinel + length-disambiguator.
        poly_struct_hash_mix(h1, h2, 0xfffdfffdfffdfffdULL);
        poly_struct_hash_mix(h1, h2, static_cast<uint64_t>(letters.size()));
        for (const auto& l : letters) {
            poly_struct_hash_raw(h1, h2, l.num());
            poly_struct_hash_mix(h1, h2, 0xfffdfffefffdfffeULL);
            poly_struct_hash_raw(h1, h2, l.den());
            poly_struct_hash_mix(h1, h2, 0xfffdffffffffffffULL);
        }
        cached_hash_ = {h1, h2};
        hash_cached_ = true;
        return cached_hash_;
    }
};

struct WordlistTerm {
    Rat  coef;
    Word word;
};

struct Wordlist {
    std::vector<WordlistTerm> terms;

    size_t size() const { return terms.size(); }
    bool   empty() const { return terms.empty(); }

    std::string to_string() const {
        std::ostringstream o;
        o << "{";
        for (size_t i = 0; i < terms.size(); ++i) {
            if (i) o << ", ";
            o << "{" << terms[i].coef.to_string() << ", "
              << terms[i].word.to_string() << "}";
        }
        o << "}";
        return o.str();
    }
};

}  // namespace hyperflint
