// MplSum impl.  Direct port of HyperIntica.wl:4158-4167.

#include "hyperflint/series/mpl_sum.hpp"

#include <stdexcept>

namespace hyperflint {

namespace {

// Helper: integer constant as a Rat in the given context.
Rat rat_int(const PolyCtx& ctx, long n) {
    return Rat::from_int(ctx, n);
}

// Recursive helper — takes a shared ctx so the empty-ns base case
// uses the caller's context (matters when the top-level zs == []).
Rat mpl_sum_in(const std::vector<long>& ns,
               const std::vector<Rat>& zs,
               long max_n,
               const PolyCtx& ctx) {
    if (ns.empty()) return Rat::one_of(ctx);
    if (max_n < 1)  return Rat::zero_of(ctx);
    if (ns.size() != zs.size()) {
        throw std::invalid_argument("mpl_sum: ns and zs have different lengths");
    }

    // Subsum recursion drops the *last* index pair (Mma: Most[ns], Most[zs]).
    std::vector<long> sub_ns(ns.begin(),  ns.end()  - 1);
    std::vector<Rat>  sub_zs(zs.begin(),  zs.end()  - 1);

    const long n_idx = ns.back();
    const Rat& z_n   = zs.back();

    // Sum_{k=1..max_n} subsum(k-1) * z_n^k / k^n_idx.
    Rat total = Rat::zero_of(ctx);
    for (long k = 1; k <= max_n; ++k) {
        Rat sub = mpl_sum_in(sub_ns, sub_zs, k - 1, ctx);
        if (sub.is_zero()) continue;
        Rat term = sub * z_n.pow(k);
        // Divide by k^n_idx (integer), which is a Rat.
        long kpow_n = 1;
        for (long i = 0; i < n_idx; ++i) kpow_n *= k;
        if (kpow_n == 0) {
            throw std::runtime_error(
                "mpl_sum: k^n overflowed / underflowed (n may be negative)");
        }
        term = term / rat_int(ctx, kpow_n);
        total = total + term;
    }
    return total;
}

}  // namespace

Rat mpl_sum(const std::vector<long>& ns,
            const std::vector<Rat>& zs,
            long max_n) {
    if (zs.empty()) {
        // Phase 6d-v-vi-0 cleanup: the previous "fabricate an empty
        // PolyCtx" path returned Rats with a dangling stack-local ctx.
        // Callers always pass non-empty zs in practice; convert the
        // bomb into a loud error so any future caller hitting this
        // path notices immediately.
        throw std::runtime_error(
            "mpl_sum: zs must be non-empty (no PolyCtx to borrow)");
    }
    return mpl_sum_in(ns, zs, max_n, zs[0].ctx());
}

}  // namespace hyperflint
