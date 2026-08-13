// LR-order search.  C++ port of SubTropica's STFasterFubini2.
// See include/hyperflint/integrator/lr_search.hpp for scope.

#include "hyperflint/integrator/lr_search.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hyperflint/algebra/euler_filter.hpp"
#include "hyperflint/integrator/lr_scan.hpp"  // carry-discharge step_fr_judge

namespace hyperflint {
namespace lr_search {

namespace {

// HF_LR_TRACE (docs/env_flags.md, Track-doppio-port section): stderr
// profiling of the find_lr_orders DP.  Level 1 = per-size summaries +
// global wall accumulators (resultant / discriminant / leading-coeff /
// factor / dedup) + the 10 slowest st_fubini_lr calls; level 2 adds one
// line per step call.  Diagnostic only — no effect on results.  The DP
// is single-threaded (handler calls it synchronously), so plain
// namespace-level state is safe.
struct LrTrace {
    int level = 0;          // 0 = off
    double t_lc = 0, t_disc = 0, t_res = 0, t_factor = 0, t_dedup = 0;
    long n_res = 0, n_factor = 0, n_steps = 0;
    size_t max_res_terms = 0;       // largest pre-factor resultant
    // top-10 slowest step calls: (wall_s, subset bits, pivot, in, out)
    struct SlowCall { double wall; uint64_t bits; size_t pivot;
                      size_t n_in, n_out; };
    std::vector<SlowCall> slow;
    // context for the step call currently executing (set by the DP loop)
    uint64_t cur_bits = 0;
    size_t cur_pivot = 0;

    void note_slow(double wall, size_t n_in, size_t n_out) {
        SlowCall c{wall, cur_bits, cur_pivot, n_in, n_out};
        slow.push_back(c);
        std::sort(slow.begin(), slow.end(),
            [](const SlowCall& a, const SlowCall& b) {
                return a.wall > b.wall; });
        if (slow.size() > 10) slow.resize(10);
    }
};
LrTrace g_lr_trace;

double now_s() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Deficiency-3 cure (2026-06-06, notes/hf_lr_search_deficiencies.md):
// the DP's inputs are massively self-similar — Standard-closure faces
// carry O(100) near-identical groups, and the same (poly set, pivot)
// step and the same resultant/discriminant factorizations recur across
// subsets, pivots, and groups.  Two value-preserving memo layers:
//   step memo:   (pivot | input poly strings) -> full st_fubini_lr output
//   factor memo: poly string -> factor bases
// Both are pure caches (results byte-identical to the uncached path),
// reset at every find_lr_orders / find_lr_orders_scan entry so memory
// stays bounded per request.  Opt-out: HF_LR_STEP_MEMO=0 /
// HF_LR_FACTOR_MEMO=0 (docs/env_flags.md).  Single-threaded by the
// same argument as LrTrace.
struct LrMemo {
    bool step_on = true;
    bool factor_on = true;
    std::unordered_map<std::string, std::vector<Poly>> step;
    std::unordered_map<std::string, std::vector<Poly>> factors;
    long step_hit = 0, step_miss = 0, fac_hit = 0, fac_miss = 0;
};
LrMemo g_lr_memo;

// Budget safety net (2026-06-20, notes: lr_search budget).  A process-
// global, single-threaded (same argument as LrTrace / LrMemo: the
// handler calls find_lr_orders synchronously) deadline + operand-size
// guard, reset at every find_lr_orders entry from the two env vars.
// Both limits default to 0 = UNLIMITED, which makes every check below a
// no-op => the verdict path is BYTE-IDENTICAL to the pre-budget engine
// when neither env var is set (hard requirement).
//
//   HF_LR_TIME_BUDGET_S      double seconds; 0 = unlimited.  A
//                            steady_clock deadline; a time deadline
//                            CANNOT preempt the single in-flight FLINT
//                            op (the wedge), so it only catches BETWEEN
//                            ops (top of the DP subset loop, top of
//                            st_fubini_lr).
//   HF_LR_MAX_OPERAND_TERMS  size_t term count; 0 = unlimited.  THE
//                            load-bearing guard: checked on FLINT
//                            operands BEFORE each expensive op
//                            (discriminant / resultant / factor) so the
//                            monster op is never STARTED.
struct LrBudget {
    bool   time_on = false;
    double budget_s = 0.0;
    std::chrono::steady_clock::time_point start{};
    size_t max_operand_terms = 0;   // 0 = unlimited

    // Elapsed wall since `start`.  Cheap (one steady_clock read).
    double elapsed_s() const {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
    }
    // Time-deadline check, for the BETWEEN-ops call sites.  Throws when a
    // finite budget has been exceeded; a no-op when time_on is false.
    void check_time(const char* where) const {
        if (!time_on) return;
        const double e = elapsed_s();
        if (e >= budget_s) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "LR time budget exceeded: %.3fs elapsed >= %.3fs budget "
                "(at %s); set HF_LR_TIME_BUDGET_S=0 to disable",
                e, budget_s, where);
            throw LrBudgetExceeded(buf);
        }
    }
};
LrBudget g_lr_budget;

bool env_flag_off(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "0") == 0;
}

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

