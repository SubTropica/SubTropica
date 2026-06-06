// LinearFactors implementation.
//
// Mirrors HyperIntica.wl:2892. The steps:
//   1. Call fmpq_mpoly_factor on the input.  Returns constant + list of
//      (base_i, exp_i) pairs.
//   2. For each base_i:
//        * If free of var      : fold into constant.
//        * If deg-in-var = 1   : extract pole = -coef(var, 0) / coef(var, 1).
//                                 coef(var, 1) is generally a polynomial in
//                                 the remaining variables, so `pole` is a
//                                 Rat, not a Poly.
//        * If deg-in-var >= 2  : record as nonlinear; Phase 2 stops here
//                                 (Phase 7 upgrades deg-2 -> Wm/Wp).

#include "hyperflint/algebra/linear_factors.hpp"
#include "hyperflint/algebra/env_flags.hpp"            // iter-74 §T7 seventh chunk: HF_FLAG_DISABLE_LF_CACHE; iter-80 §T7 eleventh chunk: HF_FLAG_LF_CACHE_SHARDS / HF_FLAG_LF_SQF / HF_FLAG_LF_LOCK_WAIT_PROFILE
#include "hyperflint/core/operator_memo.hpp"
#include "hyperflint/core/canonical_signature.hpp"
#include "hyperflint/diagnostics/structural_sharing_probe.hpp"
#include "hyperflint/runtime/hf_thread_num.hpp"

#include "hyperflint/algebra/algebraic_letters.hpp"
#include "hyperflint/algebra/poly_struct_hash.hpp"
#include "hyperflint/core/zw_table.hpp"               // ZWTable (B4)
#include "hyperflint/instrumentation/dag_hashcons_probe.hpp"  // §A.1 iter-50: op_call emit at function entry
#include "hyperflint/runtime/scalar_rep.hpp"          // runtime::scalar_rep_enabled (B4 dispatch)
#include "hyperflint/runtime/scs_roundtrip.hpp"       // runtime::roundtrip_rat_through_scs (B4 verifier site)
#include "hyperflint/runtime/trace_gate.hpp"

#include <flint/fmpq.h>
#include <flint/fmpq_mpoly_factor.h>

#ifdef HF_HAVE_OPENMP
#include <omp.h>
#endif

#include <flint/fmpz_mpoly.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>     // std::hash (REQ-16.3 fold; iter-17)
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>          // std::string (REQ-16.3 fold; iter-17)
#include <unordered_map>
#include <utility>
#include <vector>

namespace hyperflint {

// ---------- HyperIntica-parity guard: thread-local forbidden-vars ----------
//
// When non-null, linear_factors refuses to introduce a Wm[i]/Wp[i] pair
// for a degree-2 factor whose `base` polynomial uses any of these vars
// (other than the current var_idx).  See linear_factors.hpp for the why.
// Default null preserves legacy behavior.
namespace {
    thread_local const std::vector<size_t>* g_lf_forbidden_vars = nullptr;
}

LFForbiddenVarsScope::LFForbiddenVarsScope(const std::vector<size_t>& forbidden) {
    g_lf_forbidden_vars = &forbidden;
}
LFForbiddenVarsScope::~LFForbiddenVarsScope() {
    g_lf_forbidden_vars = nullptr;
}

const std::vector<size_t>* lf_current_forbidden_vars() {
    return g_lf_forbidden_vars;
}

// Phase-d15 deeper drill (round 3): file-scope per-thread accumulators.
// `lf_ff_*` = FLINT fmpq_mpoly_factor wall (covers narrow + wide branches).
// `lf_hits_*` / `lf_misses_*` = cache hit / miss counters. Same lifecycle
// as partial_fractions.cpp's lf_per_thread_storage: integration_step's
// driver calls init_+reset_ before the OMP parallel-for and sum_ after.
//
// 2026-04-26 lock-contention proxy: `lf_lock_held_*` per-thread vector
// records held-time inside each per-shard cache mutex critical section
// (acquire → release of the lock_guard). Sum across threads ÷ wall_s
// = fraction of wall during which a shard mutex was held by some
// thread. Pre-sharding (single global mutex) was 0.34% on tst2;
// post-sharding contention drops further (~13/64 = 20% of the
// pre-sharding rate at perfect distribution). See
// sqf_round/lock_proxy_results.md and
// sqf_round/sharded_lf_cache_design.md.
namespace {
std::vector<double>& lf_ff_per_thread_storage() {
    static std::vector<double> v;
    return v;
}
std::vector<long>& lf_hits_per_thread_storage() {
    static std::vector<long> v;
    return v;
}
std::vector<long>& lf_misses_per_thread_storage() {
    static std::vector<long> v;
    return v;
}
std::vector<double>& lf_lock_held_per_thread_storage() {
    static std::vector<double> v;
    return v;
}
std::vector<double>& lf_cache_key_build_per_thread_storage() {
    static std::vector<double> v;
    return v;
}
// iter-37 probe: per-thread accumulated lock-acquire wait time
// (interval between attempting std::lock_guard ctor and lock
// actually being held). Sibling to lf_lock_held; sum across
// threads = total queueing wall on shard mutexes. Default-OFF
// env-gated under HF_LF_LOCK_WAIT_PROFILE=1 so the cost is one
// branch in the lock_guard fast path when the probe is off.
std::vector<double>& lf_lock_wait_per_thread_storage() {
    static std::vector<double> v;
    return v;
}
}  // namespace (anon, file-scope)

void init_lf_flint_factor_per_thread(int n_threads) {
    auto& v = lf_ff_per_thread_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_flint_factor_per_thread() {
    auto& v = lf_ff_per_thread_storage();
    for (auto& x : v) x = 0.0;
}
double sum_lf_flint_factor_per_thread() {
    double s = 0.0;
    for (double x : lf_ff_per_thread_storage()) s += x;
    return s;
}

void init_lf_cache_hits_per_thread(int n_threads) {
    auto& v = lf_hits_per_thread_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0L);
}
void reset_lf_cache_hits_per_thread() {
    auto& v = lf_hits_per_thread_storage();
    for (auto& x : v) x = 0L;
}
long sum_lf_cache_hits_per_thread() {
    long s = 0;
    for (long x : lf_hits_per_thread_storage()) s += x;
    return s;
}

void init_lf_cache_misses_per_thread(int n_threads) {
    auto& v = lf_misses_per_thread_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0L);
}
void reset_lf_cache_misses_per_thread() {
    auto& v = lf_misses_per_thread_storage();
    for (auto& x : v) x = 0L;
}
long sum_lf_cache_misses_per_thread() {
    long s = 0;
    for (long x : lf_misses_per_thread_storage()) s += x;
    return s;
}

