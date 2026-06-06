// Word-level series expansions at 0 and at infinity.
//
// Line-by-line port of HyperIntica.wl:2391-2425 (ExpandZeroWord) and
// 2437-2470 (ExpandInfWord).  Index translation from Mma 1-based to
// C++ 0-based:
//   Mma  sub[[logpower+1]]            -> C++  sub[logpower]
//   Mma  sub[[logpower+1, ii+1]]      -> C++  sub[logpower][ii]
//   Mma  result[[logpower+2, 1]]      -> C++  result[logpower+1][0]
//   Mma  result[[jj+1, power+2]]      -> C++  result[jj][power+1]

#include "hyperflint/series/expansions.hpp"

#include <algorithm>
#include <stdexcept>

namespace hyperflint {

namespace {

// Constant Rat helpers in the given context.
Rat rat_zero(const PolyCtx& ctx) { return Rat{Poly::zero_of(ctx)}; }
Rat rat_one (const PolyCtx& ctx) { return Rat{Poly::one_of(ctx)}; }

// An integer as a Rat in the given context.
Rat rat_int(const PolyCtx& ctx, long n) {
    return Rat{Poly::from_int(ctx, n)};
}

// Build an initial (rows) x (cols) table of zeros.
SeriesTable make_zero_table(const PolyCtx& ctx, size_t rows, size_t cols) {
    Rat z = rat_zero(ctx);
    SeriesTable out;
    out.reserve(rows);
    for (size_t i = 0; i < rows; ++i) {
        out.emplace_back(cols, z);
    }
    return out;
}

// Truncate inner vector at length `len`; if len is 0, produce an
// empty inner vector.  Rat has no default ctor so we can't call
// std::vector::resize (the template instantiates the growing branch
// unconditionally); erase instead.
void truncate_row(std::vector<Rat>& row, long len) {
    if (len <= 0) { row.clear(); return; }
    if ((size_t)len < row.size()) {
        row.erase(row.begin() + len, row.end());
    }
}

// Drop trailing all-empty rows from a series table.
void drop_trailing_empty(SeriesTable& t) {
    while (!t.empty() && t.back().empty()) t.pop_back();
}

// Shared inner-loop body.  `sub_view` is the tail's expansion table,
// `w1` is the first letter.  `mode == kZero` for ExpandZeroWord, kInf
// for ExpandInfWord.  Mirrors the Do[{power, 0, minOrder-1}] block.
enum Mode { kZero, kInf };

// Internal recursion with an explicit ctx so all base cases produce
// Rats in the caller's context (avoids cross-ctx arithmetic).
SeriesTable expand_common_in(const Word& word, long min_order, Mode mode,
                             const PolyCtx& ctx);

SeriesTable expand_common(const Word& word, long min_order, Mode mode) {
    if (word.size() == 0) {
        // Phase 6d-v-vi-0 cleanup: previous "fabricate empty PolyCtx"
        // returned Rats with dangling stack-local ctx. The empty case
        // is now handled by the public expand_zero_word /
        // expand_inf_word callers (which return empty tables) and by
        // the _in_ctx overloads (which take a real ctx). This path is
        // unreachable; assert it.
        throw std::runtime_error(
            "expand_common: empty word — caller should route through "
            "expand_*_word_in_ctx or handle the empty case directly");
    }
    if (min_order < 0) return SeriesTable{};
    return expand_common_in(word, min_order, mode, word[0].ctx());
}

SeriesTable expand_common_in(const Word& word, long min_order, Mode mode,
                             const PolyCtx& ctx) {
    // Base: empty word -> {{1}} in the parent's ctx.
    if (word.size() == 0) return {{rat_one(ctx)}};
    if (min_order < 0) return SeriesTable{};

    // Recursive tail expansion, sharing the ctx.
    Word tail;
    tail.letters.assign(word.letters.begin() + 1, word.letters.end());
    SeriesTable sub = expand_common_in(tail, min_order, mode, ctx);
    if (sub.empty()) return SeriesTable{};

    const long maxlog = (long)sub.size();
    // Accumulator:  (maxlog+1) x (min_order+1), zero-initialized.
    SeriesTable result = make_zero_table(ctx,
                                         (size_t)(maxlog + 1),
                                         (size_t)(min_order + 1));
    std::vector<long> maxpowers((size_t)(maxlog + 1), -1);

    const Rat& w1 = word[0];
    const bool w1_zero = w1.is_zero();

    for (long logpower = 0; logpower < maxlog; ++logpower) {
        const std::vector<Rat>& srow = sub[(size_t)logpower];

        // Special constant-log-bump at (logpower+1, 0).
        // Mma Zero branch fires iff w1 === 0.
        // Mma Inf  branch fires unconditionally (no w1 check).
        bool bump = !srow.empty() &&
                    (mode == kInf || w1_zero);
        if (bump) {
            if (mode == kZero) {
                result[(size_t)(logpower + 1)][0] = result[(size_t)(logpower + 1)][0]
                                                  + srow[0];
            } else {
                result[(size_t)(logpower + 1)][0] = result[(size_t)(logpower + 1)][0]
                                                  - srow[0];
            }
            maxpowers[(size_t)(logpower + 1)] =
                std::max(maxpowers[(size_t)(logpower + 1)], 0L);
        }

        for (long power = 0; power < min_order; ++power) {
            Rat p = rat_zero(ctx);

            if (mode == kZero) {
                if (!w1_zero) {
                    // p -= Sum_{ii=0..min(power, len-1)} srow[ii] / w1^(power-ii+1)
                    long upper = std::min(power, (long)srow.size() - 1);
                    for (long ii = 0; ii <= upper; ++ii) {
                        long exp = power - ii + 1;
                        p = p - srow[(size_t)ii] / w1.pow(exp);
                    }
                } else {
                    // p = srow[power+1] if exists else 0
                    if ((long)srow.size() > power + 1) {
                        p = srow[(size_t)(power + 1)];
                    }
                }
            } else {  // kInf
                // p = -srow[power+1] if exists else 0
                if ((long)srow.size() > power + 1) {
                    p = -srow[(size_t)(power + 1)];
                }
                if (!w1_zero) {
                    long upper = std::min(power, (long)srow.size() - 1);
                    for (long ii = 0; ii <= upper; ++ii) {
                        long exp = power - ii + 1;
                        p = p - srow[(size_t)ii] * w1.pow(exp);
                    }
                }
            }

            if (!p.is_zero()) {
                p = -p;
                for (long jj = logpower; jj >= 0; --jj) {
                    p = (-p) / rat_int(ctx, power + 1);
                    result[(size_t)jj][(size_t)(power + 1)] =
                        result[(size_t)jj][(size_t)(power + 1)] + p;
                    maxpowers[(size_t)jj] =
                        std::max(maxpowers[(size_t)jj], power + 1);
                }
            }
        }
    }

    // Truncate each row according to maxpowers and drop trailing empties.
    for (size_t k = 0; k < result.size(); ++k) {
        truncate_row(result[k], maxpowers[k] + 1);
    }
    drop_trailing_empty(result);
    return result;
}

}  // namespace

SeriesTable expand_zero_word(const Word& word, long min_order) {
    // Empty word: caller must use the (ctx, word) overload to get a
    // properly-rooted "1" coefficient. Returning an empty table here is
    // safe — the existing pre-Phase-6d-v-vi-0 behavior was a stack-
    // local PolyCtx that left every Rat in the result dangling.
    if (word.size() == 0) return SeriesTable{};
    if (min_order < 0) return SeriesTable{};
    return expand_common(word, min_order, kZero);
}

SeriesTable expand_inf_word(const Word& word, long min_order) {
    if (word.size() == 0) return SeriesTable{};
    if (min_order < 0) return SeriesTable{};
    return expand_common(word, min_order, kInf);
}

SeriesTable expand_zero_word_in_ctx(const PolyCtx& ctx,
                                     const Word& word, long min_order) {
    if (word.size() == 0) return {{rat_one(ctx)}};
    if (min_order < 0) return SeriesTable{};
    return expand_common(word, min_order, kZero);
}

SeriesTable expand_inf_word_in_ctx(const PolyCtx& ctx,
                                    const Word& word, long min_order) {
    if (word.size() == 0) return {{rat_one(ctx)}};
    if (min_order < 0) return SeriesTable{};
    return expand_common(word, min_order, kInf);
}

}  // namespace hyperflint
