// HlogZeroExpand and HlogSeries.  Mirrors HyperIntica.wl:4170-4230.

#include "hyperflint/series/hlog_series.hpp"
#include "hyperflint/series/expansions.hpp"

#include <climits>

namespace hyperflint {

namespace {

// True iff the Rat has no dependence on var_idx.
bool is_free_of_var(const Rat& r, size_t var_idx) {
    // Poly::degree_in_var returns max exponent; 0 or -1 means the
    // variable does not appear in that polynomial.
    long dn = r.num().degree_in_var(var_idx);
    long dd = r.den().degree_in_var(var_idx);
    return dn <= 0 && dd <= 0;
}

// True iff the Word has any letter depending on var_idx.
bool word_has_var(const Word& w, size_t var_idx) {
    for (const auto& l : w.letters) {
        if (!is_free_of_var(l, var_idx)) return true;
    }
    return false;
}

// True iff `r` limits to 0 as var_idx -> 0.  Uses pole_degree: the
// limit is zero iff the min-exponent of the numerator strictly
// exceeds the min-exponent of the denominator (positive pole_degree,
// or pole_degree == LONG_MAX when num is identically zero).
bool goes_to_zero(const Rat& r, size_t var_idx) {
    long pd = r.pole_degree(var_idx);
    return pd == LONG_MAX || pd > 0;
}

}  // namespace

ExpansionSeries hlog_zero_expand(const Rat& arg, const Word& word, long order) {
    // Empty word  =>  Hlog[arg, {}] = 1. Represented as a single
    // (log_power=0, arg_power=0, coef=1) term.
    if (word.size() == 0) {
        ExpansionSeries out;
        const PolyCtx& ctx = arg.ctx();
        out.push_back({0, 0, Rat::one_of(ctx)});
        return out;
    }
    SeriesTable expa = expand_zero_word(word, order);
    if (expa.empty()) return {};

    ExpansionSeries out;
    for (size_t n = 0; n < expa.size(); ++n) {
        const std::vector<Rat>& row = expa[n];
        for (size_t m = 0; m < row.size(); ++m) {
            if (row[m].is_zero()) continue;
            out.push_back({(long)n, (long)m, row[m]});
        }
    }
    return out;
}

HlogSeriesResult hlog_series(const Rat& arg,
                             const Word& word,
                             size_t var_idx,
                             long order) {
    // (1) No var-dependence in arg or any letter -> return as-is.
    if (is_free_of_var(arg, var_idx) && !word_has_var(word, var_idx)) {
        return HlogSeriesResult{HlogSeriesBranch::kUnchanged, {}};
    }

    // (2) arg -> 0 as var -> 0: use hlog_zero_expand.
    if (goes_to_zero(arg, var_idx)) {
        return HlogSeriesResult{
            HlogSeriesBranch::kZeroLimit,
            hlog_zero_expand(arg, word, order)
        };
    }

    // (3) Everything else: Taylor expansion (deferred).  This requires
    // HyperD + NestList, which will land with Phase 5's integration
    // driver.  For now, signal the caller to fall back.
    return HlogSeriesResult{HlogSeriesBranch::kTaylorDeferred, {}};
}

}  // namespace hyperflint
