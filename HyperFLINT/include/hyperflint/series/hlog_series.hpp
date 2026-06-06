// HlogZeroExpand and HlogSeries.
//
//   hlog_zero_expand(arg, word, order)
//     Expansion of Hlog[arg, word] as a symbolic expression in `arg`,
//     valid when arg -> 0.  Returns a list of (log_power, arg_power,
//     coef) triples; the full expression is the sum of
//         coef_{k,j} * Log[arg]^k / k! * arg^j.
//
//   hlog_series(arg, word, var_idx, order)
//     Dispatcher mirroring HyperIntica.wl:4211.  Branches:
//       (1) no var-dependence in arg or letters: returns unevaluated
//           (HlogSeries::unchanged = true).
//       (2) arg -> 0 as var -> 0: returns the hlog_zero_expand result
//           (no further var-truncation here; the caller trims).
//       (3) otherwise: Taylor expansion — not yet implemented in C++.
//           Returns HlogSeries::taylor_deferred.
//
// The ExpansionTerm struct encodes a single summand in the series as
//     coef * Log[arg]^log_power / log_power! * arg^arg_power.

#pragma once

#include "hyperflint/core/rat.hpp"
#include "hyperflint/symbols/word.hpp"

#include <vector>

namespace hyperflint {

struct ExpansionTerm {
    long log_power;   // k (power of Log[arg])
    long arg_power;   // j (power of arg)
    Rat  coef;        // c_{k,j}
};
using ExpansionSeries = std::vector<ExpansionTerm>;

enum class HlogSeriesBranch {
    kZeroLimit,       // arg -> 0; `terms` is the zero-expansion.
    kUnchanged,       // no var-dependence; caller should emit Hlog[arg, word].
    kTaylorDeferred   // Taylor branch, not yet ported.
};

struct HlogSeriesResult {
    HlogSeriesBranch branch;
    ExpansionSeries  terms;
};

// Raw expansion, assuming the caller has already verified arg -> 0.
// Panzer / Brown formula via expand_zero_word.
ExpansionSeries hlog_zero_expand(const Rat& arg, const Word& word, long order);

// Top-level dispatcher.
HlogSeriesResult hlog_series(const Rat& arg,
                             const Word& word,
                             size_t var_idx,
                             long order);

}  // namespace hyperflint
