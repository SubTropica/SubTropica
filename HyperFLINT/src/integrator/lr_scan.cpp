// Doppio-port phase 3 — implementation.
// See include/hyperflint/integrator/lr_scan.hpp for the contract and
// scripts/doppiofubini/doppio/doppio_lib.wl (dpProjectiveQ, dpFRJudge,
// dpAdmissibleOrdersScan/stepScan, t24 battery) for the validated
// Mathematica reference.

#include "hyperflint/integrator/lr_scan.hpp"

#include <flint/fmpq_mpoly.h>
#include <flint/fmpq_mpoly_factor.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <unordered_map>

#include "hyperflint/algebra/euler_filter.hpp"
#include "hyperflint/integrator/lr_search.hpp"

namespace hyperflint {
namespace lr_scan {

namespace {

// per-term total degree over `idx`; -1 signals inhomogeneous or zero
long homogeneous_degree(const Poly& p, const std::vector<size_t>& idx)
{
    const auto* ctx = p.ctx().raw();
    const auto* raw = p.raw();
    const slong nt = fmpq_mpoly_length(raw, ctx);
    if (nt == 0) return -1;
    const size_t nv = p.ctx().vars().size();
    std::vector<slong> exps(nv);
    long deg0 = -1;
    for (slong t = 0; t < nt; ++t) {
        fmpq_mpoly_get_term_exp_si(exps.data(),
            const_cast<fmpq_mpoly_struct*>(raw), t, ctx);
        long d = 0;
        for (size_t v : idx) d += (long) exps[v];
        if (t == 0) deg0 = d;
        else if (d != deg0) return -1;
    }
    return deg0;
}

// dpSquareQ via FLINT: perfect square as a polynomial over Q (rejects
// 2x^2 -- sqrt(2) not rational -- and -x^2 -- no rational square root)
bool perfect_square(const Poly& p)
{
    const auto* ctx = p.ctx().raw();
    if (fmpq_mpoly_is_zero(p.raw(), ctx)) return true;   // 0 == 0^2
    fmpq_mpoly_t rt;
    fmpq_mpoly_init(rt, ctx);
    int ok = fmpq_mpoly_sqrt(rt, p.raw(), ctx);
    fmpq_mpoly_clear(rt, ctx);
    return ok != 0;
}

bool depends_on_any(const Poly& p, const std::vector<size_t>& vars)
{
    for (size_t v : vars)
        if (p.degree_in_var(v) > 0) return true;
    return false;
}

std::string canon_key(const Poly& p)
{
    return p.canonical_prop_form().to_string();
}

}  // namespace

bool conic_rationalizable(const Poly& letter, size_t pivot)
{
    if (letter.degree_in_var(pivot) != 2) return false;
    Poly A = letter.coefficient_of_var(pivot, 2);
    Poly C = letter.coefficient_of_var(pivot, 0);
    return perfect_square(A) && perfect_square(C);
}

FrJudgment fr_judge(const Poly& letter, size_t pivot,
                    const std::vector<size_t>& pending,
                    const std::vector<size_t>& all_xvars)
{
    FrJudgment out;
    const long d = letter.degree_in_var(pivot);
    if (d <= 1) { out.ok = true; return out; }
    if (d >= 3) { out.ok = false; return out; }
    if (conic_rationalizable(letter, pivot)) { out.ok = true; return out; }
    if (pending.empty()) {
        // TERMINAL quadratic: its roots are kinematic letters of the
        // final answer (FindRoots semantics)
        out.ok = true;
        out.term = 1;
        return out;
    }
    // the actual sqrt argument: the ODD-multiplicity part of the
    // pivot-discriminant (even powers exit the root rationally; the
    // numeric content is a constant sqrt -- function class unchanged)
    Poly disc = letter.discriminant_in_var(pivot);
    const auto* ctx = disc.ctx().raw();
    if (fmpq_mpoly_is_zero(disc.raw(), ctx)) {
        // zero discriminant: A (v - r)^2, a double RATIONAL line -- its
        // dlog produces no sqrt at all (review fold F1/A9): accept clean
        out.ok = true;
        return out;
    }
    fmpq_mpoly_factor_t fac;
    fmpq_mpoly_factor_init(fac, ctx);
    if (!fmpq_mpoly_factor(fac,
            const_cast<fmpq_mpoly_struct*>(disc.raw()), ctx)) {
        fmpq_mpoly_factor_clear(fac, ctx);
        out.ok = false;   // factorization failure: refuse the step
        return out;
    }
    bool any_kin_odd = false;
    std::vector<Poly> oblig;
    for (slong i = 0; i < fac->num; ++i) {
        fmpz_t e;
        fmpz_init(e);
        fmpz_set(e, fac->exp + i);
        const bool odd = fmpz_is_odd(e);
        fmpz_clear(e);
        if (!odd) continue;
        Poly base(letter.ctx());
        fmpq_mpoly_set(base.raw(), fac->poly + i, ctx);
        if (fmpq_mpoly_is_fmpq(base.raw(), ctx)) continue;  // numeric content
        if (depends_on_any(base, all_xvars))
            oblig.push_back(base.canonical_prop_form());
        else
            any_kin_odd = true;
    }
    fmpq_mpoly_factor_clear(fac, ctx);
    out.ok = true;
    if (oblig.empty()) {
        // pure-kinematic sqrt letter (Lungo's FindRoots exemption) or a
        // perfect-square disc (rational roots): count only the former
        if (any_kin_odd) out.kin = 1;
        return out;
    }
    out.carry = std::move(oblig);
    if (any_kin_odd) out.kin = 1;   // mixed disc (review fold F2)
    return out;
}

bool projective_input(
    const std::vector<std::vector<Poly>>& group_polys,
    const std::vector<size_t>& xvar_indices,
    const std::vector<std::vector<ScanExponent>>& exps)
{
    if (group_polys.empty() || exps.size() != group_polys.size())
        return false;
    const long n = (long) xvar_indices.size();
    for (size_t g = 0; g < group_polys.size(); ++g) {
        if (exps[g].size() != group_polys[g].size()) return false;
        long sum_a = 0, sum_b = 0;
        for (size_t i = 0; i < group_polys[g].size(); ++i) {
            const long d = homogeneous_degree(group_polys[g][i],
                xvar_indices);
            if (d < 0) return false;   // inhomogeneous (or zero) factor
            sum_a += exps[g][i].a * d;
            sum_b += exps[g][i].b * d;
        }
        if (sum_a != -n || sum_b != 0) return false;
    }
    return true;
}

namespace {

// per-gauge DFS state for KeepRule::FindRoots
struct PathState {
    std::vector<Poly> carried;            // canonical forms
    std::set<std::string> carried_keys;   // dedup
    unsigned long nsq = 0, nkin = 0, ntq = 0;
};

struct ScanDriver {
    const std::vector<std::vector<Poly>>& groups_aug;  // augmented
    const std::vector<size_t>& xv;                     // ctx indices
    KeepRule rule;
    size_t max_orders;
    // subset table: bitmask (over xv positions) -> per-group letters
    std::unordered_map<uint64_t, std::vector<std::vector<Poly>>> table;
    ScanResult* result;