void SingCollector::observe(const Poly& b) {
    const auto* ctx = b.ctx().raw();
    // Numeric / zero factors carry no kinematic divisor.
    if (fmpq_mpoly_is_zero(b.raw(), ctx)) return;
    if (fmpq_mpoly_is_fmpq(b.raw(), ctx)) return;
    // Kinematic-only: the factor must depend on NONE of the integration
    // variables.  A factor that still carries an integration variable is
    // an integrand letter, not a kinematic divisor, and is excluded by
    // design (the order-resolved pipeline wants only the s-plane data).
    for (size_t u : b.used_var_indices()) {
        if (integration_vars.count(u)) return;
    }
    // Canonicalize with the SAME proportionality representative the
    // dedup uses (sign + leading-coefficient normalization over Q), so
    // s - 4*mm and -(s - 4*mm) collapse to one entry, matching the
    // engine's canonical form.
    std::string key = b.canonical_prop_form().to_string();
    if (seen.insert(key).second) ordered.push_back(std::move(key));
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

std::vector<Poly> st_fubini_lr(const std::vector<Poly>& polys, size_t var_idx,
                               SingCollector* sings) {
    if (polys.empty()) return {};
    const PolyCtx& pctx = polys.front().ctx();
    const auto* ctx = pctx.raw();

    // Budget time deadline (2026-06-20): check on every st_fubini_lr
    // entry, the coarsest BETWEEN-ops granularity.  No-op unless
    // HF_LR_TIME_BUDGET_S is a finite positive value.
    g_lr_budget.check_time("st_fubini_lr entry");
    // Operand-size limit (THE load-bearing guard, latched once here for
    // the per-op checks below; 0 = unlimited).
    const size_t max_operand_terms = g_lr_budget.max_operand_terms;

    LrTrace& tr = g_lr_trace;
    const bool tron = tr.level > 0;
    const double t_call0 = tron ? now_s() : 0.0;

    // Step memo lookup (deficiency-3 cure; see LrMemo).
    std::string step_key;
    if (g_lr_memo.step_on) {
        step_key.reserve(polys.size() * 32 + 16);
        step_key += std::to_string(var_idx);
        for (const auto& p : polys) {
            step_key += '|';
            step_key += p.to_string();
        }
        auto it = g_lr_memo.step.find(step_key);
        if (it != g_lr_memo.step.end()) {
            ++g_lr_memo.step_hit;
            // On a step-memo hit the factor loop is skipped, so observe
            // the cached step output here.  `out` already holds the
            // irreducible factor bases of this step (post-factor,
            // post-dedup), so this is the same multiset the miss path
            // would feed the collector --- collection is independent of
            // memo state.  Cheap: at most |out| observe() calls, only
            // when collecting (sings != nullptr).
            if (sings != nullptr) {
                for (const auto& b : it->second) sings->observe(b);
            }
            return it->second;
        }
        ++g_lr_memo.step_miss;
    }

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
            const double t0 = tron ? now_s() : 0.0;
            Poly lc = f.coefficient_of_var(var_idx, n);
            if (tron) tr.t_lc += now_s() - t0;
            if (!fmpq_mpoly_is_zero(lc.raw(), ctx)) temp.push_back(std::move(lc));
        }
        if (n >= 1) {
            // Operand-size trace (HF_LR_TRACE>=1) + budget guard
            // (2026-06-20): a discriminant is Res(f, f') and can be the
            // uninterruptible monster.  Print the input n_terms so a
            // threshold can be picked, then SKIP-BY-THROWING if the
            // operand exceeds the budget so the op is never started.
            const size_t f_nt = f.n_terms();
            if (tron) std::fprintf(stderr,
                "[lrtrace] disc operand n_terms=%zu (pivot=%zu deg=%ld)\n",
                f_nt, var_idx, n);
            if (max_operand_terms != 0 && f_nt > max_operand_terms) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                    "LR operand-size budget exceeded: discriminant input "
                    "n_terms=%zu > %zu (HF_LR_MAX_OPERAND_TERMS)",
                    f_nt, max_operand_terms);
                throw LrBudgetExceeded(buf);
            }
            const double t0 = tron ? now_s() : 0.0;
            Poly d = f.discriminant_in_var(var_idx);
            if (tron) tr.t_disc += now_s() - t0;
            if (!fmpq_mpoly_is_zero(d.raw(), ctx)) temp.push_back(std::move(d));
        }
        // Constant term f|_{var=0} = coefficient_of_var(var_idx, 0).  This is
        // the s[{0,f}] piece of Brown's polynomial reduction (HyperInt
        // cgSingleReduction*), the singularity at the var=0 endpoint of the
        // [0,infinity) integration.  Omitting it under-approximates the
        // Landau set and yields FALSE-POSITIVE LR orders: a downstream
        // resultant against this term can be the only source of a
        // degree>=2 obstruction in a later integration variable (it is the
        // leading coeff only for n==0, which the n>=0 block above already
        // covers, so guard on n>=1 to avoid double-counting).
        if (n >= 1) {
            const double t0 = tron ? now_s() : 0.0;
            Poly ct = f.coefficient_of_var(var_idx, 0);
            if (tron) tr.t_lc += now_s() - t0;
            if (!fmpq_mpoly_is_zero(ct.raw(), ctx) &&
                !fmpq_mpoly_is_fmpq(ct.raw(), ctx))
                temp.push_back(std::move(ct));
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
            // Operand-size trace (HF_LR_TRACE>=1) + budget guard
            // (2026-06-20): the resultant is the FFT poly-mul that
            // dominates the wedge.  Cost scales with the operand sizes,
            // so guard on their PRODUCT (computed in a 128-bit
            // accumulator so the multiply cannot overflow for any
            // realistic term count).  Print both inputs + the product so
            // a threshold can be chosen, then skip-by-throwing if over
            // budget BEFORE the op is started.
            const size_t fi_nt = fi.n_terms();
            const size_t fj_nt = fj.n_terms();
            const unsigned __int128 prod =
                static_cast<unsigned __int128>(fi_nt) *
                static_cast<unsigned __int128>(fj_nt);
            if (tron) std::fprintf(stderr,
                "[lrtrace] res operands n_terms=%zu x %zu = %llu "
                "(pivot=%zu)\n", fi_nt, fj_nt,
                static_cast<unsigned long long>(
                    prod > static_cast<unsigned __int128>(~0ull)
                        ? ~0ull : static_cast<unsigned long long>(prod)),
                var_idx);
            if (max_operand_terms != 0 &&
                prod > static_cast<unsigned __int128>(max_operand_terms)) {
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                    "LR operand-size budget exceeded: resultant inputs "
                    "n_terms=%zu x %zu (product > %zu, "
                    "HF_LR_MAX_OPERAND_TERMS)",
                    fi_nt, fj_nt, max_operand_terms);
                throw LrBudgetExceeded(buf);
            }
            const double t0 = tron ? now_s() : 0.0;
            Poly r = fi.resultant(fj, var_idx);
            if (tron) {
                tr.t_res += now_s() - t0;
                ++tr.n_res;
                tr.max_res_terms = std::max(tr.max_res_terms, r.n_terms());
            }
            if (!fmpq_mpoly_is_zero(r.raw(), ctx)) temp.push_back(std::move(r));
        }
    }

    // Factor each temp entry, flatten factor bases.  The factor memo
    // collapses recurring resultants/discriminants (the same algebraic
    // objects recur across subsets, pivots, and near-identical groups).
    std::vector<Poly> factored;
    factored.reserve(temp.size() * 2);
    for (const auto& p : temp) {
        if (fmpq_mpoly_is_zero(p.raw(), ctx)) continue;
        if (fmpq_mpoly_is_fmpq(p.raw(), ctx)) continue;
        // Operand-size trace (HF_LR_TRACE>=1) + budget guard (2026-06-20):
        // fmpq_mpoly_factor on a huge resultant/discriminant is itself an
        // uninterruptible op.  Guard on the factor INPUT size BEFORE the
        // op (covering both the memo-hit-miss and memo-off paths below;
        // bailing on an oversized poly is correct regardless of memo
        // state).  Print the input n_terms for threshold selection.
        const size_t p_nt = p.n_terms();
        if (tron) std::fprintf(stderr,
            "[lrtrace] factor operand n_terms=%zu\n", p_nt);
        if (max_operand_terms != 0 && p_nt > max_operand_terms) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "LR operand-size budget exceeded: factor input n_terms=%zu "
                "> %zu (HF_LR_MAX_OPERAND_TERMS)",
                p_nt, max_operand_terms);
            throw LrBudgetExceeded(buf);
        }
        if (g_lr_memo.factor_on) {
            std::string fkey = p.to_string();
            auto it = g_lr_memo.factors.find(fkey);
            if (it != g_lr_memo.factors.end()) {
                ++g_lr_memo.fac_hit;
                for (const auto& b : it->second) factored.push_back(b);
                continue;
            }
            ++g_lr_memo.fac_miss;
            const double t0 = tron ? now_s() : 0.0;
            auto bases = factor_bases(p);
            if (tron) { tr.t_factor += now_s() - t0; ++tr.n_factor; }
            for (const auto& b : bases) factored.push_back(b);
            g_lr_memo.factors.emplace(std::move(fkey), std::move(bases));
        } else {
            const double t0 = tron ? now_s() : 0.0;
            auto bases = factor_bases(p);
            if (tron) { tr.t_factor += now_s() - t0; ++tr.n_factor; }
            for (auto& b : bases) factored.push_back(std::move(b));
        }
    }

    const double t_d0 = tron ? now_s() : 0.0;
    std::vector<Poly> out = dedup_proportional(factored);
    // Per-face kinematic-divisor collection (order-resolved sings).
    // Observe the step's deduped factor-base set: every irreducible
    // factor that is free of all integration variables is recorded.
    // `out` is the SAME object the step memo stores and the memo-hit
    // path observes, so collection is identical whether or not the step
    // is served from cache.  observe() itself filters non-kinematic /
    // numeric factors, so this loop is a no-op for integrand letters.
    if (sings != nullptr) {
        for (const auto& b : out) sings->observe(b);
    }
    if (tron) {
        tr.t_dedup += now_s() - t_d0;
        ++tr.n_steps;
        const double wall = now_s() - t_call0;
        if (tr.slow.size() < 10 || wall > tr.slow.back().wall)
            tr.note_slow(wall, polys.size(), out.size());
        if (tr.level >= 2) {
            std::fprintf(stderr,
                "[lrtrace2] bits=%llx pivot=%zu in=%zu out=%zu wall=%.3fs\n",
                (unsigned long long) tr.cur_bits, tr.cur_pivot,
                polys.size(), out.size(), wall);
        }
    }
    if (g_lr_memo.step_on) g_lr_memo.step.emplace(std::move(step_key), out);
    return out;
}