void init_lf_lock_held_per_thread(int n_threads) {
    auto& v = lf_lock_held_per_thread_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_lock_held_per_thread() {
    auto& v = lf_lock_held_per_thread_storage();
    for (auto& x : v) x = 0.0;
}
double sum_lf_lock_held_per_thread() {
    double s = 0.0;
    for (double x : lf_lock_held_per_thread_storage()) s += x;
    return s;
}

// iter-37 lock-acquire wait probe: mirrors lf_lock_held API.
void init_lf_lock_wait_per_thread(int n_threads) {
    auto& v = lf_lock_wait_per_thread_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_lock_wait_per_thread() {
    auto& v = lf_lock_wait_per_thread_storage();
    for (auto& x : v) x = 0.0;
}
double sum_lf_lock_wait_per_thread() {
    double s = 0.0;
    for (double x : lf_lock_wait_per_thread_storage()) s += x;
    return s;
}

void init_lf_cache_key_build_per_thread(int n_threads) {
    auto& v = lf_cache_key_build_per_thread_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_cache_key_build_per_thread() {
    auto& v = lf_cache_key_build_per_thread_storage();
    for (auto& x : v) x = 0.0;
}
double sum_lf_cache_key_build_per_thread() {
    double s = 0.0;
    for (double x : lf_cache_key_build_per_thread_storage()) s += x;
    return s;
}

// 2026-04-29 (Probe 2): four post-FLINT extraction sub-timers.
namespace {
std::vector<double>& lf_post_transplant_storage() {
    static std::vector<double> v;
    return v;
}
std::vector<double>& lf_post_rat_ctor_storage() {
    static std::vector<double> v;
    return v;
}
std::vector<double>& lf_post_constant_to_string_storage() {
    static std::vector<double> v;
    return v;
}
std::vector<double>& lf_post_clone_from_raw_storage() {
    static std::vector<double> v;
    return v;
}
}  // namespace

void init_lf_post_transplant_per_thread(int n_threads) {
    auto& v = lf_post_transplant_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_post_transplant_per_thread() {
    for (auto& x : lf_post_transplant_storage()) x = 0.0;
}
double sum_lf_post_transplant_per_thread() {
    double s = 0.0;
    for (double x : lf_post_transplant_storage()) s += x;
    return s;
}

void init_lf_post_rat_ctor_per_thread(int n_threads) {
    auto& v = lf_post_rat_ctor_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_post_rat_ctor_per_thread() {
    for (auto& x : lf_post_rat_ctor_storage()) x = 0.0;
}
double sum_lf_post_rat_ctor_per_thread() {
    double s = 0.0;
    for (double x : lf_post_rat_ctor_storage()) s += x;
    return s;
}

void init_lf_post_constant_to_string_per_thread(int n_threads) {
    auto& v = lf_post_constant_to_string_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_post_constant_to_string_per_thread() {
    for (auto& x : lf_post_constant_to_string_storage()) x = 0.0;
}
double sum_lf_post_constant_to_string_per_thread() {
    double s = 0.0;
    for (double x : lf_post_constant_to_string_storage()) s += x;
    return s;
}

void init_lf_post_clone_from_raw_per_thread(int n_threads) {
    auto& v = lf_post_clone_from_raw_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_post_clone_from_raw_per_thread() {
    for (auto& x : lf_post_clone_from_raw_storage()) x = 0.0;
}
double sum_lf_post_clone_from_raw_per_thread() {
    double s = 0.0;
    for (double x : lf_post_clone_from_raw_storage()) s += x;
    return s;
}

// 2026-04-29 (Probe 2): scoped accumulator. Captures the wall interval
// from construction to destruction into the per-thread slot of `v`.
// Same pattern as LockHeldTimer above but parameterised by storage.
namespace {
struct AccumTimer {
    std::chrono::steady_clock::time_point t0;
    std::vector<double>& v;
    int tid;
    AccumTimer(std::vector<double>& v_, int tid_)
        : t0(std::chrono::steady_clock::now()), v(v_), tid(tid_) {}
    ~AccumTimer() {
        if (static_cast<size_t>(tid) < v.size()) {
            v[static_cast<size_t>(tid)] +=
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t0).count();
        }
    }
};
}  // namespace

namespace {
// RAII helper: records held-time inside the surrounding lock_guard.
// Last-declared, first-destroyed semantics put the timer destructor
// before the lock_guard destructor, so the captured interval covers
// the entire critical section (including any return-by-value copy
// that runs as part of stack unwinding before lock release).
struct LockHeldTimer {
    std::chrono::steady_clock::time_point t0;
    int tid;
    LockHeldTimer(int tid_)
        : t0(std::chrono::steady_clock::now()), tid(tid_) {}
    ~LockHeldTimer() {
        auto t1 = std::chrono::steady_clock::now();
        auto& v = lf_lock_held_per_thread_storage();
        if (static_cast<size_t>(tid) < v.size()) {
            v[static_cast<size_t>(tid)] +=
                std::chrono::duration<double>(t1 - t0).count();
        }
    }
};

// iter-37 RAII helper: records lock-ACQUIRE-wait time (the queueing
// time on a shard mutex), separate from LockHeldTimer's held-time.
// Usage:
//
//   LockAcquireWaitProbe _wp(tid);          // captures t0 BEFORE lock_guard
//   std::lock_guard<std::mutex> lk(s.mu);   // ctor blocks until acquired
//   _wp.record();                           // captures t1 = (just acquired)
//   LockHeldTimer _hl(tid);                 // start held-time interval
//
// Default-OFF env-gated under HF_LF_LOCK_WAIT_PROFILE=1. When the
// probe is disabled the ctor skips the steady_clock::now() call
// (1 branch on a cached static int) and record() is a no-op,
// preserving the iter-36 cache fast-path overhead.
struct LockAcquireWaitProbe {
    std::chrono::steady_clock::time_point t0;
    int tid;
    bool enabled;
    explicit LockAcquireWaitProbe(int tid_, bool enabled_)
        : tid(tid_), enabled(enabled_) {
        if (enabled) t0 = std::chrono::steady_clock::now();
    }
    void record() {
        if (!enabled) return;
        auto t1 = std::chrono::steady_clock::now();
        auto& v = lf_lock_wait_per_thread_storage();
        if (static_cast<size_t>(tid) < v.size()) {
            v[static_cast<size_t>(tid)] +=
                std::chrono::duration<double>(t1 - t0).count();
        }
    }
};
}  // namespace

// Phase-d15 deeper drill (round 4): degree-bucketed shape histogram.
namespace {
std::vector<double>& lf_ff_deg1_per_thread_storage() {
    static std::vector<double> v;
    return v;
}
std::vector<double>& lf_ff_deg2_per_thread_storage() {
    static std::vector<double> v;
    return v;
}
std::vector<long>& lf_miss_deg1_per_thread_storage() {
    static std::vector<long> v;
    return v;
}
std::vector<long>& lf_miss_deg2_per_thread_storage() {
    static std::vector<long> v;
    return v;
}
}  // namespace (anon)

void init_lf_flint_deg1_per_thread(int n_threads) {
    auto& v = lf_ff_deg1_per_thread_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_flint_deg1_per_thread() {
    for (auto& x : lf_ff_deg1_per_thread_storage()) x = 0.0;
}
double sum_lf_flint_deg1_per_thread() {
    double s = 0.0;
    for (double x : lf_ff_deg1_per_thread_storage()) s += x;
    return s;
}

void init_lf_flint_deg2_per_thread(int n_threads) {
    auto& v = lf_ff_deg2_per_thread_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_flint_deg2_per_thread() {
    for (auto& x : lf_ff_deg2_per_thread_storage()) x = 0.0;
}
double sum_lf_flint_deg2_per_thread() {
    double s = 0.0;
    for (double x : lf_ff_deg2_per_thread_storage()) s += x;
    return s;
}

void init_lf_miss_deg1_per_thread(int n_threads) {
    auto& v = lf_miss_deg1_per_thread_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0L);
}
void reset_lf_miss_deg1_per_thread() {
    for (auto& x : lf_miss_deg1_per_thread_storage()) x = 0L;
}
long sum_lf_miss_deg1_per_thread() {
    long s = 0;
    for (long x : lf_miss_deg1_per_thread_storage()) s += x;
    return s;
}

void init_lf_miss_deg2_per_thread(int n_threads) {
    auto& v = lf_miss_deg2_per_thread_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0L);
}
void reset_lf_miss_deg2_per_thread() {
    for (auto& x : lf_miss_deg2_per_thread_storage()) x = 0L;
}
long sum_lf_miss_deg2_per_thread() {
    long s = 0;
    for (long x : lf_miss_deg2_per_thread_storage()) s += x;
    return s;
}

// Phase-d15 deeper drill (round 5): deg-3+ output-shape classifier.
namespace {
std::vector<long>& lf_d3p_all_lin_count_storage() {
    static std::vector<long> v;
    return v;
}
std::vector<double>& lf_d3p_all_lin_s_storage() {
    static std::vector<double> v;
    return v;
}
std::vector<long>& lf_d3p_has_nl_count_storage() {
    static std::vector<long> v;
    return v;
}
std::vector<double>& lf_d3p_has_nl_s_storage() {
    static std::vector<double> v;
    return v;
}
}  // namespace (anon)

void init_lf_d3p_all_linear_count_per_thread(int n_threads) {
    auto& v = lf_d3p_all_lin_count_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0L);
}
void reset_lf_d3p_all_linear_count_per_thread() {
    for (auto& x : lf_d3p_all_lin_count_storage()) x = 0L;
}
long sum_lf_d3p_all_linear_count_per_thread() {
    long s = 0;
    for (long x : lf_d3p_all_lin_count_storage()) s += x;
    return s;
}

void init_lf_d3p_all_linear_s_per_thread(int n_threads) {
    auto& v = lf_d3p_all_lin_s_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_d3p_all_linear_s_per_thread() {
    for (auto& x : lf_d3p_all_lin_s_storage()) x = 0.0;
}
double sum_lf_d3p_all_linear_s_per_thread() {
    double s = 0.0;
    for (double x : lf_d3p_all_lin_s_storage()) s += x;
    return s;
}

void init_lf_d3p_has_nonlinear_count_per_thread(int n_threads) {
    auto& v = lf_d3p_has_nl_count_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0L);
}
void reset_lf_d3p_has_nonlinear_count_per_thread() {
    for (auto& x : lf_d3p_has_nl_count_storage()) x = 0L;
}
long sum_lf_d3p_has_nonlinear_count_per_thread() {
    long s = 0;
    for (long x : lf_d3p_has_nl_count_storage()) s += x;
    return s;
}

void init_lf_d3p_has_nonlinear_s_per_thread(int n_threads) {
    auto& v = lf_d3p_has_nl_s_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_d3p_has_nonlinear_s_per_thread() {
    for (auto& x : lf_d3p_has_nl_s_storage()) x = 0.0;
}
double sum_lf_d3p_has_nonlinear_s_per_thread() {
    double s = 0.0;
    for (double x : lf_d3p_has_nl_s_storage()) s += x;
    return s;
}

// Phase-d15 deeper drill (round 6): squarefree / repeated split for
// deg-3+ all-linear FLINT calls.
namespace {
std::vector<long>& lf_d3p_sqfree_count_storage() {
    static std::vector<long> v;
    return v;
}
std::vector<double>& lf_d3p_sqfree_s_storage() {
    static std::vector<double> v;
    return v;
}
std::vector<long>& lf_d3p_repeated_count_storage() {
    static std::vector<long> v;
    return v;
}
std::vector<double>& lf_d3p_repeated_s_storage() {
    static std::vector<double> v;
    return v;
}
}  // namespace (anon)

void init_lf_d3p_squarefree_count_per_thread(int n_threads) {
    auto& v = lf_d3p_sqfree_count_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0L);
}
void reset_lf_d3p_squarefree_count_per_thread() {
    for (auto& x : lf_d3p_sqfree_count_storage()) x = 0L;
}
long sum_lf_d3p_squarefree_count_per_thread() {
    long s = 0;
    for (long x : lf_d3p_sqfree_count_storage()) s += x;
    return s;
}

void init_lf_d3p_squarefree_s_per_thread(int n_threads) {
    auto& v = lf_d3p_sqfree_s_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_d3p_squarefree_s_per_thread() {
    for (auto& x : lf_d3p_sqfree_s_storage()) x = 0.0;
}
double sum_lf_d3p_squarefree_s_per_thread() {
    double s = 0.0;
    for (double x : lf_d3p_sqfree_s_storage()) s += x;
    return s;
}

void init_lf_d3p_repeated_count_per_thread(int n_threads) {
    auto& v = lf_d3p_repeated_count_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0L);
}
void reset_lf_d3p_repeated_count_per_thread() {
    for (auto& x : lf_d3p_repeated_count_storage()) x = 0L;
}
long sum_lf_d3p_repeated_count_per_thread() {
    long s = 0;
    for (long x : lf_d3p_repeated_count_storage()) s += x;
    return s;
}

void init_lf_d3p_repeated_s_per_thread(int n_threads) {
    auto& v = lf_d3p_repeated_s_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_d3p_repeated_s_per_thread() {
    for (auto& x : lf_d3p_repeated_s_storage()) x = 0.0;
}
double sum_lf_d3p_repeated_s_per_thread() {
    double s = 0.0;
    for (double x : lf_d3p_repeated_s_storage()) s += x;
    return s;
}

// Phase-d15 deeper drill (round 7): squarefree-first path counters.
// Wall: total time inside the sqf path, decomposed into the
// `factor_squarefree` call and the inner FLINT factor calls on
// non-degree-1 u_i bases.
namespace {
std::vector<double>& lf_sqf_total_s_storage() { static std::vector<double> v; return v; }
std::vector<double>& lf_sqf_decomp_s_storage() { static std::vector<double> v; return v; }
std::vector<double>& lf_sqf_inner_factor_s_storage() { static std::vector<double> v; return v; }
std::vector<long>& lf_sqf_calls_storage() { static std::vector<long> v; return v; }
std::vector<long>& lf_sqf_inner_factor_calls_storage() { static std::vector<long> v; return v; }
std::vector<long>& lf_sqf_bailouts_storage() { static std::vector<long> v; return v; }
}  // namespace (anon)

void init_lf_sqf_total_s_per_thread(int n_threads) {
    auto& v = lf_sqf_total_s_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_sqf_total_s_per_thread() { for (auto& x : lf_sqf_total_s_storage()) x = 0.0; }
double sum_lf_sqf_total_s_per_thread() {
    double s = 0.0; for (double x : lf_sqf_total_s_storage()) s += x; return s;
}

void init_lf_sqf_decomp_s_per_thread(int n_threads) {
    auto& v = lf_sqf_decomp_s_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_sqf_decomp_s_per_thread() { for (auto& x : lf_sqf_decomp_s_storage()) x = 0.0; }
double sum_lf_sqf_decomp_s_per_thread() {
    double s = 0.0; for (double x : lf_sqf_decomp_s_storage()) s += x; return s;
}

void init_lf_sqf_inner_factor_s_per_thread(int n_threads) {
    auto& v = lf_sqf_inner_factor_s_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_lf_sqf_inner_factor_s_per_thread() { for (auto& x : lf_sqf_inner_factor_s_storage()) x = 0.0; }
double sum_lf_sqf_inner_factor_s_per_thread() {
    double s = 0.0; for (double x : lf_sqf_inner_factor_s_storage()) s += x; return s;
}

void init_lf_sqf_calls_per_thread(int n_threads) {
    auto& v = lf_sqf_calls_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0L);
}
void reset_lf_sqf_calls_per_thread() { for (auto& x : lf_sqf_calls_storage()) x = 0L; }
long sum_lf_sqf_calls_per_thread() {
    long s = 0; for (long x : lf_sqf_calls_storage()) s += x; return s;
}

void init_lf_sqf_inner_factor_calls_per_thread(int n_threads) {
    auto& v = lf_sqf_inner_factor_calls_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0L);
}
void reset_lf_sqf_inner_factor_calls_per_thread() { for (auto& x : lf_sqf_inner_factor_calls_storage()) x = 0L; }
long sum_lf_sqf_inner_factor_calls_per_thread() {
    long s = 0; for (long x : lf_sqf_inner_factor_calls_storage()) s += x; return s;
}

void init_lf_sqf_bailouts_per_thread(int n_threads) {
    auto& v = lf_sqf_bailouts_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0L);
}
void reset_lf_sqf_bailouts_per_thread() { for (auto& x : lf_sqf_bailouts_storage()) x = 0L; }
long sum_lf_sqf_bailouts_per_thread() {
    long s = 0; for (long x : lf_sqf_bailouts_storage()) s += x; return s;
}

namespace {

std::string fmpq_to_string(const fmpq_t q) {
    char* s = fmpq_get_str(nullptr, 10, q);
    std::string out(s);
    flint_free(s);
    return out;
}

// Wrap a raw fmpq_mpoly_struct into our Poly by cloning.
Poly clone_from_raw(const PolyCtx& ctx, const fmpq_mpoly_struct* src) {
    Poly r(ctx);
    fmpq_mpoly_set(r.raw(), const_cast<fmpq_mpoly_struct*>(src), ctx.raw());
    return r;
}

// Shared mutex-protected cache of prior linear-factors results.
// Was thread_local in earlier rounds; moved to shared because the
// OpenMP plan would otherwise refactor a hot poly once per worker,
// fragmenting hit rate.  Cleared by clear_linear_factors_cache() at
// the top of integration_step so entries don't outlive their
// PolyCtx across steps.
//
// Caveat: under heavy late-stage steps (e.g. Smirnov tst2 step 4)
// this cache can accumulate ~4 GB of LinearFactorization values
// within a single step.  Disabling it via HF_DISABLE_LF_CACHE=1
// drops tst2 peak RSS from 8.6 GB to 4.6 GB with no measurable
// compute slowdown — i.e. on tst2 the cache wasn't paying for
// itself.  Whether the same is true on heavier integrands is an
// open question; do not assume.
//
// Note: an earlier version of this comment claimed the cache was
// also necessary for AlgebraicLetterTable Wm_/Wp_ atom de-dup.
// That is wrong — AlgebraicLetterTable::content_index_ already
// de-dupes by content (algebraic_letters.cpp:53-58), and disabling
// this cache yields bit-identical result expressions.
// 2026-04-26 structural-hash cache key. Replaces a `std::string`
// formed via `Poly::to_string()` (~89-1300 µs/call wall on tst2;
// 26% of tst2 wall measured at `cache_key_build/tst2.stderr`) with
// a 128-bit FNV-1a structural hash over the canonical fmpq_mpoly_t
// term sequence. Birthday probability at 1M keys ≈ 1.5e-27, well
// below the practical-zero threshold; no defensive equality is
// performed. See poly_struct_hash.hpp + cache_disable_analysis.md.
using LFCacheKey = std::pair<uint64_t, uint64_t>;

// 2026-04-26 (Stage-2 sharded cache). N independent shards, each
// with its own mutex + map, keyed by `(LFCacheKey.first ^
// LFCacheKey.second) & shard_mask`. The lower bits of the FNV-1a
// output are already well-distributed (PairU64Hash uses the same
// pattern for unordered_map's bucket index).
//
// Why: the prior single-mutex cache hit a 24 GB ceiling on Smirnov
// tst3 and serialized 13-way OMP traffic on a single map. Sharding
// reduces contention (per-shard ~1/64 of total) and is a layout
// change only: `LinearFactorization` values are stored bit-for-bit
// identically, so cache-derived expressions are unchanged.
//
// Override via `HF_LF_CACHE_SHARDS=N` (rounded up to next power of
// 2, clamped to [1, 1024]). N=1 reproduces the prior single-mutex
// behaviour modulo cache layout.
// iter-38 lock-contention fix (HF MZV-rewrite C-prep.1, follow-up to
// iter-36 commit 89e000704 / iter-37 commit 0b386971f):
// `LFShard::mu` was `std::mutex`; under SCALAR_REP=1 + OMP=13 the
// reader-dominant load (iter-36 cache-hit ratio 99.85 %) created
// pure synchronization contention, +208 s of `lf_lock_wait_s` on
// tst2 (iter-37 reconciliation). Promoting `mu` to
// `std::shared_mutex` lets the LOOKUP path (read-only `s.map.find`)
// take a `std::shared_lock` — concurrent readers are contention-free
// across N threads — while STORE / CLEAR / RESIDENCY take a
// `std::unique_lock` (writer-exclusive). The cache uses
// emplace_no_replace semantics; readers only ever see the
// pre-insert (find returns end) or post-insert (find returns iter
// to fully constructed value) states, never a partially-constructed
// entry, since writers hold the unique_lock through the entire
// `s.map.emplace`.
struct LFShard {
    std::shared_mutex mu;
    std::unordered_map<LFCacheKey, LinearFactorization, PairU64Hash> map;
};

// Lazy-initialized via call_once on first cache access. Sized once
// per process; `g_lf_shards` is constructed in-place because LFShard
// (containing std::shared_mutex) is non-copyable / non-movable.
std::vector<std::unique_ptr<LFShard>> g_lf_shards;
unsigned g_lf_shard_mask = 0;
std::once_flag g_lf_shards_init_once;

inline unsigned next_pow2_ge(unsigned x) {
    if (x == 0) return 1;
    --x;
    x |= x >> 1;  x |= x >> 2;  x |= x >> 4;
    x |= x >> 8;  x |= x >> 16;
    return x + 1;
}

inline void init_lf_shards_once() {
    std::call_once(g_lf_shards_init_once, []() {
        unsigned n = 64;
        if (const char* s = HF_FLAG_LF_CACHE_SHARDS) {
            char* end = nullptr;
            long parsed = std::strtol(s, &end, 10);
            if (end != s && parsed >= 1 && parsed <= 1024) {
                n = static_cast<unsigned>(parsed);
            }
        }
        n = next_pow2_ge(n);
        if (n > 1024) n = 1024;
        g_lf_shards.reserve(n);
        for (unsigned i = 0; i < n; ++i) {
            g_lf_shards.emplace_back(std::make_unique<LFShard>());
        }
        g_lf_shard_mask = n - 1;
    });
}

inline LFShard& shard_for(const LFCacheKey& k) {
    init_lf_shards_once();
    const uint64_t mixed = k.first ^ k.second;
    return *g_lf_shards[mixed & g_lf_shard_mask];
}

LFCacheKey linear_factors_cache_key(const Poly& p, size_t var_idx,
                                     bool introduce_al,
                                     bool sqf,
                                     bool compute_constant) {
    return poly_struct_hash(p, var_idx, introduce_al, sqf, compute_constant);
}

// PINNED 2026-05-18 (v2 iter-23) — lf_cache default-ON, opt-out via HF_DISABLE_LF_CACHE=1
//   fixture/gate : Smirnov tst{0,1,2,3} hot atom-table inputs; v1 iter-13
//                  leak-diagnostic (test/Smirnov/tst3.txt)
//   measurement  : LF cache hit rate dominates the AlgebraicLetterTable
//                  re-derivation cost on tst3 (idempotent Wm_/Wp_ atom
//                  allocation contract, serial-equivalent semantics under
//                  the race-loser-noop insert below); disable measured
//                  >> 5% wall regression on tst3
//   falsifier    : (a) heap-attribution probe shows LF cache < 5% of peak
//                  RSS on tst3 AND (b) cold-build wall regression with
//                  disable < 1% on tst3 → cache can flip default-OFF.
//                  NB: HF_DISABLE_LF_CACHE=1 must NEVER be set on tst3 or
//                  heavier fixtures without RSS headroom check (v1
//                  diagnostic spike → >200 GB freeze risk).
//
// Locked emplace — race-loser's insert is a no-op (emplace keeps
// the first-inserted value), matching the serial-equivalent
// semantics that AlgebraicLetterTable::allocate relies on for
// idempotent Wm_/Wp_ atom allocation.
//
// HF_DISABLE_LF_CACHE=1 in the environment disables the insert
// path (used by the Smirnov tst3 leak diagnostic to test the
// "cache is the dominant memory cost" hypothesis without
// rebuilding).  The lookup is also short-circuited below.
static bool lf_cache_disabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* s = HF_FLAG_DISABLE_LF_CACHE;
        cached = (s && s[0] && s[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

// Phase-d15 deeper drill (round 7): squarefree-first path. When
// HF_LF_SQF=1 is set, deg-3+ inputs (without algebraic letters) are
// routed through `fmpq_mpoly_factor_squarefree` first. For each
// resulting (u_i, mult) pair: if u_i is degree-1 in the target var
// we extract the linear factor directly, skipping the inner FLINT
// factor call entirely. Higher-degree u_i bases fall through to a
// regular `fmpq_mpoly_factor` on the (smaller, squarefree) input.
//
// Cache compatibility: when this path is active, cache keys are
// prefixed with "sqf:" so entries written under different paths
// cannot collide in the shared mutex-protected cache.
static bool lf_sqf_enabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* s = HF_FLAG_LF_SQF;
        cached = (s && s[0] && s[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

// iter-37 lock-acquire wait probe env-gate. Default-OFF; enabling
// adds one steady_clock::now() pair per lock_guard ctor, accumulated
// to a per-thread vector. Surfaced in HF_STEP_TRACE JSON as
// `lf_lock_wait_s` (sibling of `lf_lock_held_s`).
static bool lf_lock_wait_profile_enabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* s = HF_FLAG_LF_LOCK_WAIT_PROFILE;
        cached = (s && s[0] && s[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

void cache_store_locked(const LFCacheKey& key,
                         const LinearFactorization& val) {
    if (lf_cache_disabled()) return;
    // Phase 1 Task 1.E: hf_get_thread_num() — see hf_thread_num.hpp.
    const int _tid = ::hyperflint::runtime::hf_get_thread_num();
    LFShard& s = shard_for(key);
    LockAcquireWaitProbe _wp(_tid, lf_lock_wait_profile_enabled());
    // iter-38: writer takes unique_lock (exclusive); blocks all readers.
    std::unique_lock<std::shared_mutex> lk(s.mu);
    _wp.record();
    LockHeldTimer _hl(_tid);
    s.map.emplace(key, val);
}


}  // namespace

void clear_linear_factors_cache() {
    // Phase 1 Task 1.E: hf_get_thread_num() — see hf_thread_num.hpp.
    const int _tid = ::hyperflint::runtime::hf_get_thread_num();
    // Hold-time accounting includes per-shard locks back-to-back.
    // Since clears happen between integration_steps (single-threaded
    // master), there is no parallel contention concern.
    init_lf_shards_once();
    for (auto& sp : g_lf_shards) {
        LFShard& s = *sp;
        LockAcquireWaitProbe _wp(_tid, lf_lock_wait_profile_enabled());
        // iter-38: writer takes unique_lock (exclusive); rare path.
        std::unique_lock<std::shared_mutex> lk(s.mu);
        _wp.record();
        LockHeldTimer _hl(_tid);
        s.map.clear();
    }
}

LFCacheResidency linear_factors_cache_residency() {
    LFCacheResidency r;
    init_lf_shards_once();
    for (auto& sp : g_lf_shards) {
        LFShard& s = *sp;
        // iter-38: residency only iterates / reads; reader takes
        // shared_lock so concurrent readers don't serialize.
        std::shared_lock<std::shared_mutex> lk(s.mu);
        for (const auto& kv : s.map) {
            const LinearFactorization& lf = kv.second;
            r.entry_count += 1;
            // LinearFactor.pole is a Rat (= num+den Polys).
            for (const auto& l : lf.linear) {
                r.poly_count += 2;
                r.pole_rats  += l.pole.total_bytes();
            }
            // NonlinearFactor.polynomial is a single Poly.
            for (const auto& nl : lf.nonlinear) {
                r.poly_count   += 1;
                r.nonlin_polys += nl.polynomial.total_bytes();
            }
        }
    }
    r.total_bytes = r.nonlin_polys + r.pole_rats;
    return r;
}

// HF FF Phase 5 §E Step E.2-impl-2 (iter-61-β.3).
//
// Renamed from `linear_factors` per implementation_memo §3.3 +
// §iter-59-fold-REQ-1. The pre-iter-61 body is unchanged below;
// the new public `linear_factors` (further down in this TU) wraps
// this with the §E outer cache.
static LinearFactorization
linear_factors_impl_se(const Poly& p, size_t var_idx,
                       std::shared_ptr<ZWTable> zw_tab,
                       bool introduce_algebraic_letters,
                       bool compute_constant) {
    const PolyCtx& ctx = p.ctx();
    if (var_idx >= ctx.vars().size()) {
        throw std::out_of_range("linear_factors: var_idx out of range");
    }
    // HF FF Phase 5 §A.1 iter-50: op_call emit at linear_factors entry
    // (§3.1 op #3). Arity=1; input hash mixes the Poly canonical-bits hash
    // with the var_idx (so distinct factor-target variables on the same
    // input still distinguish in the dedup-rate column). The
    // introduce_algebraic_letters flag is folded into the input hash so the
    // Wm/Wp pathway is also distinguished. OFF-path fast-guard via
    // `hf_probe_active` branch.
    if (hf_probe_active) {
        uint64_t ih = kFnv1a64OffsetBasis;
        ih = hf_probe_fnv1a64_mix_u64(ih,
                hf_probe_canonical_hash_poly(p.raw(), ctx.raw()));
        ih = hf_probe_fnv1a64_mix_u64(ih, (uint64_t)var_idx);
        ih = hf_probe_fnv1a64_mix_u64(ih,
                (uint64_t)(introduce_algebraic_letters ? 1u : 0u));
        hf_probe_emit_op_call("linear_factors", ih, 1);
    }

    // Phase-B B4 (design v2 §3.5a row 4 + §3.8): post-LR-cache-lookup
    // `as_rat` boundary. Under runtime::scalar_rep_enabled() (the
    // HF_USE_SCALAR_REP env-gate), every LinearFactor.pole at the
    // function-exit boundary is round-tripped through SymCoef::from_rat
    // -> SymCoefSplit::from_rat -> SymCoefSplit::as_rat ->
    // SymCoef::as_rat via the shared helper
    // `runtime::roundtrip_rat_through_scs`. The cache HIT path is the
    // headline boundary cited in §3.5a, but every return point gets the
    // verifier so the dispatch shape mirrors B1.c / B2 / B3. NonlinearFactor
    // .polynomial does not cross the rat-split boundary (it is a Poly,
    // not a Rat) and so is not round-tripped here. At B-stages on
    // Smirnov, Wm/Wp introduction never fires (linear-in-letters); the
    // round-trip is canonically a no-op (W-side empty hypothesis,
    // b1_scoping_memo.md R2 + design v2 §4.4a Note 2). Mirrors the B3
    // dispatch pattern in transform.cpp::reglim_word.
    //
    // Iter-52 C0c.1 (site 6): the previous per-call
    // `make_shared<ZWTable>(ctx)` inside this lambda body has been
    // killed (per iter-50 MEMO §3.1 + iter-51 §6.5 Q1 ratification);
    // the lambda now reads the function parameter `zw_tab` via [&]
    // capture. Callers MUST provide a non-null `zw_tab` when
    // `runtime::scalar_rep_enabled()` is true (Option A mandatory ABI
    // break per Q1; consistent with iter-42 site-4 commit `5f51b7d28`
    // precedent). When SCALAR_REP is OFF, the lambda's early-return
    // bypasses the parameter; nullptr is acceptable in that mode.
    //
    // Iter-59 reviewer (agentId ac81c3dd674cb73a9) advisory item 1:
    // tighten guard to `(!scalar_rep_enabled() || !zw_tab)` for parity
    // with site-7's iter-58 (f) tighten at integration_step.cpp:2472.
    // Currently unreachable under default callers (SymCoefSplit ctor
    // throws on null zw_table for SCALAR_REP=ON, so the null+ON path
    // surfaces as a loud runtime_error rather than silent UB), but the
    // symmetry across sites is the right defense-in-depth posture.
    auto apply_v1_roundtrip = [&](LinearFactorization&& lfact,
                                    const char* tag) -> LinearFactorization {
        if (!runtime::scalar_rep_enabled() || !zw_tab) return std::move(lfact);
        for (auto& lf : lfact.linear) {
            lf.pole = runtime::roundtrip_rat_through_scs(
                lf.pole, ctx, zw_tab, tag);
        }
        return std::move(lfact);
    };

    // Phase-d15 deeper drill (round 3): per-thread hit/miss + FLINT-wall
    // accounting. tid resolved once at function entry; vector slots are
    // bounds-checked since `init_*_per_thread` may not have been called
    // (e.g. on the standalone CLI path or before the first integration_step).
    // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num() in OMP mode;
    // under HF_USE_GCD=1 returns the GCD slot index.
    const int _lff_tid = ::hyperflint::runtime::hf_get_thread_num();

    // Cache lookup. Small-poly short-circuits below are cheap, so we
    // still let them fall through — the cache insert after factoring
    // is where the real savings come from.
    //
    // When HF_LF_SQF is active, the sqf flag is mixed into the
    // structural-hash seed so entries produced under different paths
    // cannot collide (the sqf path produces a structurally different
    // LinearFactorization than the bare-FLINT path: same poles, but
    // the constant slot is computed from per-base content multiplications
    // rather than FLINT's factor->constant directly).
    // 2026-04-26 cache_key_build instrumentation: direct wall measurement
    // of `linear_factors_cache_key()`. Post-structural-hash this drops
    // from ~89-1300 µs/call to ~0.5-1 µs/call.
    const bool _ck_tg = step_trace_enabled();
    const auto _ck_t0 = _ck_tg ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    const LFCacheKey cache_key =
        linear_factors_cache_key(p, var_idx,
                                  introduce_algebraic_letters,
                                  lf_sqf_enabled(),
                                  compute_constant);
    if (_ck_tg) {
        const auto _ck_t1 = std::chrono::steady_clock::now();
        auto& _ckv = lf_cache_key_build_per_thread_storage();
        if (static_cast<size_t>(_lff_tid) < _ckv.size()) {
            _ckv[static_cast<size_t>(_lff_tid)] +=
                std::chrono::duration<double>(_ck_t1 - _ck_t0).count();
        }
    }
    if (!lf_cache_disabled()) {
        LFShard& s = shard_for(cache_key);
        LockAcquireWaitProbe _wp(_lff_tid, lf_lock_wait_profile_enabled());
        // iter-38: cache-LOOKUP is read-only on the unordered_map.
        // shared_lock allows concurrent readers (cache hit ratio
        // 99.85 % per iter-36 finding) to proceed in parallel.
        // Writers (cache_store_locked / clear) hold unique_lock so
        // readers either see pre-emplace absence (find -> end) or
        // post-emplace stable value (find -> iter to fully
        // constructed value); never partially-constructed.
        //
        // iter-38 reviewer B1 BINDING (internal review):
        // copy-out + early-release pattern. The held shared-lock
        // region only does `s.map.find` + a deep copy of the cached
        // LinearFactorization (Poly/Rat copy ctors call
        // fmpq_mpoly_set internally; deep, no shared FLINT state).
        // After the inner block ends, the shared_lock is released,
        // and apply_v1_roundtrip (which under SCALAR_REP=1 allocates
        // a per-call ZWTable and round-trips each LinearFactor.pole
        // through SymCoef::from_rat -> SymCoefSplit -> as_rat) runs
        // OUTSIDE the lock. This both (a) frees writers from waiting
        // on round-trip CPU and (b) keeps iter-37's lf_lock_wait_s
        // collapse intact.
        LinearFactorization cached;
        bool got_hit = false;
        {
            std::shared_lock<std::shared_mutex> lk(s.mu);
            _wp.record();
            LockHeldTimer _hl(_lff_tid);
            auto it = s.map.find(cache_key);
            if (it != s.map.end()) {
                auto& _hv = lf_hits_per_thread_storage();
                if (static_cast<size_t>(_lff_tid) < _hv.size()) {
                    _hv[static_cast<size_t>(_lff_tid)] += 1;
                }
                cached = it->second;
                got_hit = true;
            }
        }  // shared_lock + LockHeldTimer destruct here.
        if (got_hit) {
            // B4 post-LR-cache-lookup `as_rat` boundary (cache HIT
            // path, the headline §3.5a row); now runs lock-free.
            return apply_v1_roundtrip(std::move(cached),
                                       "linear_factors/cache_hit");
        }
    }
    {
        // Either the cache miss or HF_DISABLE_LF_CACHE=1: we are about
        // to do real work. Counted as a miss either way; the driver
        // surfaces hits / (hits + misses) as the effective hit rate
        // (which is identically 0 when the cache is disabled).
        auto& _mv = lf_misses_per_thread_storage();
        if (static_cast<size_t>(_lff_tid) < _mv.size()) {
            _mv[static_cast<size_t>(_lff_tid)] += 1;
        }
    }

    // Phase-d15 deeper drill (round 4): bucket the cache miss by input
    // degree-in-var (the shape that actually drives FLINT cost).
    // degree_in_var returns -1 for the zero polynomial; treat as 0.
    // The early-exit branches below (zero / free-of-var) skip FLINT
    // entirely, so we only bump the deg1/deg2 miss counters AFTER
    // those checks have ruled themselves out — deferred to right
    // before the worth_narrowing decision.
    long _lff_input_deg = 0;
    // Phase-d15 deeper drill (round 5): mirror the per-call FLINT wall
    // so we can attribute it to the all-linear / has-nonlinear bucket
    // AFTER the post-processing loop has populated `out`. Set inside
    // each FLINT call site below (narrow + wide branches).
    double _lff_flint_wall_s = 0.0;

    LinearFactorization out;

    // Zero / free-of-var / nonzero-constant-in-var short-circuits.
    if (fmpq_mpoly_is_zero(p.raw(), ctx.raw())) {
        if (compute_constant) out.constant = "0";
        cache_store_locked(cache_key, out);
        return apply_v1_roundtrip(std::move(out), "linear_factors/zero");
    }
    if (p.degree_in_var(var_idx) <= 0) {
        // Entirely in the other variables: treat the whole thing as
        // a constant (in var), no pole contributions.
        if (compute_constant) {
            // Probe 2: time the free-of-var early-exit to_string().
            const bool _tg = step_trace_enabled();
            const auto _ts_t0 = _tg ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
            out.constant = p.to_string();
            if (_tg) {
                auto& _v = lf_post_constant_to_string_storage();
                if (static_cast<size_t>(_lff_tid) < _v.size()) {
                    _v[static_cast<size_t>(_lff_tid)] +=
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - _ts_t0).count();
                }
            }
        }
        cache_store_locked(cache_key, out);
        return apply_v1_roundtrip(std::move(out),
                                   "linear_factors/free_of_var");
    }

    // Phase-d15 deeper drill (round 4): degree-bucket the input now
    // that the trivial early-exit cases (zero, free-of-var) have been
    // handled. The remaining path always calls fmpq_mpoly_factor, so
    // every (deg1, deg2, deg3+) bucket maps to a real FLINT call.
    _lff_input_deg = p.degree_in_var(var_idx);
    if (_lff_input_deg == 1) {
        auto& _mv = lf_miss_deg1_per_thread_storage();
        if (static_cast<size_t>(_lff_tid) < _mv.size()) {
            _mv[static_cast<size_t>(_lff_tid)] += 1;
        }
    } else if (_lff_input_deg == 2) {
        auto& _mv = lf_miss_deg2_per_thread_storage();
        if (static_cast<size_t>(_lff_tid) < _mv.size()) {
            _mv[static_cast<size_t>(_lff_tid)] += 1;
        }
    }

    // 2026-05-04 PIVOT: hoist used_wide so the worth_narrowing block
    // below + the deg-2 narrow algebraic-letter arm can reuse it. The
    // single-shot guard (`_lff_used_wide_set`) coordinates with the
    // unconditional populate at the worth_narrowing site below.
    std::vector<size_t> used_wide;
    bool _lff_used_wide_set = false;

    // Phase-d15 deeper drill (round 7): squarefree-first path. Routes
    // deg-3+ inputs through `fmpq_mpoly_factor_squarefree` and then
    // handles each (u_i, mult) base directly when u_i is degree-1 in
    // the target var (no inner FLINT factor needed). Higher-degree
    // u_i bases bail back to the standard FLINT path. Round-6 measured
    // 100% repeated-multiplicity on this slice, but that does NOT
    // imply every squarefree base is degree-1 in var: e.g. (x-a)²(x-b)²
    // is repeated and yet has a single deg-2 squarefree base, which
    // forces a bailout. Per-call bailout rate is tracked via
    // lf_sqf_bailouts.
    //
    // We DO run this path even when introduce_algebraic_letters=true:
    // the operations here are entirely in the wide ctx (no narrow
    // transplant), so Wm/Wp atoms remain accessible. The deg-≥2
    // squarefree bailout naturally hands those cases back to the
    // existing wide-ctx FLINT path which knows how to allocate Wm/Wp.
    // A single debug session on 2026-04-26 saw intro_AL=true on every
    // deg-3+ pentagon-gauge call, so a `!intro_AL` gate would have
    // silently skipped the lever; that observation has not been
    // generalised to other regimes.
    if (_lff_input_deg >= 3 && lf_sqf_enabled()) {
        const auto _sqf_t0 = std::chrono::steady_clock::now();
        {
            auto& _v = lf_sqf_calls_storage();
            if (static_cast<size_t>(_lff_tid) < _v.size()) {
                _v[static_cast<size_t>(_lff_tid)] += 1;
            }
        }

        // Step 1: Yun-style squarefree decomposition.
        fmpq_mpoly_factor_t SQ;
        fmpq_mpoly_factor_init(SQ, ctx.raw());
        const auto _decomp_t0 = std::chrono::steady_clock::now();
        const int sqf_ok = fmpq_mpoly_factor_squarefree(
            SQ, const_cast<fmpq_mpoly_struct*>(p.raw()), ctx.raw());
        {
            auto& _dv = lf_sqf_decomp_s_storage();
            if (static_cast<size_t>(_lff_tid) < _dv.size()) {
                _dv[static_cast<size_t>(_lff_tid)] +=
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - _decomp_t0).count();
            }
        }

        if (sqf_ok != 0) {
            // Step 2: walk the squarefree decomposition. Build the
            // output `out` directly. If we hit a u_i that is degree
            // >= 2 in var and squarefree (i.e., needs a real factor
            // call), bail out of the sqf path and fall through to the
            // standard FLINT path — we explicitly do NOT recurse into
            // fmpq_mpoly_factor here because it would redo squarefree
            // work that we already paid for. (This is the conservative
            // choice; an inner factor call could be added later if
            // measurements show it pays.)
            bool sqf_succeeded = true;
            // Initialize with FLINT's reported overall constant.
            Poly running_constant(ctx);
            fmpq_mpoly_set_fmpq(running_constant.raw(), SQ->constant, ctx.raw());

            const slong nfac = SQ->num;
            for (slong i = 0; i < nfac; ++i) {
                Poly base = clone_from_raw(ctx, SQ->poly + i);
                const long mult = fmpz_get_si(SQ->exp + i);
                const long base_deg = base.degree_in_var(var_idx);

                if (base_deg <= 0) {
                    // Base is free of var: contributes to constant
                    // with multiplicity `mult`.
                    Poly bm = base.pow(static_cast<unsigned long>(mult));
                    running_constant = running_constant * bm;
                    continue;
                }

                if (base_deg == 1) {
                    // Squarefree degree-1 base: extract the linear
                    // factor directly. f's pole is -c0/lc with that
                    // multiplicity. No inner factor call needed —
                    // this is the win condition.
                    Poly lc = base.coefficient_of_var(var_idx, 1);
                    Poly c0 = base.coefficient_of_var(var_idx, 0);
                    Rat pole(-c0, std::move(lc));
                    out.linear.push_back(LinearFactor{mult, std::move(pole)});
                    continue;
                }

                // Squarefree base with degree >= 2 in var: in our
                // pentagon-gauge regime this is rare (round-5 says all
                // factors are linear in var, so any deg-2+ squarefree
                // base means there are multiple distinct roots with
                // the same multiplicity, packed into one squarefree
                // poly). Bail out and let the standard FLINT path
                // handle it. Net cost: we paid factor_squarefree for
                // nothing; downstream FLINT call redoes the squarefree
                // pass internally.
                sqf_succeeded = false;
                {
                    auto& _bv = lf_sqf_bailouts_storage();
                    if (static_cast<size_t>(_lff_tid) < _bv.size()) {
                        _bv[static_cast<size_t>(_lff_tid)] += 1;
                    }
                }
                break;
            }

            if (sqf_succeeded) {
                if (compute_constant) {
                    out.constant = running_constant.to_string();
                }
                fmpq_mpoly_factor_clear(SQ, ctx.raw());

                {
                    auto& _tv = lf_sqf_total_s_storage();
                    if (static_cast<size_t>(_lff_tid) < _tv.size()) {
                        _tv[static_cast<size_t>(_lff_tid)] +=
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - _sqf_t0).count();
                    }
                }
                cache_store_locked(cache_key, out);
                return apply_v1_roundtrip(std::move(out),
                                           "linear_factors/sqf");
            }

            // Bailout: clear `out` (we may have appended factors), then
            // fall through to the standard path which will overwrite.
            out.linear.clear();
            out.nonlinear.clear();
            out.constant.clear();
        }
        // Sqf decomposition failed or bailed; clean up and fall through.
        fmpq_mpoly_factor_clear(SQ, ctx.raw());
        {
            auto& _tv = lf_sqf_total_s_storage();
            if (static_cast<size_t>(_lff_tid) < _tv.size()) {
                _tv[static_cast<size_t>(_lff_tid)] +=
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - _sqf_t0).count();
            }
        }
    }

    // Narrow-ctx factoring: when the wide ctx has many variables (the
    // MZV-augmented 700-var tst0 case) but `p` actually uses only a
    // handful, fmpq_mpoly_factor still pays O(n_wide_vars) in its
    // variable-compression pass per term. Transplant `p` into a narrow
    // ctx containing exactly the variables it uses, factor there, and
    // lift each linear-pole Rat back to the wide ctx.
    //
    // Bypass the narrow path for the algebraic-letter branch — that
    // path relies on Rat::parse(wide_ctx, "Wm_<i>"/"Wp_<i>") for atoms
    // that live only in the wide ctx. It's rare enough that this is
    // fine as-is.
    const size_t nvars_wide = ctx.vars().size();
    if (!_lff_used_wide_set) {
        used_wide = p.used_var_indices();
        _lff_used_wide_set = true;
    }
    // 2026-05-04 PIVOT: gate on used-fraction only. Wm/Wp introduction
    // now coexists with narrowing via the deg-2 arm below, which
    // transplants base_narrow → base_wide before allocating the
    // algebraic letter (so Rat::parse(ctx, "Wm_/Wp_") still hits the
    // wide ctx that has those atoms). See spec
    // docs/superpowers/specs/2026-05-04-hf-pivot-narrow-ctx-wmwp-design.md.
    const bool worth_narrowing =
        used_wide.size() * 4 < nvars_wide   // heuristic: narrow is
                                            // faster when used
                                            // count is < ~25% of
                                            // wide
        && used_wide.size() >= 1;

    if (worth_narrowing) {
        // used_wide is guaranteed to contain var_idx (p.degree_in_var
        // > 0 above). Build a narrow ctx with just the used var names
        // in their wide-ctx order. Map wide idx -> narrow idx.
        std::vector<std::string> narrow_var_names;
        narrow_var_names.reserve(used_wide.size());
        std::vector<size_t> wide_to_narrow(nvars_wide, SIZE_MAX);
        for (size_t k = 0; k < used_wide.size(); ++k) {
            narrow_var_names.push_back(ctx.vars()[used_wide[k]]);
            wide_to_narrow[used_wide[k]] = k;
        }
        const PolyCtx narrow(std::move(narrow_var_names));

        Poly p_narrow = p.transplant(narrow, wide_to_narrow);

        const size_t narrow_var_idx = wide_to_narrow[var_idx];
        // Sanity guard — should always hold since degree_in_var(var) > 0
        // above implies var_idx ∈ used_wide.
        if (narrow_var_idx == SIZE_MAX) {
            throw std::runtime_error(
                "linear_factors: target var missing from narrow ctx");
        }

        fmpq_mpoly_factor_t F;
        fmpq_mpoly_factor_init(F, narrow.raw());
        const auto _ff_t0 = std::chrono::steady_clock::now();
        const int rc_narrow = fmpq_mpoly_factor(
            F, const_cast<fmpq_mpoly_struct*>(p_narrow.raw()), narrow.raw());
        {
            const double _dt = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - _ff_t0).count();
            _lff_flint_wall_s += _dt;
            auto& _ffv = lf_ff_per_thread_storage();
            if (static_cast<size_t>(_lff_tid) < _ffv.size()) {
                _ffv[static_cast<size_t>(_lff_tid)] += _dt;
            }
            if (_lff_input_deg == 1) {
                auto& _bv = lf_ff_deg1_per_thread_storage();
                if (static_cast<size_t>(_lff_tid) < _bv.size()) {
                    _bv[static_cast<size_t>(_lff_tid)] += _dt;
                }
            } else if (_lff_input_deg == 2) {
                auto& _bv = lf_ff_deg2_per_thread_storage();
                if (static_cast<size_t>(_lff_tid) < _bv.size()) {
                    _bv[static_cast<size_t>(_lff_tid)] += _dt;
                }
            }
        }
        if (rc_narrow == 0) {
            fmpq_mpoly_factor_clear(F, narrow.raw());
            throw std::runtime_error("linear_factors: fmpq_mpoly_factor failed (narrow)");
        }

        // Inverse mapping narrow -> wide. For transplanting linear-
        // factor components (lc, c0, nonlinear base, and the overall
        // constant) back.
        std::vector<size_t> narrow_to_wide(used_wide);  // narrow idx -> wide idx

        Poly constant_narrow(narrow);
        fmpq_mpoly_set_fmpq(constant_narrow.raw(), F->constant, narrow.raw());

        const bool _p2_tg = step_trace_enabled();
        const slong nfac = F->num;
        for (slong i = 0; i < nfac; ++i) {
            // Probe 2: time clone_from_raw (deep-copy of FLINT base into Poly).
            const auto _cfr_t0 = _p2_tg ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
            Poly base_narrow = clone_from_raw(narrow, F->poly + i);
            if (_p2_tg) {
                auto& _v = lf_post_clone_from_raw_storage();
                if (static_cast<size_t>(_lff_tid) < _v.size()) {
                    _v[static_cast<size_t>(_lff_tid)] +=
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - _cfr_t0).count();
                }
            }
            long m = fmpz_get_si(F->exp + i);
            long d = base_narrow.degree_in_var(narrow_var_idx);

            if (d <= 0) {
                Poly bm = base_narrow.pow(static_cast<unsigned long>(m));
                constant_narrow = constant_narrow * bm;
                continue;
            }

            if (d == 1) {
                Poly lc_narrow = base_narrow.coefficient_of_var(narrow_var_idx, 1);
                Poly c0_narrow = base_narrow.coefficient_of_var(narrow_var_idx, 0);
                // Probe 2: time narrow→wide transplant of pole lc and c0.
                const auto _tp_t0 = _p2_tg ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
                Poly lc_wide = lc_narrow.transplant(ctx, narrow_to_wide);
                Poly c0_wide = c0_narrow.transplant(ctx, narrow_to_wide);
                if (_p2_tg) {
                    auto& _v = lf_post_transplant_storage();
                    if (static_cast<size_t>(_lff_tid) < _v.size()) {
                        _v[static_cast<size_t>(_lff_tid)] +=
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - _tp_t0).count();
                    }
                }
                // Probe 2: time Rat pole(...) constructor (canonicalises).
                const auto _rc_t0 = _p2_tg ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
                Rat pole(-c0_wide, std::move(lc_wide));
                if (_p2_tg) {
                    auto& _v = lf_post_rat_ctor_storage();
                    if (static_cast<size_t>(_lff_tid) < _v.size()) {
                        _v[static_cast<size_t>(_lff_tid)] +=
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - _rc_t0).count();
                    }
                }
                out.linear.push_back(LinearFactor{m, std::move(pole)});
                continue;
            }

            // 2026-05-04 PIVOT: deg-2 algebraic-letter arm in narrow
            // path. Mirrors the wide-path arm at line ~2207. Transplant
            // base_narrow → base_wide first because:
            //   (a) lf_current_forbidden_vars() is wide-indexed;
            //   (b) Rat::parse(ctx, "Wm_/Wp_") needs the wide ctx
            //       (which has the Wm_/Wp_ atoms);
            //   (c) AlgebraicLetterTable's content-dedup key is the
            //       polynomial's to_string() — to match what the wide
            //       path would have produced, base_wide must live in
            //       the wide ctx.
            // PRE: used_wide and narrow_to_wide are populated by the
            // narrow block setup at lines ~1842 and ~1922. Pivot
            // depends on these being unconditional.
            // narrow_to_wide is the inverse of wide_to_narrow built
            // at line 1860 (= used_wide vector).
            if (d == 2 && introduce_algebraic_letters) {
                const auto _tp2a_t0 = _p2_tg ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
                Poly base_wide = base_narrow.transplant(ctx, narrow_to_wide);
                if (_p2_tg) {
                    auto& _v = lf_post_transplant_storage();
                    if (static_cast<size_t>(_lff_tid) < _v.size()) {
                        _v[static_cast<size_t>(_lff_tid)] +=
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - _tp2a_t0).count();
                    }
                }

                // HyperIntica-parity guard: same logic as the wide
                // path. Forbidden var set is wide-indexed by
                // construction of LFForbiddenVarsScope.
                if (const std::vector<size_t>* forbidden =
                        lf_current_forbidden_vars();
                    forbidden != nullptr && !forbidden->empty()) {
                    std::vector<size_t> used = base_wide.used_var_indices();
                    bool has_forbidden_dep = false;
                    for (size_t v : *forbidden) {
                        if (v == var_idx) continue;
                        for (size_t u : used) {
                            if (u == v) { has_forbidden_dep = true; break; }
                        }
                        if (has_forbidden_dep) break;
                    }
                    if (has_forbidden_dep) {
                        out.nonlinear.push_back(
                            NonlinearFactor{m, std::move(base_wide), d});
                        continue;
                    }
                }

                // 2026-05-04 PIVOT B1: sign-canonicalize base_wide
                // before allocate. FLINT's fmpq_mpoly_factor may
                // return sign-flipped irreducibles when the monomial
                // order differs between narrow and wide ctxs (different
                // nvars => different bits_per_exp packing). Without
                // this, the same physical letter pair could allocate
                // two distinct al_idx (one per sign), splitting the
                // dedup map. Vieta data (sum, product, disc) is
                // sign-invariant, so this normalization is safe.
                if (base_wide.leading_coef_is_negative()) {
                    base_wide = -base_wide;
                }

                long al_idx = AlgebraicLetterTable::global().allocate(
                    base_wide, var_idx);
                try {
                    Rat wm_pole = Rat::parse(ctx, "Wm_" + std::to_string(al_idx));
                    Rat wp_pole = Rat::parse(ctx, "Wp_" + std::to_string(al_idx));
                    out.linear.push_back(LinearFactor{m, std::move(wm_pole)});
                    out.linear.push_back(LinearFactor{m, std::move(wp_pole)});
                } catch (const std::exception& e) {
                    throw std::runtime_error(
                        "linear_factors (narrow deg-2): "
                        "introduce_algebraic_letters=true but PolyCtx "
                        "is missing Wm_/Wp_ atoms — call "
                        "build_algebraic_letter_var_list at ctx "
                        "construction. Underlying parse error: "
                        + std::string(e.what()));
                }
                continue;
            }

            // d >= 2 nonlinear (deg-2 algebraic-letter case handled
            // above; this catches deg-3+ and deg-2 when
            // introduce_algebraic_letters=false). Transplant base
            // back to wide.
            const auto _tp2_t0 = _p2_tg ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
            Poly base_wide = base_narrow.transplant(ctx, narrow_to_wide);
            if (_p2_tg) {
                auto& _v = lf_post_transplant_storage();
                if (static_cast<size_t>(_lff_tid) < _v.size()) {
                    _v[static_cast<size_t>(_lff_tid)] +=
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - _tp2_t0).count();
                }
            }
            out.nonlinear.push_back(
                NonlinearFactor{m, std::move(base_wide), d});
        }

        // The final-constant lift is only useful if the caller
        // actually wants the constant string. Skip both the
        // transplant and the to_string when compute_constant=false.
        if (compute_constant) {
            // Probe 2: time the final constant transplant + to_string.
            const auto _tp3_t0 = _p2_tg ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
            Poly constant_wide = constant_narrow.transplant(ctx, narrow_to_wide);
            if (_p2_tg) {
                auto& _v = lf_post_transplant_storage();
                if (static_cast<size_t>(_lff_tid) < _v.size()) {
                    _v[static_cast<size_t>(_lff_tid)] +=
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - _tp3_t0).count();
                }
            }
            const auto _ts_t0 = _p2_tg ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
            out.constant = constant_wide.to_string();
            if (_p2_tg) {
                auto& _v = lf_post_constant_to_string_storage();
                if (static_cast<size_t>(_lff_tid) < _v.size()) {
                    _v[static_cast<size_t>(_lff_tid)] +=
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - _ts_t0).count();
                }
            }
        }

        // Phase-d15 deeper drill (round 5): classify deg-3+ outcome
        // by whether FLINT returned any nonlinear factors. Probe-eligibility
        // = has_nonlinear branch (expected ~0 on successful runs because
        // partial_fractions throws on nonlinear factors).
        if (_lff_input_deg >= 3) {
            if (out.nonlinear.empty()) {
                auto& cv = lf_d3p_all_lin_count_storage();
                auto& sv = lf_d3p_all_lin_s_storage();
                if (static_cast<size_t>(_lff_tid) < cv.size()) cv[static_cast<size_t>(_lff_tid)] += 1;
                if (static_cast<size_t>(_lff_tid) < sv.size()) sv[static_cast<size_t>(_lff_tid)] += _lff_flint_wall_s;
                // Phase-d15 deeper drill (round 6): within all-linear,
                // split by whether any factor has multiplicity > 1.
                bool _any_repeated = false;
                for (const auto& lf : out.linear) {
                    if (lf.multiplicity > 1) { _any_repeated = true; break; }
                }
                if (_any_repeated) {
                    auto& rc = lf_d3p_repeated_count_storage();
                    auto& rs = lf_d3p_repeated_s_storage();
                    if (static_cast<size_t>(_lff_tid) < rc.size()) rc[static_cast<size_t>(_lff_tid)] += 1;
                    if (static_cast<size_t>(_lff_tid) < rs.size()) rs[static_cast<size_t>(_lff_tid)] += _lff_flint_wall_s;
                } else {
                    auto& sc = lf_d3p_sqfree_count_storage();
                    auto& ss = lf_d3p_sqfree_s_storage();
                    if (static_cast<size_t>(_lff_tid) < sc.size()) sc[static_cast<size_t>(_lff_tid)] += 1;
                    if (static_cast<size_t>(_lff_tid) < ss.size()) ss[static_cast<size_t>(_lff_tid)] += _lff_flint_wall_s;
                }
            } else {
                auto& cv = lf_d3p_has_nl_count_storage();
                auto& sv = lf_d3p_has_nl_s_storage();
                if (static_cast<size_t>(_lff_tid) < cv.size()) cv[static_cast<size_t>(_lff_tid)] += 1;
                if (static_cast<size_t>(_lff_tid) < sv.size()) sv[static_cast<size_t>(_lff_tid)] += _lff_flint_wall_s;
            }
        }

        fmpq_mpoly_factor_clear(F, narrow.raw());
        cache_store_locked(cache_key, out);
        return apply_v1_roundtrip(std::move(out),
                                   "linear_factors/narrow");
    }

    // Fallback / algebraic-letter path: factor directly in the wide ctx.
    fmpq_mpoly_factor_t F;
    fmpq_mpoly_factor_init(F, ctx.raw());
    const auto _ff_t0 = std::chrono::steady_clock::now();
    const int rc_wide = fmpq_mpoly_factor(
        F, const_cast<fmpq_mpoly_struct*>(p.raw()), ctx.raw());
    {
        const double _dt = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - _ff_t0).count();
        _lff_flint_wall_s += _dt;
        auto& _ffv = lf_ff_per_thread_storage();
        if (static_cast<size_t>(_lff_tid) < _ffv.size()) {
            _ffv[static_cast<size_t>(_lff_tid)] += _dt;
        }
        if (_lff_input_deg == 1) {
            auto& _bv = lf_ff_deg1_per_thread_storage();
            if (static_cast<size_t>(_lff_tid) < _bv.size()) {
                _bv[static_cast<size_t>(_lff_tid)] += _dt;
            }
        } else if (_lff_input_deg == 2) {
            auto& _bv = lf_ff_deg2_per_thread_storage();
            if (static_cast<size_t>(_lff_tid) < _bv.size()) {
                _bv[static_cast<size_t>(_lff_tid)] += _dt;
            }
        }
    }
    if (rc_wide == 0) {
        fmpq_mpoly_factor_clear(F, ctx.raw());
        throw std::runtime_error("linear_factors: fmpq_mpoly_factor failed");
    }

    Poly constant_poly(ctx);
    fmpq_mpoly_set_fmpq(constant_poly.raw(), F->constant, ctx.raw());

    const bool _wp2_tg = step_trace_enabled();
    const slong nfac = F->num;
    for (slong i = 0; i < nfac; ++i) {
        // Probe 2: time clone_from_raw on the wide branch.
        const auto _cfr_t0 = _wp2_tg ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
        Poly base = clone_from_raw(ctx, F->poly + i);
        if (_wp2_tg) {
            auto& _v = lf_post_clone_from_raw_storage();
            if (static_cast<size_t>(_lff_tid) < _v.size()) {
                _v[static_cast<size_t>(_lff_tid)] +=
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - _cfr_t0).count();
            }
        }
        long m    = fmpz_get_si(F->exp + i);
        long d    = base.degree_in_var(var_idx);

        if (d <= 0) {
            Poly bm = base.pow(static_cast<unsigned long>(m));
            constant_poly = constant_poly * bm;
            continue;
        }

        if (d == 1) {
            Poly lc = base.coefficient_of_var(var_idx, 1);
            Poly c0 = base.coefficient_of_var(var_idx, 0);
            // Probe 2: time Rat pole(...) constructor on wide branch.
            const auto _rc_t0 = _wp2_tg ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
            Rat pole(-c0, std::move(lc));
            if (_wp2_tg) {
                auto& _v = lf_post_rat_ctor_storage();
                if (static_cast<size_t>(_lff_tid) < _v.size()) {
                    _v[static_cast<size_t>(_lff_tid)] +=
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - _rc_t0).count();
                }
            }
            out.linear.push_back(LinearFactor{m, std::move(pole)});
            continue;
        }

        if (d == 2 && introduce_algebraic_letters) {
            // HyperIntica-parity guard: refuse Wm/Wp introduction when the
            // resulting algebraic-letter definitions would still depend on
            // a remaining (un-integrated) Feynman parameter.  See
            // LFForbiddenVarsScope in the header for rationale.
            //
            // The discriminant of `base` (treated as a poly in `var_idx`)
            // is built from `base`'s coefficients in `var_idx` — which are
            // polynomials in the rest of the variables.  If `base` uses
            // any forbidden var (other than `var_idx` itself), so will at
            // least one of those coefficients, and so will the
            // discriminant, and so will Wm/Wp.  The conservative test
            // `used_var_indices() ∩ forbidden \ {var_idx} != ∅` is exact
            // up to the unlikely accident of perfect-square cancellation
            // inside the discriminant; rejecting in that edge case is
            // safe (HyperIntica also rejects it).
            if (const std::vector<size_t>* forbidden =
                    lf_current_forbidden_vars();
                forbidden != nullptr && !forbidden->empty()) {
                std::vector<size_t> used = base.used_var_indices();
                bool has_forbidden_dep = false;
                for (size_t v : *forbidden) {
                    if (v == var_idx) continue;
                    for (size_t u : used) {
                        if (u == v) { has_forbidden_dep = true; break; }
                    }
                    if (has_forbidden_dep) break;
                }
                if (has_forbidden_dep) {
                    out.nonlinear.push_back(
                        NonlinearFactor{m, std::move(base), d});
                    continue;
                }
            }

            // 2026-05-04 PIVOT B1: same sign canonicalization as the
            // narrow-path deg-2 arm. Defends against future FLINT
            // version drift in the leading-monomial sign convention.
            if (base.leading_coef_is_negative()) {
                base = -base;
            }
            long al_idx = AlgebraicLetterTable::global().allocate(base, var_idx);
            try {
                Rat wm_pole = Rat::parse(ctx, "Wm_" + std::to_string(al_idx));
                Rat wp_pole = Rat::parse(ctx, "Wp_" + std::to_string(al_idx));
                out.linear.push_back(LinearFactor{m, std::move(wm_pole)});
                out.linear.push_back(LinearFactor{m, std::move(wp_pole)});
            } catch (const std::exception& e) {
                throw std::runtime_error(
                    "linear_factors: introduce_algebraic_letters=true but "
                    "PolyCtx is missing Wm_/Wp_ atoms — call "
                    "build_algebraic_letter_var_list at ctx construction. "
                    "Underlying parse error: " + std::string(e.what()));
            }
            continue;
        }

        out.nonlinear.push_back(
            NonlinearFactor{m, std::move(base), d});
    }

    if (compute_constant) {
        // Probe 2: time the wide-path constant to_string().
        const auto _ts_t0 = _wp2_tg ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
        out.constant = constant_poly.to_string();
        if (_wp2_tg) {
            auto& _v = lf_post_constant_to_string_storage();
            if (static_cast<size_t>(_lff_tid) < _v.size()) {
                _v[static_cast<size_t>(_lff_tid)] +=
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - _ts_t0).count();
            }
        }
    }

    // Phase-d15 deeper drill (round 5): wide-path equivalent classifier.
    if (_lff_input_deg >= 3) {
        if (out.nonlinear.empty()) {
            auto& cv = lf_d3p_all_lin_count_storage();
            auto& sv = lf_d3p_all_lin_s_storage();
            if (static_cast<size_t>(_lff_tid) < cv.size()) cv[static_cast<size_t>(_lff_tid)] += 1;
            if (static_cast<size_t>(_lff_tid) < sv.size()) sv[static_cast<size_t>(_lff_tid)] += _lff_flint_wall_s;
            // Phase-d15 deeper drill (round 6): squarefree vs repeated split.
            bool _any_repeated = false;
            for (const auto& lf : out.linear) {
                if (lf.multiplicity > 1) { _any_repeated = true; break; }
            }
            if (_any_repeated) {
                auto& rc = lf_d3p_repeated_count_storage();
                auto& rs = lf_d3p_repeated_s_storage();
                if (static_cast<size_t>(_lff_tid) < rc.size()) rc[static_cast<size_t>(_lff_tid)] += 1;
                if (static_cast<size_t>(_lff_tid) < rs.size()) rs[static_cast<size_t>(_lff_tid)] += _lff_flint_wall_s;
            } else {
                auto& sc = lf_d3p_sqfree_count_storage();
                auto& ss = lf_d3p_sqfree_s_storage();
                if (static_cast<size_t>(_lff_tid) < sc.size()) sc[static_cast<size_t>(_lff_tid)] += 1;
                if (static_cast<size_t>(_lff_tid) < ss.size()) ss[static_cast<size_t>(_lff_tid)] += _lff_flint_wall_s;
            }
        } else {
            auto& cv = lf_d3p_has_nl_count_storage();
            auto& sv = lf_d3p_has_nl_s_storage();
            if (static_cast<size_t>(_lff_tid) < cv.size()) cv[static_cast<size_t>(_lff_tid)] += 1;
            if (static_cast<size_t>(_lff_tid) < sv.size()) sv[static_cast<size_t>(_lff_tid)] += _lff_flint_wall_s;
        }
    }

    fmpq_mpoly_factor_clear(F, ctx.raw());
    cache_store_locked(cache_key, out);
    return apply_v1_roundtrip(std::move(out), "linear_factors/wide");
}

