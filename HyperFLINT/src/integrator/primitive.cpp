// Phase 5d: IntegrateII — port of HyperIntica.wl:4448 and
// HyperInt.mpl:736 (integrateInplace).

#include "hyperflint/integrator/primitive.hpp"
#include "hyperflint/runtime/hf_thread_num.hpp"

#include "hyperflint/algebra/partial_fractions.hpp"
#include "hyperflint/algebra/poly_struct_hash.hpp"  // axis-D: pre-gating
#include "hyperflint/core/addpf_probe.hpp"          // HF_ADDPF_PROBE bump-layer (2026-06-04)
#include "hyperflint/runtime/trace_gate.hpp"
                                                      // dedup hash.
#include "hyperflint/algebra/shuffle.hpp"   // collect_words (not strictly
                                             // required since we dedupe
                                             // while accumulating, but
                                             // pulled in for safety).
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/zw_table.hpp"               // ZWTable (B5)
#include "hyperflint/runtime/scalar_rep.hpp"          // runtime::scalar_rep_enabled (B5 dispatch)
#include "hyperflint/runtime/scs_roundtrip.hpp"       // runtime::roundtrip_rat_through_scs (B5 verifier site)

#ifdef HF_HAVE_OPENMP
#include <omp.h>
#endif

#include <chrono>
#include <cstdlib>      // std::abort (iter-65 require_persistent assertion)
#include <iostream>     // std::cerr (iter-65 require_persistent assertion)
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hyperflint {

// Phase-d15 follow-up: file-scope per-thread accumulator for the
// partial_fractions cost inside integrate_ii. Sized & cleared by the
// caller (integration_step) before the OMP parallel region; summed
// after. Each worker writes its own slot so no atomic / contention.
namespace {
std::vector<double>& pf_per_thread_storage() {
    static std::vector<double> v;
    return v;
}
// 2026-04-26 sanity-matrix follow-up: timers for the integrate_ii
// body outside partial_fractions (the "53 % non-pf wall block"
// identified by the matrix on tst2). Same per-thread vector pattern.
std::vector<double>& bump_lookup_storage()  { static std::vector<double> v; return v; }
std::vector<double>& bump_addto_storage()   { static std::vector<double> v; return v; }
std::vector<double>& push_ibp_storage()     { static std::vector<double> v; return v; }
std::vector<double>& antideriv_storage()    { static std::vector<double> v; return v; }
std::vector<long>&   bump_calls_storage()   { static std::vector<long>   v; return v; }
// 2026-04-27 Lever-1 extended: split bump_addto_s into the new-row
// emplace branch (rows.push_back on miss) and the existing-row
// Rat-add branch (coef + c on hit). Disambiguates the 125 s step-3
// "dark mass" by attributing the cost to the specific bump branch.
std::vector<double>& bump_emplace_storage()       { static std::vector<double> v; return v; }
std::vector<double>& bump_rat_add_storage()       { static std::vector<double> v; return v; }
std::vector<long>&   bump_rat_add_calls_storage() { static std::vector<long>   v; return v; }
// Phase-0 P1' pre-gating instrumentation (2026-04-26): per-invocation
// distinct-denominator accounting. `pf_calls_in_loop_storage` accumulates
// the number of queue iterations in `integrate_ii` that would actually
// trigger a `linear_factors` call (matches partial_fractions's early-exit
// gate). `pf_unique_dens_storage` accumulates the per-invocation distinct-
// denominator count summed across invocations on this thread. The ratio
// (pf_calls_in_loop - pf_unique_dens) / pf_calls_in_loop upper-bounds
// what within-invocation bucketing (P1') could save from the global
// linear_factors cache-machinery cost — the decisive measurement that
// gates whether P1' can capture the tst2 linear_factors_s wall.
std::vector<long>&   pf_calls_in_loop_storage() { static std::vector<long> v; return v; }
std::vector<long>&   pf_unique_dens_storage()   { static std::vector<long> v; return v; }
// Phase-0b RatAccumulator pre-gating (2026-04-26): per-`integrate_ii`-
// invocation `rows.size()` at end of loop, summed across invocations.
// `bump_unique_rows = Σ rows.size()` per thread, `bump_calls` already
// counts total non-zero bump invocations. The RatAccumulator-collapsible
// fraction = (bump_calls − bump_unique_rows) / bump_calls — the share
// of bump calls that hit an existing row and pay a Rat::operator+
// canonicalization that a deferred-canonicalization accumulator would
// avoid.
std::vector<long>&   bump_unique_rows_storage() { static std::vector<long> v; return v; }
// 2026-04-29 (Probe 3): three sub-timers attributing the
// integrate_ii body residual outside partial_fractions / bump /
// push_ibp / antideriv. See primitive.hpp comment block.
std::vector<double>& ii_queue_copy_storage()    { static std::vector<double> v; return v; }
std::vector<double>& ii_pole_arith_storage()    { static std::vector<double> v; return v; }
std::vector<double>& ii_pole_word_ctor_storage(){ static std::vector<double> v; return v; }

// The expensive part of the Phase-0 pre-gating instrumentation
// (`seen_dens.insert(w.coef.den().to_string())`) inflated tst2 wall
// by +47% in the original measurement run because every queue
// iteration paid a Poly::to_string and a hash insert, even when no
// trace was being emitted. Gate it behind HF_STEP_TRACE so production
// builds (and untraced wall-A/B runs) skip the overhead. The
// pf_calls_in_loop counter is still bumped unconditionally — it's
// cheap and useful as a fast diagnostic — but pf_unique_dens reads
// 0 when tracing is off (consumers should treat 0 as "not measured"
// in that mode).
inline bool pregating_full_accounting_enabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* s = std::getenv("HF_STEP_TRACE");
        cached = (s && s[0] && s[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}
}  // namespace (anon, file-scope)

void init_partial_fractions_per_thread(int n_threads) {
    auto& v = pf_per_thread_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_partial_fractions_per_thread() {
    auto& v = pf_per_thread_storage();
    for (auto& x : v) x = 0.0;
}
double sum_partial_fractions_per_thread() {
    double s = 0.0;
    for (double x : pf_per_thread_storage()) s += x;
    return s;
}

void init_ii_sub_timers_per_thread(int n_threads) {
    const size_t n = static_cast<size_t>(n_threads > 0 ? n_threads : 1);
    bump_lookup_storage().assign(n, 0.0);
    bump_addto_storage().assign(n, 0.0);
    push_ibp_storage().assign(n, 0.0);
    antideriv_storage().assign(n, 0.0);
    bump_calls_storage().assign(n, 0L);
    pf_calls_in_loop_storage().assign(n, 0L);
    pf_unique_dens_storage().assign(n, 0L);
    bump_unique_rows_storage().assign(n, 0L);
    bump_emplace_storage().assign(n, 0.0);
    bump_rat_add_storage().assign(n, 0.0);
    bump_rat_add_calls_storage().assign(n, 0L);
    // Probe 3.
    ii_queue_copy_storage().assign(n, 0.0);
    ii_pole_arith_storage().assign(n, 0.0);
    ii_pole_word_ctor_storage().assign(n, 0.0);
}
void reset_ii_sub_timers_per_thread() {
    for (auto& x : bump_lookup_storage()) x = 0.0;
    for (auto& x : bump_addto_storage())  x = 0.0;
    for (auto& x : push_ibp_storage())    x = 0.0;
    for (auto& x : antideriv_storage())   x = 0.0;
    for (auto& x : bump_calls_storage())  x = 0L;
    for (auto& x : pf_calls_in_loop_storage()) x = 0L;
    for (auto& x : pf_unique_dens_storage())   x = 0L;
    for (auto& x : bump_unique_rows_storage()) x = 0L;
    for (auto& x : bump_emplace_storage())       x = 0.0;
    for (auto& x : bump_rat_add_storage())       x = 0.0;
    for (auto& x : bump_rat_add_calls_storage()) x = 0L;
    // Probe 3.
    for (auto& x : ii_queue_copy_storage())     x = 0.0;
    for (auto& x : ii_pole_arith_storage())     x = 0.0;
    for (auto& x : ii_pole_word_ctor_storage()) x = 0.0;
}
double sum_bump_lookup_s_per_thread() { double s=0; for (double x : bump_lookup_storage()) s+=x; return s; }
double sum_bump_addto_s_per_thread()  { double s=0; for (double x : bump_addto_storage())  s+=x; return s; }
double sum_push_ibp_s_per_thread()    { double s=0; for (double x : push_ibp_storage())    s+=x; return s; }
double sum_antideriv_s_per_thread()   { double s=0; for (double x : antideriv_storage())   s+=x; return s; }
long   sum_bump_calls_per_thread()    { long   s=0; for (long   x : bump_calls_storage())  s+=x; return s; }
long   sum_pf_calls_in_loop_per_thread() { long s=0; for (long x : pf_calls_in_loop_storage()) s+=x; return s; }
long   sum_pf_unique_dens_per_thread()   { long s=0; for (long x : pf_unique_dens_storage())   s+=x; return s; }
long   sum_bump_unique_rows_per_thread() { long s=0; for (long x : bump_unique_rows_storage()) s+=x; return s; }
double sum_bump_emplace_s_per_thread()       { double s=0; for (double x : bump_emplace_storage())        s+=x; return s; }
double sum_bump_rat_add_s_per_thread()       { double s=0; for (double x : bump_rat_add_storage())        s+=x; return s; }
long   sum_bump_rat_add_calls_per_thread()   { long   s=0; for (long   x : bump_rat_add_calls_storage()) s+=x; return s; }
// Probe 3.
double sum_ii_queue_copy_s_per_thread()      { double s=0; for (double x : ii_queue_copy_storage())     s+=x; return s; }
double sum_ii_pole_arith_s_per_thread()      { double s=0; for (double x : ii_pole_arith_storage())     s+=x; return s; }
double sum_ii_pole_word_ctor_s_per_thread()  { double s=0; for (double x : ii_pole_word_ctor_storage()) s+=x; return s; }

namespace {

// Antiderivative of a polynomial-in-var Rat, pinned at var=0.
// The input polynomial_part from partial_fractions is a Rat whose
// numerator is a polynomial in var and whose denominator is free of
// var (by the partial-fraction invariant). We just read off
// coefficients-of-var^k from the numerator and apply
//     integral(c_k * var^k, var) = c_k * var^(k+1) / (k+1).
Rat antideriv_poly_in_var(const PolyCtx& ctx,
                           const Rat& poly,
                           size_t var_idx) {
    if (poly.is_zero()) return Rat{Poly::zero_of(ctx)};

    long max_deg = poly.num().degree_in_var(var_idx);
    if (max_deg < 0) return Rat{Poly::zero_of(ctx)};

    const Poly var_poly = Poly::gen(ctx, var_idx);
    Rat acc{Poly::zero_of(ctx)};
    for (long k = 0; k <= max_deg; ++k) {
        Poly ck_num = poly.num().coefficient_of_var(var_idx, k);
        if (ck_num.is_zero()) continue;
        // Coefficient of var^k as a Rat (over the other vars).
        Rat ck(ck_num, poly.den());
        // var^(k+1) as a Rat — built via fmpq_mpoly_gen + pow, not string.
        Rat var_pow_rat{var_poly.pow(static_cast<unsigned long>(k + 1))};
        Rat denom{Poly::from_int(ctx, k + 1)};
        acc = acc + (ck * var_pow_rat) / denom;
    }
    return acc;
}

}  // namespace

Wordlist integrate_ii(const PolyCtx& ctx,
                      const Wordlist& wl,
                      size_t var_idx,
                      std::shared_ptr<ZWTable> zw_tab,
                      bool introduce_algebraic_letters) {
    // iter-83 HF_ITER83_DUMP gate: print the input wordlist (every
    // (coef.num, coef.den, word) entry) before queue construction so
    // HJ-vs-HF diff at the integrate_ii entry boundary is possible.
    // Off by default; activate with HF_ITER83_DUMP=1 in the env.
    if (std::getenv("HF_ITER83_DUMP")) {
        std::cerr << "[HF_ITER83_DUMP] integrate_ii entry: var_idx="
                  << var_idx << " wl.size=" << wl.terms.size()
                  << " introduce_algebraic_letters="
                  << (introduce_algebraic_letters ? 1 : 0) << "\n";
        for (size_t i = 0; i < wl.terms.size(); ++i) {
            const auto& t = wl.terms[i];
            std::cerr << "[HF_ITER83_DUMP]   term[" << i << "]\n";
            std::cerr << "[HF_ITER83_DUMP]     coef.num="
                      << t.coef.num().to_string() << "\n";
            std::cerr << "[HF_ITER83_DUMP]     coef.den="
                      << t.coef.den().to_string() << "\n";
            std::cerr << "[HF_ITER83_DUMP]     word.size="
                      << t.word.letters.size() << "\n";
            for (size_t j = 0; j < t.word.letters.size(); ++j) {
                std::cerr << "[HF_ITER83_DUMP]       letter[" << j << "]="
                          << t.word.letters[j].to_string() << "\n";
            }
        }
    }

    // Queue of terms to process. New IBP terms are appended; we
    // advance a running index rather than popping from the front.
    std::vector<WordlistTerm> queue = wl.terms;

    // Accumulated result: one cell per unique output word.
    struct Cell { Word word; Rat coef; };
    std::vector<Cell> rows;
    std::unordered_map<std::string, size_t> row_index;

    // HF_ADDPF_PROBE bump-layer extension (2026-06-04): per-row distinct
    // INCOMING denominator hashes (group-by-denominator statistic).
    // Probe-only side storage, parallel to `rows`; capped per row so a
    // pathological row cannot blow memory. Empty when the probe is off.
    constexpr std::size_t kAddpfDenCap = 16;
    std::vector<std::vector<std::pair<uint64_t, uint64_t>>> addpf_row_dens;
    const bool addpf_on = addpf_probe::enabled();
    auto addpf_den_hash = [](const Rat& r) {
        auto seed = poly_struct_hash_seed();
        uint64_t h1 = seed.first, h2 = seed.second;
        poly_struct_hash_raw(h1, h2, r.den());
        return std::make_pair(h1, h2);
    };
    auto addpf_note_den = [&](std::size_t row, const Rat& c) {
        if (row >= addpf_row_dens.size())
            addpf_row_dens.resize(row + 1);
        auto& v = addpf_row_dens[row];
        if (v.size() >= kAddpfDenCap) return;  // capped; lower bound
        const auto h = addpf_den_hash(c);
        for (const auto& e : v) if (e == h) return;
        v.push_back(h);
    };

    // Resolve thread id once; per-thread storage slots are bounds-checked
    // since init_ii_sub_timers_per_thread may not have been called on the
    // standalone CLI path.
    // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num() in OMP mode;
    // under HF_USE_GCD=1 returns the GCD slot index.
    const int _ii_tid = ::hyperflint::runtime::hf_get_thread_num();

    auto bump = [&](const Word& w, const Rat& c) {
        if (c.is_zero()) return;
        {
            auto& _cv = bump_calls_storage();
            if (static_cast<size_t>(_ii_tid) < _cv.size()) _cv[_ii_tid] += 1;
        }
        const bool _tg = step_trace_enabled();
        const auto _bk_t0 = _tg ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        std::string k = w.content_key();
        auto it = row_index.find(k);
        const auto _bk_t1 = _tg ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        if (_tg) {
            auto& _lv = bump_lookup_storage();
            if (static_cast<size_t>(_ii_tid) < _lv.size())
                _lv[_ii_tid] += std::chrono::duration<double>(_bk_t1 - _bk_t0).count();
        }
        if (it == row_index.end()) {
            row_index[k] = rows.size();
            rows.push_back(Cell{w, c});
            if (addpf_on) addpf_note_den(rows.size() - 1, c);
            if (_tg) {
                const auto _bk_t2 = std::chrono::steady_clock::now();
                auto& _av = bump_addto_storage();
                if (static_cast<size_t>(_ii_tid) < _av.size())
                    _av[_ii_tid] += std::chrono::duration<double>(_bk_t2 - _bk_t1).count();
                auto& _ev = bump_emplace_storage();
                if (static_cast<size_t>(_ii_tid) < _ev.size())
                    _ev[_ii_tid] += std::chrono::duration<double>(_bk_t2 - _bk_t1).count();
            }
        } else {
            // 2026-04-27 reviewer-flagged hot path.  In-place += avoids
            // the temporary Rat / 4 Poly tempor moves that the prior
            // `coef = coef + c` pattern incurred.  The temporary's
            // assign-back used to deep-copy via fmpq_mpoly_set because
            // Poly lacked a move-assign; both holes are now closed.
            //
            // HF_ADDPF_PROBE bump-layer extension (2026-06-04): split
            // this += by (incoming den == row's current den) and time
            // it; track incoming-den groups per row. Default-OFF.
            if (addpf_on) {
                const bool same_den =
                    c.den().equal(rows[it->second].coef.den());
                addpf_note_den(it->second, c);
                const auto _ap_t0 = std::chrono::steady_clock::now();
                rows[it->second].coef += c;
                addpf_probe::record_bump_add(
                    same_den,
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - _ap_t0).count());
            } else {
                rows[it->second].coef += c;
            }
            if (_tg) {
                const auto _bk_t2 = std::chrono::steady_clock::now();
                auto& _av = bump_addto_storage();
                if (static_cast<size_t>(_ii_tid) < _av.size())
                    _av[_ii_tid] += std::chrono::duration<double>(_bk_t2 - _bk_t1).count();
                auto& _rv = bump_rat_add_storage();
                if (static_cast<size_t>(_ii_tid) < _rv.size())
                    _rv[_ii_tid] += std::chrono::duration<double>(_bk_t2 - _bk_t1).count();
                auto& _cv = bump_rat_add_calls_storage();
                if (static_cast<size_t>(_ii_tid) < _cv.size())
                    _cv[_ii_tid] += 1;
            }
        }
    };

    // Phase-B B5 (design v2 §3.5a row 5 + §4.2 commit (5) — 3-month
    // checkpoint). Per-Wordlist-element `as_rat` boundary inside the
    // queue drain: under runtime::scalar_rep_enabled() (the
    // HF_USE_SCALAR_REP env-gate), every queued-coef Rat is round-
    // tripped through SymCoef::from_rat -> SymCoefSplit::from_rat ->
    // SymCoefSplit::as_rat -> SymCoef::as_rat via the shared helper
    // `runtime::roundtrip_rat_through_scs`. This simulates the future
    // state where the queue holds SymCoefSplit-form coefs and each
    // loop iteration reconstitutes them to a wide-ctx Rat for the
    // Symanzik-side combine (partial_fractions). The verifier site is
    // the queue-drain top, before the Rat is consumed by run_parfr /
    // antideriv_poly_in_var / pole arithmetic. At B-stages on Smirnov,
    // the W-side variable set is empty by hypothesis (b1_scoping_memo.md
    // R2; design v2 §4.4a Note 2), so the round-trip is canonically a
    // no-op and the Rat is byte-exact preserved. Mirrors the B1.c / B2 /
    // B3 / B4 dispatch pattern.
    //
    // C0c.1 iter-65 (path 1a sub-iter 2): lambda kill. The persistent
    // ZWTable is threaded by the caller through integrate_ii's
    // `zw_tab` parameter (signature widened in iter-52b cascade); the
    // `[&]` reference capture brings it into scope. Outer callers
    // allocate once per hyperflint_sym driver entry
    // (hyper_int.cpp:463-466), once per CLI invocation
    // (bridge/cli/main.cpp:2976), once per integration_step's serial
    // post-OMP divergence-check pass (integration_step.cpp:1155), or
    // per-thread via integration_step's outer parallel-for per
    // iter-58 Option A (integration_step.cpp:1648,
    // `zw_for_this_thread`). The prior local
    // `auto zw_tab = make_shared<ZWTable>(ctx);` shadowed the outer
    // parameter and discarded handle reuse across nested calls. Same
    // pattern as iter-64 site 2 at transform.cpp:917 and iter-65
    // site 3 at transform.cpp:999.
    //
    // Iter-65 advisory-2 fold (from iter-64 reviewer agentId
    // a8d6453b4307f6d1f): defense-in-depth
    // HF_SCALAR_REP_REQUIRE_PERSISTENT abort guard mirrors site 4
    // pattern at break_up_contour.cpp:305 and sites 1/7 at
    // transform.cpp:605 / integration_step.cpp:1003.
    auto apply_v1_roundtrip = [&](const Rat& coef,
                                   const char* tag) -> Rat {
        if (!runtime::scalar_rep_enabled()) return coef;
        if (runtime::require_persistent_enabled() && !zw_tab) {
            std::cerr << "[HF_SCALAR_REP_REQUIRE_PERSISTENT=1]"
                << " integrate_ii apply_v1_roundtrip:"
                << " zw_tab is null at tag=" << tag
                << ". Caller did not thread the persistent ZWTable"
                << " from hyperflint_sym (hyper_int.cpp:463-466),"
                << " CLI (bridge/cli/main.cpp:2976), or"
                << " integration_step (1155/1648)."
                << " Migrate per design v2 sec 3.6a."
                << std::endl;
            std::abort();
        }
        return runtime::roundtrip_rat_through_scs(coef, ctx, zw_tab, tag);
    };

    // "var" as a Rat (used in  (var - pole) and  (var - word[0]) ).
    Rat var_rat{Poly::gen(ctx, var_idx)};

    // Integration-by-parts helper: push {-p/(var - word[0]), Rest[word]}
    // into the queue. Matches HyperIntica:4478 and HyperInt.mpl:746.
    auto push_ibp = [&](const Rat& p, const Word& word) {
        if (word.empty()) return;
        const bool _tg = step_trace_enabled();
        const auto _pi_t0 = _tg ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        Rat chain_denom = var_rat - word[0];
        if (chain_denom.is_zero()) {
            if (_tg) {
                auto& _v = push_ibp_storage();
                if (static_cast<size_t>(_ii_tid) < _v.size())
                    _v[_ii_tid] += std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - _pi_t0).count();
            }
            return;
        }
        Rat new_coef = -p / chain_denom;
        if (new_coef.is_zero()) {
            if (_tg) {
                auto& _v = push_ibp_storage();
                if (static_cast<size_t>(_ii_tid) < _v.size())
                    _v[_ii_tid] += std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - _pi_t0).count();
            }
            return;
        }
        Word rest;
        rest.letters.assign(word.letters.begin() + 1, word.letters.end());
        queue.push_back(WordlistTerm{std::move(new_coef), std::move(rest)});
        if (_tg) {
            auto& _v = push_ibp_storage();
            if (static_cast<size_t>(_ii_tid) < _v.size())
                _v[_ii_tid] += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - _pi_t0).count();
        }
    };

    auto run_parfr = [&](const Rat& coef) -> PartialFractionization {
        // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num() in OMP mode;
        // under HF_USE_GCD=1 returns the GCD slot index.
        const int tid = ::hyperflint::runtime::hf_get_thread_num();
        const bool _tg = step_trace_enabled();
        const auto _pf_t0 = _tg ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        try {
            auto result = partial_fractions(coef, var_idx, zw_tab,
                                            introduce_algebraic_letters);
            if (_tg) {
                auto& pf_v = pf_per_thread_storage();
                if (static_cast<size_t>(tid) < pf_v.size()) {
                    pf_v[static_cast<size_t>(tid)] +=
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - _pf_t0).count();
                }
            }
            return result;
        } catch (const std::exception& e) {
            if (_tg) {
                auto& pf_v = pf_per_thread_storage();
                if (static_cast<size_t>(tid) < pf_v.size()) {
                    pf_v[static_cast<size_t>(tid)] +=
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - _pf_t0).count();
                }
            }
            throw IntegrateIIFailed(
                std::string("partial_fractions: ") + e.what());
        }
    };

    // Phase-0 P1' pre-gating: track distinct denominators encountered in
    // THIS invocation's queue. The accumulators are summed into the
    // per-thread totals after the loop. Skips entries that would early-
    // exit inside partial_fractions before reaching `linear_factors`
    // (zero coef or den free of var_idx) — those don't contribute to
    // the cache traffic that bucketing could collapse.
    //
    // 2026-04-29 (axis-D-pregating-cleanup): switched the dedup key from
    // `den.to_string()` to a 128-bit `poly_struct_hash` of the denominator.
    // The to_string variant added ~350 wall-s on parity-1 ord_1_face_1
    // under HF_STEP_TRACE — same kind of expensive Poly serialisation
    // that axis-C-lf-constant-defer eliminated from the lookup path.
    // Probe-3 measurement (notes/2026-04-29_hf-vs-maple-investigation.md)
    // localised the inflation here. The structural hash gives the same
    // dedup count at ~µs/call instead of ~ms/call.
    std::unordered_set<std::pair<uint64_t, uint64_t>, PairU64Hash> seen_dens;
    long pf_calls_in_loop_local = 0;

    for (size_t q_idx = 0; q_idx < queue.size(); ++q_idx) {
        // COPY w at loop top: push_ibp's queue.push_back can reallocate
        // queue's storage, invalidating any reference into queue[q_idx].
        // We hit this on inputs with two distinct double poles + a
        // non-empty word — see Phase 6d-v-vi-0 SEGV trace.
        // Probe 3: time the per-iter Rat copy from the queue.
        const bool _qc_tg = step_trace_enabled();
        const auto _qc_t0 = _qc_tg ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        WordlistTerm w = queue[q_idx];
        if (_qc_tg) {
            auto& _v = ii_queue_copy_storage();
            if (static_cast<size_t>(_ii_tid) < _v.size())
                _v[_ii_tid] += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - _qc_t0).count();
        }
        // Phase-B B5 verifier site: round-trip the queued-coef Rat
        // through SymCoefSplit at the per-Wordlist-element as_rat
        // boundary. No-op under HF_USE_SCALAR_REP=0 (production path).
        w.coef = apply_v1_roundtrip(w.coef, "integrate_ii/queue_drain");
        // Phase-0 P1' pre-gating instrumentation. Match
        // partial_fractions.cpp:147-154 early-exit gate so the counter
        // sums match `lf_cache_hits + lf_cache_misses` from the
        // partial_fractions caller path (modulo transform_shuffle's own
        // lf calls, which inflate the global counter but not this one).
        if (!w.coef.is_zero() &&
            w.coef.den().degree_in_var(var_idx) > 0) {
            ++pf_calls_in_loop_local;
            if (pregating_full_accounting_enabled()) {
                // axis-D: structural hash dedup, ~µs/call.
                auto seed = poly_struct_hash_seed();
                uint64_t h1 = seed.first;
                uint64_t h2 = seed.second;
                poly_struct_hash_raw(h1, h2, w.coef.den());
                seen_dens.insert({h1, h2});
            }
        }
        PartialFractionization parfr = run_parfr(w.coef);

        // Polynomial part.
        if (!parfr.polynomial_part.is_zero()) {
            const bool _ad_tg = step_trace_enabled();
            const auto _ad_t0 = _ad_tg ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
            Rat p = antideriv_poly_in_var(ctx, parfr.polynomial_part, var_idx);
            if (_ad_tg) {
                auto& _v = antideriv_storage();
                if (static_cast<size_t>(_ii_tid) < _v.size())
                    _v[_ii_tid] += std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - _ad_t0).count();
            }
            bump(w.word, p);
            push_ibp(p, w.word);
        }

        // Pole terms.
        for (const auto& pole : parfr.poles) {
            for (long n = 1; n <= pole.multiplicity; ++n) {
                const Rat& cn = pole.coefs[static_cast<size_t>(n - 1)];
                if (cn.is_zero()) continue;
                if (n >= 2) {
                    // integral  c/(var - a)^n dvar
                    //     = c / ((1 - n) * (var - a)^(n-1))
                    // Probe 3: time the three Rat ops (subtract, pow, divide).
                    const bool _pa_tg = step_trace_enabled();
                    const auto _pa_t0 = _pa_tg ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
                    Rat var_minus_pole = var_rat - pole.pole;
                    Rat vmp_pow = var_minus_pole.pow(n - 1);
                    Rat inv_one_minus_n{Poly::from_int(ctx, 1 - n)};
                    Rat p = cn / (vmp_pow * inv_one_minus_n);
                    if (_pa_tg) {
                        auto& _v = ii_pole_arith_storage();
                        if (static_cast<size_t>(_ii_tid) < _v.size())
                            _v[_ii_tid] += std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - _pa_t0).count();
                    }
                    bump(w.word, p);
                    push_ibp(p, w.word);
                } else {   // n == 1
                    // integral c/(var - a) dvar * Hlog[var, word]
                    //     = c * Hlog[var, {a, word_1, ..., word_k}]
                    // Probe 3: time the Word construction.
                    const bool _wc_tg = step_trace_enabled();
                    const auto _wc_t0 = _wc_tg ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
                    Word new_word;
                    new_word.letters.reserve(w.word.size() + 1);
                    new_word.letters.push_back(pole.pole);
                    for (const auto& l : w.word.letters)
                        new_word.letters.push_back(l);
                    if (_wc_tg) {
                        auto& _v = ii_pole_word_ctor_storage();
                        if (static_cast<size_t>(_ii_tid) < _v.size())
                            _v[_ii_tid] += std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - _wc_t0).count();
                    }
                    bump(new_word, cn);
                }
            }
        }
    }

    // Phase-0 P1' pre-gating: contribute this invocation's pf-call total
    // and distinct-denominator count to the per-thread accumulators.
    {
        auto& _tv = pf_calls_in_loop_storage();
        if (static_cast<size_t>(_ii_tid) < _tv.size())
            _tv[_ii_tid] += pf_calls_in_loop_local;
        auto& _uv = pf_unique_dens_storage();
        if (static_cast<size_t>(_ii_tid) < _uv.size())
            _uv[_ii_tid] += static_cast<long>(seen_dens.size());
        // Phase-0b RatAccumulator pre-gating: rows.size() at loop exit
        // is the count of unique (word) rows this invocation emitted.
        // Combined with the existing `bump_calls` counter (total non-
        // zero bumps), gives RA-collapsible-fraction =
        // (bump_calls − bump_unique_rows) / bump_calls per fixture.
        auto& _rv = bump_unique_rows_storage();
        if (static_cast<size_t>(_ii_tid) < _rv.size())
            _rv[_ii_tid] += static_cast<long>(rows.size());
    }

    // HF_ADDPF_PROBE bump-layer extension: per-row distinct-den group
    // counts at the drain (the group-by-denominator statistic).
    if (addpf_on) {
        for (std::size_t ri = 0; ri < rows.size(); ++ri) {
            const std::size_t g = ri < addpf_row_dens.size()
                                      ? addpf_row_dens[ri].size() : 0;
            addpf_probe::record_bump_row(g, g >= kAddpfDenCap);
        }
    }

    // Emit a Wordlist, dropping zero-coef rows.
    Wordlist out;
    for (auto& row : rows) {
        if (!row.coef.is_zero()) {
            out.terms.push_back(
                WordlistTerm{std::move(row.coef), std::move(row.word)});
        }
    }
    return out;
}

}  // namespace hyperflint