void reset_lr_memos() {
    g_lr_memo = LrMemo{};
    g_lr_memo.step_on   = !env_flag_off("HF_LR_STEP_MEMO");
    g_lr_memo.factor_on = !env_flag_off("HF_LR_FACTOR_MEMO");
}

void reset_lr_trace() { g_lr_trace = LrTrace{}; }

void reset_lr_budget() {
    // Read the two env vars and reset the process-global budget.  Both
    // default to 0 = UNLIMITED, so an unset environment makes every
    // downstream check a no-op (the verdict path stays byte-identical).
    // The time deadline starts from NOW (steady_clock); the size guard
    // is a static term-count threshold latched for st_fubini_lr.  Single-
    // threaded by the same argument as LrTrace / LrMemo.
    g_lr_budget = LrBudget{};
    g_lr_budget.start = std::chrono::steady_clock::now();
    const char* tb_env = std::getenv("HF_LR_TIME_BUDGET_S");
    if (tb_env != nullptr && *tb_env != '\0') {
        const double b = std::atof(tb_env);
        if (b > 0.0) { g_lr_budget.time_on = true; g_lr_budget.budget_s = b; }
    }
    const char* mot_env = std::getenv("HF_LR_MAX_OPERAND_TERMS");
    if (mot_env != nullptr && *mot_env != '\0') {
        const long long m = std::atoll(mot_env);
        if (m > 0) g_lr_budget.max_operand_terms = static_cast<size_t>(m);
    }
}

// issue #52 round 3 (item 9): exported checkpoints for op bodies outside
// st_fubini_lr.  No-ops when the env vars are unset (byte-identical
// requirement); throw LrBudgetExceeded otherwise, which every bridge
// handler now maps to the structured budget_exceeded response.
void lr_budget_check_time(const char* where) {
    g_lr_budget.check_time(where);
}

void lr_budget_check_operand(const char* what, std::size_t n_terms) {
    const size_t cap = g_lr_budget.max_operand_terms;
    if (cap != 0 && n_terms > cap) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "LR operand-size budget exceeded: %s input n_terms=%zu > %zu "
            "(HF_LR_MAX_OPERAND_TERMS)", what, n_terms, cap);
        throw LrBudgetExceeded(buf);
    }
}