// HF FF Phase 5 §E Step E.2-impl-2 (iter-61-β.3).
//
// Public `linear_factors` entry-point with operator-memoization wrap.
// On cache HIT, returns a deep-copy of the stored `LinearFactorization`;
// on cache MISS, delegates to `linear_factors_impl_se` (the pre-iter-61
// body, including the inner per-shard LF cache at `linear_factors.cpp:981+`).
//
// Per implementation_memo §3.3 + §iter-59-fold-REQ-1 corrected snippet,
// §iter-59-fold-REQ-3 (option b): the outer §E cache is DISABLED under
// HF_USE_SCALAR_REP=1 via `operator_memo::lf_enabled()` returning false
// regardless of HF_OPERATOR_MEMO_OFF_LF. The inner LF cache continues
// to operate under SCALAR_REP=1 (its existing FOLD-ER cliff disposition
// is handled at the inner layer; iter-64+ lambda kills further reduced
// the ZWTable hazard at sites 2/3/4/5/6/7).
//
// The key includes `zw_tab.get()` (raw pointer identity) per
// §iter-59-fold-REQ-3 defense-in-depth (so SCALAR_REP=0 builds with
// distinct ZWTable instances still keep distinct cache slots even when
// the rest of the key matches).
LinearFactorization linear_factors(const Poly& p, size_t var_idx,
                                    std::shared_ptr<ZWTable> zw_tab,
                                    bool introduce_algebraic_letters,
                                    bool compute_constant) {
    // HF FF Phase 6 REVISED §6.P iter-17 (BINDING pre-build reviewer at
    // iter-16): env-gated structural-sharing probe.  Default-OFF guarded
    // by master predicate.  Observes the PUBLIC entry-point (both
    // §E-cache HIT and MISS paths).  pre_bytes = bytes(p); post_bytes
    // = Σ bytes of all linear- and nonlinear-factor polynomials in the
    // returned factorisation.
    //
    // unchanged_bytes (REQ-16.3 fold; iter-16 BINDING reviewer
    // ae98902a768f7f242): iter-17 implements factor-level structural
    // intersection per design memo §2.1.  For each factor in the
    // returned `LinearFactorization` (both linear `.pole` Rats and
    // nonlinear `.polynomial` Polys), a 64-bit signature hash is
    // computed from the factor's canonical string representation
    // (`Rat::to_string()` / `Poly::to_string()`, cached internally),
    // mixed with the `compute_constant` and `introduce_algebraic_letters`
    // bits (R17.8 scope-by-key-attribute carry: the cache key includes
    // `compute_constant`, so factors of LF calls with different
    // `compute_constant` must not be conflated).  The signature is
    // submitted to `structural_sharing::observe_lf_factor_bytes(sig, bytes)`,
    // which checks a process-global mutex-guarded
    // `std::unordered_set<uint64_t>` and returns either `bytes`
    // (signature previously observed → factor is shareable across calls)
    // or 0 (first observation → factor is new).  Sum across all factors
    // gives `unchanged_bytes` for this call.  At sample_rate >= 10 on
    // LF (design memo §2.2 FOLD-DC5 stratification), the mutex
    // acquisition rate is bounded and the +10 % wall budget holds.
    const bool probe = structural_sharing::probe_linear_factors_instrumented()
                       && structural_sharing::should_emit_op("linear_factors");
    int64_t pre_bytes = 0;
    if (probe) {
        pre_bytes = static_cast<int64_t>(p.total_bytes());
    }

    // R17.8 scope-by-key-attribute: mix the cache-key attributes that
    // distinguish factor universes (`compute_constant`,
    // `introduce_algebraic_letters`) into the signature so the probe
    // does not collapse semantically distinct factorisations.  These
    // are 1-bit values; we OR them into the high bits of the hash to
    // partition the seen-set by (factor_canonical, compute_constant,
    // introduce_algebraic_letters).
    const std::uint64_t scope_bits =
        (static_cast<std::uint64_t>(compute_constant ? 1u : 0u) << 62) |
        (static_cast<std::uint64_t>(introduce_algebraic_letters ? 1u : 0u) << 63);

    auto emit_probe = [&](const LinearFactorization& lf) {
        if (!probe) return;
        int64_t post_bytes = 0;
        int64_t unchanged_bytes = 0;
        std::hash<std::string> str_hasher;
        for (const auto& f : lf.linear) {
            const int64_t b = static_cast<int64_t>(f.pole.total_bytes());
            post_bytes += b;
            // f.pole is a Rat; to_string() returns a canonical, cached
            // string representation (see rat.hpp:158).
            const std::uint64_t sig =
                static_cast<std::uint64_t>(str_hasher(f.pole.to_string()))
                ^ scope_bits;
            unchanged_bytes +=
                structural_sharing::observe_lf_factor_bytes(sig, b);
        }
        for (const auto& f : lf.nonlinear) {
            const int64_t b = static_cast<int64_t>(f.polynomial.total_bytes());
            post_bytes += b;
            // f.polynomial is a Poly; to_string() uses
            // fmpq_mpoly_get_str_pretty (canonical, see poly.hpp:202).
            const std::uint64_t sig =
                static_cast<std::uint64_t>(str_hasher(f.polynomial.to_string()))
                ^ scope_bits;
            unchanged_bytes +=
                structural_sharing::observe_lf_factor_bytes(sig, b);
        }
        structural_sharing::emit("linear_factors", pre_bytes, post_bytes,
                                 unchanged_bytes);
    };

    if (operator_memo::lf_enabled()) {
        canonical_signature::LfKey key = canonical_signature::make_lf_key(
            p, var_idx, zw_tab.get(),
            introduce_algebraic_letters, compute_constant);
        const std::uint64_t key_hash =
            canonical_signature::hash_lf_key(key);
        // iter-70 REC-2: try_lookup returns
        // std::shared_ptr<const LinearFactorization> (ref-counted COW handle).
        // On HIT, `return *cached_sp` invokes LinearFactorization's copy
        // ctor at the caller-by-value return boundary; the cache-side
        // lock-held window shrinks to O(1) ref-count++.
        auto cached_sp = g_lf_cache_outer().try_lookup(key, key_hash);
        if (cached_sp) {
            counter_replay::lf_on_hit();
            // REQ-17.1 fold option (a) — iter-17 BINDING reviewer
            // a25d12e86d3f96bf8. Deep-copy `*cached_sp` to a local
            // before invoking emit_probe so the to_string() calls on
            // f.pole / f.polynomial mutate this thread's local
            // Rat::cached_str_ / Poly::cached_str_, not the shared
            // cache payload. Without this copy, two concurrent OMP
            // workers that HIT the same cache slot would race on the
            // shared cached_sp->linear[i].pole.cached_str_ string,
            // corrupting non-SSO heap or write-tearing SSO. Default-OFF
            // (HF_STRUCTURAL_SHARING_PROBE unset) keeps the previous
            // ref-count++ fast path; the deep-copy only fires under
            // the probe gate. The trailing `return *cached_sp` is
            // unchanged (consumer-side copy preserves the iter-70
            // REC-2 COW contract).
            LinearFactorization result = *cached_sp;
            emit_probe(result);
            return result;
        }
        LinearFactorization result = linear_factors_impl_se(
            p, var_idx, zw_tab,
            introduce_algebraic_letters, compute_constant);
        g_lf_cache_outer().insert(std::move(key), key_hash,
                                  LinearFactorization(result));
        emit_probe(result);
        return result;
    }
    LinearFactorization result = linear_factors_impl_se(
        p, var_idx, zw_tab,
        introduce_algebraic_letters, compute_constant);
    emit_probe(result);
    return result;
}

}  // namespace hyperflint