    // dehomogenize at the gauge variable (skip when letter is gauge-free)
    static Poly degauge(const Poly& p, size_t gauge)
    {
        if (p.degree_in_var(gauge) <= 0) return p;
        return p.substitute_one_rat(gauge, "1");
    }

    bool step_ok_strict(const std::vector<Poly>& letters, size_t pivot,
                        size_t gauge)
    {
        const auto* ctx0 = letters.empty()
            ? nullptr : letters.front().ctx().raw();
        (void) ctx0;
        for (const auto& L : letters) {
            Poly p = degauge(L, gauge);
            const auto* ctx = p.ctx().raw();
            if (fmpq_mpoly_is_fmpq(p.raw(), ctx)) continue;
            const long d = p.degree_in_var(pivot);
            if (d <= 1) continue;
            if (d == 2 && conic_rationalizable(p, pivot)) continue;
            return false;
        }
        return true;
    }

    // FindRoots step: judges table letters + carried obligations on the
    // DEHOMOGENIZED forms, with join-dedup by canonical key (parity with
    // the Mathematica stepScan after its t24 counter-parity folds)
    bool step_fr(const std::vector<Poly>& letters, size_t pivot,
                 size_t gauge, const std::vector<size_t>& pending,
                 PathState& st)
    {
        // discharge obligations now free of pending variables
        std::vector<Poly> kept;
        std::set<std::string> kept_keys;
        for (const auto& c : st.carried)
            if (depends_on_any(c, pending) ||
                c.degree_in_var(pivot) > 0) {
                kept_keys.insert(canon_key(c));
                kept.push_back(c);
            }
        // join (letters dehomogenized first) + dedup, table-first
        std::vector<Poly> judged;
        std::set<std::string> seen;
        for (const auto& L : letters) {
            Poly p = degauge(L, gauge);
            const auto* ctx = p.ctx().raw();
            if (fmpq_mpoly_is_fmpq(p.raw(), ctx)) continue;
            if (seen.insert(canon_key(p)).second) judged.push_back(p);
        }
        for (const auto& c : kept)
            if (seen.insert(canon_key(c)).second) judged.push_back(c);

        std::vector<size_t> all_pending = pending;  // for kin-vs-carry split
        for (const auto& p : judged) {
            FrJudgment j = fr_judge(p, pivot, pending, all_pending);
            if (!j.ok) return false;
            st.nkin += j.kin;
            st.ntq += j.term;
            for (const auto& c : j.carry) {
                const std::string k = canon_key(c);
                if (kept_keys.insert(k).second) {
                    kept.push_back(c);
                    ++st.nsq;
                }
            }
        }
        st.carried = std::move(kept);
        st.carried_keys = std::move(kept_keys);
        return true;
    }

