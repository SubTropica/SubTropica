// Factor-prediction table builder.
// See include/hyperflint/integrator/factor_table.hpp for scope and
// docs/superpowers/specs/2026-06-11-stfactorpredictor-design.md for the
// full contract.

#include "hyperflint/integrator/factor_table.hpp"

#include <flint/fmpq.h>
#include <flint/fmpq_mpoly.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "hyperflint/integrator/lr_search.hpp"

namespace hyperflint {
namespace factor_table {
namespace {

double now_s() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Intern the canonical proportionality rep of p.  Constants are the
// caller's responsibility: never interned (asserted by callers via
// is_fmpq guards).
size_t intern(FactorTable& t, const Poly& p) {
    Poly cp = p.canonical_prop_form();
    std::string key = cp.to_string();
    auto it = t.intern_idx.find(key);
    if (it != t.intern_idx.end()) return it->second;
    const size_t id = t.intern_strs.size();
    t.intern_strs.push_back(key);
    t.intern_polys.push_back(std::move(cp));
    t.intern_idx.emplace(std::move(key), id);
    return id;
}

// Multiply acc by the rational value of the constant poly `cpoly`,
// `exp` times (exp may be negative; cpoly must be a nonzero constant
// when exp < 0).
void fold_const(fmpq_t acc, const Poly& cpoly, long exp) {
    fmpq_t v;
    fmpq_init(v);
    fmpq_mpoly_get_fmpq(v, cpoly.raw(), cpoly.ctx().raw());
    if (exp >= 0) {
        for (long i = 0; i < exp; ++i) fmpq_mul(acc, acc, v);
    } else {
        for (long i = 0; i < -exp; ++i) fmpq_div(acc, acc, v);
    }
    fmpq_clear(v);
}

// Trial-divide `target` (non-constant, nonzero) by the pool; on a
// non-constant remainder, run the full factorization (factor_bases +
// repeated exact division, which also yields multiplicities and leaves
// the rational unit in the remainder).  Exponents accumulate into
// `exps` scaled by sign_exp (+1 numerator, -1 denominator); the
// terminal rational remainder folds into c_acc.  Returns the oop flag.
bool factor_into(FactorTable& t, Stats& st, const Poly& target,
                 const std::vector<size_t>& pool_ids,
                 std::map<size_t, long>& exps, fmpq_t c_acc,
                 long sign_exp) {
    Poly work = target;
    const double t0 = now_s();
    for (size_t pid : pool_ids) {
        if (work.is_fmpq()) break;
        const Poly& P = t.intern_polys[pid];
        if (P.is_fmpq()) continue;
        while (P.divides(work)) {  // P | work (note the argument order)
            work = work.divexact(P);
            exps[pid] += sign_exp;
            if (work.is_fmpq()) break;
        }
    }
    st.trial_s += now_s() - t0;
    if (work.is_fmpq()) {
        fold_const(c_acc, work, sign_exp);
        return false;
    }
    // Fallback: full factorization of the remainder.  factor_bases
    // returns one Poly per distinct irreducible base; dividing out the
    // canonical rep of each base to its maximal power leaves the
    // rational unit in `work`.
    // issue #52 round 3 (item 9): budget checkpoints -- this op body ran
    // unguarded (only st_fubini_lr had them).  No-ops when unset.
    lr_search::lr_budget_check_time("factor_table factor");
    lr_search::lr_budget_check_operand("factor_table factor", work.n_terms());
    const double t1 = now_s();
    std::vector<Poly> bases = hyperflint::factor_bases(work);
    for (const Poly& base : bases) {
        if (base.is_fmpq()) continue;
        Poly cb = base.canonical_prop_form();
        long e = 0;
        while (cb.divides(work)) {
            work = work.divexact(cb);
            ++e;
            if (work.is_fmpq()) break;
        }
        if (e > 0) exps[intern(t, cb)] += sign_exp * e;
    }
    st.fallback_s += now_s() - t1;
    if (!work.is_fmpq())
        throw std::runtime_error(
            "factor_table: fallback factorization left a non-constant "
            "remainder (internal error)");
    fold_const(c_acc, work, sign_exp);
    return true;
}

// Build c * Prod intern^exp for (Prod numers) / (Prod denoms).
// Numerator zero => "0" with no factors (spec 4.4).  Denominators must
// be nonzero (leading coefficients of genuine letters).
FactoredObject make_object(FactorTable& t, Stats& st,
                           const std::vector<Poly>& numers,
                           const std::vector<Poly>& denoms,
                           const std::vector<size_t>& pool_ids) {
    FactoredObject fo;
    std::map<size_t, long> exps;
    fmpq_t c;
    fmpq_init(c);
    fmpq_one(c);
    // fmpq_t has no RAII: clear on every exit path, including throws
    // from factor_into / string allocation (adversarial review fold).
    try {
        bool zero = false;
        for (const auto& nP : numers) {
            if (nP.is_zero()) { zero = true; break; }
            if (nP.is_fmpq()) { fold_const(c, nP, +1); continue; }
            fo.oop = factor_into(t, st, nP, pool_ids, exps, c, +1) || fo.oop;
        }
        if (!zero) {
            for (const auto& dP : denoms) {
                if (dP.is_zero())
                    throw std::runtime_error(
                        "factor_table: zero denominator (internal error)");
                if (dP.is_fmpq()) { fold_const(c, dP, -1); continue; }
                fo.oop = factor_into(t, st, dP, pool_ids, exps, c, -1) || fo.oop;
            }
        }
        if (zero) {
            fo.c = "0";
            fo.factors.clear();
        } else {
            char* cs = fmpq_get_str(nullptr, 10, c);
            fo.c = cs;
            flint_free(cs);
            for (const auto& [id, e] : exps)
                if (e != 0) fo.factors.emplace_back(id, e);
        }
    } catch (...) {
        fmpq_clear(c);
        throw;
    }
    fmpq_clear(c);
    return fo;
}

}  // namespace

FactorTable build(const PolyCtx& ctx,
                  const std::vector<std::vector<Poly>>& group_polys,
                  const std::vector<size_t>& order_indices,
                  bool algebraic_letters,
                  const Limits& limits) {
    FactorTable t;
    long max_deg = algebraic_letters ? 2L : 1L;
    if (const char* env = std::getenv("HF_LR_MAX_DEG");
        env != nullptr && algebraic_letters) {
        max_deg = std::atol(env);
        if (max_deg < 1) max_deg = 1;  // non-positive cap would silently
                                       // empty the table (review fold)
    }

    std::set<std::tuple<size_t, size_t, size_t>> pair_keys;  // (var, f, g)
    std::set<std::pair<size_t, size_t>> singleton_keys;      // (var, id)

    std::vector<std::vector<Poly>> cur = group_polys;
    for (size_t k = 0; k < order_indices.size(); ++k) {
        const size_t var_idx = order_indices[k];
        StageInfo stage;
        stage.var_idx = var_idx;
        const double tk = now_s();
        const std::vector<size_t> forbidden_after(
            order_indices.begin() + static_cast<long>(k) + 1,
            order_indices.end());
        std::vector<std::vector<Poly>> next(cur.size());
        std::set<size_t> stage_pool_set;
        for (size_t g = 0; g < cur.size(); ++g) {
            // 1. Admissible letters (shared helper; single-chain
            //    forbidden set = variables after this stage).
            std::vector<size_t> adm_ids;       // interned, all admissible
            std::vector<size_t> adm_deg1_ids;  // interned, deg-1 only
            // 2. Step output (next letters) and the stage pool:
            //    output factor bases + all current letters.
            std::vector<Poly> out =
                lr_search::st_fubini_lr(cur[g], var_idx, nullptr);
            std::vector<size_t> pool_ids;
            {
                std::set<size_t> seen;
                for (const auto& b : out) {
                    if (b.is_fmpq() || b.is_zero()) continue;
                    size_t id = intern(t, b);
                    if (seen.insert(id).second) pool_ids.push_back(id);
                }
                for (const auto& p : cur[g]) {
                    if (p.is_fmpq() || p.is_zero()) continue;
                    size_t id = intern(t, p);
                    if (seen.insert(id).second) pool_ids.push_back(id);
                }
            }
            for (size_t id : pool_ids) stage_pool_set.insert(id);
            for (const auto& p : cur[g]) {
                if (p.is_zero() || p.is_fmpq()) continue;
                const long d = p.degree_in_var(var_idx);
                if (d < 1) continue;
                if (!lr_search::lr_letter_admissible(
                        p, var_idx,
                        algebraic_letters ? forbidden_after
                                          : std::vector<size_t>{},
                        max_deg)) {
                    ++stage.n_inadmissible;
                    continue;
                }
                const size_t id = intern(t, p);
                adm_ids.push_back(id);
                // NOTE: contracts and degrees below are on the INTERNED
                // canonical rep, which is proportional to p, so the
                // degree is the same.
                if (t.intern_polys[id].degree_in_var(var_idx) == 1)
                    adm_deg1_ids.push_back(id);
            }
            // 3. Singletons.
            for (size_t id : adm_ids) {
                if (!singleton_keys.insert({var_idx, id}).second) continue;
                if (++t.stats.singletons_total > limits.max_singletons)
                    throw std::runtime_error(
                        "max_singletons exceeded (reached " +
                        std::to_string(t.stats.singletons_total) + ")");
                SingletonEntry se;
                se.var_idx = var_idx;
                se.id = id;
                const Poly L = t.intern_polys[id];  // copy: intern() below
                                                    // may reallocate
                se.deg = L.degree_in_var(var_idx);
                for (long pw = se.deg; pw >= 0; --pw) {
                    Poly cf = L.coefficient_of_var(var_idx, pw);
                    SingletonCoeff sc;
                    sc.power = pw;
                    sc.fo = make_object(t, t.stats, {cf}, {}, pool_ids);
                    if (sc.fo.oop) ++t.stats.oop;
                    se.coeffs.push_back(std::move(sc));
                }
                if (se.deg == 2) {
                    se.has_disc = true;
                    // issue #52 round 3 (item 9): operand fuse before the
                    // singleton discriminant.  Belt: under the current
                    // serialized build the same L already passed
                    // st_fubini_lr's per-poly guard (codex), so this fires
                    // only if the build order changes -- kept for
                    // robustness against exactly that.
                    lr_search::lr_budget_check_operand(
                        "factor_table singleton discriminant", L.n_terms());
                    se.disc = make_object(
                        t, t.stats, {L.discriminant_in_var(var_idx)}, {},
                        pool_ids);
                    if (se.disc.oop) ++t.stats.oop;
                }
                t.singletons.push_back(std::move(se));
                ++stage.n_singletons;
            }
            stage.admissible.push_back(std::move(adm_ids));
            // 4. Pairs (deg-1 only, within group, on interned reps).
            for (size_t i = 0; i < adm_deg1_ids.size(); ++i) {
                for (size_t j = i + 1; j < adm_deg1_ids.size(); ++j) {
                    size_t a = adm_deg1_ids[i];
                    size_t b = adm_deg1_ids[j];
                    if (a == b) continue;  // proportional letters
                    if (t.intern_strs[b] < t.intern_strs[a]) std::swap(a, b);
                    if (!pair_keys.insert({var_idx, a, b}).second) continue;
                    if (++t.stats.pairs_total > limits.max_pairs)
                        throw std::runtime_error(
                            "max_pairs exceeded (reached " +
                            std::to_string(t.stats.pairs_total) + ")");
                    const Poly F = t.intern_polys[a];  // copies: intern()
                    const Poly G = t.intern_polys[b];  // may reallocate
                    Poly lcF = F.coefficient_of_var(var_idx, 1);
                    Poly lcG = G.coefficient_of_var(var_idx, 1);
                    Poly N = lcG * F - lcF * G;  // == -Res_var(F,G)
                    PairEntry pe;
                    pe.var_idx = var_idx;
                    pe.f_id = a;
                    pe.g_id = b;
                    pe.diff =
                        make_object(t, t.stats, {N}, {lcF, lcG}, pool_ids);
                    if (pe.diff.oop) {
                        ++t.stats.oop;
                        ++t.stats.pair_fallbacks;
                    }
                    t.pairs.push_back(std::move(pe));
                    ++stage.n_pairs;
                }
            }
            next[g] = std::move(out);
        }
        stage.pool.assign(stage_pool_set.begin(), stage_pool_set.end());
        stage.t_build_s = now_s() - tk;
        t.stages.push_back(std::move(stage));
        cur = std::move(next);
    }
    // Deterministic emission order (spec 4.3): sort by key.
    std::sort(t.pairs.begin(), t.pairs.end(),
              [](const PairEntry& x, const PairEntry& y) {
                  return std::tie(x.var_idx, x.f_id, x.g_id) <
                         std::tie(y.var_idx, y.f_id, y.g_id);
              });
    std::sort(t.singletons.begin(), t.singletons.end(),
              [](const SingletonEntry& x, const SingletonEntry& y) {
                  return std::tie(x.var_idx, x.id) <
                         std::tie(y.var_idx, y.id);
              });
    return t;
}

}  // namespace factor_table
}  // namespace hyperflint