bool lr_letter_admissible(const Poly& p, size_t var_idx,
                          const std::vector<size_t>& forbidden_after,
                          long max_deg) {
    const long d = p.degree_in_var(var_idx);
    if (d < 1 || d > max_deg) return false;
    if (d >= 2) {
        // Forbidden-var check (parity with the runtime guard in
        // linear_factors.cpp:1444-1460): a deg-2 letter whose Wm/Wp
        // definition would use a not-yet-integrated Feynman parameter
        // is refused at runtime; model the same rejection here.
        std::vector<size_t> used = p.used_var_indices();
        for (size_t v : forbidden_after) {
            if (v == var_idx) continue;  // structural no-op, kept for parity
            for (size_t u : used) {
                if (u == v) return false;
            }
        }
    }
    return true;
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

// ---------------------------------------------------------------------
// Carry-discharge DFS over find_lr_orders' (un-pruned) set_table.
//
// This is the gauge-FREE analog of lr_scan's per-gauge DFS: it integrates
// ALL n variables (no Cheng-Wu gauge — the production per-gauge integrand
// is already gauge-fixed upstream, so find_lr_orders sees exactly one
// gauge's system) and judges each step with the SHARED carry-discharge
// primitive lr_scan::step_fr_judge (gauge == kNoGauge).  set_table[bits]
// is the path-INDEPENDENT S-marginal letter set per group; the PathState
// is the path-DEPENDENT carried-obligation state.  That split is exactly
// why the carry verdict cannot ride the best-score subset-DP memo: two
// orders reaching the same `bits` may carry different obligations.
//
// We keep the lexicographic (carried_sqrts, nonexec, score)-minimal
// admissible full-order (spec 4a.2 + 2026-06-11-carry-phase2 §4), with
// its carried-sqrt profile and the deg-2 letters encountered along it
// (for root_polys).  Score is the same accumulator as the DP / scan:
// sum over groups of leaf_count_proxy(parent-subset letters)^1.15 at
// each step.
// Lexicographic (carried_sqrts, nonexec, score) strict improvement
// test — single source of truth for CarryDfs' leaf keep and its
// branch-and-bound prune (the prune is its negation; keeping them
// mirrored by hand risks silent desync).  The middle coordinate (spec
// P2 §4) prefers, at equal carried count, an order whose every
// obligation is executable-shaped (single integration variable,
// degree <= 2): carried orders compete as execution CANDIDATES, and a
// cheaper executor-inadmissible order must not shadow an executable
// one.  All three coordinates are monotone non-decreasing along a
// path (score additive nonnegative; nsq increment-only; nonexec a
// monotone OR — obligations are never un-minted), so the prune stays
// sound by the Phase-1 argument.
static bool lex_beats(unsigned long nsq_a, bool nonexec_a, double score_a,
                      unsigned long nsq_b, bool nonexec_b, double score_b) {
    if (nsq_a != nsq_b) return nsq_a < nsq_b;
    if (nonexec_a != nonexec_b) return !nonexec_a;
    return score_a < score_b;
}

struct CarryDfs {
    // set_table[bits] -> per-group letters (G lists), un-pruned.
    const std::unordered_map<uint64_t,
        std::vector<std::vector<Poly>>>& set_table;
    const std::vector<size_t>& xvar_indices;  // bit -> ctx index
    size_t n = 0;
    double INF = std::numeric_limits<double>::infinity();

    // best-so-far
    bool found = false;
    double best_score = std::numeric_limits<double>::infinity();
    std::vector<size_t> best_order;     // ctx indices
    std::vector<Poly>   best_roots;     // deg-2 letters along best path
    unsigned long best_nsq = 0, best_nkin = 0, best_ntq = 0;
    bool best_nonexec = false;          // middle lex coordinate (spec P2 §4)

    // diagnostic levers, latched once (getenv is not for hot loops)
    const bool leaf_trace = std::getenv("HF_CARRY_LEAF_TRACE") != nullptr;
    const bool no_prune   = std::getenv("HF_CARRY_NO_PRUNE") != nullptr;

    // current path's deg-2-letter accumulator (parallel to `order`)
    void dfs(uint64_t bits, std::vector<size_t>& order, double score,
             std::vector<Poly>& roots, const lr_scan::PathState& st) {
        if (order.size() == n) {
            // Leaf-profile trace (diagnostic, env-gated): the gate-4b /
            // gate-8 derivation instrument.  Combine with
            // HF_CARRY_NO_PRUNE=1 to enumerate EVERY admissible leaf
            // (the branch-and-bound below otherwise skips provably-worse
            // branches; disabling it never changes the kept best, only
            // the visit count).
            if (leaf_trace) {
                std::string ord;
                for (size_t v : order) {
                    if (!ord.empty()) ord += ",";
                    ord += std::to_string(v);
                }
                std::fprintf(stderr,
                    "[carry-leaf] order=[%s] score=%.4f nsq=%lu nkin=%lu "
                    "ntq=%lu nonexec=%d\n",
                    ord.c_str(), score, st.nsq, st.nkin, st.ntq,
                    (int) st.nonexec);
            }
            // Lexicographic (carried_sqrts, nonexec, score) minimum
            // (spec 2026-06-10-carry-option-design.md 4a.2 + carry-phase2
            // §4): an admissible uncarried order must never be shadowed
            // by a cheaper carried one, and an executable-shaped carried
            // order must never be shadowed by a cheaper executor-
            // inadmissible one at equal carried count.
            if (!found || lex_beats(st.nsq, st.nonexec, score,
                                    best_nsq, best_nonexec, best_score)) {
                found = true;
                best_score = score;
                best_order = order;
                best_roots = roots;
                best_nsq = st.nsq;
                best_nkin = st.nkin;
                best_ntq = st.ntq;
                best_nonexec = st.nonexec;
            }
            return;
        }
        // Branch-and-bound on the SAME lexicographic key: all three
        // coordinates are monotone non-decreasing along a path (score
        // is additive with nonnegative increments; st.nsq is only ever
        // incremented; st.nonexec is a monotone OR, lr_scan.cpp
        // PathState fold), so a partial path whose (nsq, nonexec,
        // score) already fails to lex-beat the incumbent cannot
        // complete to a leaf that does.  HF_CARRY_NO_PRUNE=1 disables
        // the cut for leaf-enumeration diagnostics (selection unchanged
        // — the prune only skips provably-worse branches).
        if (found && !lex_beats(st.nsq, st.nonexec, score,
                                best_nsq, best_nonexec, best_score) &&
            !no_prune)
            return;

        auto it_parent = set_table.find(bits);
        if (it_parent == set_table.end()) return;  // shouldn't happen
        const std::vector<std::vector<Poly>>& parent = it_parent->second;
        const size_t G = parent.size();

        for (size_t bit = 0; bit < n; ++bit) {
            if (bits & (1ull << bit)) continue;
            const size_t var_idx = xvar_indices[bit];

            // pending AFTER this step: unset bits except the pivot (no
            // gauge to exclude — kNoGauge).
            std::vector<size_t> pending;
            pending.reserve(n);
            for (size_t b = 0; b < n; ++b)
                if (b != bit && !(bits & (1ull << b)))
                    pending.push_back(xvar_indices[b]);

            // letters of the PARENT subset, all groups flattened (same
            // source lr_scan's DFS uses), and the score extension (same
            // accumulator as the DP / scan: per-group leaf_count^1.15).
            std::vector<Poly> letters;
            double ext = score;
            std::vector<Poly> step_roots;  // deg-2-in-pivot letters here
            for (size_t g = 0; g < G; ++g) {
                const auto& gl = parent[g];
                letters.insert(letters.end(), gl.begin(), gl.end());
                ext += std::pow(
                    static_cast<double>(leaf_count_proxy(gl)), 1.15);
                for (const auto& p : gl)
                    if (p.degree_in_var(var_idx) == 2)
                        step_roots.push_back(p);
            }

            lr_scan::PathState next = st;
            if (!lr_scan::step_fr_judge(letters, var_idx,
                    lr_scan::kNoGauge, pending, xvar_indices, next))
                continue;

            order.push_back(var_idx);
            const size_t roots_mark = roots.size();
            roots.insert(roots.end(),
                std::make_move_iterator(step_roots.begin()),
                std::make_move_iterator(step_roots.end()));
            dfs(bits | (1ull << bit), order, ext, roots, next);
            // Pop the step's roots back off (Poly is not default-
            // constructible, so erase rather than resize).
            roots.erase(roots.begin() + static_cast<std::ptrdiff_t>(roots_mark),
                        roots.end());
            order.pop_back();
        }
    }
};

}  // namespace

