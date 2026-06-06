// MplSeries dispatcher.  Mirrors HyperIntica.wl:4239.

#include "hyperflint/series/mpl_series.hpp"
#include "hyperflint/series/mpl_sum.hpp"

#include <climits>

namespace hyperflint {

namespace {

bool is_free_of_var(const Rat& r, size_t var_idx) {
    long dn = r.num().degree_in_var(var_idx);
    long dd = r.den().degree_in_var(var_idx);
    return dn <= 0 && dd <= 0;
}

bool goes_to_zero(const Rat& r, size_t var_idx) {
    long pd = r.pole_degree(var_idx);
    return pd == LONG_MAX || pd > 0;
}

// True iff r limits to 1 at var_idx -> 0.
bool goes_to_one(const Rat& r, size_t var_idx) {
    // (r - 1) -> 0
    if (r.ctx().vars().empty()) {
        // degenerate ctx (shouldn't happen in practice)
        return false;
    }
    Rat one{Poly(r.ctx(), "1")};
    return goes_to_zero(r - one, var_idx);
}

}  // namespace

MplSeriesResult mpl_series(const std::vector<long>& ns,
                           const std::vector<Rat>& zs,
                           size_t var_idx,
                           long order) {
    if (ns.size() != zs.size()) {
        return MplSeriesResult(MplSeriesBranch::kUnchanged);
    }
    if (ns.empty()) {
        // Mpl[{}, {}] = 1 by convention.  No var-dependence to expand.
        return MplSeriesResult(MplSeriesBranch::kUnchanged);
    }

    // (1) var does not appear in any z  =>  unchanged.
    bool any_depends = false;
    for (const auto& z : zs) {
        if (!is_free_of_var(z, var_idx)) { any_depends = true; break; }
    }
    if (!any_depends) {
        return MplSeriesResult(MplSeriesBranch::kUnchanged);
    }

    // (2) last z -> 0  =>  MplSum.
    if (goes_to_zero(zs.back(), var_idx)) {
        return MplSeriesResult(MplSeriesBranch::kMplSum,
                               mpl_sum(ns, zs, order));
    }

    // (3) first z -> 0  =>  MplSum.
    if (goes_to_zero(zs.front(), var_idx)) {
        return MplSeriesResult(MplSeriesBranch::kMplSum,
                               mpl_sum(ns, zs, order));
    }

    // (4) depth-1, lim != 0  =>  defer to PolyLog[n, z] series expansion.
    if (ns.size() == 1) {
        return MplSeriesResult(MplSeriesBranch::kPolyLogDeferred);
    }

    // (5) depth>1, last arg -> 1  =>  log singularity; Mma returns
    //     the Mpl unevaluated.
    if (goes_to_one(zs.back(), var_idx)) {
        return MplSeriesResult(MplSeriesBranch::kLogSingularity);
    }

    // (6) anything else: Taylor expansion, deferred.
    return MplSeriesResult(MplSeriesBranch::kTaylorDeferred);
}

}  // namespace hyperflint
