// LR-order search.  C++ port of SubTropica's STFasterFubini2.
// See include/hyperflint/integrator/lr_search.hpp for scope.

#include "hyperflint/integrator/lr_search.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hyperflint/algebra/euler_filter.hpp"

namespace hyperflint {
namespace lr_search {

namespace {

// Poll each monomial for its exponent in `var_idx` and its coefficient.
// Not optimal but fine for MVP — FLINT's fmpq_mpoly iterators are cheap.
//
// Leaf count per monomial:
//   1 for each variable with exponent >= 1  (the variable symbol)
//   1 for each variable with exponent >= 2  (the exponent integer)
//   1 if the coefficient is not ±1         (the numeric atom)
//
// Summed over monomials of a single poly, then over polys in the list.
long leaf_count_single(const Poly& p) {
    const fmpq_mpoly_struct* raw = p.raw();
    const auto* ctx = p.ctx().raw();
    const slong nt = fmpq_mpoly_length(raw, ctx);
    const size_t nv = p.ctx().vars().size();
    if (nt == 0) return 0;

    std::vector<slong> exps(nv);
    long total = 0;
    fmpq_t coef;
    fmpq_init(coef);
    for (slong t = 0; t < nt; ++t) {
        fmpq_mpoly_get_term_coeff_fmpq(coef, raw, t, ctx);
        fmpq_mpoly_get_term_exp_si(exps.data(),
            const_cast<fmpq_mpoly_struct*>(raw), t, ctx);

        // Coefficient atom.  Mma's LeafCount counts the literal `-1`
        // atom in Times[-1, x, y] (it is an atom in the expression tree),
        // so we count any non-unit coef — whether +k, -1, -k, or 1/k.
        // Rational coefs are undercounted relative to Mma's tree
        // structure (Rational[p, q] is 2 atoms there), but Symanzik
        // inputs are integer-coefficient, so this doesn't bite.
        if (!fmpq_is_one(coef)) total += 1;

        // variable / exponent atoms
        for (size_t v = 0; v < nv; ++v) {
            if (exps[v] >= 1) total += 1;
            if (exps[v] >= 2) total += 1;
        }
    }
    fmpq_clear(coef);
    return total;
}

}  // namespace

long leaf_count_proxy(const std::vector<Poly>& polys) {
    long total = 0;
    for (const auto& p : polys) total += leaf_count_single(p);
    return total;
}

std::vector<Poly> dedup_proportional(const std::vector<Poly>& polys) {
    std::vector<Poly> out;
    std::unordered_set<std::string> seen;
    for (const auto& p : polys) {
        const auto* ctx = p.ctx().raw();
        if (fmpq_mpoly_is_zero(p.raw(), ctx)) continue;
        if (fmpq_mpoly_is_fmpq(p.raw(), ctx)) continue;
        std::string key = p.canonical_prop_form().to_string();
        if (seen.insert(key).second) out.push_back(p);
    }
    return out;
}

std::vector<Poly> intersect_proportional(
    const std::vector<std::vector<Poly>>& lists) {
    if (lists.empty()) return {};
    if (lists.size() == 1) return dedup_proportional(lists[0]);

    // Canonical-form sets per input list.
    std::vector<std::unordered_set<std::string>> canon_sets;
    canon_sets.reserve(lists.size());
    for (const auto& list : lists) {
        std::unordered_set<std::string> s;
        for (const auto& p : list) {
            const auto* ctx = p.ctx().raw();
            if (fmpq_mpoly_is_zero(p.raw(), ctx)) continue;
            if (fmpq_mpoly_is_fmpq(p.raw(), ctx)) continue;
            s.insert(p.canonical_prop_form().to_string());
        }
        canon_sets.push_back(std::move(s));
    }

    // Intersect all canonical-form sets.
    std::unordered_set<std::string> common = canon_sets[0];
    for (size_t i = 1; i < canon_sets.size(); ++i) {
        std::unordered_set<std::string> next;
        for (const auto& key : common) {
            if (canon_sets[i].count(key)) next.insert(key);
        }
        common = std::move(next);
        if (common.empty()) return {};
    }

    // Representative from the first list.
    std::vector<Poly> out;
    std::unordered_set<std::string> used;
    for (const auto& p : lists[0]) {
        const auto* ctx = p.ctx().raw();
        if (fmpq_mpoly_is_zero(p.raw(), ctx)) continue;
        if (fmpq_mpoly_is_fmpq(p.raw(), ctx)) continue;
        std::string key = p.canonical_prop_form().to_string();
        if (common.count(key) && used.insert(key).second) {
            out.push_back(p);
        }
    }
    return out;
}

std::vector<Poly> st_fubini_lr(const std::vector<Poly>& polys, size_t var_idx) {
    if (polys.empty()) return {};
    const PolyCtx& pctx = polys.front().ctx();
    const auto* ctx = pctx.raw();

    std::vector<Poly> temp;
    temp.reserve(polys.size() * (polys.size() + 1) / 2 + polys.size() * 2);

    // Per-poly: leading coefficient + discriminant (when deg >= 1).
    for (const auto& f : polys) {
        if (fmpq_mpoly_is_zero(f.raw(), ctx)) continue;
        if (fmpq_mpoly_is_fmpq(f.raw(), ctx)) continue;
        const long n = f.degree_in_var(var_idx);
        if (n < 0) continue;
        // Leading coefficient in var_idx (returns a Poly free of var_idx).
        if (n >= 0) {
            Poly lc = f.coefficient_of_var(var_idx, n);
            if (!fmpq_mpoly_is_zero(lc.raw(), ctx)) temp.push_back(std::move(lc));
        }
        if (n >= 1) {
            Poly d = f.discriminant_in_var(var_idx);
            if (!fmpq_mpoly_is_zero(d.raw(), ctx)) temp.push_back(std::move(d));
        }
    }

    // Pairwise: resultant when both polys have degree >= 1 in var_idx.
    const size_t N = polys.size();
    for (size_t i = 0; i < N; ++i) {
        const Poly& fi = polys[i];
        if (fmpq_mpoly_is_zero(fi.raw(), ctx)) continue;
        if (fi.degree_in_var(var_idx) < 1) continue;
        for (size_t j = i + 1; j < N; ++j) {
            const Poly& fj = polys[j];
            if (fmpq_mpoly_is_zero(fj.raw(), ctx)) continue;
            if (fj.degree_in_var(var_idx) < 1) continue;
            Poly r = fi.resultant(fj, var_idx);
            if (!fmpq_mpoly_is_zero(r.raw(), ctx)) temp.push_back(std::move(r));
        }
    }

    // Factor each temp entry, flatten factor bases.
    std::vector<Poly> factored;
    factored.reserve(temp.size() * 2);
    for (const auto& p : temp) {
        if (fmpq_mpoly_is_zero(p.raw(), ctx)) continue;
        if (fmpq_mpoly_is_fmpq(p.raw(), ctx)) continue;
        auto bases = factor_bases(p);
        for (auto& b : bases) factored.push_back(std::move(b));
    }

    return dedup_proportional(factored);
}

namespace {

// Enumerate all bitmask subsets of {0,...,n-1} of a given size.
// Uses the "next higher bitmask with same popcount" trick (Gosper's hack).
std::vector<uint64_t> subsets_of_size(size_t n, size_t k) {
    std::vector<uint64_t> out;
    if (k > n) return out;
    if (k == 0) { out.push_back(0); return out; }
    uint64_t mask = (1ull << k) - 1;
    const uint64_t limit = 1ull << n;
    while (mask < limit) {
        out.push_back(mask);
        // Gosper's hack: next bitmask with same popcount
        uint64_t c = mask & -mask;
        uint64_t r = mask + c;
        mask = (((r ^ mask) >> 2) / c) | r;
    }
    return out;
}

}  // namespace

LrResult find_lr_orders(
    const std::vector<std::vector<Poly>>& group_polys,
    const std::vector<size_t>& xvar_indices,
    bool allow_algebraic_letters) {
    if (group_polys.empty()) {
        return LrResult{{}, 0.0, {}};
    }
    const size_t G = group_polys.size();
    const size_t n = xvar_indices.size();
    if (n == 0) return LrResult{{}, 0.0, {}};
    if (n > 63) {
        throw std::runtime_error(
            "find_lr_orders: > 63 integration variables (bitmask overflow)");
    }

    // Memoized state: bitmask -> per-group poly lists
    // (value.size() == G for every populated key).
    std::unordered_map<uint64_t, std::vector<std::vector<Poly>>> set_table;
    // Memoized order+score per subset.
    std::unordered_map<uint64_t, LrResult> orders_table;

    // Doppio-C Euler chi-drop filter (HF_EULER_FILTER=1, default OFF;
    // docs/env_flags.md): after the Fubini intersection at every subset,
    // each surviving letter is tested against the genuine Euler
    // discriminant of the S-marginal (chi_count_sectors via msolve) and
    // fictitious letters are dropped.  OFF-mode is byte-identical (the
    // branch below never runs).  Boundary monomials are exempt; the
    // verdict is conservative (Indeterminate/failure keep), mirroring
    // dpGenuineDKQ.  The per-call cache memoizes generic chi per
    // (group, subset) marginal.
    const char* euler_env = std::getenv("HF_EULER_FILTER");
    const bool euler_filter_on =
        euler_env != nullptr && *euler_env != '\0'
        && std::strcmp(euler_env, "0") != 0;
    std::vector<ChiFilterCache> chi_caches(euler_filter_on ? G : 0);

    const double INF = std::numeric_limits<double>::infinity();

    // Seed: set[g][{}] = group_polys[g] for every g, orders[{}] = ({}, 0).
    set_table[0] = group_polys;
    orders_table[0] = LrResult{{}, 0.0, {}};

    // DP over subset size.
    for (size_t size = 1; size <= n; ++size) {
        auto subsets = subsets_of_size(n, size);
        for (uint64_t bits : subsets) {
            // Step A: for each group g, build preSTable (one list per
            // pivot bit v ∈ bits), then intersect.
            std::vector<std::vector<Poly>> set_for_bits(G);
            for (size_t g = 0; g < G; ++g) {
                std::vector<std::vector<Poly>> preTable;
                preTable.reserve(size);
                for (size_t bit = 0; bit < n; ++bit) {
                    if (!(bits & (1ull << bit))) continue;
                    const uint64_t prev_bits = bits ^ (1ull << bit);
                    auto it = set_table.find(prev_bits);
                    if (it == set_table.end()) {
                        // Parent subset was dropped by memory pruning or
                        // never computed — fatal in single-group MVP.
                        throw std::runtime_error(
                            "find_lr_orders: missing parent subset state");
                    }
                    const auto& prev_polys = it->second[g];
                    preTable.push_back(
                        st_fubini_lr(prev_polys, xvar_indices[bit]));
                }
                set_for_bits[g] = intersect_proportional(preTable);
                if (euler_filter_on && !set_for_bits[g].empty()) {
                    std::vector<size_t> subset_vars;
                    subset_vars.reserve(size);
                    for (size_t b = 0; b < n; ++b)
                        if (bits & (1ull << b))
                            subset_vars.push_back(xvar_indices[b]);
                    set_for_bits[g] = chi_filter_letters(
                        group_polys[g], subset_vars, set_for_bits[g],
                        chi_caches[g]);
                }
            }
            set_table[bits] = std::move(set_for_bits);

            // Step B: compute orders_table[bits] via DP extension.
            LrResult best{{}, INF, {}};

            // HyperIntica-parity guard.  Mirrors the runtime check in
            // linear_factors.cpp:1444-1460 (wide path) / :1212-1229
            // (narrow PIVOT path), which is gated by
            // LFForbiddenVarsScope set at integration_step.cpp:1265.
            // A deg-2 letter base polynomial whose Wm/Wp definition
            // uses any not-yet-integrated Feynman parameter is refused
            // at runtime (pushed to out.nonlinear, then partial_fractions
            // throws "nonlinear factor in denominator").  Modelling the
            // same rejection here keeps the LR-scorer in sync with the
            // integrator: gauges whose runtime guard would crash are
            // scored NOLR rather than picked optimistically.
            //
            // Forbidden set for this DP node is the set of vars that
            // are still un-integrated AFTER taking this step --- i.e.,
            // the unset bits of `bits` (the post-step bitmask), mapped
            // through xvar_indices to wide-context indices.  The set
            // is the same for every candidate `bit` at this `bits`
            // node, so it is computed once.  By construction `var_idx
            // = xvar_indices[bit]` is NEVER in forbidden_after_step
            // (its bit is set in `bits`; xvar_indices is injective by
            // handlers.cpp:367-369), so the runtime's defensive
            // `if (v == var_idx) continue` skip is structurally a
            // no-op here, but kept for line-for-line parity.
            std::vector<size_t> forbidden_after_step;
            if (allow_algebraic_letters) {
                forbidden_after_step.reserve(n);
                for (size_t b = 0; b < n; ++b) {
                    if (!(bits & (1ull << b))) {
                        forbidden_after_step.push_back(xvar_indices[b]);
                    }
                }
            }

            for (size_t bit = 0; bit < n; ++bit) {
                if (!(bits & (1ull << bit))) continue;
                const uint64_t prev_bits = bits ^ (1ull << bit);
                auto it = orders_table.find(prev_bits);
                if (it == orders_table.end()) continue;
                const LrResult& prev = it->second;
                if (prev.nolr()) continue;  // parent is NOLR; skip

                // Precondition: every poly in set[g][prev_bits] has
                // degree ≤ max_deg in x_bit.  max_deg = 1 under classic
                // FindRoots=False; = 2 when algebraic letters are allowed
                // (HF's integrator allocates Wm_i/Wp_i for deg-2 factors
                // at integration time).
                const size_t var_idx = xvar_indices[bit];
                const long max_deg = allow_algebraic_letters ? 2L : 1L;
                const auto& prev_set_all = set_table[prev_bits];
                bool all_linear = true;
                double extension_score = prev.score;
                std::vector<Poly> step_root_polys;  // deg-2 polys here
                for (size_t g = 0; g < G; ++g) {
                    const auto& gpolys = prev_set_all[g];
                    for (const auto& p : gpolys) {
                        const long d = p.degree_in_var(var_idx);
                        if (d > max_deg) { all_linear = false; break; }
                        if (d == 2 && allow_algebraic_letters) {
                            // Forbidden-var check (parity with the
                            // runtime guard).  If `p`'s used vars
                            // intersect forbidden_after_step, the
                            // runtime would push this factor to
                            // out.nonlinear and partial_fractions
                            // would throw -- mark this DP step NOLR
                            // so the LR-scorer doesn't score it.
                            std::vector<size_t> used = p.used_var_indices();
                            bool has_forbidden_dep = false;
                            for (size_t v : forbidden_after_step) {
                                if (v == var_idx) continue;  // see scope comment above
                                for (size_t u : used) {
                                    if (u == v) { has_forbidden_dep = true; break; }
                                }
                                if (has_forbidden_dep) break;
                            }
                            if (has_forbidden_dep) {
                                all_linear = false;
                                break;
                            }
                            // Phase 7-vii: collect the deg-2 polynomial
                            // so the caller knows which polys to turn
                            // into Wm/Wp at integration time.  Mma's
                            // STFasterFubini2 returns the same list as
                            // result[[2]] under FindRoots=True.
                            step_root_polys.push_back(p);
                        }
                    }
                    if (!all_linear) break;
                    long leaf_sum = leaf_count_proxy(gpolys);
                    extension_score +=
                        std::pow(static_cast<double>(leaf_sum), 1.15);
                }
                if (!all_linear) continue;

                if (extension_score < best.score) {
                    std::vector<size_t> new_order = prev.order;
                    new_order.push_back(xvar_indices[bit]);
                    std::vector<Poly> new_roots = prev.root_polys;
                    new_roots.insert(new_roots.end(),
                                     std::make_move_iterator(step_root_polys.begin()),
                                     std::make_move_iterator(step_root_polys.end()));
                    best = LrResult{std::move(new_order),
                                     extension_score,
                                     std::move(new_roots)};
                }
            }
            orders_table[bits] = best;
        }

        // Memory pruning: drop size-(size-1) set entries, they're no
        // longer referenced at size-(size+1) iterations.
        if (size >= 1) {
            auto prev_subsets = subsets_of_size(n, size - 1);
            for (uint64_t k : prev_subsets) set_table.erase(k);
        }
    }

    const uint64_t full = (n == 64) ? ~0ull : ((1ull << n) - 1);
    auto it = orders_table.find(full);
    if (it == orders_table.end()) return LrResult{{}, INF, {}};
    return it->second;
}

}  // namespace lr_search
}  // namespace hyperflint