LrResult find_lr_orders(
    const std::vector<std::vector<Poly>>& group_polys,
    const std::vector<size_t>& xvar_indices,
    bool allow_algebraic_letters,
    SingCollector* sings,
    bool carry_discharge,
    double score_prune_factor) {
    // Carry-discharge (Doppio FindRoots) tier is only meaningful when
    // deg-2 letters are admitted: with deg<=1 letters there is no
    // sqrt-obligation to carry, so the flag is a no-op there and the
    // classic subset-DP runs unchanged.  When active, the full
    // (un-pruned) set_table must survive the size loop so the per-path
    // DFS can read every parent subset, and the Strict early-NOLR exit
    // must be suppressed (carry can rescue a Strict-NOLR face).
    const bool do_carry = allow_algebraic_letters && carry_discharge;
    // Seed the kinematic-divisor collector (if requested) with the
    // integration-variable index set, so st_fubini_lr can decide which
    // factors are kinematic-only.  Empty/degenerate early returns below
    // leave the collector empty, which is correct (no walk => no
    // divisors).
    if (sings != nullptr) {
        sings->integration_vars.clear();
        for (size_t v : xvar_indices) sings->integration_vars.insert(v);
    }
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

    // Per-request memo lifetime (bounded memory; captures all
    // within-face duplication across subsets, pivots, groups).
    reset_lr_memos();
    if (euler_filter_on) reset_chi_filter_stats();

    // HF_LR_TRACE diagnostic profiling (docs/env_flags.md).  Reset the
    // accumulators on every entry so repeated calls in one process
    // (LibraryLink transport) profile independently.
    {
        const char* tr_env = std::getenv("HF_LR_TRACE");
        g_lr_trace = LrTrace{};
        if (tr_env != nullptr && *tr_env != '\0'
            && std::strcmp(tr_env, "0") != 0) {
            g_lr_trace.level = std::atoi(tr_env) >= 2 ? 2 : 1;
            std::fprintf(stderr,
                "[lrtrace] find_lr_orders: G=%zu n=%zu algebraic=%d\n",
                G, n, allow_algebraic_letters ? 1 : 0);
        }
    }

    // Budget safety net (2026-06-20): reset the process-global budget
    // from the environment on every entry, mirroring how HF_LR_TRACE is
    // read just above.  Both limits default to 0 = UNLIMITED, so an
    // unset environment makes every downstream check a no-op (the
    // verdict path stays byte-identical).  The steady_clock deadline
    // starts here, after the (cheap) setup but before the DP work.
    reset_lr_budget();
    if (g_lr_trace.level > 0 &&
        (g_lr_budget.time_on || g_lr_budget.max_operand_terms != 0)) {
        std::fprintf(stderr,
            "[lrtrace] budget: time_budget_s=%s max_operand_terms=%zu\n",
            g_lr_budget.time_on
                ? std::to_string(g_lr_budget.budget_s).c_str() : "off",
            g_lr_budget.max_operand_terms);
    }

    const double INF = std::numeric_limits<double>::infinity();

    // Seed: set[g][{}] = group_polys[g] for every g, orders[{}] = ({}, 0).
    set_table[0] = group_polys;
    orders_table[0] = LrResult{{}, 0.0, {}};

    // Score-based branch-and-bound (ScorePruneFactor).  A subset whose
    // best-order score exceeded score_prune_factor * the best score at its
    // size is recorded here; it is never used as a parent, so its (most
    // expensive) next-size reductions are never computed.  Inactive when
    // score_prune_factor is +inf (the default).
    std::unordered_set<uint64_t> score_pruned;
    const bool prune_on =
        score_prune_factor < INF && score_prune_factor > 0.0 && !do_carry;

    // DP over subset size.
    for (size_t size = 1; size <= n; ++size) {
        auto subsets = subsets_of_size(n, size);
        bool any_live_at_size = false;  // any non-NOLR order at this size
        for (uint64_t bits : subsets) {
            // Budget time deadline (2026-06-20): the per-subset BETWEEN-
            // ops checkpoint.  No-op unless HF_LR_TIME_BUDGET_S is a
            // finite positive value (g_lr_budget.time_on).
            g_lr_budget.check_time("find_lr_orders DP subset loop");
            // ScorePruneFactor: if EVERY parent of this subset was pruned,
            // it is unreachable via a surviving order — skip its (most
            // expensive) Step A reduction entirely, and propagate the prune.
            if (prune_on) {
                bool any_live_parent = false;
                for (size_t bit = 0; bit < n; ++bit) {
                    if (!(bits & (1ull << bit))) continue;
                    if (!score_pruned.count(bits ^ (1ull << bit))) {
                        any_live_parent = true;
                        break;
                    }
                }
                if (!any_live_parent) {
                    score_pruned.insert(bits);
                    continue;
                }
            }
            // Step A: for each group g, build preSTable (one list per
            // pivot bit v ∈ bits), then intersect.
            std::vector<std::vector<Poly>> set_for_bits(G);
            for (size_t g = 0; g < G; ++g) {
                std::vector<std::vector<Poly>> preTable;
                preTable.reserve(size);
                for (size_t bit = 0; bit < n; ++bit) {
                    if (!(bits & (1ull << bit))) continue;
                    const uint64_t prev_bits = bits ^ (1ull << bit);
                    // ScorePruneFactor: a score-pruned parent contributes no
                    // pivot path (do not extend the expensive branch).
                    if (prune_on && score_pruned.count(prev_bits)) continue;
                    auto it = set_table.find(prev_bits);
                    if (it == set_table.end()) {
                        // Parent subset was dropped by memory pruning or
                        // never computed — fatal in single-group MVP (but a
                        // benign skip once score pruning is active).
                        if (prune_on) continue;
                        throw std::runtime_error(
                            "find_lr_orders: missing parent subset state");
                    }
                    const auto& prev_polys = it->second[g];
                    if (g_lr_trace.level > 0) {
                        g_lr_trace.cur_bits = bits;
                        g_lr_trace.cur_pivot = xvar_indices[bit];
                    }
                    preTable.push_back(
                        st_fubini_lr(prev_polys, xvar_indices[bit], sings));
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
                long max_deg = allow_algebraic_letters ? 2L : 1L;
                {
                    const char* env = std::getenv("HF_LR_MAX_DEG");
                    if (env && allow_algebraic_letters) max_deg = std::atol(env);
                }
                const auto& prev_set_all = set_table[prev_bits];
                bool all_linear = true;
                double extension_score = prev.score;
                std::vector<Poly> step_root_polys;
                for (size_t g = 0; g < G; ++g) {
                    const auto& gpolys = prev_set_all[g];
                    for (const auto& p : gpolys) {
                        const long d = p.degree_in_var(var_idx);
                        // Pivot-free letters pass through the step.
                        if (d < 1) continue;
                        // Shared helper (factor-table spec): degree
                        // window + deg-2 forbidden-var guard; the
                        // forbidden set is empty when algebraic
                        // letters are off, and d > max_deg = 1 already
                        // rejects deg-2 there.
                        if (!lr_letter_admissible(p, var_idx,
                                forbidden_after_step, max_deg)) {
                            all_linear = false;
                            break;
                        }
                        if (d >= 2 && allow_algebraic_letters) {
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
            if (!best.nolr()) any_live_at_size = true;
            orders_table[bits] = best;
        }

        // ScorePruneFactor: mark, for the NEXT size, every subset whose
        // best-order score exceeds score_prune_factor * (the lowest score
        // among non-pruned subsets at THIS size).  NOLR / unreachable
        // subsets are pruned too (they cannot lead to an LR order).  The
        // full subset (size == n) has no children, so there is nothing to
        // prune for it.
        if (prune_on && size < n) {
            double best_score = INF;
            for (uint64_t bits : subsets) {
                if (score_pruned.count(bits)) continue;
                auto it = orders_table.find(bits);
                if (it != orders_table.end() && !it->second.nolr())
                    best_score = std::min(best_score, it->second.score);
            }
            if (best_score < INF) {
                const double cutoff = score_prune_factor * best_score;
                for (uint64_t bits : subsets) {
                    if (score_pruned.count(bits)) continue;
                    auto it = orders_table.find(bits);
                    if (it == orders_table.end() || it->second.nolr()
                        || it->second.score > cutoff)
                        score_pruned.insert(bits);
                }
            }
        }

        // Early NOLR exit (2026-06-06, value-preserving): NOLR
        // propagates — an order at size k+1 extends a non-NOLR parent
        // of size k, so if EVERY subset at this size is NOLR the final
        // verdict is NOLR with certainty.  Returning now skips the
        // remaining (and most expensive) levels of the DP; the result
        // is identical to running them.  This is what made genuinely
        // NOLR faces burn the full 2^n table before reporting.
        //
        // SUPPRESSED under carry-discharge: `any_live_at_size` is the
        // STRICT (terminal-only) verdict; a face that is Strict-NOLR at
        // some size can still be carried-LR, so exiting here would
        // under-find.  The carry DFS below needs the full set_table.
        if (!do_carry && !any_live_at_size) {
            if (g_lr_trace.level > 0) {
                std::fprintf(stderr,
                    "[lrtrace] early NOLR exit at size=%zu/%zu "
                    "(all subsets NOLR)\n", size, n);
                std::fflush(stderr);
            }
            return LrResult{{}, INF, {}};
        }

        // Memory pruning: drop size-(size-1) set entries, they're no
        // longer referenced at size-(size+1) iterations.
        //
        // SUPPRESSED under carry-discharge: the per-path DFS reads the
        // parent subset's letters at EVERY depth (size 0..n-1), so the
        // whole table must survive the size loop.  In the production
        // per-gauge regime n is the post-gauge variable count (small),
        // so retaining 2^n subset states is acceptable — and the DP
        // already materializes set_table[bits] for all bits at each
        // size before this prune, so suppressing it only changes the
        // table's LIFETIME, not its peak per-size width.
        if (!do_carry && size >= 1) {
            auto prev_subsets = subsets_of_size(n, size - 1);
            for (uint64_t k : prev_subsets) set_table.erase(k);
        }

        if (g_lr_trace.level > 0) {
            // Largest surviving letter set at this size (post-intersect).
            size_t max_set = 0, max_terms = 0;
            for (const auto& kv : set_table) {
                for (const auto& gl : kv.second) {
                    max_set = std::max(max_set, gl.size());
                    for (const auto& p : gl)
                        max_terms = std::max(max_terms, p.n_terms());
                }
            }
            std::fprintf(stderr,
                "[lrtrace] size=%zu/%zu done: steps=%ld res=%ld(%.1fs) "
                "disc=%.1fs lc=%.1fs factor=%ld(%.1fs) dedup=%.1fs "
                "maxset=%zu maxterms=%zu maxresterms=%zu "
                "stepmemo=%ld/%ld facmemo=%ld/%ld\n",
                size, n, g_lr_trace.n_steps, g_lr_trace.n_res,
                g_lr_trace.t_res, g_lr_trace.t_disc, g_lr_trace.t_lc,
                g_lr_trace.n_factor, g_lr_trace.t_factor,
                g_lr_trace.t_dedup, max_set, max_terms,
                g_lr_trace.max_res_terms,
                g_lr_memo.step_hit, g_lr_memo.step_hit + g_lr_memo.step_miss,
                g_lr_memo.fac_hit, g_lr_memo.fac_hit + g_lr_memo.fac_miss);
            if (euler_filter_on) {
                const ChiFilterStats cs = chi_filter_stats();
                std::fprintf(stderr,
                    "[lrtrace]   chi: judged=%lu dropped=%lu "
                    "boundary_exempt=%lu msolve_calls=%lu\n",
                    cs.judged, cs.dropped, cs.boundary_exempt,
                    cs.msolve_calls);
            }
            std::fflush(stderr);
        }
    }

    if (g_lr_trace.level > 0) {
        std::fprintf(stderr, "[lrtrace] slowest step calls:\n");
        for (const auto& c : g_lr_trace.slow) {
            std::fprintf(stderr,
                "[lrtrace]   wall=%.2fs bits=%llx pivot=%zu in=%zu out=%zu\n",
                c.wall, (unsigned long long) c.bits, c.pivot,
                c.n_in, c.n_out);
        }
        std::fflush(stderr);
    }

    // Carry-discharge (Doppio FindRoots) verdict: a per-path DFS over the
    // full (un-pruned) set_table, judging each step with the shared
    // step_fr_judge primitive (gauge-free).  Strictly supersedes the
    // Strict subset-DP result: every Strict-admissible order is also
    // carry-admissible (no obligation is ever generated when every letter
    // is already terminal/conic/deg<=1), so the carry DFS finds at least
    // what the DP found and possibly more.  Score-minimal order wins,
    // matching the DP's MinimalBy.  Empty set_table guards (n==0 etc.)
    // were handled by the early returns at the top of the function.
    if (do_carry) {
        // n >= 1 here (n == 0 returned early).  Seed bits=0.
        CarryDfs drv{set_table, xvar_indices, n, INF};
        std::vector<size_t> order;
        std::vector<Poly> roots;
        lr_scan::PathState st0;
        drv.dfs(0ull, order, 0.0, roots, st0);
        if (!drv.found) return LrResult{{}, INF, {}};
        LrResult out;
        out.order = std::move(drv.best_order);
        out.score = drv.best_score;
        out.root_polys = std::move(drv.best_roots);
        out.carried_sqrts = drv.best_nsq;
        out.kin_sqrts = drv.best_nkin;
        out.terminal_quads = drv.best_ntq;

        // Leaf-replay obligation emission (spec 2026-06-11-carry-phase2
        // §3.1).  The DFS records no per-node polynomials (PathState is
        // value-copied on every descent; an append-only vector<Poly>
        // would repeat the dark-mass regression, poly.hpp:180-189), so
        // the obligations are materialized ONCE here: replay the winning
        // best_order through the SAME deterministic primitive
        // (step_fr_judge over the same un-pruned set_table letters) and
        // collect each obligation into a path-CUMULATIVE ledger keyed on
        // the canonical proportionality form.  An obligation that is
        // minted, discharged, and later re-produced re-fires ++nsq in
        // the live DFS (per-step kept_keys re-seed, step_fr_judge) but
        // enters the ledger once: obligation_polys.size() <=
        // carried_sqrts, with equality iff no remint occurred.  Mints
        // are captured by scanning st.carried AFTER each step — a fresh
        // mint survives in `carried` at least until the NEXT step's
        // discharge filter, so the post-step scan sees every mint, and
        // the ledger dedup makes the scan idempotent across steps.
        {
            // ctx index -> bit position (inverse of xvar_indices)
            size_t max_idx = 0;
            for (size_t v : xvar_indices) max_idx = std::max(max_idx, v);
            std::vector<size_t> bit_of(max_idx + 1, SIZE_MAX);
            for (size_t b = 0; b < n; ++b) bit_of[xvar_indices[b]] = b;

            uint64_t bits = 0ull;
            lr_scan::PathState replay;
            std::set<std::string> ledger;
            bool replay_ok = true;
            for (size_t k = 0; k < out.order.size(); ++k) {
                const size_t var_idx = out.order[k];
                const size_t bit = bit_of[var_idx];
                std::vector<size_t> pending;
                pending.reserve(n);
                for (size_t b = 0; b < n; ++b)
                    if (b != bit && !(bits & (1ull << b)))
                        pending.push_back(xvar_indices[b]);
                const auto it_parent = set_table.find(bits);
                if (it_parent == set_table.end()) { replay_ok = false; break; }
                std::vector<Poly> letters;
                for (const auto& gl : it_parent->second)
                    letters.insert(letters.end(), gl.begin(), gl.end());
                if (!lr_scan::step_fr_judge(letters, var_idx,
                        lr_scan::kNoGauge, pending, xvar_indices, replay)) {
                    replay_ok = false;
                    break;
                }
                for (const auto& c : replay.carried)
                    if (ledger.insert(
                            c.canonical_prop_form().to_string()).second) {
                        out.obligation_polys.push_back(c);
                        if (std::getenv("HF_CARRY_REPLAY_TRACE")) {
                            std::fprintf(stderr,
                                "[carry-replay] step %zu (pivot ctx %zu): "
                                "ledger += %s\n",
                                k + 1, var_idx,
                                c.canonical_prop_form().to_string().c_str());
                        }
                    }
                bits |= (1ull << bit);
            }
            // Both failure legs are unreachable for a DFS-admissible
            // order (same primitive, same table, same flatten order);
            // if one fires, or the replayed profile disagrees with the
            // DFS's, the emitted list cannot be trusted — emit NOTHING
            // and say so loudly.  CONSUMER CONTRACT (adversarial review
            // 2026-06-11, surface 3): suppression leaves carried_sqrts
            // at the DFS value, so a suppressed face is observable as
            // carried_sqrts > 0 with carried_polys == [].  The Stage-2
            // executor must count obligations from carried_polys (never
            // from CarriedSqrts) and must treat that inconsistent
            // signature as a LOUD demote, not as the conic-only class.
            if (!replay_ok || replay.nsq != out.carried_sqrts) {
                std::fprintf(stderr,
                    "[carry-replay] INTERNAL: best_order replay %s "
                    "(replay nsq=%lu, dfs nsq=%lu) — carried_polys "
                    "suppressed\n",
                    replay_ok ? "profile mismatch" : "step rejected",
                    replay.nsq, out.carried_sqrts);
                std::fflush(stderr);
                out.obligation_polys.clear();
            }
        }
        return out;
    }

    const uint64_t full = (n == 64) ? ~0ull : ((1ull << n) - 1);
    auto it = orders_table.find(full);
    if (it == orders_table.end()) return LrResult{{}, INF, {}};
    return it->second;
}

// Build the intersection-refined subset reduction table that find_lr_orders'
// order SEARCH uses ("Step A").  set_table[bits][g] is the letter set obtained
// by reducing group g down to the variable subset encoded by `bits`, where a
// letter survives ONLY if it is produced along EVERY single-pivot path into
// that subset (intersect_proportional over the per-parent reductions).  That
// intersection is what removes order-dependent spurious resultants: a single
// reduction path over-approximates the Landau set (st_fubini_lr emits ALL
// pairwise resultants, no compatibility graph), so a letter can appear via one
// last-pivot choice but not another and is then genuinely fictitious.  The
// SEARCH (and HyperInt's cgReduction) drop such letters; a single-path walk
// does not.  Default semantics only --- no Euler chi-filter, no score-prune, no
// carry-discharge (those are find_lr_orders levers, all OFF by default), and
// sings collection is off (nullptr) --- so this reproduces the SEARCH's
// DEFAULT-mode set_table exactly.  Mirrors the Step-A loop in find_lr_orders;
// the two MUST stay in sync (cross-checked by the multi-group regression
// fixture and notes/verify_multigroup_bug/verify_sweep.py).
static std::unordered_map<uint64_t, std::vector<std::vector<Poly>>>
build_lr_set_table(const std::vector<std::vector<Poly>>& group_polys,
                   const std::vector<size_t>& xvar_indices) {
    const size_t G = group_polys.size();
    const size_t n = xvar_indices.size();
    std::unordered_map<uint64_t, std::vector<std::vector<Poly>>> set_table;
    // Seed: set_table[{}] = the raw group polynomials.
    set_table[0] = group_polys;
    for (size_t size = 1; size <= n; ++size) {
        for (uint64_t bits : subsets_of_size(n, size)) {
            std::vector<std::vector<Poly>> set_for_bits(G);
            for (size_t g = 0; g < G; ++g) {
                std::vector<std::vector<Poly>> preTable;
                preTable.reserve(size);
                for (size_t bit = 0; bit < n; ++bit) {
                    if (!(bits & (1ull << bit))) continue;
                    const uint64_t prev_bits = bits ^ (1ull << bit);
                    auto it = set_table.find(prev_bits);
                    if (it == set_table.end())
                        throw std::runtime_error(
                            "verify_order_is_lr: missing parent subset state");
                    preTable.push_back(
                        st_fubini_lr(it->second[g], xvar_indices[bit], nullptr));
                }
                set_for_bits[g] = intersect_proportional(preTable);
            }
            set_table[bits] = std::move(set_for_bits);
        }
    }
    return set_table;
}

OrderVerifyResult verify_order_is_lr(
    const std::vector<std::vector<Poly>>& group_polys,
    const std::vector<size_t>& xvar_indices,
    const std::vector<size_t>& order_var_indices,
    bool allow_algebraic_letters) {
    OrderVerifyResult res;
    const size_t n = xvar_indices.size();
    // The order must be a permutation of xvar_indices (same set, same size).
    if (order_var_indices.size() != n) { res.malformed = true; return res; }
    {
        std::unordered_set<size_t> xs(xvar_indices.begin(), xvar_indices.end());
        std::unordered_set<size_t> os(order_var_indices.begin(),
                                      order_var_indices.end());
        if (os.size() != n || xs != os) { res.malformed = true; return res; }
    }
    if (group_polys.empty() || n == 0) { res.is_lr = true; return res; }
    const size_t G = group_polys.size();

    // st_fubini_lr is called directly here (not via find_lr_orders), so reset
    // its per-request step/factor memos (header contract).  Also reset the
    // budget from the environment (header contract): the steady_clock
    // deadline must start fresh, and resetting prevents a stale deadline
    // (already in the past) from a prior budgeted find_lr_orders call in the
    // same process from throwing LrBudgetExceeded spuriously here.  With the
    // env vars unset this leaves the budget inert (no-op checks), so the
    // verify verdict stays byte-identical to the pre-budget engine.
    reset_lr_memos();
    reset_lr_budget();

    long max_deg = allow_algebraic_letters ? 2L : 1L;
    {
        const char* env = std::getenv("HF_LR_MAX_DEG");
        if (env && allow_algebraic_letters) max_deg = std::atol(env);
    }

    // Per-step admissibility predicate, SHARED by the fast single-path screen
    // and the authoritative intersection walk so they can never diverge.
    // Returns true (and fills `out`.blocking_*) iff some letter in `letters`
    // has degree > max_deg in `pivot`, or is a deg-2 letter whose sqrt
    // obligation uses a still-pending variable (order[k+1..]).  Identical to
    // find_lr_orders' Step-B test (degree window + forbidden-pending guard).
    auto step_blocks = [&](const std::vector<std::vector<Poly>>& letters,
                           size_t pivot, size_t k,
                           OrderVerifyResult& out) -> bool {
        for (size_t g = 0; g < letters.size(); ++g) {
            for (const auto& p : letters[g]) {
                const long d = p.degree_in_var(pivot);
                if (d < 1) continue;  // pivot-free letters pass (Step-B parity)
                if (d > max_deg) {
                    out.is_lr = false;
                    out.blocking_step = static_cast<int>(k);
                    out.blocking_degree = d;
                    out.blocking_letter = p.canonical_prop_form().to_string();
                    return true;
                }
                if (d >= 2 && allow_algebraic_letters) {
                    std::vector<size_t> used = p.used_var_indices();
                    bool has_forbidden = false;
                    for (size_t j = k + 1; j < n && !has_forbidden; ++j) {
                        const size_t v = order_var_indices[j];
                        for (size_t u : used) {
                            if (u == v) { has_forbidden = true; break; }
                        }
                    }
                    if (has_forbidden) {
                        out.is_lr = false;
                        out.blocking_step = static_cast<int>(k);
                        out.blocking_degree = d;
                        out.forbidden_dep = true;
                        out.blocking_letter = p.canonical_prop_form().to_string();
                        return true;
                    }
                }
            }
        }
        return false;
    };

    // FAST SCREEN (O(n)): walk the SINGLE reduction path along the order.
    // st_fubini_lr is monotone under input-set inclusion, and the
    // intersection-refined set at each prefix (what the order SEARCH and
    // HyperInt's cgReduction effectively use) is a SUBSET of this single-path
    // set.  So if NO single-path prefix blocks, the smaller intersection set
    // cannot block either => the order is genuinely LR.  This keeps the common
    // carry case (a rationalized, strictly-linear transformed term) at O(n); a
    // single-path block is only POSSIBLY real and is adjudicated below.  A
    // single-path "LR" can never be a false accept (subset of a linear set is
    // linear), so this screen is sound.
    {
        std::vector<std::vector<Poly>> cur = group_polys;
        OrderVerifyResult scratch;
        bool blocked = false;
        for (size_t k = 0; k < n; ++k) {
            const size_t pivot = order_var_indices[k];
            if (step_blocks(cur, pivot, k, scratch)) { blocked = true; break; }
            if (k + 1 < n) {
                std::vector<std::vector<Poly>> nxt(G);
                for (size_t g = 0; g < G; ++g)
                    nxt[g] = st_fubini_lr(cur[g], pivot, nullptr);
                cur = std::move(nxt);
            }
        }
        if (!blocked) { res.is_lr = true; return res; }
    }

    // The single-path screen hit a blocker, but a single path
    // over-approximates the Landau set (st_fubini_lr emits ALL pairwise
    // resultants, no compatibility graph), so the blocker may be a spurious
    // letter (e.g. x8^2+x8+1) that the intersection drops.  Re-decide against
    // the SAME intersection-refined set_table the SEARCH uses: a letter
    // survives a subset only if produced along EVERY pivot path into it.  This
    // is what makes verify agree with find_lr_orders by construction (and so
    // with HyperInt on every order the SEARCH accepts).  Single-path was the
    // multi-group false-negative; see notes/verify_multigroup_bug/REPRO.md.
    if (n > 63) {  // bitmask subset table, same cap as find_lr_orders
        throw std::runtime_error(
            "verify_order_is_lr: > 63 integration variables (bitmask overflow)");
    }
    const auto set_table = build_lr_set_table(group_polys, xvar_indices);

    // ctx-variable-index -> bit position in xvar_indices, for the prefix mask.
    std::unordered_map<size_t, size_t> bit_of;
    bit_of.reserve(n * 2);
    for (size_t b = 0; b < n; ++b) bit_of[xvar_indices[b]] = b;

    // At step k the already-integrated subset is {order[0..k-1]} (= `bits`),
    // whose intersection-refined letter set is set_table[bits]; require it
    // linear (<= max_deg) in the next pivot order[k].  Same per-step test
    // find_lr_orders' Step B applies to set_table[prev_bits].
    uint64_t bits = 0;
    for (size_t k = 0; k < n; ++k) {
        const size_t pivot = order_var_indices[k];
        auto itc = set_table.find(bits);
        if (itc == set_table.end())  // every prefix subset was built above
            throw std::runtime_error("verify_order_is_lr: missing prefix state");
        if (step_blocks(itc->second, pivot, k, res)) return res;
        bits |= (1ull << bit_of[pivot]);  // integrate order[k]
    }
    res.is_lr = true;
    return res;
}

}  // namespace lr_search
}  // namespace hyperflint
