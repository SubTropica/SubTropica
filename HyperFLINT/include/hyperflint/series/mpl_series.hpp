// MplSeries dispatcher.  Mirrors HyperIntica.wl:4239.
//
//   mpl_series(ns, zs, var_idx, order)
//     Branches:
//       (1) no var in any z: return unchanged.
//       (2) last z -> 0 as var -> 0: MplSum(ns, zs, order).
//       (3) first z -> 0 as var -> 0: MplSum(ns, zs, order).
//       (4) depth-1 (ns.size() == 1), lim != 0: unresolved here —
//           Mma delegates to `Series[PolyLog[n, z], {var, 0, order}]`;
//           we return PolyLogDeferred.
//       (5) depth>1 with last arg -> 1 and first arg fixed: return
//           unevaluated (logarithmic singularity; Mma does the same).
//       (6) otherwise: Taylor expansion — not yet implemented here.
//
// The MplSum-branches produce a single Rat (scalar sum).  All other
// branches return no scalar.

#pragma once

#include "hyperflint/core/rat.hpp"

#include <optional>
#include <vector>

namespace hyperflint {

enum class MplSeriesBranch {
    kMplSum,            // scalar truncation from the defining series
    kUnchanged,         // no var-dependence
    kPolyLogDeferred,   // depth-1, lim != 0  -> PolyLog Series
    kLogSingularity,    // depth>1, last arg -> 1  (Mma returns unevaluated)
    kTaylorDeferred     // generic case: Taylor expansion not ported yet
};

struct MplSeriesResult {
    MplSeriesBranch        branch;
    std::optional<Rat>     scalar;   // populated iff branch == kMplSum.
    // No default constructor: a default would need a Rat, which needs
    // a PolyCtx, which we don't have at the bare default. Phase
    // 6d-v-vi-0 cascade fix: previously this stored a stack-local
    // empty PolyCtx, leaving every default-constructed result with a
    // dangling ctx pointer.
    explicit MplSeriesResult(MplSeriesBranch b)
        : branch(b) {}
    MplSeriesResult(MplSeriesBranch b, Rat s)
        : branch(b), scalar(std::move(s)) {}
};

MplSeriesResult mpl_series(const std::vector<long>& ns,
                           const std::vector<Rat>& zs,
                           size_t var_idx,
                           long order);

}  // namespace hyperflint