    void dfs(uint64_t bits, std::vector<size_t>& order, double score,
             size_t gauge_pos, const PathState& st)
    {
        const size_t n = xv.size();
        if (result->truncated) return;
        if (order.size() == n - 1) {
            if (result->orders.size() >= max_orders) {
                result->truncated = true;
                return;
            }
            ScanOrder so;
            so.order = order;
            so.gauge = xv[gauge_pos];
            so.score = score;
            so.carried_sqrts = st.nsq;
            so.kin_sqrts = st.nkin;
            so.terminal_quads = st.ntq;
            result->orders.push_back(std::move(so));
            return;
        }
        for (size_t bit = 0; bit < n; ++bit) {
            if (bit == gauge_pos) continue;
            if (bits & (1ull << bit)) continue;
            // letters of the PARENT subset (all groups)
            auto it = table.find(bits);
            if (it == table.end()) return;   // table truncated: prune
            std::vector<Poly> letters;
            for (const auto& gl : it->second)
                letters.insert(letters.end(), gl.begin(), gl.end());
            // pending AFTER this step: unset bits except pivot + gauge
            std::vector<size_t> pending;
            for (size_t b = 0; b < n; ++b)
                if (b != gauge_pos && b != bit && !(bits & (1ull << b)))
                    pending.push_back(xv[b]);

            double ext = score;
            for (const auto& gl : it->second) {
                long ls = lr_search::leaf_count_proxy(gl);
                ext += std::pow(static_cast<double>(ls), 1.15);
            }

            if (rule == KeepRule::Strict) {
                bool ok = true;
                for (const auto& L : letters) {
                    Poly p = degauge(L, xv[gauge_pos]);
                    const auto* ctx = p.ctx().raw();
                    if (fmpq_mpoly_is_fmpq(p.raw(), ctx)) continue;
                    const long d = p.degree_in_var(xv[bit]);
                    if (d <= 1) continue;
                    if (d == 2 && conic_rationalizable(p, xv[bit])) continue;
                    ok = false;
                    break;
                }
                if (!ok) continue;
                order.push_back(xv[bit]);
                dfs(bits | (1ull << bit), order, ext, gauge_pos, st);
                order.pop_back();
            } else {
                PathState next = st;
                if (!step_fr(letters, xv[bit], xv[gauge_pos], pending,
                        next))
                    continue;
                order.push_back(xv[bit]);
                dfs(bits | (1ull << bit), order, ext, gauge_pos, next);
                order.pop_back();
            }
        }
    }
};

}  // namespace

ScanResult find_lr_orders_scan(
    const std::vector<std::vector<Poly>>& group_polys,
    const std::vector<size_t>& xvar_indices,
    const std::vector<std::vector<ScanExponent>>& exps,
    KeepRule keep_rule,
    bool euler_filter,
    size_t max_orders)
{
    ScanResult res;
    res.projective = projective_input(group_polys, xvar_indices, exps);
    if (!res.projective) return res;
    const size_t n = xvar_indices.size();
    if (n < 2 || n > 62) return res;

    // augment each group with the boundary monomials (dpLungoCore seed)
    const PolyCtx& ctx = group_polys.front().front().ctx();
    std::vector<std::vector<Poly>> groups_aug = group_polys;
    for (auto& g : groups_aug)
        for (size_t v : xvar_indices)
            g.push_back(Poly(ctx, ctx.vars()[v]));
    const size_t G = groups_aug.size();

    ScanDriver drv{groups_aug, xvar_indices, keep_rule, max_orders,
        {}, &res};

    // ---- table fill to depth n-1 (the lr_search DP, chi filter
    //      optional), on the UNGAUGED system ----
    std::vector<ChiFilterCache> caches(euler_filter ? G : 0);
    drv.table[0] = groups_aug;
    for (size_t size = 1; size <= n - 1; ++size) {
        // enumerate subsets of this size (Gosper)
        uint64_t mask = (1ull << size) - 1;
        const uint64_t limit = 1ull << n;
        while (mask < limit) {
            std::vector<std::vector<Poly>> entry(G);
            for (size_t g = 0; g < G; ++g) {
                std::vector<std::vector<Poly>> pre;
                pre.reserve(size);
                for (size_t bit = 0; bit < n; ++bit) {
                    if (!(mask & (1ull << bit))) continue;
                    const uint64_t prev = mask ^ (1ull << bit);
                    auto it = drv.table.find(prev);
                    if (it == drv.table.end()) continue;
                    pre.push_back(lr_search::st_fubini_lr(
                        it->second[g], xvar_indices[bit]));
                }
                entry[g] = lr_search::intersect_proportional(pre);
                if (euler_filter && !entry[g].empty()) {
                    std::vector<size_t> subset_vars;
                    for (size_t b = 0; b < n; ++b)
                        if (mask & (1ull << b))
                            subset_vars.push_back(xvar_indices[b]);
                    entry[g] = chi_filter_letters(groups_aug[g],
                        subset_vars, entry[g], caches[g]);
                }
            }
            drv.table[mask] = std::move(entry);
            uint64_t c = mask & -mask;
            uint64_t r = mask + c;
            mask = (((r ^ mask) >> 2) / c) | r;
        }
    }

    // ---- per-gauge DFS over the shared table ----
    for (size_t gauge_pos = 0; gauge_pos < n; ++gauge_pos) {
        std::vector<size_t> order;
        PathState st;
        drv.dfs(0, order, 0.0, gauge_pos, st);
    }
    std::sort(res.orders.begin(), res.orders.end(),
        [](const ScanOrder& a, const ScanOrder& b) {
            return a.score < b.score;
        });
    return res;
}

}  // namespace lr_scan
}  // namespace hyperflint
