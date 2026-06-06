// Phase 5e: IntegrationStep — port of HyperIntica.wl:4717.

#include "hyperflint/integrator/integration_step.hpp"
#include "hyperflint/integrator/env_flags.hpp"  // iter-74 §T7 seventh chunk: HF_FLAG_DISABLE_PARALLEL_MERGE
#include "hyperflint/runtime/hf_thread_num.hpp"
#include "hyperflint/integrator/gcd_dispatch.hpp"  // Phase 1 Task 1.E: GCD/OMP wrap
#include "hyperflint/integrator/section_6d_dfs_cap.hpp"  // §6.D iter-22b: env-var thread-cap
#include "hyperflint/integrator/sharded_flat_map.hpp"  // MonomialAcc

#include "hyperflint/algebra/linear_factors.hpp"    // clear_linear_factors_cache
#include "hyperflint/algebra/partial_fractions.hpp"  // init/reset/sum_linear_factors_per_thread
#include "hyperflint/algebra/shuffle.hpp"
#include "hyperflint/diagnostics/env_flags.hpp"             // §T7 iter-63: HF_FLAG_POLES_STREAM, HF_FLAG_REGKEY_DUMP
#include "hyperflint/diagnostics/integration_node_rss.hpp"  // Task 0-5: per-node RSS sampler
#include "hyperflint/instrumentation/phase_timer.hpp"        // Track 4.2 iter-26: PoC OMP-region scope guard
#include "hyperflint/integrator/primitive.hpp"
#include "hyperflint/integrator/regularize.hpp"
#include "hyperflint/integrator/transform.hpp"
#include "hyperflint/core/addpf_probe.hpp"          // HF_ADDPF_PROBE (additive-parfrac Phase 0)
#include "hyperflint/reduce/periods.hpp"           // test_zero_function_sym
#include "hyperflint/runtime/narrow_ctx_flag.hpp"  // R24v2 sentinel-tolerance
#include "hyperflint/runtime/scalar_rep.hpp"        // Phase-B B6.b: scalar_rep_enabled()
#include "hyperflint/runtime/scs_roundtrip.hpp"     // Phase-B B6.b: roundtrip_rat_through_scs (transitively pulls ZWTable)
#include "hyperflint/runtime/trace_gate.hpp"
#include "hyperflint/series/expansions.hpp"
#include "hyperflint/series/laurent.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>     // std::getenv (HF_BUCKET_HASH_STATS, HF_DISABLE_PARALLEL_MERGE)
#include <cstring>     // std::strcmp
#include <iostream>    // std::cerr (HF_BUCKET_HASH_STATS)
#include <mutex>       // Branch A pre-flight: HF_REGKEY_DUMP serialization
#include <optional>    // Lever C-merge: per-slot canonicalize result
#include <sstream>
#include <stdexcept>   // R24v2: std::runtime_error catch in OMP body wrapper
#include <string>
#include <unordered_map>
#include <vector>

#ifdef HF_HAVE_OPENMP
#  include <omp.h>
#endif

#if defined(__APPLE__)
// Phase 9b-1: QoS-class promotion.  macOS routes QOS_CLASS_DEFAULT
// threads to E-cores under load; libomp inherits whatever QoS the
// spawning thread had, and `OMP_PROC_BIND` / `OMP_PLACES` are silently
// ignored on Darwin (no documented pinning API).  On an M-series chip
// an E-core is ~35-45% of a P-core on integer-heavy serial workloads,
// so letting OMP workers drop to E-cores silently leaves perf on the
// table.
#  include <pthread/qos.h>
#  include <cstdlib>
#  include <cstring>
#endif

namespace hyperflint {

#if defined(__APPLE__)
// Called inside the OMP parallel region; the thread_local guard ensures
// each worker promotes itself exactly once per thread-lifetime (across
// all parallel regions, not per iteration).
//
// Opt out with `HF_QOS_USER_INITIATED=0`.  Any other value, or an unset
// var, leaves promotion on.
static void hf_promote_qos_once() {
    static thread_local bool promoted = false;
    if (promoted) return;
    promoted = true;
    // iter-94 Track-OMP macro-layer LAND: env-var literal relocated to
    // integrator/env_flags.hpp 4th extension under §5.1 rule-1 BINDING
    // from adversarial-reviewer iter-94 Q-19 B1 substantive-pattern
    // dispatch. The exact-match-"0" predicate-family semantics are
    // preserved verbatim.
    const char* env = HF_FLAG_QOS_USER_INITIATED;
    if (env && std::strcmp(env, "0") == 0) return;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
}
#else
static inline void hf_promote_qos_once() {}
#endif

// ---------------------------------------------------------------------------
// Task 0-5: per-integration-node RSS sampler RAII scope.
//
// NodeScope wraps one entry in the OMP parallel-for body.  The constructor
// calls IntegrationNodeRssSampler::enter_node(); the destructor calls
// exit_node() unconditionally (even on exception), which is safe because
// the OMP body already has try/catch wrappers that swallow or rethrow.
//
// depth  = static_cast<int>(var_idx), the integration-variable index for
//          this step (unique per hyperflint_sym step level).
// letter_id = static_cast<int>(entry_i), the per-step entry index (which
//          shuffle entry this OMP iteration is processing).
// ---------------------------------------------------------------------------
namespace {
struct NodeScope {
    int depth;
    NodeScope(int d, int letter_id) : depth(d) {
        hyperflint::IntegrationNodeRssSampler::instance()
            .enter_node(d, letter_id);
    }
    ~NodeScope() {
        hyperflint::IntegrationNodeRssSampler::instance()
            .exit_node(depth);
    }
    // Non-copyable, non-movable.
    NodeScope(const NodeScope&)            = delete;
    NodeScope& operator=(const NodeScope&) = delete;
};

}  // namespace

// Phase-B follow-up telemetry: when hyperflint_sym's HF_STEP_TRACE is
// on, the driver resets these counters before each step and reads them
// back to emit sub-timings. Seconds since the last reset. Thread-local
// because integration_step can be called re-entrantly from different
// threads via parallel tests / OpenMP nesting (though in practice only
// the master thread mutates these fields).
thread_local double g_integration_step_canon_s = 0.0;
thread_local double g_closure_body_s = 0.0;
thread_local double g_closure_canon_s = 0.0;
// Phase-d15 follow-up: per-primitive timers, populated by integration_step's
// main loop only (not check_divergences_pass / fib_recurse_sym). Summed into
// these master-thread globals from per-thread accumulator vectors after the
// parallel region. Cleared by reset_step_sub_timers before each step trace.
thread_local double g_transform_shuffle_s = 0.0;
thread_local double g_integrate_ii_s = 0.0;
thread_local double g_loop_residual_s = 0.0;
// Phase-d15 follow-up: cumulative partial_fractions wall (subset of
// integrate_ii_s). Populated by integrate_ii via a file-scope per-thread
// vector in primitive.cpp; summed and added here after the OMP barrier.
thread_local double g_partial_fractions_s = 0.0;
// Phase-d15 deeper drill: cumulative linear_factors wall (FLINT factor
// call, subset of partial_fractions_s). Populated by partial_fractions
// via a file-scope per-thread vector in partial_fractions.cpp.
thread_local double g_linear_factors_s = 0.0;
// Phase-d15 deeper drill (round 3): split linear_factors_s into the
// FLINT fmpq_mpoly_factor wall (`g_lf_flint_factor_s`) and everything
// else (cache lookup, narrow-ctx setup, transplants — implicit
// = lf - flint_factor in the JSON). Hit/miss counters tell us whether
// the lever is "improve cache hit rate" or "speed up FLINT".
thread_local double g_lf_flint_factor_s = 0.0;
thread_local long   g_lf_cache_hits     = 0;
thread_local long   g_lf_cache_misses   = 0;
// Phase-d15 deeper drill (round 4): degree-bucketed split of the
// FLINT factor wall + miss count. deg3+ is implicit:
//   lf_flint_factor_s - deg1_s - deg2_s, similarly for the count.
thread_local double g_lf_flint_deg1_s   = 0.0;
thread_local double g_lf_flint_deg2_s   = 0.0;
thread_local long   g_lf_miss_deg1      = 0;
thread_local long   g_lf_miss_deg2      = 0;
// Phase-d15 deeper drill (round 5): deg-3+ output classifier. Settles
// the irreducibility-probe lever sizing — if has_nonlinear ≈ 0 (which
// is forced on successful runs by partial_fractions throwing on any
// nonlinear factor), the probe shortcuts essentially nothing.
thread_local long   g_lf_d3p_all_linear_count   = 0;
thread_local double g_lf_d3p_all_linear_s       = 0.0;
thread_local long   g_lf_d3p_has_nonlinear_count = 0;
thread_local double g_lf_d3p_has_nonlinear_s    = 0.0;
// Phase-d15 deeper drill (round 6): within all-linear deg-3+, split
// by whether any factor has multiplicity > 1. Sizes the GCD-peel-off
// lever (Yun's squarefree decomposition) — only useful if a substantial
// fraction has repeated roots.
thread_local long   g_lf_d3p_squarefree_count = 0;
thread_local double g_lf_d3p_squarefree_s     = 0.0;
thread_local long   g_lf_d3p_repeated_count   = 0;
thread_local double g_lf_d3p_repeated_s       = 0.0;
// Phase-d15 deeper drill (round 7): squarefree-first path counters.
// Active when HF_LF_SQF=1; otherwise all zeros. The total wall is
// what counts; decomp + inner_factor sum to (something close to)
// total minus per-base bookkeeping. inner_factor_calls being zero
// for a given run means every u_i was degree-1 (the win condition).
thread_local double g_lf_sqf_total_s             = 0.0;
thread_local double g_lf_sqf_decomp_s            = 0.0;
thread_local double g_lf_sqf_inner_factor_s      = 0.0;
thread_local long   g_lf_sqf_calls               = 0;
thread_local long   g_lf_sqf_inner_factor_calls  = 0;
thread_local long   g_lf_sqf_bailouts            = 0;
// 2026-04-26 sanity-matrix follow-up: integrate_ii body sub-timers.
thread_local double g_bump_lookup_s              = 0.0;
thread_local double g_bump_addto_s               = 0.0;
thread_local double g_push_ibp_s                 = 0.0;
thread_local double g_antideriv_s                = 0.0;
thread_local long   g_bump_calls                 = 0;
// 2026-04-27 Lever-1 extended: bump-local split of `bump_addto_s`.
thread_local double g_bump_emplace_s             = 0.0;
thread_local double g_bump_rat_add_s             = 0.0;
thread_local long   g_bump_rat_add_calls         = 0;
// 2026-04-26 P1' pre-gating: per-invocation distinct-denominator counters.
thread_local long   g_pf_calls_in_loop           = 0;
thread_local long   g_pf_unique_dens             = 0;
// 2026-04-26 RA pre-gating: per-invocation rows.size() at loop exit.
thread_local long   g_bump_unique_rows           = 0;
// 2026-04-26 lock-contention proxy: held-time inside `g_linear_factors_mu`.
thread_local double g_lf_lock_held_s             = 0.0;
// iter-37 lock-acquire wait probe: queueing time on shard mutex
// acquire (sibling of g_lf_lock_held_s, which covers held-time
// once acquired). Default-OFF env-gated under HF_LF_LOCK_WAIT_PROFILE=1.
thread_local double g_lf_lock_wait_s             = 0.0;
// 2026-04-26 cache_key_build direct measurement.
thread_local double g_lf_cache_key_build_s       = 0.0;
// 2026-04-29 (Probe 2): four sub-timers splitting post-FLINT extraction
// in linear_factors. Aggregated across threads at end of OMP region.
thread_local double g_lf_post_transplant_s          = 0.0;
thread_local double g_lf_post_rat_ctor_s            = 0.0;
thread_local double g_lf_post_constant_to_string_s  = 0.0;
thread_local double g_lf_post_clone_from_raw_s      = 0.0;
// 2026-04-29 (Probe 3): integrate_ii body residual sub-timers.
thread_local double g_ii_queue_copy_s               = 0.0;
thread_local double g_ii_pole_arith_s               = 0.0;
thread_local double g_ii_pole_word_ctor_s           = 0.0;
// 2026-04-27 (3l3pt profile-deepening): narrow-vs-wide branch in
// `reduce_inplace`. Lives in core/rat.cpp. Renamed from g_rat_add_*
// to g_reduce_* after the reviewer correction (timers fire on every
// Rat-producing op, not just Rat::add).
thread_local double g_reduce_narrow_s            = 0.0;
thread_local double g_reduce_wide_s              = 0.0;
thread_local long   g_reduce_narrow_calls        = 0;
thread_local long   g_reduce_wide_calls          = 0;
thread_local long   g_reduce_zero_calls          = 0;
// 2026-05-02 (HF FLINT-pool experiment, re-scoped to GCD): wraps just
// the fmpq_mpoly_gcd_cofactors call inside reduce_inplace, no
// transplant overhead. See notes/hf_flint_pool_experiment/baseline.md.
thread_local double g_gcd_cofactors_s            = 0.0;
thread_local long   g_gcd_cofactors_calls        = 0;
// 2026-05-02 (Phase-0-GCD follow-up): non-GCD sub-timers in the
// narrow path of reduce_inplace. See rat.cpp.
thread_local double g_rn_used_vars_s             = 0.0;
thread_local double g_rn_setup_s                 = 0.0;
thread_local double g_rn_post_s                  = 0.0;
// 2026-05-02 (volume probe for from_canonical extension to sub/mul/div).
thread_local long   g_rat_mul_calls              = 0;
thread_local long   g_rat_sub_calls              = 0;
thread_local long   g_rat_div_calls              = 0;
// 2026-04-27 Lever-1 extended: per-Poly-op timers inside Rat::add.
thread_local double g_rat_add_polymul_s          = 0.0;
thread_local double g_rat_add_polyadd_s          = 0.0;
thread_local long   g_rat_add_calls              = 0;
// 2026-05-03 (chain 20, PF3 prep): per-backend wall+calls at the
// Rat::add / operator+= dispatch fork. Anchors the Tier A1 baseline.
thread_local double g_rat_add_legacy_wall_s      = 0.0;
thread_local double g_rat_add_via_qu_wall_s      = 0.0;
thread_local long   g_rat_add_legacy_calls       = 0;
thread_local long   g_rat_add_via_qu_calls       = 0;
// 2026-04-27 Avenue A: narrow-vs-wide branch counters/timers in
// Poly::mul.  Lives in core/poly.cpp.  Accessors here harvest the
// per-thread sums into the per-step thread_local globals after the
// OMP parallel region.
thread_local double g_mul_narrow_s               = 0.0;
thread_local double g_mul_wide_s                 = 0.0;
thread_local long   g_mul_narrow_calls           = 0;
thread_local long   g_mul_wide_calls             = 0;
thread_local long   g_mul_gated_calls            = 0;
// 2026-04-28 reviewer round 7: per-call distribution histograms in
// the narrow path.  6 la·lb log bins, 5 used_count bins.
thread_local long   g_nbin_lalb_count[6]         = {0, 0, 0, 0, 0, 0};
thread_local double g_nbin_lalb_us[6]            = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
thread_local double g_nbin_lalb_max[6]           = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
thread_local long   g_nbin_u_count[5]            = {0, 0, 0, 0, 0};
thread_local double g_nbin_u_us[5]               = {0.0, 0.0, 0.0, 0.0, 0.0};
// 2026-04-27 (3l3pt profile-deepening): bucket_bump_s sub-timers
// (PolesBucket::bump body).  File-local to this translation unit.
thread_local double g_bucket_canon_regkey_s      = 0.0;
thread_local double g_bucket_struct_hash_s       = 0.0;
thread_local double g_bucket_index_find_s        = 0.0;
thread_local double g_bucket_symcoef_add_s       = 0.0;
thread_local double g_bucket_emplace_s           = 0.0;
// 2026-04-28 (Lever-D gate counters, reviewer-prescribed):
// PolesBucket::bump collision-branch counters + merge-slot in/out
// monomial counts.  Used to verify the lazy-canonicalize savings
// model before any algorithmic change in PolesBucket.  Decision rule:
// ship Lever D iff mean_K (= collision_calls / out_terms-from-merge)
// >= 8 AND mean_post / mean_pre <= 1.3.
thread_local long   g_bucket_collision_calls     = 0;
thread_local long   g_bucket_collision_pre_terms = 0;
thread_local long   g_bucket_collision_post_terms= 0;
thread_local long   g_merge_in_terms             = 0;
thread_local long   g_merge_out_terms            = 0;
thread_local long   g_merge_slots                = 0;
// 2026-04-26 loop_residual_s drill: post-integrate_ii pole-expansion
// triage (lines ~846-937 of this file). The two `expand` timers are
// mutually exclusive (each primitive goes through Zero OR Inf); the
// `bucket_bump_s` timer is a subset of (zero+inf), measuring just the
// polesZero/polesInf bump() calls inside both branches.
thread_local double g_pole_zero_expand_s         = 0.0;
thread_local double g_pole_inf_expand_s          = 0.0;
thread_local double g_bucket_bump_s              = 0.0;
// 2026-05-01 (Branch E pre-flight): sub-bucket timers inside the
// pole_inf_expand block (lines ~1378-1474). Together with
// g_pole_inf_expand_s, the consumer derives a 5th `pie_remainder_s`
// = pie_total - sum(4 leaves) which captures regzero_word_in_ctx,
// log_zero_row_as_poly, Rat::operator*, polesInf.bump, and loop
// overhead. Used to identify whether one leaf is a single-target
// lever candidate vs the work being broadly distributed.
thread_local double g_pie_substitute_var_reciprocal_s = 0.0;
thread_local double g_pie_series_expansion_s          = 0.0;
thread_local double g_pie_expand_inf_word_in_ctx_s    = 0.0;
thread_local double g_pie_rat_var0_coef_s             = 0.0;
// 2026-05-01 (Tier 3 Phase-0 lazy-pair diagnostic): per-step
// nterm-blowup accumulators around `reduce_inplace`.  See rat.hpp
// for the diagnostic question.  Env-gated by HF_REDUCE_NTERM_LOG=1
// (default off).
thread_local long   g_reduce_nterm_calls          = 0;
thread_local long   g_reduce_nterm_pre_total      = 0;
thread_local long   g_reduce_nterm_post_total     = 0;
thread_local long   g_reduce_nterm_pre_max        = 0;
thread_local long   g_reduce_nterm_post_max       = 0;
thread_local long   g_reduce_wide_smallfall_calls = 0;
// 2026-04-26 (Lever-C scout): two OMP/serial-merge diagnostic timers
// added per Reviewer #12 verdict. Goal is to decompose the 62-s
// "untimed gap" on tst2 step 3 between (a) parallelism inefficiency
// and (b) the serial post-OMP merge.
//
//   g_omp_parallel_wall_s  — wall-clock around the `#pragma omp
//     parallel for` in integration_step. Sum over threads of
//     accum_t[t].entry_wall_s / (n_threads * g_omp_parallel_wall_s) is the
//     OMP utilization η; barrier-idle = n_threads * wall - sum(entry).
//
//   g_omp_post_merge_s     — wall-clock around the serial
//     collect_into_flat + canonicalize-once-per-key block (lines
//     ~1130-1191). This is the unavoidable serial work after the
//     parallel region; if it's large, it caps achievable speedup.
//
// Cost: 5 chrono::steady_clock::now() per step boundary (~250 ns).
// Always-on; no env gate.
thread_local double g_omp_parallel_wall_s        = 0.0;
thread_local double g_omp_post_merge_s           = 0.0;
// 2026-04-30 (Probe 4 — axis-E omp_post_merge attribution): split the
// post-merge wall (97.5 wall-s aggregate, 43.6% of run on parity-1
// ord_1_face_1 post-axis-D) into its four sub-phases.
//   omp_collect_into_flat_s     — serial copy of per-thread polesZero/Inf
//                                  buckets into the flat unordered_map.
//   omp_merge_sort_s            — serial std::sort(flat_v) by hash key.
//   omp_parallel_canonicalize_s — wall around the #pragma omp parallel
//                                  for over flat_v slots.
//   omp_serial_assembly_s       — serial walk of canon_slots into
//                                  polesInf.terms.
thread_local double g_omp_collect_into_flat_s     = 0.0;
thread_local double g_omp_merge_sort_s            = 0.0;
thread_local double g_omp_parallel_canonicalize_s = 0.0;
thread_local double g_omp_serial_assembly_s       = 0.0;
// 2026-04-30 (Tier 1.4a — η probe per adversarial reviewer): per-step
// max/min/total per-thread wall in the post-merge parallel-for. The
// upstream parallel-for has entry_{max,min}_per_thread_s already; the
// post-merge one (which Probe 4 located the 99.7%-of-omp_post_merge
// budget in) hadn't been load-balance-measured. If
// pm_canon_{max,min} skew is large (η = sum/(n*max) << 0.7), the
// next intervention is heavy-slot redistribution, NOT algorithmic
// per-call canonicalize speedup.
thread_local double g_pm_canon_max_per_thread_s   = 0.0;
thread_local double g_pm_canon_min_per_thread_s   = 0.0;
thread_local double g_pm_canon_sum_per_thread_s   = 0.0;
// 2026-04-27 (Lever A): partial_fractions cache hit/miss summed
// across worker threads via post-OMP atomic harvest. Per-step.
thread_local long   g_pf_cache_hits_step         = 0;
thread_local long   g_pf_cache_misses_step       = 0;
thread_local long   g_pf_cache_collisions_step   = 0;
// Reviewer #13 Hole 3: per-step spread of accum_t[t].entry_wall_s, the right
// metric for diagnosing load imbalance vs serial setup. The existing
// average-thread-idle = n_threads * par_wall - sum(entry) conflates
// both. The spread directly drives Lever-C-schedule decisions:
//   - large entry_max - entry_min on step 3 → schedule(dynamic, k)
//     (one straggler iteration is the bottleneck)
//   - small spread but large idle → serial setup outside per-thread
//     work, attack omp_post_merge_s or pre_omp_s instead.
thread_local double g_entry_max_per_thread_s     = 0.0;
thread_local double g_entry_min_per_thread_s     = 0.0;

void reset_step_sub_timers() {
    g_integration_step_canon_s = 0.0;
    g_closure_body_s = 0.0;
    g_closure_canon_s = 0.0;
    g_transform_shuffle_s = 0.0;
    g_integrate_ii_s = 0.0;
    g_loop_residual_s = 0.0;
    g_partial_fractions_s = 0.0;
    g_linear_factors_s = 0.0;
    g_lf_flint_factor_s = 0.0;
    g_lf_cache_hits = 0;
    g_lf_cache_misses = 0;
    g_lf_flint_deg1_s = 0.0;
    g_lf_flint_deg2_s = 0.0;
    g_lf_miss_deg1 = 0;
    g_lf_miss_deg2 = 0;
    g_lf_d3p_all_linear_count = 0;
    g_lf_d3p_all_linear_s = 0.0;
    g_lf_d3p_has_nonlinear_count = 0;
    g_lf_d3p_has_nonlinear_s = 0.0;
    g_lf_d3p_squarefree_count = 0;
    g_lf_d3p_squarefree_s = 0.0;
    g_lf_d3p_repeated_count = 0;
    g_lf_d3p_repeated_s = 0.0;
    g_lf_sqf_total_s = 0.0;
    g_lf_sqf_decomp_s = 0.0;
    g_lf_sqf_inner_factor_s = 0.0;
    g_lf_sqf_calls = 0;
    g_lf_sqf_inner_factor_calls = 0;
    g_lf_sqf_bailouts = 0;
    g_bump_lookup_s = 0.0;
    g_bump_addto_s = 0.0;
    g_push_ibp_s = 0.0;
    g_antideriv_s = 0.0;
    g_bump_calls = 0;
    g_bump_emplace_s = 0.0;
    g_bump_rat_add_s = 0.0;
    g_bump_rat_add_calls = 0;
    g_pf_calls_in_loop = 0;
    g_pf_unique_dens = 0;
    g_bump_unique_rows = 0;
    g_lf_lock_held_s = 0.0;
    g_lf_lock_wait_s = 0.0;
    g_lf_cache_key_build_s = 0.0;
    g_lf_post_transplant_s = 0.0;
    g_lf_post_rat_ctor_s = 0.0;
    g_lf_post_constant_to_string_s = 0.0;
    g_lf_post_clone_from_raw_s = 0.0;
    g_ii_queue_copy_s = 0.0;
    g_ii_pole_arith_s = 0.0;
    g_ii_pole_word_ctor_s = 0.0;
    g_reduce_narrow_s = 0.0;
    g_reduce_wide_s = 0.0;
    g_reduce_narrow_calls = 0;
    g_reduce_wide_calls = 0;
    g_reduce_zero_calls = 0;
    g_gcd_cofactors_s = 0.0;
    g_gcd_cofactors_calls = 0;
    g_rn_used_vars_s = 0.0;
    g_rn_setup_s = 0.0;
    g_rn_post_s = 0.0;
    g_rat_mul_calls = 0;
    g_rat_sub_calls = 0;
    g_rat_div_calls = 0;
    g_rat_add_polymul_s = 0.0;
    g_rat_add_polyadd_s = 0.0;
    g_rat_add_calls = 0;
    g_rat_add_legacy_wall_s = 0.0;
    g_rat_add_via_qu_wall_s = 0.0;
    g_rat_add_legacy_calls = 0;
    g_rat_add_via_qu_calls = 0;
    g_mul_narrow_s = 0.0;
    g_mul_wide_s = 0.0;
    g_mul_narrow_calls = 0;
    g_mul_wide_calls = 0;
    g_mul_gated_calls = 0;
    for (int i = 0; i < 6; ++i) {
        g_nbin_lalb_count[i] = 0;
        g_nbin_lalb_us[i] = 0.0;
        g_nbin_lalb_max[i] = 0.0;
    }
    for (int i = 0; i < 5; ++i) {
        g_nbin_u_count[i] = 0;
        g_nbin_u_us[i] = 0.0;
    }
    g_bucket_canon_regkey_s = 0.0;
    g_bucket_struct_hash_s = 0.0;
    g_bucket_index_find_s = 0.0;
    g_bucket_symcoef_add_s = 0.0;
    g_bucket_emplace_s = 0.0;
    g_bucket_collision_calls = 0;
    g_bucket_collision_pre_terms = 0;
    g_bucket_collision_post_terms = 0;
    g_merge_in_terms = 0;
    g_merge_out_terms = 0;
    g_merge_slots = 0;
    g_pole_zero_expand_s = 0.0;
    g_pole_inf_expand_s = 0.0;
    g_bucket_bump_s = 0.0;
    g_pie_substitute_var_reciprocal_s = 0.0;
    g_pie_series_expansion_s = 0.0;
    g_pie_expand_inf_word_in_ctx_s = 0.0;
    g_pie_rat_var0_coef_s = 0.0;
    g_reduce_nterm_calls = 0;
    g_reduce_nterm_pre_total = 0;
    g_reduce_nterm_post_total = 0;
    g_reduce_nterm_pre_max = 0;
    g_reduce_nterm_post_max = 0;
    g_reduce_wide_smallfall_calls = 0;
    g_omp_parallel_wall_s = 0.0;
    g_omp_post_merge_s = 0.0;
    g_omp_collect_into_flat_s = 0.0;
    g_omp_merge_sort_s = 0.0;
    g_omp_parallel_canonicalize_s = 0.0;
    g_omp_serial_assembly_s = 0.0;
    g_pm_canon_max_per_thread_s = 0.0;
    g_pm_canon_min_per_thread_s = 0.0;
    g_pm_canon_sum_per_thread_s = 0.0;
    g_pf_cache_hits_step = 0;
    g_pf_cache_misses_step = 0;
    g_pf_cache_collisions_step = 0;
    g_entry_max_per_thread_s = 0.0;
    g_entry_min_per_thread_s = 0.0;
}

double read_integration_step_canon_s() { return g_integration_step_canon_s; }
double read_closure_body_s()           { return g_closure_body_s; }
double read_closure_canon_s()          { return g_closure_canon_s; }
double read_transform_shuffle_s()      { return g_transform_shuffle_s; }
double read_integrate_ii_s()           { return g_integrate_ii_s; }
double read_loop_residual_s()          { return g_loop_residual_s; }
double read_partial_fractions_s()      { return g_partial_fractions_s; }
double read_linear_factors_s()         { return g_linear_factors_s; }
double read_lf_flint_factor_s()        { return g_lf_flint_factor_s; }
long   read_lf_cache_hits()            { return g_lf_cache_hits; }
long   read_lf_cache_misses()          { return g_lf_cache_misses; }
double read_lf_flint_deg1_s()          { return g_lf_flint_deg1_s; }
double read_lf_flint_deg2_s()          { return g_lf_flint_deg2_s; }
long   read_lf_miss_deg1()             { return g_lf_miss_deg1; }
long   read_lf_miss_deg2()             { return g_lf_miss_deg2; }
long   read_lf_d3p_all_linear_count()  { return g_lf_d3p_all_linear_count; }
double read_lf_d3p_all_linear_s()      { return g_lf_d3p_all_linear_s; }
long   read_lf_d3p_has_nonlinear_count(){return g_lf_d3p_has_nonlinear_count; }
double read_lf_d3p_has_nonlinear_s()   { return g_lf_d3p_has_nonlinear_s; }
long   read_lf_d3p_squarefree_count()  { return g_lf_d3p_squarefree_count; }
double read_lf_d3p_squarefree_s()      { return g_lf_d3p_squarefree_s; }
long   read_lf_d3p_repeated_count()    { return g_lf_d3p_repeated_count; }
double read_lf_d3p_repeated_s()        { return g_lf_d3p_repeated_s; }
double read_lf_sqf_total_s()           { return g_lf_sqf_total_s; }
double read_lf_sqf_decomp_s()          { return g_lf_sqf_decomp_s; }
double read_lf_sqf_inner_factor_s()    { return g_lf_sqf_inner_factor_s; }
long   read_lf_sqf_calls()             { return g_lf_sqf_calls; }
long   read_lf_sqf_inner_factor_calls(){ return g_lf_sqf_inner_factor_calls; }
long   read_lf_sqf_bailouts()          { return g_lf_sqf_bailouts; }
double read_bump_lookup_s()            { return g_bump_lookup_s; }
double read_bump_addto_s()             { return g_bump_addto_s; }
double read_push_ibp_s()               { return g_push_ibp_s; }
double read_antideriv_s()              { return g_antideriv_s; }
long   read_bump_calls()               { return g_bump_calls; }
double read_bump_emplace_s()           { return g_bump_emplace_s; }
double read_bump_rat_add_s()           { return g_bump_rat_add_s; }
long   read_bump_rat_add_calls()       { return g_bump_rat_add_calls; }
long   read_pf_calls_in_loop()         { return g_pf_calls_in_loop; }
long   read_pf_unique_dens()           { return g_pf_unique_dens; }
long   read_bump_unique_rows()         { return g_bump_unique_rows; }
double read_lf_lock_held_s()           { return g_lf_lock_held_s; }
double read_lf_lock_wait_s()           { return g_lf_lock_wait_s; }
double read_lf_cache_key_build_s()     { return g_lf_cache_key_build_s; }
double read_lf_post_transplant_s()         { return g_lf_post_transplant_s; }
double read_lf_post_rat_ctor_s()           { return g_lf_post_rat_ctor_s; }
double read_lf_post_constant_to_string_s() { return g_lf_post_constant_to_string_s; }
double read_lf_post_clone_from_raw_s()     { return g_lf_post_clone_from_raw_s; }
double read_ii_queue_copy_s()              { return g_ii_queue_copy_s; }
double read_ii_pole_arith_s()              { return g_ii_pole_arith_s; }
double read_ii_pole_word_ctor_s()          { return g_ii_pole_word_ctor_s; }
double read_reduce_narrow_s()          { return g_reduce_narrow_s; }
double read_reduce_wide_s()            { return g_reduce_wide_s; }
double read_gcd_cofactors_s()          { return g_gcd_cofactors_s; }
long   read_gcd_cofactors_calls()      { return g_gcd_cofactors_calls; }
double read_rn_used_vars_s()           { return g_rn_used_vars_s; }
double read_rn_setup_s()               { return g_rn_setup_s; }
double read_rn_post_s()                { return g_rn_post_s; }
long   read_rat_mul_calls()            { return g_rat_mul_calls; }
long   read_rat_sub_calls()            { return g_rat_sub_calls; }
long   read_rat_div_calls()            { return g_rat_div_calls; }
long   read_reduce_narrow_calls()      { return g_reduce_narrow_calls; }
long   read_reduce_wide_calls()        { return g_reduce_wide_calls; }
long   read_reduce_zero_calls()        { return g_reduce_zero_calls; }
double read_rat_add_polymul_s()        { return g_rat_add_polymul_s; }
double read_rat_add_polyadd_s()        { return g_rat_add_polyadd_s; }
long   read_rat_add_calls()            { return g_rat_add_calls; }
double read_rat_add_legacy_wall_s()    { return g_rat_add_legacy_wall_s; }
double read_rat_add_via_qu_wall_s()    { return g_rat_add_via_qu_wall_s; }
long   read_rat_add_legacy_calls()     { return g_rat_add_legacy_calls; }
long   read_rat_add_via_qu_calls()     { return g_rat_add_via_qu_calls; }
double read_mul_narrow_s()             { return g_mul_narrow_s; }
double read_mul_wide_s()               { return g_mul_wide_s; }
long   read_mul_narrow_calls()         { return g_mul_narrow_calls; }
long   read_mul_wide_calls()           { return g_mul_wide_calls; }
long   read_mul_gated_calls()          { return g_mul_gated_calls; }
long   read_nbin_lalb_count(int i)     { return (i>=0&&i<6) ? g_nbin_lalb_count[i] : 0; }
double read_nbin_lalb_us(int i)        { return (i>=0&&i<6) ? g_nbin_lalb_us[i]    : 0.0; }
double read_nbin_lalb_max(int i)       { return (i>=0&&i<6) ? g_nbin_lalb_max[i]   : 0.0; }
long   read_nbin_u_count(int i)        { return (i>=0&&i<5) ? g_nbin_u_count[i]    : 0; }
double read_nbin_u_us(int i)           { return (i>=0&&i<5) ? g_nbin_u_us[i]       : 0.0; }
double read_bucket_canon_regkey_s()    { return g_bucket_canon_regkey_s; }
double read_bucket_struct_hash_s()     { return g_bucket_struct_hash_s; }
double read_bucket_index_find_s()      { return g_bucket_index_find_s; }
double read_bucket_symcoef_add_s()     { return g_bucket_symcoef_add_s; }
double read_bucket_emplace_s()         { return g_bucket_emplace_s; }
long   read_bucket_collision_calls()      { return g_bucket_collision_calls; }
long   read_bucket_collision_pre_terms()  { return g_bucket_collision_pre_terms; }
long   read_bucket_collision_post_terms() { return g_bucket_collision_post_terms; }
long   read_merge_in_terms()              { return g_merge_in_terms; }
long   read_merge_out_terms()             { return g_merge_out_terms; }
long   read_merge_slots()                 { return g_merge_slots; }
double read_pole_zero_expand_s()       { return g_pole_zero_expand_s; }
double read_pole_inf_expand_s()        { return g_pole_inf_expand_s; }
double read_pie_substitute_var_reciprocal_s() { return g_pie_substitute_var_reciprocal_s; }
double read_pie_series_expansion_s()          { return g_pie_series_expansion_s; }
double read_pie_expand_inf_word_in_ctx_s()    { return g_pie_expand_inf_word_in_ctx_s; }
double read_pie_rat_var0_coef_s()             { return g_pie_rat_var0_coef_s; }
long   read_reduce_nterm_calls()              { return g_reduce_nterm_calls; }
long   read_reduce_nterm_pre_total()          { return g_reduce_nterm_pre_total; }
long   read_reduce_nterm_post_total()         { return g_reduce_nterm_post_total; }
long   read_reduce_nterm_pre_max()            { return g_reduce_nterm_pre_max; }
long   read_reduce_nterm_post_max()           { return g_reduce_nterm_post_max; }
long   read_reduce_wide_smallfall_calls()     { return g_reduce_wide_smallfall_calls; }
double read_bucket_bump_s()            { return g_bucket_bump_s; }
double read_omp_parallel_wall_s()      { return g_omp_parallel_wall_s; }
double read_omp_post_merge_s()         { return g_omp_post_merge_s; }
double read_omp_collect_into_flat_s()      { return g_omp_collect_into_flat_s; }
double read_omp_merge_sort_s()             { return g_omp_merge_sort_s; }
double read_omp_parallel_canonicalize_s()  { return g_omp_parallel_canonicalize_s; }
double read_omp_serial_assembly_s()        { return g_omp_serial_assembly_s; }
double read_pm_canon_max_per_thread_s()    { return g_pm_canon_max_per_thread_s; }
double read_pm_canon_min_per_thread_s()    { return g_pm_canon_min_per_thread_s; }
double read_pm_canon_sum_per_thread_s()    { return g_pm_canon_sum_per_thread_s; }
long   read_pf_cache_hits_step()       { return g_pf_cache_hits_step; }
long   read_pf_cache_misses_step()     { return g_pf_cache_misses_step; }
long   read_pf_cache_collisions_step() { return g_pf_cache_collisions_step; }
double read_entry_max_per_thread_s()   { return g_entry_max_per_thread_s; }
double read_entry_min_per_thread_s()   { return g_entry_min_per_thread_s; }

// 2026-04-27 (3l3pt profile-deepening): per-thread sub-timers inside
// PolesBucket::bump.  The reviewer's analysis of the prior
// 3l3pt diagnostic showed bucket_bump_s = 95-99% of loop_residual_s
// on heavy steps (5/6); the diagnostic's recommended split (Laurent-
// expand / divergent-residue / replacement-integrand) targets the
// remaining ~5%, not the dominant cost.  This split decomposes the
// dominant 95-99% into its actual sub-cost lines.
//
// Five sub-counters, one per non-trivial line inside bump():
//   bucket_canon_regkey_s  : canonicalize_regkey(key)
//   bucket_struct_hash_s   : regkey_struct_hash(canon)
//   bucket_index_find_s    : index.find(k) (unordered_map lookup)
//   bucket_symcoef_add_s   : terms[..].coef + coef (the SUSPECT;
//                              SymCoef::add → SymCoef::from_monomials
//                              → canonicalize → Rat::Rat → Poly::transplant
//                              → fmpq_mpoly_reduce on every existing-row hit)
//   bucket_emplace_s       : index[k] = ... + terms.push_back(...)
//                              (the new-row path)
//
// Phase 1 (poles streaming, 2026-05-03, Task 1.3): these storage
// helpers were promoted from the anonymous namespace into
// hyperflint::detail with external linkage so the struct PolesBucket
// (now in include/hyperflint/integrator/poles_bucket.hpp) can address
// them from its inline bump() body.
namespace detail {
std::vector<double>& bucket_canon_regkey_storage()
    { static std::vector<double> v; return v; }
std::vector<double>& bucket_struct_hash_storage()
    { static std::vector<double> v; return v; }
std::vector<double>& bucket_index_find_storage()
    { static std::vector<double> v; return v; }
std::vector<double>& bucket_symcoef_add_storage()
    { static std::vector<double> v; return v; }
std::vector<double>& bucket_emplace_storage()
    { static std::vector<double> v; return v; }
// 2026-04-28 (Lever-D gate counters)
std::vector<long>& bucket_collision_calls_storage()
    { static std::vector<long> v; return v; }
std::vector<long>& bucket_collision_pre_terms_storage()
    { static std::vector<long> v; return v; }
std::vector<long>& bucket_collision_post_terms_storage()
    { static std::vector<long> v; return v; }

// Avenue F gate. Default ON.
// iter-86 §T7 sixteenth chunk Track-rat-counter macro layer:
// raw std::getenv call replaced by HF_FLAG_DEFER_BUMP from
// hyperflint/integrator/env_flags.hpp. Call-site gate logic
// (default-ON: !(e && e[0]=='0')) preserved verbatim. The
// default-direction discrepancy vs the docs §2 table row for
// this gate (table says unset-implies-OFF; code is unset-implies-ON)
// is observed but not addressed here (mirrors iter-65 F8
// advisory pattern; macro-layer LAND scope is mechanical
// literal-to-symbol).
bool defer_bump_enabled() {
    static const bool on = []{
        const char* e = HF_FLAG_DEFER_BUMP;
        return !(e && e[0] == '0');
    }();
    return on;
}
}  // namespace detail

namespace {

std::vector<long>& merge_in_terms_storage()
    { static std::vector<long> v; return v; }
std::vector<long>& merge_out_terms_storage()
    { static std::vector<long> v; return v; }

void init_bucket_sub_timers_per_thread(int n_threads) {
    const size_t n = static_cast<size_t>(n_threads > 0 ? n_threads : 1);
    detail::bucket_canon_regkey_storage().assign(n, 0.0);
    detail::bucket_struct_hash_storage().assign(n, 0.0);
    detail::bucket_index_find_storage().assign(n, 0.0);
    detail::bucket_symcoef_add_storage().assign(n, 0.0);
    detail::bucket_emplace_storage().assign(n, 0.0);
    detail::bucket_collision_calls_storage().assign(n, 0L);
    detail::bucket_collision_pre_terms_storage().assign(n, 0L);
    detail::bucket_collision_post_terms_storage().assign(n, 0L);
    merge_in_terms_storage().assign(n, 0L);
    merge_out_terms_storage().assign(n, 0L);
}
void reset_bucket_sub_timers_per_thread() {
    for (auto& x : detail::bucket_canon_regkey_storage()) x = 0.0;
    for (auto& x : detail::bucket_struct_hash_storage()) x = 0.0;
    for (auto& x : detail::bucket_index_find_storage())  x = 0.0;
    for (auto& x : detail::bucket_symcoef_add_storage()) x = 0.0;
    for (auto& x : detail::bucket_emplace_storage())     x = 0.0;
    for (auto& x : detail::bucket_collision_calls_storage())     x = 0L;
    for (auto& x : detail::bucket_collision_pre_terms_storage()) x = 0L;
    for (auto& x : detail::bucket_collision_post_terms_storage())x = 0L;
    for (auto& x : merge_in_terms_storage())  x = 0L;
    for (auto& x : merge_out_terms_storage()) x = 0L;
}
double sum_bucket_canon_regkey_s_per_thread() {
    double s = 0; for (double x : detail::bucket_canon_regkey_storage()) s += x; return s;
}
double sum_bucket_struct_hash_s_per_thread() {
    double s = 0; for (double x : detail::bucket_struct_hash_storage()) s += x; return s;
}
double sum_bucket_index_find_s_per_thread() {
    double s = 0; for (double x : detail::bucket_index_find_storage()) s += x; return s;
}
double sum_bucket_symcoef_add_s_per_thread() {
    double s = 0; for (double x : detail::bucket_symcoef_add_storage()) s += x; return s;
}
double sum_bucket_emplace_s_per_thread() {
    double s = 0; for (double x : detail::bucket_emplace_storage()) s += x; return s;
}
long sum_bucket_collision_calls_per_thread() {
    long s = 0; for (long x : detail::bucket_collision_calls_storage()) s += x; return s;
}
long sum_bucket_collision_pre_terms_per_thread() {
    long s = 0; for (long x : detail::bucket_collision_pre_terms_storage()) s += x; return s;
}
long sum_bucket_collision_post_terms_per_thread() {
    long s = 0; for (long x : detail::bucket_collision_post_terms_storage()) s += x; return s;
}
long sum_merge_in_terms_per_thread() {
    long s = 0; for (long x : merge_in_terms_storage()) s += x; return s;
}
long sum_merge_out_terms_per_thread() {
    long s = 0; for (long x : merge_out_terms_storage()) s += x; return s;
}

// A single {logpower, power}-indexed bucket of regulator contributions,
// keyed internally by RegKey content-hash so repeated RegKeys sum.
// Bug #6 lift: coefficients are SymCoef so positive-letter
// contour-deformation residues (I*Pi*delta[var]) can live inside
// polesInf/polesZero entries.
//
// 2026-04-26 (a-prime lever): index keyed by 128-bit structural hash
// instead of the ostringstream-built `regkey_content_key()` string —
// the bump path was the dominant `loop_residual_s` cost on tst2 step 3
// (377.9 CPU-s of 452.9). canonicalize_regkey() is unchanged so the
// canonical byte-form (used for cross-call cache identity in
// transform.cpp) stays stable. See notes/benchmark_smirnov/sqf_round/
// loop_residual/residual_triage.md.
// Avenue F (2026-04-30): defer-bump-canonicalize. The collision branch
// previously did `terms[i].coef += coef` per bump, paying one
// SymCoef::canonicalize → reduce_inplace per collision. Step 6 of
// parity-1 had ~49k collisions across ~1054 buckets (K_per_bucket ≈ 47),
// so eager canonicalize forfeit 46/47 of the achievable savings.
//
// Defer-mode collects raw chunks per bucket entry in a parallel
// `pending` vector. `collect_into_flat` drains both `terms[i].coef` and
// every entry of `pending[i]` into the per-key `chunks` vector that
// already feeds Lever-C-merge's parallel-by-key `from_monomials`. Net
// effect: the per-thread bucket-fill loop avoids reduce_inplace
// entirely on collisions; all canonicalize work moves to the existing
// post-merge phase, where Tier 1.6b's tree-merge already parallelises
// it.
//
// Falsifier expectation: bucket_symcoef_add_s on parity-1 step 6 drops
// from 261 → ≤ 50 CPU-s. omp_post_merge_s may grow but parity-1 wall
// must improve ≥ 10 s. Bit-identity preserved on tst0/1/2 + parity-1.
//
// Env gate: `HF_DEFER_BUMP=0` falls back to the eager (pre-Avenue-F)
// path. Default ON. The flag's definition lives in
// `hyperflint::detail` (above) so the inline PolesBucket::bump body
// in the public header can reach it from outside this TU.
//
// Phase 1 / Task 1.3 (2026-05-03): struct PolesBucket extracted to
// include/hyperflint/integrator/poles_bucket.hpp so the new
// flush_threshold / flush_callback / drain() machinery is reachable
// from test/unit/test_poles_bucket_flush.cpp. The struct is the same
// per-thread accumulator used by the integration_step OMP region;
// only the location and the new opt-in flush members changed (the
// pre-existing bit-identical behaviour is preserved when
// flush_threshold == SIZE_MAX, the default).
}  // namespace
}  // namespace hyperflint

#include "hyperflint/integrator/poles_bucket.hpp"

namespace hyperflint {
namespace {

// Phase 0.5 Item U Stage 1: per-thread accumulator bundle for the outer
// OMP parallel-for.  Groups the four per-slot objects that Stage 2 (the
// optional GCD/dispatch_apply path, HF_USE_GCD=1) needs to resolve via a
// thread-id source other than omp_get_thread_num().  Keeping them in a
// single struct makes the Stage 2 slot-access hook a one-line change:
// replace `accum_t[omp_get_thread_num()]` with
// `accum_t[gcd_thread_slot()]` at the two OMP-body call sites below.
//
// Recursion safety: this struct is allocated on the stack inside
// integration_step (as std::vector<PerThreadAccum>) and is NOT
// thread_local, so re-entrant calls each get their own independent
// vector — exactly the same guarantee as the original four separate
// std::vector declarations.
struct PerThreadAccum {
    PolesBucket              polesZero;
    PolesBucket              polesInf;
    std::shared_ptr<ZWTable> zw;              // null unless scalar_rep_enabled()
    double                   entry_wall_s = 0.0; // per-entry body wall (spread tracking)
};

// ----- Phase 1 / Task 1.4: HF_POLES_STREAM env-gate ----------------
//
// When HF_POLES_STREAM=1, the per-thread PolesBuckets are configured
// with a flush_callback that streams entries into a per-step
// stack-local ShardedFlatMap<16>. Default-off path keeps the legacy
// `collect_into_flat` serial drain (bit-identical pre-Task-1.4
// behaviour). R28 fixes folded in:
//   C1: clear (terms, pending, index) atomically AFTER shard append
//       in poles_flush_to_sharded.
//   C2: drain via extract_into_sorted at end of region (chunks
//       ordering depends only on canonical content, not arrival).
//   C3: skip wiring entirely when the OMP region is serial
//       (do_parallel == false || n_threads_outer == 1).
//   R3: default threshold = 512 (geometric mean of {256, 1024}).
//   R5: per-step stack-local lifetime; RAII clean-up on the throw
//       path is implicit (ShardedFlatMap default destructor; the
//       PolesBucket destructor doesn't reach back into the shard).
struct PolesStreamConfig {
    bool        enabled;
    std::size_t threshold;
};

inline PolesStreamConfig poles_stream_config() {
    static const PolesStreamConfig cfg = []{
        const char* on = HF_FLAG_POLES_STREAM;
        const char* th = HF_FLAG_POLES_STREAM_N;
        std::size_t T = (th && *th)
            ? static_cast<std::size_t>(std::atol(th))
            : static_cast<std::size_t>(512);  // R28 R3 default
        // "0" / empty / unset all read as disabled. Any other value
        // turns it on (matches the convention used by
        // HF_DISABLE_PARALLEL_MERGE / HF_REGKEY_DUMP elsewhere).
        bool E = on && *on && on[0] != '0';
        return PolesStreamConfig{E, T};
    }();
    return cfg;
}

// PolesBucket flush callback: walk the bucket and merge entries into
// the ShardedFlatMap pointed to by user_data. R28 C1 contract: clear
// (terms, pending, index) atomically AFTER the shard append. The
// callback for zero-bucket and inf-bucket streams is structurally
// identical; the host code passes a different ShardedFlatMap* per
// bucket.
void poles_flush_to_sharded(PolesBucket& self, void* user_data) {
    auto* shard = static_cast<hyperflint::ShardedFlatMap<16>*>(user_data);
    const std::size_t n = self.terms.size();
    for (std::size_t i = 0; i < n; ++i) {
        auto& z    = self.terms[i];
        auto& pend = self.pending[i];
        const auto k = regkey_struct_hash(z.key);
        if (!z.coef.is_zero()) {
            shard->merge_one(k, z.key, std::move(z.coef));
        }
        if (!pend.empty()) {
            shard->merge_pending(k, z.key, pend);
        }
    }
    // R28 C1: clear all three together AFTER the shard append.
    self.terms.clear();
    self.pending.clear();
    self.index.clear();
}

// Expand the log^0 row of a SeriesTable into a polynomial in var:
//     sum_{m=0..len-1} row[m] * var^m.
// The row's coefficients are Rats over the non-var variables.
Rat log_zero_row_as_poly(const PolyCtx& ctx,
                         const std::vector<Rat>& row,
                         size_t var_idx) {
    Rat out{Poly::zero_of(ctx)};
    if (row.empty()) return out;
    // Build the var^m factors via fmpq_mpoly_gen + pow instead of
    // routing each monomial through FLINT's string parser — this path
    // is hit once per pole per integration step, where the wide-ctx
    // string parse was the dominant cost.
    const Poly var_poly = Poly::gen(ctx, var_idx);
    for (size_t m = 0; m < row.size(); ++m) {
        if (row[m].is_zero()) continue;
        Rat term = row[m];
        if (m > 0) {
            term = term * Rat{var_poly.pow(static_cast<unsigned long>(m))};
        }
        out = out + term;
    }
    return out;
}

bool word_is_all_minus_one(const PolyCtx& ctx, const Word& w) {
    if (w.empty()) return false;
    Rat minus_one{Poly::from_int(ctx, -1)};
    for (const auto& l : w.letters) {
        if (!l.equal(minus_one)) return false;
    }
    return true;
}

// [var^power](r). For power <= 0 this multiplies r by var^(-power)
// so rat_var0_coefficient lands on the right Laurent term.
// Input invariant: r = N/D with D a pure var-monomial × constants
// (the shape produced by `series_expansion` and its products with
// other Rats whose denominators don't vanish at var=0).
Rat var_power_coefficient(const PolyCtx& ctx, const Rat& r,
                           size_t var_idx, long power) {
    if (power == 0) return rat_var0_coefficient(ctx, r, var_idx);
    const Rat var_rat(Poly::gen(ctx, var_idx));
    if (power > 0) {
        // [var^p](r) = [var^0](r / var^p)
        Rat denom = var_rat.pow(static_cast<long>(power));
        return rat_var0_coefficient(ctx, r / denom, var_idx);
    } else {
        // power < 0: [var^p](r) = [var^0](r * var^(-p))
        Rat factor = var_rat.pow(static_cast<long>(-power));
        return rat_var0_coefficient(ctx, r * factor, var_idx);
    }
}

// True iff `s` is a literal positive-integer string ("1", "2", ...).
bool is_positive_integer_literal(const std::string& s) {
    if (s.empty() || s[0] == '-' || s == "0") return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

bool word_has_positive_letter(const Word& w) {
    for (const auto& l : w.letters) {
        if (is_positive_integer_literal(l.to_string())) return true;
    }
    return false;
}

// Replay Fragment-P2 positive-letter closure on a RegulatorSym.
// Mirrors HyperIntica.wl:4832-4856 (the post-TestZeroFunction
// redistribution in the reference IntegrationStep). For each term:
//   - Partition the term's key words into with_pos / without_pos.
//   - If exactly one positive-letter word exists, call
//     break_up_contour_sym on it and re-bucket the result into the
//     remaining (positive-letter-free) key.
//   - 0 or >= 2 positive-letter words: pass through unchanged
//     (matches Mma's Continue[] skip for the multi-positive case).
// Used by integration_step_sym on the final Rat-integrated output,
// and by check_divergences_pass on each non-(0,0) pole bin before
// the zero test (P1 caveat-2 fix — without this, convergent
// integrands whose bin residues cancel only after positive-letter
// closure would be falsely flagged divergent).
RegulatorSym close_positive_letters_in_regulator_sym(
        const PolyCtx& ctx,
        const RegulatorSym& base,
        const std::string& var_name,
        const MzvReductionTable& table) {
    const auto t_body0 = std::chrono::steady_clock::now();

    // Phase 9b-3: per-term parallelism.  Each iteration of the loop
    // below is fully independent — no shared mutation of `base` or any
    // external state, only per-iteration accumulation into thread-local
    // partial buffers that we merge after the parallel region.
    //
    // The per-bin closure pass dominates Step 4 of tst2 (71 s, the
    // largest absolute HF-vs-Maple gap per
    // docs/hf_vs_hyperint_tst2_diagnostic.md), and the OUTER OMP loop
    // in `integration_step` is dead at Step 4 (single entry → no
    // parallelism), so this is the right place to add parallelism.
    //
    // Each thread accumulates into its own `RegulatorSym` partial.
    // After the loop we concatenate; canonicalize_regulator_sym then
    // runs once on the merged result.
    const long n = static_cast<long>(base.size());
#ifdef HF_HAVE_OPENMP
    const int n_threads_inner = omp_get_max_threads();
    // Phase 0.5 Item T env-var override: HF_OMP_PER_TERM_CLOSURE=0 disables
    // the per-term parallelization here; default is ON when n >= 8 (the
    // existing team-overhead amortization gate).
    // iter-94 Track-OMP macro-layer LAND: literal relocated to
    // integrator/env_flags.hpp 4th extension under §5.1 rule-1 BINDING
    // from adversarial-reviewer iter-94 Q-19 B1 substantive-pattern
    // dispatch. The exact-match-"0" predicate-family semantics and the
    // magic-static once-init pattern are preserved verbatim.
    static const bool s_per_term_closure_disabled = []() {
        const char* s = HF_FLAG_OMP_PER_TERM_CLOSURE;
        return s != nullptr && std::strcmp(s, "0") == 0;
    }();
    const bool do_parallel_closure = (n >= 8) && !s_per_term_closure_disabled;
#else
    const int n_threads_inner = 1;
    const bool do_parallel_closure = false;
#endif
    (void)do_parallel_closure;
    std::vector<RegulatorSym> partials(static_cast<size_t>(n_threads_inner));

#ifdef HF_OMP_FULL_SCHEDULES
    #pragma omp parallel for schedule(dynamic, 1) if(do_parallel_closure)
#else
    #pragma omp parallel for schedule(static, 1) if(do_parallel_closure)
#endif
    for (long ti = 0; ti < n; ++ti) {
        hf_promote_qos_once();
        // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num() in OMP mode;
        // under HF_USE_GCD=1 returns the GCD slot index.
        const int tid = ::hyperflint::runtime::hf_get_thread_num();
        // R24 rev 2 / chain 17 — OMP-iteration-body try/catch wrapper.
        // Catches `std::runtime_error` escape from the iteration body
        // (e.g. `Rat::div: division by zero` triggered by a
        // safe-failure zero produced by `parse_or_none` in
        // `to_mzv_one_word`).  Without this wrapper, OMP semantics
        // convert any uncaught exception escape from a parallel region
        // into `std::terminate`.  Default-off (tolerance disabled):
        // exceptions propagate as before, no behavior change.
        //
        // Fast-fail under tolerance: see the matching block in the main
        // `integration_step` parallel-for above.
        if (tolerance_enabled() && narrow_ctx_was_too_narrow()) continue;
        try {
        RegulatorSym& local = partials[static_cast<size_t>(tid)];
        const RegTermSym& t = base[static_cast<size_t>(ti)];

        std::vector<Word> with_pos, without_pos;
        for (const auto& w : t.key) {
            if (word_has_positive_letter(w)) with_pos.push_back(w);
            else                             without_pos.push_back(w);
        }
        if (with_pos.size() != 1) {
            local.push_back(RegTermSym{t.coef,
                                         canonicalize_regkey(t.key)});
            continue;
        }
        const Word& word = with_pos[0];
        std::vector<Rat> pos_letters;
        std::vector<long> pos_letters_val;
        for (const auto& l : word.letters) {
            std::string s = l.to_string();
            if (!is_positive_integer_literal(s)) continue;
            long v = std::stol(s);
            bool found = false;
            for (long u : pos_letters_val) if (u == v) { found = true; break; }
            if (!found) { pos_letters_val.push_back(v); pos_letters.push_back(l); }
        }
        std::vector<size_t> perm(pos_letters_val.size());
        for (size_t k = 0; k < perm.size(); ++k) perm[k] = k;
        std::sort(perm.begin(), perm.end(),
            [&](size_t a, size_t b) {
                return pos_letters_val[a] < pos_letters_val[b];
            });
        std::vector<OnAxisSymEntry> on_axis;
        for (size_t k : perm) {
            on_axis.push_back(OnAxisSymEntry{
                pos_letters[k],
                SymCoef::delta_factor(ctx, var_name)});
        }
        WordlistSym seed;
        seed.terms.push_back(WordlistSymTerm{
            SymCoef::from_rat(Rat::one_of(ctx)), word});
        // C0b.4 (iter-42): break_up_contour_sym now takes a mandatory
        // `std::shared_ptr<ZWTable>` parameter. `close_positive_letters_in_regulator_sym`
        // does not yet receive a `zw_tab` from its callers (deferred to
        // iter-43+ when the integration-step API thread-through lands).
        // Per-iteration allocation here is byte-identical to pre-iter-42
        // because break_up_contour_sym's body lambda used to allocate
        // its own per-call table; under default-OFF the lambda short-
        // circuits before any `zw_tab` use, so a null shared_ptr is
        // safe. NOTE: this callsite lives inside an OMP `parallel for`
        // (line 920/922) — each worker therefore holds its own per-
        // iteration ZWTable, matching the per-thread merge-on-join
        // discipline of design v2 §3.6a (single owner, no cross-thread
        // ZWTable sharing).
        std::shared_ptr<ZWTable> bcs_zw_tab_local;
        if (runtime::scalar_rep_enabled()) {
            // Iter-44 (2026-05-09): HF_SCALAR_REP_REQUIRE_PERSISTENT=1
            // assertion (Concern-2 mitigation). See scalar_rep.hpp.
            // NOTE: this callsite lives inside an OMP parallel-for
            // (line 920+); the OMP catch block at line 1032 swallows
            // std::runtime_error, so we use std::abort() to bypass.
            if (runtime::require_persistent_enabled()) {
                std::cerr << "[HF_SCALAR_REP_REQUIRE_PERSISTENT=1]"
                    << " integration_step.cpp:close_positive_letters_in"
                    << "_regulator_sym (around line 996): allocating"
                    << " per-call ZWTable inside OMP parallel-for."
                    << " Migrate to a per-worker persistent table"
                    << " supplied by the integrate_ii caller per"
                    << " design v2 sec 3.6a." << std::endl;
                std::abort();
            }
            bcs_zw_tab_local = std::make_shared<ZWTable>(ctx);
        }
        RegulatorSym buc =
            break_up_contour_sym(ctx, seed, on_axis, table,
                                   bcs_zw_tab_local);
        const SymCoef& orig = t.coef;
        for (const auto& b : buc) {
            RegKey new_key = without_pos;
            for (const auto& w : b.key) new_key.push_back(w);
            local.push_back(RegTermSym{orig.mul(b.coef),
                                         canonicalize_regkey(new_key)});
        }
        } catch (const NarrowCtxTooNarrow&) {
            // Already correctly attributed; let the OMP barrier carry
            // it as flag-set + post-barrier rethrow.  This catch is
            // defensive: NarrowCtxTooNarrow shouldn't escape into the
            // iteration body today (it's only thrown from host code),
            // but if a future deeper helper rethrows it inline we
            // want the same flag-set semantics.
            g_narrow_ctx_too_narrow.store(true,
                                            std::memory_order_relaxed);
            continue;
        } catch (const IntegrationStepFailed&) {
            // R26 C1 — legitimate divergence from integrate_ii (re-thrown
            // as IntegrationStepFailed).  NOT a narrow-ctx signal.  Must
            // propagate so the handler emits {"failed": true, ...}
            // instead of swallowing it as narrow_ctx_insufficient.
            // (defense-in-depth — break_up_contour_sym does not call
            // integrate_ii today, but we want this guard in place if
            // a future closure body does.)
            throw;
        } catch (const HyperFLINTDivergentIntegral&) {
            // R26 C1 — divergence detection.  NOT a narrow-ctx signal.
            throw;
        } catch (const std::runtime_error&) {
            if (tolerance_enabled()) {
                // Set the global flag; host code will throw
                // NarrowCtxTooNarrow after the OMP barrier.  Skip
                // remaining work in this iteration; subsequent
                // iterations are a wasted no-op but the loop must
                // run to its scheduled end (no `break` from a
                // worker iteration in OpenMP).
                g_narrow_ctx_too_narrow.store(true,
                                                std::memory_order_relaxed);
                continue;
            }
            throw;  // re-throw for default-off path (will std::terminate
                    // in OMP — same as before this commit).
        }
    }

    // R24 rev 2 / chain 17 — post-OMP flag check.  After the
    // implicit barrier of the parallel-for above, observe the
    // global narrow-ctx flag.  If any worker set it during
    // iteration, throw `NarrowCtxTooNarrow` from the host thread
    // (NOT from inside the parallel region — the OMP standard makes
    // exception escape from a parallel region implementation-
    // defined; on Apple-clang/libomp it calls `std::terminate`).
    if (narrow_ctx_was_too_narrow()) {
        throw NarrowCtxTooNarrow{
            "close_positive_letters_in_regulator_sym"};
    }

    // Concatenate per-thread partials into the final accumulator.
    RegulatorSym acc;
    size_t total = 0;
    for (const auto& p : partials) total += p.size();
    acc.reserve(total);
    for (auto& p : partials) {
        acc.insert(acc.end(),
                   std::make_move_iterator(p.begin()),
                   std::make_move_iterator(p.end()));
    }

    // Phase-B telemetry: split closure-body time from final-canonicalize time.
    const auto t_canon0 = std::chrono::steady_clock::now();
    RegulatorSym canon = canonicalize_regulator_sym(acc);
    const auto t_canon1 = std::chrono::steady_clock::now();
    g_closure_body_s  +=
        std::chrono::duration<double>(t_canon0 - t_body0).count();
    g_closure_canon_s +=
        std::chrono::duration<double>(t_canon1 - t_canon0).count();
    return canon;
}

// Run the divergence-check pass: re-do the per-entry work of
// integration_step but accumulate at every (logpower, power) pole
// bin, then test each non-(0,0) bin via test_zero_function_sym.
// Throws HyperFLINTDivergentIntegral on the first non-zero bin.
//
// Each bin is passed through close_positive_letters_in_regulator_sym
// first (P1 caveat-2): this replays the Fragment-P2 redistribution
// that integration_step_sym applies to its output, so residues that
// only cancel after I*Pi*delta[var] substitution are correctly
// recognized as zero. HyperIntica does the same (HyperIntica.wl:4832
// redistributes polesInf BEFORE running TestZeroFunction on it).
//
// Runs SERIALLY (no OpenMP) — this path is opt-in and rarely hit;
// simplicity beats parallelism here.
//
// `remaining_var_indices` is forwarded to `test_zero_function_sym` so
// the zero-check runs across the full fibration basis when the caller
// knows the remaining integration schedule. Empty list falls back to
// the base case (per-term is_zero check), which may produce false
// negatives on integrands whose divergent-bin SymCoefs only cancel
// via fibration-basis shuffle identities.
void check_divergences_pass(const PolyCtx& ctx,
                             const ShuffleListSym& input,
                             size_t var_idx,
                             const MzvReductionTable& table,
                             std::shared_ptr<ZWTable> zw_tab,
                             bool introduce_algebraic_letters,
                             const std::vector<size_t>&
                               remaining_var_indices) {
    using BinKey = std::pair<long, long>;  // (logpower, power)
    struct BinKeyHash {
        size_t operator()(const BinKey& k) const noexcept {
            return std::hash<long>{}(k.first) * 1315423911u +
                   std::hash<long>{}(k.second);
        }
    };
    std::unordered_map<BinKey, PolesBucket, BinKeyHash> bins_zero;
    std::unordered_map<BinKey, PolesBucket, BinKeyHash> bins_inf;

    for (const auto& entry : input) {
        if (entry.coef.is_zero()) continue;
        TransformResultSym transformed =
            transform_shuffle(ctx, entry.shuffle, var_idx, table,
                               zw_tab,
                               introduce_algebraic_letters);
        for (const auto& mon : entry.coef.terms()) {
            const Rat& prefactor = mon.prefactor;
            if (prefactor.is_zero()) continue;
            SymMonomial sym_only = mon;
            sym_only.prefactor = Rat::one_of(ctx);
            const SymCoef sym_outer =
                SymCoef::from_monomials(ctx, {sym_only});

            for (const auto& sub : transformed) {
                Wordlist scaled = scalar_mul_wordlist(sub.shuffle, prefactor);
                Wordlist primitive;
                try {
                    primitive = integrate_ii(ctx, scaled, var_idx,
                                              zw_tab,
                                              introduce_algebraic_letters);
                } catch (const IntegrateIIFailed& e) {
                    throw IntegrationStepFailed(e.what());
                }
                const RegulatorSym& reginfs = sub.regulator;

                for (const auto& prim : primitive.terms) {
                    const Rat& p_coef = prim.coef;
                    const Word& p_word = prim.word;

                    // Zero expansion at every (logpower, power) bin.
                    long minOrder_z = -p_coef.pole_degree(var_idx);
                    if (minOrder_z >= 0) {
                        SeriesTable zeroExp = expand_zero_word_in_ctx(
                            ctx, p_word, minOrder_z);
                        for (long lp = 0; lp < static_cast<long>(zeroExp.size());
                             ++lp) {
                            if (zeroExp[static_cast<size_t>(lp)].empty()) continue;
                            Rat sum_term = log_zero_row_as_poly(
                                ctx, zeroExp[static_cast<size_t>(lp)], var_idx);
                            Rat se = series_expansion(
                                ctx, p_coef, var_idx, minOrder_z);
                            Rat prod = sum_term * se;
                            for (long p = 0; p <= minOrder_z; ++p) {
                                if (lp == 0 && p == 0) continue;  // finite bin
                                Rat coef_z = -var_power_coefficient(
                                    ctx, prod, var_idx, -p);
                                if (coef_z.is_zero()) continue;
                                for (const auto& vv : reginfs) {
                                    RegKey L = normalize_shuffle_key(vv.key);
                                    bins_zero[{lp, p}].bump(
                                        L,
                                        sym_outer.mul(
                                            vv.coef.mul_rat(coef_z)));
                                }
                            }
                        }
                    }
                    // ---- Infinity expansion (caveat-1 fix) ----
                    // Symmetric to the zero side: substitute var -> 1/var,
                    // then iterate (lp, p) over Laurent coefficients at
                    // var=0 (which are the large-var divergences of the
                    // original).
                    Rat poly_sub_i = substitute_var_reciprocal(
                        ctx, p_coef, var_idx);
                    long minOrder_i = -poly_sub_i.pole_degree(var_idx);
                    if (minOrder_i >= 0) {
                        // Match main-loop series path: prepare poly_se
                        // once and iterate positions `i` over the word;
                        // each position carries its own SeriesTable.
                        Rat poly_se = series_expansion(
                            ctx, poly_sub_i, var_idx, minOrder_i);
                        Rat minus_one{Poly::from_int(ctx, -1)};
                        bool allone = true;
                        const long n = static_cast<long>(p_word.size());
                        for (long wi = n; wi >= 0; --wi) {
                            if (wi < n) {
                                if (!p_word[static_cast<size_t>(wi)].equal(minus_one))
                                    allone = false;
                                if (allone) continue;
                            }
                            Word take_i;
                            take_i.letters.assign(p_word.letters.begin(),
                                                  p_word.letters.begin() + wi);
                            Word drop_i;
                            drop_i.letters.assign(p_word.letters.begin() + wi,
                                                  p_word.letters.end());
                            SeriesTable part = expand_inf_word_in_ctx(
                                ctx, take_i, minOrder_i);
                            Wordlist reg = regzero_word_in_ctx(ctx, drop_i);
                            if (reg.terms.empty() || part.empty()) continue;
                            for (long lp = 0;
                                 lp < static_cast<long>(part.size()); ++lp) {
                                if (part[static_cast<size_t>(lp)].empty())
                                    continue;
                                Rat sum_term = log_zero_row_as_poly(
                                    ctx, part[static_cast<size_t>(lp)], var_idx);
                                Rat prod = sum_term * poly_se;
                                for (long p = 0; p <= minOrder_i; ++p) {
                                    if (lp == 0 && p == 0) continue;
                                    Rat coef_i = var_power_coefficient(
                                        ctx, prod, var_idx, -p);
                                    if (coef_i.is_zero()) continue;
                                    for (const auto& q : reginfs) {
                                        for (const auto& t : reg.terms) {
                                            RegKey t_key{t.word};
                                            RegKey L = combine_shuffle_keys(
                                                q.key, t_key);
                                            Rat reginfs_coef = t.coef;
                                            bins_inf[{lp, p}].bump(
                                                L,
                                                sym_outer.mul(
                                                    q.coef.mul_rat(
                                                        coef_i * reginfs_coef)));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Test each non-(0,0) bin (both zero and infinity): its accumulated
    // RegulatorSym must be the zero function. Each bin is first passed
    // through close_positive_letters_in_regulator_sym (caveat-2 fix)
    // so residues that only cancel after Fragment-P2 redistribution
    // are recognized. When `remaining_var_indices` is non-empty,
    // test_zero_function_sym projects onto the fibration basis over
    // those vars and tests per-term is_zero; empty falls back to the
    // base case (per-term is_zero check).
    const std::string& var_name = ctx.vars()[var_idx];
    auto check_bin_map = [&](auto& bin_map, const char* boundary_desc) {
        for (auto& kv : bin_map) {
            RegulatorSym rsym;
            rsym.reserve(kv.second.terms.size());
            for (auto& t : kv.second.terms) {
                if (!t.coef.is_zero())
                    rsym.push_back(std::move(t));
            }
            if (rsym.empty()) continue;
            RegulatorSym closed = close_positive_letters_in_regulator_sym(
                ctx, rsym, var_name, table);
            if (closed.empty()) continue;
            if (!test_zero_function_sym(ctx, closed,
                                          remaining_var_indices, table)) {
                throw HyperFLINTDivergentIntegral(
                    "var=" + var_name + " (" + boundary_desc + ")",
                    kv.first.first, kv.first.second);
            }
        }
    };
    check_bin_map(bins_zero, "zero boundary");
    check_bin_map(bins_inf,  "infinity boundary");
}

}  // namespace

RegulatorSym integration_step(const PolyCtx& ctx,
                                const ShuffleListSym& input,
                                size_t var_idx,
                                const MzvReductionTable& table,
                                std::shared_ptr<ZWTable> zw_tab,
                                bool check_divergences,
                                bool introduce_algebraic_letters,
                                const std::vector<size_t>&
                                  remaining_var_indices) {
    // HyperIntica-parity guard: forbid Wm/Wp introduction whose
    // discriminant would still depend on a remaining (un-integrated)
    // Feynman parameter.  See LFForbiddenVarsScope docs in
    // linear_factors.hpp.  Active for the duration of this step; the
    // RAII destructor restores the previous (typically null) value.
    LFForbiddenVarsScope _lf_forbid_scope(remaining_var_indices);

    // Phase 5e-iii: when check_divergences=true, run a secondary
    // serial pass that accumulates the NON-(0,0) {logpower, power}
    // bins and tests each via test_zero_function_sym. This pass does
    // not affect the main (0,0)-only accumulator below — that fast
    // parallel path produces the finite result. The secondary pass
    // only validates that no divergent residue survives. Implemented
    // after the main loop to keep the hot path untouched.
    //
    // Cost: the divergence-check pass adds ~1x the main cost and
    // calls test_zero_function_sym (fibration_basis_sym internally)
    // on each non-zero bin. Users who don't set the flag pay
    // nothing.
    // Drop the linear_factors cache at each step boundary to bound
    // memory and avoid stale-ctx references (Mma's $LinearFactorsCache
    // is cleared at HyperIntica.wl:4730 for the same reason).
    clear_linear_factors_cache();

    // 2026-04-27 (Lever A): bump the per-thread partial_fractions cache
    // generation. Workers lazily clear their own thread_local PF caches
    // on the next call into partial_fractions. Same lazy-invalidation
    // pattern as the LF cache flush above.
    bump_pf_cache_generation();

    // OpenMP outer-loop parallelism. Each entry is algebraically
    // independent; per-thread PolesBuckets accumulate and merge
    // serially after the parallel region. The `if(input.size() >= 4)`
    // guard keeps small tst0-style jobs serial to avoid thread-startup
    // overhead. When OpenMP is disabled the code still compiles as a
    // serial index-based for-loop via the 1-thread fallback.
#ifdef HF_HAVE_OPENMP
    const int n_threads_outer = omp_get_max_threads();
    const bool do_parallel = input.size() >= 4;
#else
    const int n_threads_outer = 1;
    const bool do_parallel = false;
#endif
    // Phase 0.5 Item U Stage 1: bundle the four per-thread accumulators into
    // a single struct vector.  Indexing is identical to the original four
    // separate vectors: accum_t[tid].{polesZero,polesInf,zw,entry_wall_s}.
    // Stage 2 (HF_USE_GCD=1 dispatch_apply path) will replace the
    // `omp_get_thread_num()` tid source with a GCD-thread-slot resolver
    // at the OMP-body call sites; no change to storage or merge logic.
    std::vector<PerThreadAccum> accum_t(static_cast<size_t>(n_threads_outer));

    // Phase 1 / Task 1.4 (HF_POLES_STREAM): conditionally wire each
    // bucket's flush_callback to stream into a per-step stack-local
    // ShardedFlatMap<16>. R28 C3: skip wiring entirely when the OMP
    // region is serial — the existing `collect_into_flat` walk is
    // already optimal in that case, and any extra cost in serial
    // would be pure overhead. R28 R5: stack-local lifetime so RAII
    // unwinding cleans up on the IntegrationStepFailed throw path.
    const auto _stream_cfg = poles_stream_config();
    const bool stream_active = _stream_cfg.enabled && do_parallel
                                && n_threads_outer > 1;
    hyperflint::ShardedFlatMap<16> sharded_zero;
    hyperflint::ShardedFlatMap<16> sharded_inf;
    if (stream_active) {
        for (auto& a : accum_t) {
            a.polesZero.flush_threshold = _stream_cfg.threshold;
            a.polesZero.flush_user_data = &sharded_zero;
            a.polesZero.flush_callback  = &poles_flush_to_sharded;
            a.polesInf.flush_threshold  = _stream_cfg.threshold;
            a.polesInf.flush_user_data  = &sharded_inf;
            a.polesInf.flush_callback   = &poles_flush_to_sharded;
        }
    }
    // Phase-d15 follow-up: per-thread per-primitive timers. Each worker
    // writes its own slot inside the OMP region; master sums them after the
    // implicit parallel-for barrier and adds to the thread_local globals.
    std::vector<double> ts_per_thread(static_cast<size_t>(n_threads_outer), 0.0);
    std::vector<double> ii_per_thread(static_cast<size_t>(n_threads_outer), 0.0);
    // entry_wall_s lives in accum_t[t].entry_wall_s (Phase 0.5 Item U Stage 1).
    // 2026-04-26 loop_residual_s drill: per-thread sub-timers for the
    // post-integrate_ii pole-expansion blocks (lines ~846-937 below).
    std::vector<double> pze_per_thread(static_cast<size_t>(n_threads_outer), 0.0);
    std::vector<double> pie_per_thread(static_cast<size_t>(n_threads_outer), 0.0);
    std::vector<double> bb_per_thread(static_cast<size_t>(n_threads_outer), 0.0);
    // 2026-05-01 (Branch E pre-flight): per-thread sub-buckets inside
    // pie_per_thread. Sum after barrier into the thread_local globals.
    std::vector<double> pie_subst_per_thread(static_cast<size_t>(n_threads_outer), 0.0);
    std::vector<double> pie_se_per_thread(static_cast<size_t>(n_threads_outer), 0.0);
    std::vector<double> pie_exp_per_thread(static_cast<size_t>(n_threads_outer), 0.0);
    std::vector<double> pie_rvc_per_thread(static_cast<size_t>(n_threads_outer), 0.0);
    // Phase-d15 follow-up: prepare the cross-TU per-thread partial_fractions
    // accumulator (lives in primitive.cpp). Init+reset BEFORE the parallel
    // region, sum AFTER.
    init_partial_fractions_per_thread(n_threads_outer);
    reset_partial_fractions_per_thread();
    // 2026-04-26: bump/push_ibp/antideriv per-call timers (lives in
    // primitive.cpp). Activated for the sanity-matrix follow-up to
    // attribute the "53 % non-pf wall block" identified on tst2.
    init_ii_sub_timers_per_thread(n_threads_outer);
    reset_ii_sub_timers_per_thread();
    // 2026-04-27 (3l3pt profile-deepening): per-thread narrow-vs-wide
    // counters for `Rat::add`/`reduce_inplace` (lives in core/rat.cpp).
    // Hypothesis under test: wide-ctx FLINT GCD dominates step-3 wall
    // on 3l3pt due to ~30+ var contexts; narrow-ctx hoist gates out
    // when used_count grows alongside nvars_wide.
    init_reduce_per_thread(n_threads_outer);
    init_rat_op_calls_per_thread(n_threads_outer);
    reset_reduce_per_thread();
    init_mul_per_thread(n_threads_outer);
    reset_mul_per_thread();
    // 2026-04-27 (3l3pt profile-deepening): per-thread sub-timers
    // inside PolesBucket::bump (file-local in this translation unit).
    // Decomposes bucket_bump_s (95-99% of loop_residual_s on heavy
    // 3l3pt steps) into 5 sub-costs.  Localizes which line(s)
    // dominate before any code change.
    init_bucket_sub_timers_per_thread(n_threads_outer);
    reset_bucket_sub_timers_per_thread();
    // Phase-d15 deeper drill: same for linear_factors (lives in
    // partial_fractions.cpp).
    init_linear_factors_per_thread(n_threads_outer);
    reset_linear_factors_per_thread();
    // Phase-d15 deeper drill (round 3): FLINT-wall + hit/miss counters
    // (lives in linear_factors.cpp).
    init_lf_flint_factor_per_thread(n_threads_outer);
    reset_lf_flint_factor_per_thread();
    init_lf_cache_hits_per_thread(n_threads_outer);
    reset_lf_cache_hits_per_thread();
    init_lf_cache_misses_per_thread(n_threads_outer);
    reset_lf_cache_misses_per_thread();
    // Phase-d15 deeper drill (round 4): degree-bucketed FLINT-wall + miss
    // count storage (lives in linear_factors.cpp).
    init_lf_flint_deg1_per_thread(n_threads_outer);
    reset_lf_flint_deg1_per_thread();
    init_lf_flint_deg2_per_thread(n_threads_outer);
    reset_lf_flint_deg2_per_thread();
    init_lf_miss_deg1_per_thread(n_threads_outer);
    reset_lf_miss_deg1_per_thread();
    init_lf_miss_deg2_per_thread(n_threads_outer);
    reset_lf_miss_deg2_per_thread();
    // Phase-d15 deeper drill (round 5): deg-3+ output classifier
    // (lives in linear_factors.cpp).
    init_lf_d3p_all_linear_count_per_thread(n_threads_outer);
    reset_lf_d3p_all_linear_count_per_thread();
    init_lf_d3p_all_linear_s_per_thread(n_threads_outer);
    reset_lf_d3p_all_linear_s_per_thread();
    init_lf_d3p_has_nonlinear_count_per_thread(n_threads_outer);
    reset_lf_d3p_has_nonlinear_count_per_thread();
    init_lf_d3p_has_nonlinear_s_per_thread(n_threads_outer);
    reset_lf_d3p_has_nonlinear_s_per_thread();
    // Phase-d15 deeper drill (round 6): squarefree / repeated split for
    // deg-3+ all-linear (lives in linear_factors.cpp).
    init_lf_d3p_squarefree_count_per_thread(n_threads_outer);
    reset_lf_d3p_squarefree_count_per_thread();
    init_lf_d3p_squarefree_s_per_thread(n_threads_outer);
    reset_lf_d3p_squarefree_s_per_thread();
    init_lf_d3p_repeated_count_per_thread(n_threads_outer);
    reset_lf_d3p_repeated_count_per_thread();
    init_lf_d3p_repeated_s_per_thread(n_threads_outer);
    reset_lf_d3p_repeated_s_per_thread();
    // Phase-d15 deeper drill (round 7): squarefree-first path counters
    // (lives in linear_factors.cpp).
    init_lf_sqf_total_s_per_thread(n_threads_outer);
    reset_lf_sqf_total_s_per_thread();
    init_lf_sqf_decomp_s_per_thread(n_threads_outer);
    reset_lf_sqf_decomp_s_per_thread();
    init_lf_sqf_inner_factor_s_per_thread(n_threads_outer);
    reset_lf_sqf_inner_factor_s_per_thread();
    init_lf_sqf_calls_per_thread(n_threads_outer);
    reset_lf_sqf_calls_per_thread();
    init_lf_sqf_inner_factor_calls_per_thread(n_threads_outer);
    reset_lf_sqf_inner_factor_calls_per_thread();
    init_lf_sqf_bailouts_per_thread(n_threads_outer);
    reset_lf_sqf_bailouts_per_thread();
    // 2026-04-26 lock-contention proxy: held-time on g_linear_factors_mu.
    init_lf_lock_held_per_thread(n_threads_outer);
    reset_lf_lock_held_per_thread();
    // iter-37 lock-acquire wait probe: queueing time before lock_guard
    // ctor returns. Default-OFF; instantiated unconditionally so the
    // accumulator vector is always sized correctly (the env-gate only
    // controls whether ctor/record write into it).
    init_lf_lock_wait_per_thread(n_threads_outer);
    reset_lf_lock_wait_per_thread();
    // 2026-04-26 cache_key_build direct measurement.
    init_lf_cache_key_build_per_thread(n_threads_outer);
    reset_lf_cache_key_build_per_thread();
    // 2026-04-29 (Probe 2): post-FLINT extraction sub-timers.
    init_lf_post_transplant_per_thread(n_threads_outer);
    reset_lf_post_transplant_per_thread();
    init_lf_post_rat_ctor_per_thread(n_threads_outer);
    reset_lf_post_rat_ctor_per_thread();
    init_lf_post_constant_to_string_per_thread(n_threads_outer);
    reset_lf_post_constant_to_string_per_thread();
    init_lf_post_clone_from_raw_per_thread(n_threads_outer);
    reset_lf_post_clone_from_raw_per_thread();
    (void)do_parallel;  // used by pragma when OpenMP is on

    // Iter-58 C0c.1 Increment β B1 fix (folded; reviewer agentId
    // a16b4f479d33c0114 verdict CONCERNS-BINDING). The OUTER parallel-for
    // below calls transform_shuffle (line ~1565) and integrate_ii (line
    // ~1599), both of which propagate `zw_tab` into linear_factors site 6
    // (transform.cpp:829-834 and partial_fractions.cpp:461). Under
    // runtime::scalar_rep_enabled() the iter-52a Increment α lambda body at
    // linear_factors.cpp:904 invokes `runtime::roundtrip_rat_through_scs`
    // → ZWTable::intern from N OMP workers concurrently on the SAME shared
    // driver-entry ZWTable. That is a data race (intern_regular_calls_++
    // non-atomic; by_hash_.find/emplace UB on unordered_map; entries_
    // .push_back UB on vector). Iter-52b applied Protocol A only at the
    // INNER site-7 parallel-for (lines ~2378-2483); the outer was missed.
    // Iter-58 mirrors that pattern here:
    //   STEP 1: allocate per-thread ZWTable instances under
    //           runtime::scalar_rep_enabled() before the pragma.
    //   STEP 2: lambda body / direct call sites (transform_shuffle,
    //           integrate_ii) use `accum_t[tid].zw` instead of
    //           the shared `zw_tab` (see local `zw_for_this_thread` ref
    //           inside the body).
    //   STEP 3: post-barrier master walks the per-thread tables and
    //           merges them deterministically into the function-parameter
    //           `zw_tab` via the combined-then-master 2-stage pattern
    //           (single-pass union sort over the FULL union per design v2
    //           §3.6a STEPS 1-3 / iter-50 MEMO §6 Q3 / iter-51 §6.5).
    // See the HyperFLINT development notes (internal) for the full
    // race trace (iter-57 reviewer B1 confirmation). Without scalar_rep_enabled() the vector stays
    // empty and zw_for_this_thread aliases the input zw_tab (which is
    // typically null at default-OFF), so the iter-52a "thread now, kill
    // later" parameter-add path is preserved bit-identically.
    // Phase 0.5 Item U Stage 1: per-thread ZWTable moved to accum_t[t].zw.
    if (zw_tab && runtime::scalar_rep_enabled()) {
        for (int t = 0; t < n_threads_outer; ++t) {
            accum_t[static_cast<size_t>(t)].zw =
                std::make_shared<ZWTable>(ctx);
        }
    }

    // Schedule selection: dynamic is preferred for load balancing on
    // heterogeneous iteration costs (typical for Symanzik integrands —
    // some entries are linear-in-var, others involve heavy partial-
    // fractions), but it requires `___kmpc_dispatch_deinit` from libomp.
    // Wolfram's libomp doesn't export that symbol, so the LibraryLink
    // build (`hyperflint_nomp`, no `HF_OMP_FULL_SCHEDULES`) falls back
    // to `static, 1`.  CLI builds against brew's libomp and gets the
    // dynamic path.  Phase 9b post-mortem on tst2 showed ~25 % wall-time
    // regression from the static-everywhere approach (323 s vs 258 s);
    // the conditional restores parity for the CLI while keeping the
    // LibraryLink load constraint.
    // 2026-04-26 (Lever-C scout): wall-clock around the parallel-for.
    // Combined with the per-thread accum_t[t].entry_wall_s accumulators
    // already collected below, this lets us derive OMP utilization
    // and barrier-idle in post-processing.
    // Task 0-5: clear the cross-thread aggregate so records from a prior
    // integration_step call do not contaminate this step's output.
    hyperflint::reset_node_aggregate();
    const auto _omp_parallel_t0 = std::chrono::steady_clock::now();

    // Phase 1 Task 1.E (post-FOLD-7): hoist the entry-body into a lambda
    // so it can be invoked from BOTH the existing OMP parallel-for path
    // (default; bit-stable with hyperflint_v1 baseline at OMP=13
    // HF_USE_GCD=0) AND the libdispatch dispatch_parallel_for path
    // (HF_USE_GCD=1, Apple only).  The 4 outer-loop `continue;` sites
    // (pre-try tolerance fast-skip; post-try is_zero skip; the two
    // catch-handlers that swallow + advance to next entry) become
    // `return;` from the lambda.  The 9 nested-loop `continue;` sites
    // (1 in the mon loop, 4 in the prim loop, 4 in the i loop) stay
    // untouched.
    auto process_entry_body = [&](long entry_i, int tid) -> void {
        // Phase 9b-1: promote every OMP worker to QOS_CLASS_USER_INITIATED
        // exactly once.  Thread_local guard inside the helper keeps the
        // per-iteration overhead to a single compare after the first call.
        hf_promote_qos_once();
        // HF_PROGRESS heartbeat (relocated 2026-06-05): this lambda is
        // the SINGLE entry-execution path shared by the GCD-dispatch
        // (default-ON since the Phase-1 verdict), OMP, and serial
        // branches -- the original placement in the OMP loop was dead
        // code in production. Reports every ~5% of entries (min 1).
        {
            static const bool hf_progress = [] {
                const char* e = std::getenv("HF_PROGRESS");
                return e && e[0] == '1';
            }();
            if (hf_progress) {
                static std::atomic<long> done{0};
                const long n = static_cast<long>(input.size());
                const long d = done.fetch_add(1,
                    std::memory_order_relaxed) + 1;
                const long stride = n > 20 ? n / 20 : 1;
                if (d % stride == 0 || d == n) {
                    std::fprintf(stderr,
                        "[hf-progress]   entry %ld/%ld (var=%s)\n",
                        d, n, ctx.vars()[var_idx].c_str());
                    std::fflush(stderr);
                }
                if (d == n) done.store(0, std::memory_order_relaxed);
            }
        }
        // R24 rev 2 / chain 17 — OMP-iteration-body try/catch wrapper.
        // Catches `std::runtime_error` escape (e.g. `Rat::div: division
        // by zero` from a parse_or_none safe-failure zero substituted
        // into a denominator).  Default-off path: re-throw → OMP
        // terminate (unchanged behavior).  Tolerance-on path: set the
        // global narrow-ctx flag and continue; host code throws
        // NarrowCtxTooNarrow after the OMP barrier.
        //
        // Fast-fail under tolerance: once any worker has set the flag
        // (this iteration's `parse_or_none` failure or any prior
        // iteration's), subsequent iterations short-circuit to avoid
        // accumulating per-thread state that the host will discard
        // when the post-OMP throw fires.  Falsifier matrix evidence:
        // without this, parity-1 N1_T1 peaked at 14.2 GB; the wide-ctx
        // baseline peaks at 2.5 GB.  Default-off path: relaxed-load
        // no-op (tolerance_enabled() returns false → check elided).
        if (tolerance_enabled() && narrow_ctx_was_too_narrow()) return;
        try {
        // Phase 0.5 Item U Stage 1: per-thread accumulators via accum_t[tid].
        // Stage 2 (HF_USE_GCD=1) will replace `tid` with a GCD slot index
        // at this single access point.
        PerThreadAccum& this_accum = accum_t[static_cast<size_t>(tid)];
        PolesBucket& polesZero = this_accum.polesZero;
        PolesBucket& polesInf  = this_accum.polesInf;
        // Iter-58 C0c.1 B1 fix STEP 2: pick the worker's per-thread
        // ZWTable (allocated above under scalar_rep_enabled()) so
        // transform_shuffle and integrate_ii do not race on the shared
        // driver-entry zw_tab. When scalar_rep is OFF or the driver
        // didn't allocate a master zw_tab, accum_t[tid].zw is null
        // and we alias the input shared_ptr directly (typically null
        // at default-OFF, preserving the iter-52a parameter-add
        // bit-identical path).
        const std::shared_ptr<ZWTable>& zw_for_this_thread =
            (zw_tab && runtime::scalar_rep_enabled())
                ? this_accum.zw
                : zw_tab;
        const auto& entry = input[static_cast<size_t>(entry_i)];
        if (entry.coef.is_zero()) return;

        // Task 0-5: RAII scope for the per-node RSS sampler.
        // depth   = var_idx (integration-variable index, proxy for nesting level).
        // letter  = entry_i (which shuffle entry this OMP iteration handles).
        // NodeScope destructor fires unconditionally (even on throw) and calls
        // exit_node; the snapshot into the cross-thread aggregate happens after
        // the entry body below (after the catch wrappers close).
        NodeScope _node_scope(static_cast<int>(var_idx),
                              static_cast<int>(entry_i));

        // Phase-d15: time the full entry body for the loop_residual_s timer.
        const bool _tg = step_trace_enabled();
        const auto _entry_t0 = _tg ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};

        // transform_shuffle depends only on the `shuffle` spine, not
        // on the coefficient. Compute once per entry and reuse across
        // the monomial loop below.
        const auto _ts_t0 = _tg ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        TransformResultSym transformed =
            transform_shuffle(ctx, entry.shuffle, var_idx, table,
                               zw_for_this_thread,
                               introduce_algebraic_letters);
        if (_tg) {
            ts_per_thread[static_cast<size_t>(tid)] +=
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - _ts_t0).count();
        }

        // Iterate each monomial of entry.coef independently. Each has a
        // Rat prefactor (which scales the Wordlist and participates in
        // the polynomial integration) and a pure-symbolic tail (Pi, I,
        // Log[n], delta[var] powers) that rides through as an outer
        // SymCoef multiplier on the output contribs. For tst0-style
        // inputs (Rat coef only) this reduces to the original flow with
        // sym_outer == 1. For tst1-style inputs (intermediate-step
        // I*Pi*delta[prev_var] residue) the Rat prefactor still scales
        // the wordlist correctly, and the residue rides through cleanly.
        for (const auto& mon : entry.coef.terms()) {
            const Rat& prefactor = mon.prefactor;
            if (prefactor.is_zero()) continue;

            // Pure-symbolic factor: same monomial with prefactor = 1.
            SymMonomial sym_only = mon;
            sym_only.prefactor = Rat::one_of(ctx);
            const SymCoef sym_outer =
                SymCoef::from_monomials(ctx, {sym_only});

            for (const auto& sub : transformed) {
                Wordlist scaled = scalar_mul_wordlist(sub.shuffle, prefactor);
                Wordlist primitive;
                const auto _ii_t0 = _tg ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
                try {
                    primitive = integrate_ii(ctx, scaled, var_idx,
                                              zw_for_this_thread,
                                              introduce_algebraic_letters);
                } catch (const IntegrateIIFailed& e) {
                    if (_tg) {
                        ii_per_thread[static_cast<size_t>(tid)] +=
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - _ii_t0).count();
                    }
                    throw IntegrationStepFailed(e.what());
                }
                if (_tg) {
                    ii_per_thread[static_cast<size_t>(tid)] +=
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - _ii_t0).count();
                }
                const RegulatorSym& reginfs = sub.regulator;

                for (const auto& prim : primitive.terms) {
                    const Rat&  p_coef = prim.coef;
                    const Word& p_word = prim.word;

                    // ---- Zero expansion ----
                    {
                        const auto _pze_t0 = _tg ? std::chrono::steady_clock::now()
                                                 : std::chrono::steady_clock::time_point{};
                        long minOrder = -p_coef.pole_degree(var_idx);
                        if (minOrder >= 0) {
                            SeriesTable zeroExp = expand_zero_word_in_ctx(ctx, p_word, minOrder);
                            if (!zeroExp.empty() && !zeroExp[0].empty()) {
                                Rat sum_term = log_zero_row_as_poly(
                                    ctx, zeroExp[0], var_idx);
                                Rat se = series_expansion(ctx, p_coef, var_idx, 0);
                                Rat prod = sum_term * se;
                                Rat coef_z = -rat_var0_coefficient(ctx, prod, var_idx);
                                if (!coef_z.is_zero()) {
                                    const auto _bb_t0 = _tg ? std::chrono::steady_clock::now()
                                                            : std::chrono::steady_clock::time_point{};
                                    for (const auto& vv : reginfs) {
                                        RegKey L = normalize_shuffle_key(vv.key);
                                        polesZero.bump(L, sym_outer.mul(
                                            vv.coef.mul_rat(coef_z)));
                                    }
                                    if (_tg) {
                                        bb_per_thread[static_cast<size_t>(tid)] +=
                                            std::chrono::duration<double>(
                                                std::chrono::steady_clock::now() - _bb_t0).count();
                                    }
                                }
                            }
                        }
                        if (_tg) {
                            pze_per_thread[static_cast<size_t>(tid)] +=
                                std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - _pze_t0).count();
                        }
                    }

                    // ---- Infinity expansion ----
                    const auto _pie_t0 = _tg ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
                    const auto _pie_subst_t0 = _tg ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
                    Rat poly_sub =
                        substitute_var_reciprocal(ctx, p_coef, var_idx);
                    if (_tg) {
                        pie_subst_per_thread[static_cast<size_t>(tid)] +=
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - _pie_subst_t0).count();
                    }
                    long minOrder = -poly_sub.pole_degree(var_idx);
                    if (minOrder < 0) {
                        if (_tg) {
                            pie_per_thread[static_cast<size_t>(tid)] +=
                                std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - _pie_t0).count();
                        }
                        continue;
                    }

                    if (minOrder == 0) {
                        // Trivial-convergence branch.
                        if (word_is_all_minus_one(ctx, p_word)) {
                            if (_tg) {
                                pie_per_thread[static_cast<size_t>(tid)] +=
                                    std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - _pie_t0).count();
                            }
                            continue;
                        }
                        Wordlist temp = regzero_word_in_ctx(ctx, p_word);
                        if (temp.terms.empty()) {
                            if (_tg) {
                                pie_per_thread[static_cast<size_t>(tid)] +=
                                    std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - _pie_t0).count();
                            }
                            continue;
                        }
                        // coef = lim_{var -> 0} poly_sub = lim_{var -> infinity} p_coef
                        const auto _pie_rvc_t0 = _tg ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
                        Rat coef_i = rat_var0_coefficient(ctx, poly_sub, var_idx);
                        if (_tg) {
                            pie_rvc_per_thread[static_cast<size_t>(tid)] +=
                                std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - _pie_rvc_t0).count();
                        }
                        if (coef_i.is_zero()) {
                            if (_tg) {
                                pie_per_thread[static_cast<size_t>(tid)] +=
                                    std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - _pie_t0).count();
                            }
                            continue;
                        }
                        const auto _bb_t0 = _tg ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                        for (const auto& q : reginfs) {
                            for (const auto& t : temp.terms) {
                                RegKey t_key{t.word};
                                RegKey L = combine_shuffle_keys(q.key, t_key);
                                Rat reginfs_coef = t.coef;
                                SymCoef contrib = sym_outer.mul(
                                    q.coef.mul_rat(coef_i * reginfs_coef));
                                polesInf.bump(L, contrib);
                            }
                        }
                        if (_tg) {
                            bb_per_thread[static_cast<size_t>(tid)] +=
                                std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - _bb_t0).count();
                        }
                    } else {
                        // minOrder > 0: series-expansion branch.
                        const auto _pie_se_t0 = _tg ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
                        Rat poly_se = series_expansion(
                            ctx, poly_sub, var_idx, minOrder);
                        if (_tg) {
                            pie_se_per_thread[static_cast<size_t>(tid)] +=
                                std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - _pie_se_t0).count();
                        }
                        Rat minus_one{Poly::from_int(ctx, -1)};
                        bool allone = true;
                        const long n = static_cast<long>(p_word.size());
                        for (long i = n; i >= 0; --i) {
                            if (i < n) {
                                if (!p_word[static_cast<size_t>(i)].equal(minus_one))
                                    allone = false;
                                if (allone) continue;
                            }
                            Word take_i;
                            take_i.letters.assign(p_word.letters.begin(),
                                                  p_word.letters.begin() + i);
                            Word drop_i;
                            drop_i.letters.assign(p_word.letters.begin() + i,
                                                  p_word.letters.end());
                            const auto _pie_exp_t0 = _tg ? std::chrono::steady_clock::now()
                                                         : std::chrono::steady_clock::time_point{};
                            SeriesTable part = expand_inf_word_in_ctx(ctx, take_i, minOrder);
                            if (_tg) {
                                pie_exp_per_thread[static_cast<size_t>(tid)] +=
                                    std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - _pie_exp_t0).count();
                            }
                            Wordlist reg = regzero_word_in_ctx(ctx, drop_i);
                            if (reg.terms.empty() || part.empty()) continue;
                            if (part[0].empty()) continue;   // log^0 row only
                            Rat sum_term =
                                log_zero_row_as_poly(ctx, part[0], var_idx);
                            Rat prod = sum_term * poly_se;
                            const auto _pie_rvc2_t0 = _tg ? std::chrono::steady_clock::now()
                                                          : std::chrono::steady_clock::time_point{};
                            Rat coef_i =
                                rat_var0_coefficient(ctx, prod, var_idx);
                            if (_tg) {
                                pie_rvc_per_thread[static_cast<size_t>(tid)] +=
                                    std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - _pie_rvc2_t0).count();
                            }
                            if (coef_i.is_zero()) continue;
                            const auto _bb_t0 = _tg ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
                            for (const auto& q : reginfs) {
                                for (const auto& t : reg.terms) {
                                    RegKey t_key{t.word};
                                    RegKey L = combine_shuffle_keys(q.key, t_key);
                                    Rat reginfs_coef = t.coef;
                                    SymCoef contrib = sym_outer.mul(
                                        q.coef.mul_rat(coef_i * reginfs_coef));
                                    polesInf.bump(L, contrib);
                                }
                            }
                            if (_tg) {
                                bb_per_thread[static_cast<size_t>(tid)] +=
                                    std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - _bb_t0).count();
                            }
                        }
                    }
                    if (_tg) {
                        pie_per_thread[static_cast<size_t>(tid)] +=
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - _pie_t0).count();
                    }
                }
            }
        }
        // Phase-d15: close out the entry-body wall measurement.
        if (_tg) {
            this_accum.entry_wall_s +=
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - _entry_t0).count();
        }
        } catch (const NarrowCtxTooNarrow&) {
            // Already correctly attributed (defensive — see closure
            // body's matching catch for rationale).
            g_narrow_ctx_too_narrow.store(true,
                                            std::memory_order_relaxed);
            // Task 0-5 fix: snapshot pending records before continuing so
            // they are attributed to the current step, not swept into the
            // next successful iteration's aggregate.
            hyperflint::IntegrationNodeRssSampler::instance()
                .snapshot_thread_records();
            return;
        } catch (const IntegrationStepFailed&) {
            // R26 C1 — legitimate divergence from integrate_ii (re-thrown
            // by the inner catch at line ~1419-1424 as
            // IntegrationStepFailed).  Under tolerance mode the outer
            // generic catch would silently misattribute this as a
            // narrow-ctx fall-forward, leading to a wasteful Mma-side
            // wide-ctx retry that crashes on the same divergence.
            // Rethrow so the handler emits {"failed": true} cleanly.
            throw;
        } catch (const HyperFLINTDivergentIntegral&) {
            // R26 C1 — divergence detection from check_divergences
            // mode.  Same rationale as IntegrationStepFailed.
            throw;
        } catch (const std::runtime_error&) {
            if (tolerance_enabled()) {
                g_narrow_ctx_too_narrow.store(true,
                                                std::memory_order_relaxed);
                // Task 0-5 fix: snapshot pending records before continuing
                // so they are attributed to the current step, not swept
                // into the next successful iteration's aggregate.
                hyperflint::IntegrationNodeRssSampler::instance()
                    .snapshot_thread_records();
                return;
            }
            throw;
        }
        // Task 0-5: snapshot this thread's completed node records into the
        // cross-thread aggregate.  _node_scope has already been destroyed
        // (try-block scope exited above), so exit_node was called and the
        // record for this entry is fully populated before this snapshot.
        // snapshot_thread_records() is also called in each catch-then-return
        // handler above, so every path (normal completion or caught exception)
        // drains pending records before this lambda invocation ends, correctly
        // attributing them to the current step.
        hyperflint::IntegrationNodeRssSampler::instance()
            .snapshot_thread_records();
    };  // end process_entry_body lambda

    // HF FF Phase 6 REVISED §6.D iter-22b (REQ-21.4 BINDING placement
    // constraint, ratified by TWO CONCURRING BINDING reviewers a7c3f8b2…
    // + b3f24a9c… at iter-21 close commit f45332fa5).
    //
    // Read $HF_SECTION_6D_DFS_THREAD_CAP exactly ONCE per dispatcher
    // invocation, here, BEFORE the GCD-vs-OMP branch. Forbidden
    // placements per design.md §5 + REQ-21.4: (a) inside
    // `process_entry_body` lambda (per-entry repeat); (b) inside
    // `dispatch_parallel_for` callback (per-slot repeat); (c) inside
    // the `#pragma omp parallel` region (per-thread race + repeat).
    //
    // Default-OFF (env var unset/empty/<=0): dfs_thread_cap == 0 →
    // dispatch_cap_active(0, max) returns max = current 13-thread
    // behaviour. Opt-in Strategy (c) hybrid (env var set to N ∈ {1..13}):
    // dispatch_cap_active(N, max) returns min(N, max), which is passed
    // to the GCD slot pool (max_slots) and to OMP via num_threads(...).
    //
    // See design memo:
    //   notes/hf_finite_field_program/phase6_combined/
    //     section_6d_engineering/design.md §3.4 ship target + §4 REQ-C
    //     default-OFF wall ≤ +2 % gate + REQ-21.4 BINDING placement.
    const int dfs_thread_cap =
        ::hyperflint::section_6d::parse_dfs_cap_env();
    const int max_active_slots =
        ::hyperflint::section_6d::dispatch_cap_active(
            dfs_thread_cap, static_cast<int>(accum_t.size()));

    // Phase 1 Task 1.E (post-FOLD-7): runtime gate between GCD dispatch
    // and OMP parallel-for.  Default (HF_USE_GCD unset or "0"): OMP path,
    // bit-stable with hyperflint_v1 baseline.  HF_USE_GCD=1 on Apple:
    // libdispatch dispatch_apply on the global QOS_CLASS_USER_INITIATED
    // queue, with stable per-thread slots via the semaphore-bounded slot
    // resolver in gcd_dispatch.cpp.
    if (::hyperflint::integrator::gcd_dispatch_enabled() &&
        ::hyperflint::integrator::gcd_dispatch_available()) {
        // §6.D iter-22b: GCD slot-pool cap. max_active_slots ==
        // accum_t.size() at default-OFF (cap == 0); when cap > 0, the
        // slot pool degenerates gracefully (slot 0..N-1 only).
        ::hyperflint::integrator::dispatch_parallel_for(
            0, static_cast<size_t>(input.size()), max_active_slots,
            [&](size_t entry_i, int slot) {
                ::hyperflint::runtime::hf_set_thread_num(slot);
                process_entry_body(static_cast<long>(entry_i), slot);
                ::hyperflint::runtime::hf_clear_thread_num();
            });
    } else {
        // §6.D iter-22b: OMP thread-count cap via num_threads(...) clause.
        // At default-OFF (dfs_thread_cap == 0), max_active_slots ==
        // accum_t.size() == omp_get_max_threads(), so the clause is a
        // no-op vs the iter-21 baseline. When cap > 0, OMP caps the
        // worker pool to min(cap, max_threads) for the duration of this
        // parallel-for; OMP restores the prior thread count on exit.
        // PINNED 2026-05-18 (v2 iter-23) — outer integration-step OMP schedule;
        //   fixture/gate : Smirnov tst{0,1,2,3} hot CLI binary
        //                  (CMakeLists.txt:154-163 sets HF_OMP_FULL_SCHEDULES=1
        //                   unconditionally for the `hyperflint` CLI target;
        //                   v1 baseline binary already on dynamic,1)
        //   measurement  : on tst3 the per-thread task durations span
        //                  microseconds-to-seconds within a single
        //                  parallel-for region; v1 paired baselines showed
        //                  schedule(dynamic, 1) ≥ schedule(static, 1) on
        //                  every Smirnov fixture (Track 1 Phase 1 declined
        //                  to re-sweep; locked by S.4 sign-off)
        //   falsifier    : (a) tst-fixture paired n_reps=3 mean-of-means
        //                  shows static beats dynamic > 5% wall AT OMP=13
        //                  → re-open Track 1; OR
        //                  (b) Phase 2 cross-OMP harness sweeps small
        //                  fixtures (d1 regime where wall-weighted
        //                  imbalance hits 78.9% per iter-22 recompute
        //                  HyperFLINT development notes: iter22 d1 gate1b recompute;
        //                  129x ratio vs tst3 0.61%) and surfaces a
        //                  schedule-policy lever this OMP=13 ceiling missed.
#ifdef HF_OMP_FULL_SCHEDULES
        #pragma omp parallel for schedule(dynamic, 1) if(do_parallel) num_threads(max_active_slots)
#else
        #pragma omp parallel for schedule(static, 1) if(do_parallel) num_threads(max_active_slots)
#endif
        for (long entry_i = 0; entry_i < static_cast<long>(input.size()); ++entry_i) {
            // HF_PROGRESS heartbeat (2026-06-04): entries are the big
            // work units; report every ~5% (min 1). Atomic counter,
            // default-OFF, one branch when off.
            {
                static const bool hf_progress = [] {
                    const char* e = std::getenv("HF_PROGRESS");
                    return e && e[0] == '1';
                }();
                if (hf_progress) {
                    static std::atomic<long> done{0};
                    const long n = static_cast<long>(input.size());
                    const long d = done.fetch_add(1,
                        std::memory_order_relaxed) + 1;
                    const long stride = n > 20 ? n / 20 : 1;
                    if (d % stride == 0 || d == n) {
                        std::fprintf(stderr,
                            "[hf-progress]   entry %ld/%ld (var=%s)\n",
                            d, n, ctx.vars()[var_idx].c_str());
                        std::fflush(stderr);
                    }
                    if (d == n) done.store(0, std::memory_order_relaxed);
                }
            }
            // Track 4.2 iter-26 PoC: per-iteration scope guard for the
            // OMP parallel-for region.  Sets g_active_scope_mask bit on
            // entry, RAII pops on iteration-body exit.  Each OMP worker
            // thread reaches this on every iteration it owns, so the
            // scope is active during process_entry_body() — which calls
            // reduce_inplace() transitively, where the
            // {gcd_cofactors_narrow, gcd_cofactors_wide} timer predicates
            // require this bit set.
            HF_SCOPE_ENTER(omp_parallel_for_integration_step);
            // Phase 1 Task 1.E: hf_get_thread_num() returns
            // omp_get_thread_num() inside an OMP parallel region.
            process_entry_body(entry_i,
                ::hyperflint::runtime::hf_get_thread_num());
        }
    }
    // 2026-04-26 (Lever-C scout): close the parallel-region wall.
    g_omp_parallel_wall_s += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - _omp_parallel_t0).count();

    // Task 0-5: drain and emit all per-node RSS records collected across all
    // OMP threads.  drain_node_aggregate() is called once here, after the
    // implicit parallel-for barrier, so all thread-local snapshots are visible.
    // The emit mirrors the HF_STEP_TRACE JSONL format: one JSON object per
    // line on stderr, each tagged with record_type="integ_node".
    {
        auto& _node_sampler = hyperflint::IntegrationNodeRssSampler::instance();
        if (_node_sampler.enabled()) {
            const auto _node_records = hyperflint::drain_node_aggregate();
            for (const auto& rec : _node_records) {
                // §6.D iter-13 (REQ-A + REQ-C): emit the 4-column extension
                // alongside the original Phase-0 columns so downstream
                // probes can read per-entry attribution + outer-step
                // context.  Field names match the column names in
                // NodeRssRecord verbatim; ordering follows struct layout.
                std::cerr << "{\"record_type\":\"integ_node\","
                          << "\"node_id\":"          << rec.node_id        << ","
                          << "\"node_depth\":"        << rec.depth          << ","
                          << "\"node_letter_id\":"    << rec.letter_id      << ","
                          << "\"node_t_wall_s\":"     << rec.t_wall_s       << ","
                          << "\"node_rss_current_kib\":" << rec.rss_current_kib << ","
                          << "\"node_rss_peak_kib_delta\":" << rec.rss_peak_kib_delta << ","
                          << "\"entry_bytes_thread_local\":"  << rec.entry_bytes_thread_local  << ","
                          << "\"entry_bytes_aggregate_omp\":" << rec.entry_bytes_aggregate_omp << ","
                          << "\"lf_cache_bytes\":"            << rec.lf_cache_bytes            << ","
                          << "\"reg_sym_bytes\":"             << rec.reg_sym_bytes
                          << "}\n";
            }
            std::cerr.flush();
        }
    }

    // Iter-58 C0c.1 Increment β B1 fix STEP 3: post-barrier master merge of
    // per-thread ZWTables into the function-parameter `zw_tab` (the
    // persistent driver-entry ZWTable, when scalar_rep_enabled()). Mirrors
    // the site-7 inner-merge pattern at lines ~2476-2483. Single-pass
    // union sort over the FULL union per design v2 §3.6a STEPS 1-3 / iter-50
    // MEMO §6 Q3 / iter-51 §6.5: build a `combined` ZWTable by merging each
    // per-thread table into it (canonical-content-keyed iteration inside
    // merge_into is partition-order-dependent on this stage), then merge
    // `combined` into the master `zw_tab` (canonical-content-keyed
    // iteration over the full UNION is partition-order-INDEPENDENT). The
    // two-stage approach makes master's handle assignment depend only on
    // the set of unique contents — identical under OMP=1 and OMP=N.
    //
    // NOTE: the returned remap tables are discarded; the SymCoef bumps
    // collected in accum_t[t].polesZero/polesInf carry round-tripped Rats
    // whose structure does not embed handle values (per A2 unstated-invariant
    // / iter-54 audit AUDIT_A2_U1_U3.md), so no caller-side handle remap
    // on the per-thread polesZero/polesInf is needed. This matches the
    // reasoning at site 7 (lines 2470-2475).
    if (zw_tab && runtime::scalar_rep_enabled()) {
        ZWTable combined(ctx);
        for (auto& a : accum_t) {
            if (a.zw) (void)combined.merge_into(*a.zw);
        }
        (void)zw_tab->merge_into(combined);
    }

    // R24 rev 2 / chain 17 — post-OMP flag check.  See the
    // matching block in `close_positive_letters_in_regulator_sym`
    // above for rationale.  Throw `NarrowCtxTooNarrow` from host
    // code so the OMP region's barrier guarantees worker writes
    // are visible.  Default-off path: flag is never set, this is
    // a single relaxed-load no-op.
    if (narrow_ctx_was_too_narrow()) {
        throw NarrowCtxTooNarrow{"integration_step"};
    }

    // Phase-d15: sum per-thread per-primitive timers and write into the
    // master-thread thread_local globals. Runs after the implicit
    // parallel-for barrier (see TSan note below). loop_residual_s is the
    // entry-body wall NOT spent in transform_shuffle or integrate_ii —
    // accounts for expand_zero_word_in_ctx, series_expansion,
    // log_zero_row_as_poly, polesZero/polesInf bump, etc.
    {
        double ts_sum = 0.0, ii_sum = 0.0, entry_sum = 0.0;
        double pze_sum = 0.0, pie_sum = 0.0, bb_sum = 0.0;
        double pie_subst_sum = 0.0, pie_se_sum = 0.0;
        double pie_exp_sum = 0.0, pie_rvc_sum = 0.0;
        // Reviewer #13 Hole 3: track accum_t[t].entry_wall_s spread for the
        // load-imbalance diagnosis. Initialize max/min from the first
        // thread that actually did work (entry > 0); ignore zero slots
        // since serial fixtures (do_parallel == false) only populate
        // tid=0.
        double entry_max = 0.0, entry_min = -1.0;
        for (int t = 0; t < n_threads_outer; ++t) {
            const double et = accum_t[static_cast<size_t>(t)].entry_wall_s;
            ts_sum    += ts_per_thread[static_cast<size_t>(t)];
            ii_sum    += ii_per_thread[static_cast<size_t>(t)];
            entry_sum += et;
            pze_sum   += pze_per_thread[static_cast<size_t>(t)];
            pie_sum   += pie_per_thread[static_cast<size_t>(t)];
            bb_sum    += bb_per_thread[static_cast<size_t>(t)];
            pie_subst_sum += pie_subst_per_thread[static_cast<size_t>(t)];
            pie_se_sum    += pie_se_per_thread[static_cast<size_t>(t)];
            pie_exp_sum   += pie_exp_per_thread[static_cast<size_t>(t)];
            pie_rvc_sum   += pie_rvc_per_thread[static_cast<size_t>(t)];
            if (et > 0.0) {
                if (et > entry_max) entry_max = et;
                if (entry_min < 0.0 || et < entry_min) entry_min = et;
            }
        }
        if (entry_min < 0.0) entry_min = 0.0;  // no thread did work
        g_entry_max_per_thread_s += entry_max;
        g_entry_min_per_thread_s += entry_min;
        g_transform_shuffle_s += ts_sum;
        g_integrate_ii_s      += ii_sum;
        const double residual = entry_sum - ts_sum - ii_sum;
        g_loop_residual_s     += (residual > 0.0 ? residual : 0.0);
        g_pole_zero_expand_s  += pze_sum;
        g_pole_inf_expand_s   += pie_sum;
        g_bucket_bump_s       += bb_sum;
        g_pie_substitute_var_reciprocal_s += pie_subst_sum;
        g_pie_series_expansion_s          += pie_se_sum;
        g_pie_expand_inf_word_in_ctx_s    += pie_exp_sum;
        g_pie_rat_var0_coef_s             += pie_rvc_sum;
        // Phase-d15 follow-up: accumulate the cross-TU partial_fractions
        // total (subset of integrate_ii_s). primitive.cpp's pf_per_thread
        // was reset before the OMP region above and written to by every
        // worker inside integrate_ii.
        g_partial_fractions_s += sum_partial_fractions_per_thread();
        // Phase-d15 deeper drill: linear_factors total (subset of
        // partial_fractions_s). partial_fractions.cpp's lf_per_thread.
        g_linear_factors_s    += sum_linear_factors_per_thread();
        // Phase-d15 deeper drill (round 3): FLINT factor wall (subset
        // of linear_factors_s) + hit/miss counters. The "cache+narrow-
        // setup overhead" inside linear_factors_s is implicit:
        //   linear_factors_s - lf_flint_factor_s.
        g_lf_flint_factor_s   += sum_lf_flint_factor_per_thread();
        g_lf_cache_hits       += sum_lf_cache_hits_per_thread();
        g_lf_cache_misses     += sum_lf_cache_misses_per_thread();
        // Phase-d15 deeper drill (round 4): degree-bucketed
        // breakdown.
        g_lf_flint_deg1_s     += sum_lf_flint_deg1_per_thread();
        g_lf_flint_deg2_s     += sum_lf_flint_deg2_per_thread();
        g_lf_miss_deg1        += sum_lf_miss_deg1_per_thread();
        g_lf_miss_deg2        += sum_lf_miss_deg2_per_thread();
        // Phase-d15 deeper drill (round 5): deg-3+ outcome buckets.
        g_lf_d3p_all_linear_count    += sum_lf_d3p_all_linear_count_per_thread();
        g_lf_d3p_all_linear_s        += sum_lf_d3p_all_linear_s_per_thread();
        g_lf_d3p_has_nonlinear_count += sum_lf_d3p_has_nonlinear_count_per_thread();
        g_lf_d3p_has_nonlinear_s     += sum_lf_d3p_has_nonlinear_s_per_thread();
        // Phase-d15 deeper drill (round 6): squarefree vs repeated.
        g_lf_d3p_squarefree_count    += sum_lf_d3p_squarefree_count_per_thread();
        g_lf_d3p_squarefree_s        += sum_lf_d3p_squarefree_s_per_thread();
        g_lf_d3p_repeated_count      += sum_lf_d3p_repeated_count_per_thread();
        g_lf_d3p_repeated_s          += sum_lf_d3p_repeated_s_per_thread();
        // Phase-d15 deeper drill (round 7): sqf-first path totals.
        g_lf_sqf_total_s             += sum_lf_sqf_total_s_per_thread();
        g_lf_sqf_decomp_s            += sum_lf_sqf_decomp_s_per_thread();
        g_lf_sqf_inner_factor_s      += sum_lf_sqf_inner_factor_s_per_thread();
        g_lf_sqf_calls               += sum_lf_sqf_calls_per_thread();
        g_lf_sqf_inner_factor_calls  += sum_lf_sqf_inner_factor_calls_per_thread();
        g_lf_sqf_bailouts            += sum_lf_sqf_bailouts_per_thread();
        g_bump_lookup_s              += sum_bump_lookup_s_per_thread();
        g_bump_addto_s               += sum_bump_addto_s_per_thread();
        g_push_ibp_s                 += sum_push_ibp_s_per_thread();
        g_antideriv_s                += sum_antideriv_s_per_thread();
        g_bump_calls                 += sum_bump_calls_per_thread();
        // 2026-04-27 Lever-1 extended: bump-local split.
        g_bump_emplace_s             += sum_bump_emplace_s_per_thread();
        g_bump_rat_add_s             += sum_bump_rat_add_s_per_thread();
        g_bump_rat_add_calls         += sum_bump_rat_add_calls_per_thread();
        g_pf_calls_in_loop           += sum_pf_calls_in_loop_per_thread();
        g_pf_unique_dens             += sum_pf_unique_dens_per_thread();
        g_bump_unique_rows           += sum_bump_unique_rows_per_thread();
        // Lock-contention proxy.
        g_lf_lock_held_s             += sum_lf_lock_held_per_thread();
        // iter-37 lock-acquire wait probe.
        g_lf_lock_wait_s             += sum_lf_lock_wait_per_thread();
        // cache_key_build direct measurement.
        g_lf_cache_key_build_s       += sum_lf_cache_key_build_per_thread();
        // 2026-04-29 (Probe 2): post-FLINT extraction sub-timers.
        g_lf_post_transplant_s          += sum_lf_post_transplant_per_thread();
        g_lf_post_rat_ctor_s            += sum_lf_post_rat_ctor_per_thread();
        g_lf_post_constant_to_string_s  += sum_lf_post_constant_to_string_per_thread();
        g_lf_post_clone_from_raw_s      += sum_lf_post_clone_from_raw_per_thread();
        // 2026-04-29 (Probe 3): integrate_ii body residual sub-timers.
        g_ii_queue_copy_s               += sum_ii_queue_copy_s_per_thread();
        g_ii_pole_arith_s               += sum_ii_pole_arith_s_per_thread();
        g_ii_pole_word_ctor_s           += sum_ii_pole_word_ctor_s_per_thread();
        // 2026-04-27 (3l3pt profile-deepening): narrow-vs-wide branch
        // in reduce_inplace (every Rat-producing op, not just
        // Rat::add — renamed from rat_add_* after reviewer correction).
        g_reduce_narrow_s            += sum_reduce_narrow_s_per_thread();
        g_reduce_wide_s              += sum_reduce_wide_s_per_thread();
        g_reduce_narrow_calls        += sum_reduce_narrow_calls_per_thread();
        g_reduce_wide_calls          += sum_reduce_wide_calls_per_thread();
        g_reduce_zero_calls          += sum_reduce_zero_calls_per_thread();
        g_gcd_cofactors_s            += sum_gcd_cofactors_s_per_thread();
        g_gcd_cofactors_calls        += sum_gcd_cofactors_calls_per_thread();
        g_rn_used_vars_s             += sum_rn_used_vars_s_per_thread();
        g_rn_setup_s                 += sum_rn_setup_s_per_thread();
        g_rn_post_s                  += sum_rn_post_s_per_thread();
        g_rat_mul_calls              += sum_rat_mul_calls_per_thread();
        g_rat_sub_calls              += sum_rat_sub_calls_per_thread();
        g_rat_div_calls              += sum_rat_div_calls_per_thread();
        // 2026-05-01 Tier 3 Phase-0: nterm-blowup accumulators.
        g_reduce_nterm_calls          += sum_reduce_nterm_calls_per_thread();
        g_reduce_nterm_pre_total      += sum_reduce_nterm_pre_total_per_thread();
        g_reduce_nterm_post_total     += sum_reduce_nterm_post_total_per_thread();
        g_reduce_wide_smallfall_calls += sum_reduce_wide_smallfall_calls_per_thread();
        {
            const long pmax = max_reduce_nterm_pre_per_thread();
            const long qmax = max_reduce_nterm_post_per_thread();
            if (pmax > g_reduce_nterm_pre_max)  g_reduce_nterm_pre_max  = pmax;
            if (qmax > g_reduce_nterm_post_max) g_reduce_nterm_post_max = qmax;
        }
        // 2026-04-27 Lever-1 extended: per-Poly-op timers in Rat::add.
        g_rat_add_polymul_s          += sum_rat_add_polymul_s_per_thread();
        g_rat_add_polyadd_s          += sum_rat_add_polyadd_s_per_thread();
        g_rat_add_calls              += sum_rat_add_calls_per_thread();
        // 2026-05-03 chain 20: per-backend Rat::add wall + calls.
        g_rat_add_legacy_wall_s      += sum_rat_add_legacy_wall_s_per_thread();
        g_rat_add_via_qu_wall_s      += sum_rat_add_via_qu_wall_s_per_thread();
        g_rat_add_legacy_calls       += sum_rat_add_legacy_calls_per_thread();
        g_rat_add_via_qu_calls       += sum_rat_add_via_qu_calls_per_thread();
        // 2026-04-27 Avenue A: narrow-vs-wide path in Poly::mul.
        g_mul_narrow_s               += sum_mul_narrow_s_per_thread();
        g_mul_wide_s                 += sum_mul_wide_s_per_thread();
        g_mul_narrow_calls           += sum_mul_narrow_calls_per_thread();
        g_mul_wide_calls             += sum_mul_wide_calls_per_thread();
        g_mul_gated_calls            += sum_mul_gated_calls_per_thread();
        // 2026-04-28 reviewer round 7: distribution histograms.
        {
            auto lc = sum_nbin_lalb_count_per_thread();
            auto lt = sum_nbin_lalb_us_per_thread();
            auto lm = sum_nbin_lalb_max_per_thread();
            auto uc = sum_nbin_u_count_per_thread();
            auto ut = sum_nbin_u_us_per_thread();
            for (int i = 0; i < 6 && i < (int)lc.size(); ++i) {
                g_nbin_lalb_count[i] += lc[i];
                g_nbin_lalb_us[i]    += lt[i];
                if (lm[i] > g_nbin_lalb_max[i]) g_nbin_lalb_max[i] = lm[i];
            }
            for (int i = 0; i < 5 && i < (int)uc.size(); ++i) {
                g_nbin_u_count[i] += uc[i];
                g_nbin_u_us[i]    += ut[i];
            }
        }
        // 2026-04-27 (3l3pt profile-deepening): PolesBucket::bump
        // sub-timers.  Identifies which line inside bucket_bump_s
        // (95-99% of loop_residual_s on heavy steps) dominates.
        g_bucket_canon_regkey_s      += sum_bucket_canon_regkey_s_per_thread();
        g_bucket_struct_hash_s       += sum_bucket_struct_hash_s_per_thread();
        g_bucket_index_find_s        += sum_bucket_index_find_s_per_thread();
        g_bucket_symcoef_add_s       += sum_bucket_symcoef_add_s_per_thread();
        g_bucket_emplace_s           += sum_bucket_emplace_s_per_thread();
        // 2026-04-28 (Lever-D gate): harvest PolesBucket collision
        // counters from the just-closed main parallel-for. The merge
        // counters are NOT harvested here: the merge parallel-for has
        // not run yet (it lives below at line ~1700+). They are
        // harvested after that loop completes, before the JSON emit.
        // Reset the per-thread storage immediately after harvest so
        // any later serial-context bumps (check_divergences_pass at
        // ~1750) don't bleed into the merge harvest's accounting.
        g_bucket_collision_calls     += sum_bucket_collision_calls_per_thread();
        g_bucket_collision_pre_terms += sum_bucket_collision_pre_terms_per_thread();
        g_bucket_collision_post_terms+= sum_bucket_collision_post_terms_per_thread();
        for (auto& x : detail::bucket_collision_calls_storage())     x = 0L;
        for (auto& x : detail::bucket_collision_pre_terms_storage()) x = 0L;
        for (auto& x : detail::bucket_collision_post_terms_storage())x = 0L;

        // 2026-04-27 (Lever A): harvest the partial_fractions cache
        // hit/miss/collision counters from each worker's thread_local.
        // Each worker visits its own counter inside an OMP parallel
        // block and atomic-sums into the per-step accumulators. Gated
        // on do_parallel so small fixtures (tst0) don't pay the OMP
        // fork. Collisions = hash matched but stored input != current
        // input (hash-collision recompute path); see
        // partial_fractions.cpp for rationale.
        long pf_hits_sum = 0, pf_misses_sum = 0, pf_collisions_sum = 0;
#ifdef HF_HAVE_OPENMP
        if (do_parallel) {
            #pragma omp parallel
            {
                const long h = read_pf_cache_hits();
                const long m = read_pf_cache_misses();
                const long c = read_pf_cache_collisions();
                #pragma omp atomic update
                pf_hits_sum       += h;
                #pragma omp atomic update
                pf_misses_sum     += m;
                #pragma omp atomic update
                pf_collisions_sum += c;
            }
        } else {
            pf_hits_sum       = read_pf_cache_hits();
            pf_misses_sum     = read_pf_cache_misses();
            pf_collisions_sum = read_pf_cache_collisions();
        }
#else
        pf_hits_sum       = read_pf_cache_hits();
        pf_misses_sum     = read_pf_cache_misses();
        pf_collisions_sum = read_pf_cache_collisions();
#endif
        g_pf_cache_hits_step       += pf_hits_sum;
        g_pf_cache_misses_step     += pf_misses_sum;
        g_pf_cache_collisions_step += pf_collisions_sum;
    }

    // Merge per-thread PolesBuckets into the final polesInf. The old
    // loop called PolesBucket::bump once per (thread, key, term), and
    // each bump on a matching key invoked SymCoef::add →
    // from_monomials → canonicalize → Rat::add → FLINT GCD. That chain
    // ran up to `n_threads_outer` times per key and made the serial
    // merge 42% of wall time on the 13-thread tst2 bench (see profile
    // analysis 2026-04-23: workers were uniformly 41-43% idle while
    // the master burned through canonicalize).
    //
    // The fix is a flat-collect-then-single-canonicalize pattern:
    // concatenate every thread-bucket's monomial list into one vector
    // per regkey content-hash, then run SymCoef::from_monomials
    // (which calls canonicalize exactly once) per key at the end.
    // Algebraically identical because canonicalize is a pure function
    // of the monomial multiset. The final three-way combine
    // `polesInf[L] += polesZero[L]` also folds in for free — we
    // accumulate both bucket families into the same map.
    //
    // TSan note: ThreadSanitizer flags these reads of accum_t[i].polesZero
    // / accum_t[i].polesInf as racing against the workers' writes inside
    // the parallel region. That is a **false positive** — OpenMP's
    // `#pragma omp parallel for` establishes an implicit barrier at
    // the end of the parallel region that creates a happens-before
    // edge between every worker's writes and this merge, but Apple's
    // clang TSan does not instrument libomp's internal
    // synchronization primitives (the runtime calls through to
    // `__kmp_*` which are not TSan-annotated). Output is numerically
    // correct; regression (288 fixtures × serial and × NT={4,8})
    // passes. Do not add an `omp barrier` or atomic fence here —
    // they wouldn't silence TSan and only add overhead.
    // 2026-04-26 (Lever-C scout): wall-clock around the serial post-OMP
    // merge. Spans the flat-collect → canonicalize-once-per-key block.
    // Scout n=3 (tst2 step 3): omp_post_merge_s = 63.18 / 64.67 / 61.56
    // = 63.13 ± 4.9% wall-s, i.e. 46% of step 3 wall. Since η in the
    // upstream parallel-for is 0.99 (idle ≈ 0.5 s/thread), this serial
    // merge is THE bottleneck.
    const auto _omp_merge_t0 = std::chrono::steady_clock::now();
    // 2026-04-30 (Tier 1.6b): MonomialAcc now stores per-thread
    // canonical SymCoef chunks (one per contributing thread per slot)
    // rather than a flat vector<SymMonomial>. Each per-thread bucket's
    // coef is canonical by the PolesBucket::bump → operator+= invariant
    // (SymCoef::canonicalize at the end of every += call). This lets
    // the post-merge phase use SymCoef::tree_merge over the chunks
    // instead of canonicalize-from-scratch on the concatenation —
    // O((N+M) log K) total work via pairwise merges instead of
    // O(N+M) per-monomial keying + O((N+M) log (N+M)) sort.
    // 2026-05-03 (Phase 1 / Task 1.1): MonomialAcc promoted to
    // hyperflint::MonomialAcc in include/hyperflint/integrator/
    // sharded_flat_map.hpp so that ShardedFlatMap and this map
    // share a single type. No behaviour change here.
    // 2026-04-26 (a-prime lever): swapped string key for 128-bit
    // structural hash. Each `z.key` here is already canonicalised by
    // `PolesBucket::bump`, and Word::struct_hash is memoised, so the
    // per-Word hash from the per-thread bump map carries forward.
    std::unordered_map<std::pair<uint64_t, uint64_t>, MonomialAcc,
                       PairU64Hash> flat;
    // Phase 0.5 Item U Stage 1: collect_into_flat now takes a member pointer
    // (PolesBucket PerThreadAccum::*) to select polesZero or polesInf from
    // each accum_t slot.  Called twice — once for zero, once for inf — as
    // before.  No change to the inner bucket-iteration logic.
    auto collect_into_flat = [&](PolesBucket PerThreadAccum::* bucket_field) {
        for (auto& a : accum_t) {
            auto& b = a.*bucket_field;
            // Avenue F: drain `pending[i]` alongside `terms[i].coef`.
            // PolesBucket maintains pending.size() == terms.size() at
            // every bump, so a direct index is always safe. When
            // defer-bump is off, pending[i] is always empty and this
            // loop reduces to the pre-Avenue-F shape.
            const size_t n = b.terms.size();
            for (size_t i = 0; i < n; ++i) {
                auto& z = b.terms[i];
                auto& pend = b.pending[i];
                const bool z_zero = z.coef.is_zero();
                if (z_zero && pend.empty()) continue;
                const auto k = regkey_struct_hash(z.key);
                auto it = flat.find(k);
                if (it == flat.end()) {
                    MonomialAcc acc;
                    acc.key = z.key;
                    acc.chunks.reserve(4 + pend.size());
                    if (!z_zero) acc.chunks.push_back(std::move(z.coef));
                    for (auto& p : pend) acc.chunks.push_back(std::move(p));
                    flat.emplace(k, std::move(acc));
                } else {
                    if (!z_zero) it->second.chunks.push_back(std::move(z.coef));
                    for (auto& p : pend) it->second.chunks.push_back(std::move(p));
                }
            }
        }
    };
    // Probe 4: time the serial collect_into_flat phase.
    const auto _pm_collect_t0 = std::chrono::steady_clock::now();
    if (stream_active) {
        // Phase 1 / Task 1.4: drain whatever each per-thread bucket
        // didn't push past the streaming threshold, then merge the
        // two ShardedFlatMaps into `flat`. R28 C2: extract_into_sorted
        // sorts each MonomialAcc.chunks by SymCoef::to_string() so
        // the post-merge tree_merge is byte-deterministic regardless
        // of shard arrival order — REQUIRED for result_sha bit-
        // identity across runs.
        for (auto& a : accum_t) a.polesZero.drain();
        for (auto& a : accum_t) a.polesInf.drain();
        sharded_zero.extract_into_sorted(flat);
        sharded_inf.extract_into_sorted(flat);
    } else {
        collect_into_flat(&PerThreadAccum::polesZero);
        collect_into_flat(&PerThreadAccum::polesInf);
    }
    g_omp_collect_into_flat_s += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - _pm_collect_t0).count();

    // 2026-05-01 (Branch A pre-flight, Avenue J cross-ordering bucket
    // sharing): opt-in dump of every RegKey hash from the merged `flat`
    // map.  Each step emits ~10² to 10³ JSONL records to stderr.  Total
    // dump size ~120 KB on parity-1 (3608 unique RegKeys × ~33 B/record).
    // Set HF_REGKEY_DUMP=1 to enable; default off, zero overhead when off.
    if (const char* env = HF_FLAG_REGKEY_DUMP;
        env && env[0] == '1') {
        static std::mutex regkey_dump_mu;
        std::lock_guard<std::mutex> _lk(regkey_dump_mu);
        for (auto& kv : flat) {
            std::cerr << "{\"hf_regkey_dump\":true"
                      << ",\"h1\":" << kv.first.first
                      << ",\"h2\":" << kv.first.second << "}\n";
        }
    }

    // 2026-04-26 (a-prime lever) sanity probe: opt-in hash-distribution
    // dump on the merged `flat` map. Reviewer's GO threshold = max
    // bucket size <= 4 (FNV-1a 128-bit at ~10^5 keys -> expected <= 2).
    // Iter-96 §T7 26th chunk Track-diagnostic-dump partial: VALUE-family
    // macro relocation to integrator/env_flags.hpp (sixth extension of
    // that header). The exact-match-"1" opt-in predicate semantics are
    // preserved verbatim by the local guard below; the macro only
    // consolidates the env-name string literal in one header per
    // docs/env_flags.md §5.1 rule-1 effect-domain placement.
    if (const char* env = HF_FLAG_BUCKET_HASH_STATS;
        env && env[0] == '1') {
        size_t max_depth = 0;
        size_t nonempty_buckets = 0;
        for (size_t b = 0; b < flat.bucket_count(); ++b) {
            const size_t sz = flat.bucket_size(b);
            if (sz == 0) continue;
            ++nonempty_buckets;
            if (sz > max_depth) max_depth = sz;
        }
        std::cerr << "{\"hf_bucket_hash_stats\":true"
                  << ",\"keys\":" << flat.size()
                  << ",\"buckets\":" << flat.bucket_count()
                  << ",\"nonempty_buckets\":" << nonempty_buckets
                  << ",\"max_bucket_size\":" << max_depth
                  << ",\"load_factor\":" << flat.load_factor()
                  << "}\n";
    }

    // 2026-04-26 (Lever C-merge): parallelize the canonicalize-per-key
    // loop. Each `from_monomials` call is independent (operates on
    // local SymMonomial data + reads the shared `const PolyCtx&`). This
    // change inherits the same FLINT-shared-`fmpq_mpoly_ctx_t`-with-
    // distinct-output invariant that the upstream `#pragma omp parallel
    // for` (line ~810) already depends on — that loop's workers also
    // call `from_monomials → canonicalize → Rat::add → fmpq_mpoly_*`
    // concurrently on the same `ctx`, and η = 0.99 on tst2 step 3
    // confirms FLINT 3.x is concurrent-read-safe on a shared ctx for
    // these arithmetic primitives.
    //
    // Bit-identity: the previous code iterated `flat` in
    // implementation-defined unordered_map bucket order; we sort
    // `flat_v` by `(h1, h2)` first so the intermediate `polesInf.terms`
    // ordering is deterministic across runs and OMP schedules. The
    // observable downstream output is normalized regardless: the caller
    // wraps the returned RegulatorSym in `canonicalize_regulator_sym`
    // which sorts by `regkey_content_key` (transform.cpp:497), so this
    // intermediate ordering tightening is a no-op at every observable
    // boundary.
    //
    // Risk: if two distinct RegKeys produced the same 128-bit
    // structural hash, the existing `flat[k] = ...` insert would
    // already silently merge them (the same risk model as the a-prime
    // sharded-LF lever). FNV-1a 128-bit collision probability at ~10^5
    // keys is ~1.5e-27; the 288-fixture regression has not produced
    // any divergence. This change inherits the same risk class.
    //
    // TSan: inherits the prior libomp-barrier false-positive note from
    // line ~1145 above; no new races to suppress.
    //
    // Kill switch: `HF_DISABLE_PARALLEL_MERGE=1` forces the legacy
    // serial path for two release cycles' worth of A/B observability.

    // Convert flat → sorted vector by hash key for deterministic
    // intermediate ordering. unordered_map node storage is
    // pointer-stable across non-rehashing operations, so the raw
    // pointers to MonomialAcc remain valid through the parallel region.
    std::vector<std::pair<std::pair<uint64_t, uint64_t>, MonomialAcc*>> flat_v;
    flat_v.reserve(flat.size());
    for (auto& kv : flat) flat_v.emplace_back(kv.first, &kv.second);
    // Probe 4: time the serial sort.
    const auto _pm_sort_t0 = std::chrono::steady_clock::now();
    std::sort(flat_v.begin(), flat_v.end(),
              [](const auto& a, const auto& b) {
                  return a.first < b.first;
              });
    g_omp_merge_sort_s += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - _pm_sort_t0).count();

    // Per-slot canonicalize result. std::optional handles the
    // not-default-constructible RegTermSym cleanly; sparse fill is
    // harmless (5.6 MB peak at K=28k, vs many GB of FLINT scratch).
    std::vector<std::optional<RegTermSym>> canon_slots(flat_v.size());

    bool kill_parallel_merge = false;
    if (const char* env = HF_FLAG_DISABLE_PARALLEL_MERGE;
        env && env[0] == '1') {
        kill_parallel_merge = true;
    }
#ifdef HF_HAVE_OPENMP
    const bool do_parallel_merge =
        do_parallel && flat_v.size() >= 32 && !kill_parallel_merge;
#else
    const bool do_parallel_merge = false;
#endif
    (void)kill_parallel_merge;  // referenced only when OpenMP is on
    // Probe 4: time the parallel canonicalize-per-key region.
    const auto _pm_canon_t0 = std::chrono::steady_clock::now();
    // 2026-04-30 (Tier 1.4a η probe): per-thread wall accumulators
    // around the body. After the parallel-for we harvest max/min/sum
    // and load η = sum / (n_threads * parallel_canon_wall) drops out
    // of the JSON. Reuses bucket-emplace storage as a clean per-thread
    // double slot — bucket_emplace is already cleared per step and
    // its post-OMP harvest into g_bucket_emplace_s happens BEFORE this
    // post-merge phase runs (line ~1568), so the slots are free to
    // reuse here without conflict.
    auto& _pm_canon_walltime = detail::bucket_emplace_storage();
    for (auto& x : _pm_canon_walltime) x = 0.0;
    // 2026-04-30 (axis-E dynamic-1 schedule): switched from
    // static/dynamic-8 chunks to dynamic-1 work-stealing. Probe-4
    // attribution (commit 039412492) located 99.8% of post-merge
    // wall in this parallel-for; subsequent measurement showed the
    // slot work distribution is heavy-tailed (a few slots dominate),
    // so dynamic-1 work-stealing rebalances. On parity-1 ord_1_face_1
    // traced: parallel_canon 96.37 s -> 84.28 s, total wall 223.7 s
    // -> 216.2 s. Step 6 alone: 73 -> 56 wall-s, 117 -> 102 wall-s.
    //
    // 2026-04-30 (Tier 1.5 LPT scheduling — RETIRED null result):
    // Tried sorting `flat_v` by mons.size() descending into a
    // `schedule_v` permutation, expecting the 4/3-OPT LPT heuristic
    // to lift η > 0.85. n=3 untraced + n=1 traced on parity-1
    // ord_1_face_1: step-6 η 0.616 -> 0.626 (within noise), step-5 η
    // 0.232 -> 0.232 (zero change), wall +5 s within combined σ. The
    // imbalance is *structural*: one slot per step is heavy enough
    // that any thread drawing it pays the full cost — ordering
    // doesn't help. The right intervention is slot-splitting (split
    // an over-threshold slot's input mons into chunks, canonicalize
    // chunks in parallel, then merge). LPT scaffolding removed to
    // keep the hot loop minimal; slot-splitting will be its own lever.

    // Phase-B B6.b (design v2 §3.5a row 6 + §3.6a + §4.2 commit (6)).
    // Per-canon-slot `as_rat` boundary at the post-OMP-merge master-
    // accumulator boundary: under runtime::scalar_rep_enabled() (the
    // HF_USE_SCALAR_REP env-gate), every SymMonomial.prefactor in the
    // merged canonical SymCoef `c` is round-tripped through the
    // `runtime::roundtrip_rat_through_scs` helper, then SymCoef is
    // rebuilt via SymCoef::from_monomials. Mirrors the B1.c/B2/B3/B4/B5
    // dispatch pattern, with SymCoef-typed input/output via per-prefactor
    // walk (B5's siblings all operate on Rat directly; B6.b is the first
    // SymCoef-typed verifier site under the bit-identity-only Option A
    // gate). Iter-18 b6_scoping_memo.md justifies collapsing B6 to this
    // single site (B6.c dropped: `partial_fractions(coef, ...)` is
    // called only at primitive.cpp:350, and B5's iter-17 dispatch at
    // primitive.cpp:412 already round-trips the consumed coef).
    //
    // Iter-52 C0c.1 Increment β (site 7 lambda body kill + Protocol A
    // per design v2 §3.6a STEPS 1-3, ratified at iter-50 MEMO §6 Q3 +
    // iter-51 §6.5). Pre-iter-52 the lambda allocated a fresh ZWTable
    // per call (`auto zw_tab = std::make_shared<ZWTable>(ctx);`),
    // making each canon-slot trivially thread-safe under the
    // dynamic-1 parallel-for. The persistent driver-entry ZWTable
    // (function-parameter `zw_tab`, allocated at hyper_int.cpp:~463 when
    // scalar_rep_enabled()) is non-thread-safe (entries_ vector,
    // by_hash_ map; iter-40 inventory §2.4), so threading it directly
    // into the lambda body across OMP workers would race. Protocol A:
    // (1) allocate per-thread ZWTable instances at OMP region entry;
    // (2) lambda body uses worker's per-thread table via
    // `per_thread_zw_site7[tid]`; (3) post-barrier master walks the
    // per-thread tables and merges them deterministically into the
    // function-parameter `zw_tab` via the combined-then-master pattern
    // (single-pass union sort over the FULL union per A6 — NOT
    // per-thread-sequential merge_into in omp_get_thread_num() order,
    // which would break OMP=1 vs OMP=N determinism on ZWTable handle
    // assignment). zw_table.cpp:270 `merge_into` is documented
    // deterministic (canonical-content order via FNV-1a + struct_compare
    // tiebreak) so the combined-table second-stage merge fixes handle
    // assignment regardless of partition order in the first stage.
    const int n_threads_site7 =
#ifdef HF_HAVE_OPENMP
        do_parallel_merge ? omp_get_max_threads() : 1;
#else
        1;
#endif
    std::vector<std::shared_ptr<ZWTable>> per_thread_zw_site7;
    if (zw_tab && runtime::scalar_rep_enabled()) {
        per_thread_zw_site7.resize(static_cast<size_t>(n_threads_site7));
        for (int t = 0; t < n_threads_site7; ++t) {
            per_thread_zw_site7[static_cast<size_t>(t)] =
                std::make_shared<ZWTable>(ctx);
        }
    }
    auto apply_v1_roundtrip_symcoef = [&](SymCoef&& c,
                                          const char* tag,
                                          int worker_tid) -> SymCoef {
        // Iter-58 (f) defense-in-depth: also bypass when zw_tab is null.
        // Currently unreachable because per_thread_zw_site7 is populated
        // only when (zw_tab && scalar_rep_enabled()), but the guard
        // belongs at the lambda's entry (advisory item from iter-57
        // reviewer agentId a16b4f479d33c0114).
        if (!runtime::scalar_rep_enabled() || !zw_tab) return std::move(c);
        // Iter-52 C0c.1 Increment β: use per-thread ZWTable allocated
        // before the OMP region (Protocol A STEP 1). Master-side merge
        // into the function-parameter `zw_tab` happens post-barrier
        // (Protocol A STEP 3). Lambda body is now thread-safe under
        // dynamic-1 schedule because each worker reads/writes its own
        // ZWTable instance.
        auto& my_zw =
            per_thread_zw_site7[static_cast<size_t>(worker_tid)];
        std::vector<SymMonomial> roundtripped;
        roundtripped.reserve(c.terms().size());
        for (const auto& m : c.terms()) {
            SymMonomial m2 = m;
            m2.prefactor = runtime::roundtrip_rat_through_scs(
                m.prefactor, ctx, my_zw, tag);
            roundtripped.push_back(std::move(m2));
        }
        return SymCoef::from_monomials(ctx, std::move(roundtripped));
    };
    // FIX-B (2026-06-01): this schedule(dynamic,1) was the ONLY one of the three
    // omp parallel-for schedules in this file not gated on HF_OMP_FULL_SCHEDULES
    // (cf. lines ~1004 and ~2087). schedule(dynamic) emits ___kmpc_dispatch_deinit,
    // which Wolfram's bundled libomp does NOT export; the in-process LibraryLink
    // dylib (hyperflint_nomp_sd, no HF_OMP_FULL_SCHEDULES) then binds it to null and
    // SIGSEGVs at PC=0 inside integration_step on the dbox's do_parallel_merge path.
    // Gate it like the siblings so the dylib uses schedule(static,1); the CLI
    // (HF_OMP_FULL_SCHEDULES=1) keeps the dynamic path for load balancing.
#ifdef HF_OMP_FULL_SCHEDULES
    #pragma omp parallel for schedule(dynamic, 1) if(do_parallel_merge)
#else
    #pragma omp parallel for schedule(static, 1) if(do_parallel_merge)
#endif
    for (long i = 0; i < static_cast<long>(flat_v.size()); ++i) {
        auto& mp = flat_v[static_cast<size_t>(i)];
        // 2026-04-28 (Lever-D gate): per-slot in/out monomial counts.
        // Captured before move into the merge.
        // 2026-04-30 (Tier 1.6b): _in_n is now the total monomial
        // count summed across the slot's chunks (was a single mons
        // vector under the old shape).
        long _in_n = 0;
        for (const auto& ch : mp.second->chunks) {
            _in_n += static_cast<long>(ch.terms().size());
        }
        // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num() in OMP mode;
        // under HF_USE_GCD=1 returns the GCD slot index.
        const int _mt_tid = ::hyperflint::runtime::hf_get_thread_num();
        const auto _pm_iter_t0 = std::chrono::steady_clock::now();
        // 2026-04-30 (Tier 1.6b): tree-merge per-thread canonical
        // chunks via SymCoef::merge_sorted_canonical pairwise. K=1
        // takes the only chunk directly. K>1 does log2(K) levels.
        // Iter-52 C0c.1 Increment β: pass _mt_tid so the lambda picks
        // the worker's per-thread ZWTable (Protocol A STEP 2).
        // HF_ADDPF_PROBE (additive-parfrac Phase 0): addends-per-key at
        // the fusion site. Read BEFORE the move below consumes chunks.
        const std::size_t _addpf_n_chunks = mp.second->chunks.size();
        SymCoef c = apply_v1_roundtrip_symcoef(
            (mp.second->chunks.size() == 1)
                ? std::move(mp.second->chunks[0])
                : SymCoef::tree_merge(std::move(mp.second->chunks)),
            "integration_step/post_omp_merge_canon_slot",
            _mt_tid);
        const auto _pm_iter_t1 = std::chrono::steady_clock::now();
        const long _out_n = static_cast<long>(c.terms().size());
        if (addpf_probe::enabled()) {
            std::size_t _max_num_terms = 0;
            const std::size_t _nuser = addpf_probe::user_var_count();
            for (const auto& _m : c.terms()) {
                const std::size_t nt = _m.prefactor.num().n_terms();
                if (nt > _max_num_terms) _max_num_terms = nt;
                // Task 0.2 census: does this monomial's prefactor use
                // any atom var (index >= kinematic prefix)?
                bool _uses = false;
                if (_nuser > 0) {
                    for (std::size_t vi :
                             _m.prefactor.num().used_var_indices())
                        if (vi >= _nuser) { _uses = true; break; }
                    if (!_uses)
                        for (std::size_t vi :
                                 _m.prefactor.den().used_var_indices())
                            if (vi >= _nuser) { _uses = true; break; }
                }
                addpf_probe::record_monomial(_uses);
            }
            addpf_probe::record_drain_key(_addpf_n_chunks, _max_num_terms);
        }
        {
            auto& _imv = merge_in_terms_storage();
            if (static_cast<size_t>(_mt_tid) < _imv.size())
                _imv[_mt_tid] += _in_n;
            auto& _omv = merge_out_terms_storage();
            if (static_cast<size_t>(_mt_tid) < _omv.size())
                _omv[_mt_tid] += _out_n;
            if (static_cast<size_t>(_mt_tid) < _pm_canon_walltime.size())
                _pm_canon_walltime[_mt_tid] +=
                    std::chrono::duration<double>(
                        _pm_iter_t1 - _pm_iter_t0).count();
        }
        if (!c.is_zero()) {
            canon_slots[static_cast<size_t>(i)] =
                RegTermSym{std::move(c), std::move(mp.second->key)};
        }
    }
    // Probe 4: close the parallel-canonicalize wall.
    g_omp_parallel_canonicalize_s += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - _pm_canon_t0).count();
    // Iter-52 C0c.1 Increment β (Protocol A STEP 3): post-barrier master
    // merge of per-thread ZWTables into the function-parameter `zw_tab`
    // (the persistent driver-entry ZWTable, when scalar_rep_enabled()).
    // Single-pass union sort over the FULL union per design v2 §3.6a:
    // build a `combined` ZWTable by merging each per-thread table into
    // it (canonical-content-keyed iteration inside merge_into is
    // partition-order-dependent on this stage), then merge `combined`
    // into the master `zw_tab` (canonical-content-keyed iteration over
    // the full UNION is partition-order-INDEPENDENT). The two-stage
    // approach makes master's handle assignment depend only on the set
    // of unique contents — identical under OMP=1 and OMP=N. NOTE: the
    // returned remap tables are discarded; site 7's lambda output (a
    // SymCoef of round-tripped Rat prefactors) does not embed handle
    // values, so no caller-side handle remap on `canon_slots` is
    // needed (per A2 unstated-invariant: `roundtrip_rat_through_scs`
    // is opaque on handles, returning Rats reconstructed from N-side
    // and W-side data).
    if (zw_tab && runtime::scalar_rep_enabled() &&
        !per_thread_zw_site7.empty()) {
        ZWTable combined(ctx);
        for (auto& tt : per_thread_zw_site7) {
            if (tt) (void)combined.merge_into(*tt);
        }
        (void)zw_tab->merge_into(combined);
    }
    // Tier 1.4a η probe: harvest per-thread wall max/min/sum.
    {
        double pm_max = 0.0, pm_min = -1.0, pm_sum = 0.0;
        for (double t : _pm_canon_walltime) {
            pm_sum += t;
            if (t > 0.0) {
                if (t > pm_max) pm_max = t;
                if (pm_min < 0.0 || t < pm_min) pm_min = t;
            }
        }
        if (pm_min < 0.0) pm_min = 0.0;
        g_pm_canon_max_per_thread_s += pm_max;
        g_pm_canon_min_per_thread_s += pm_min;
        g_pm_canon_sum_per_thread_s += pm_sum;
    }
    // 2026-04-28 (Lever-D gate): the merge parallel-for has just
    // closed; harvest the per-slot in/out monomial counts now (the
    // earlier post-OMP harvest skipped these because the merge
    // hadn't run yet).
    g_merge_in_terms  += sum_merge_in_terms_per_thread();
    g_merge_out_terms += sum_merge_out_terms_per_thread();
    g_merge_slots += static_cast<long>(flat_v.size());

    // Serial assembly preserves the sorted-by-hash iteration order.
    // polesInf.index population must be serial (terms.size() depends on
    // the prior iteration's push_back).
    // Probe 4: time the serial assembly.
    const auto _pm_asm_t0 = std::chrono::steady_clock::now();
    PolesBucket polesInf;
    polesInf.terms.reserve(canon_slots.size());
    for (size_t i = 0; i < canon_slots.size(); ++i) {
        if (!canon_slots[i]) continue;
        polesInf.index[flat_v[i].first] = polesInf.terms.size();
        polesInf.terms.push_back(std::move(*canon_slots[i]));
    }
    g_omp_serial_assembly_s += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - _pm_asm_t0).count();
    // 2026-04-26 (Lever-C scout): close the serial-merge wall.
    // Note: under Lever C-merge, this timer captures the SUM of the
    // serial sort + parallel canonicalize + serial assembly. We expect
    // the dominant chunk (canonicalize, ~63 s on tst2 step 3) to
    // collapse by ~12/13 to ~6 s under perfect parallelism.
    g_omp_post_merge_s += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - _omp_merge_t0).count();

    // Phase 5e-iii: divergence-check pass. Runs AFTER the parallel
    // finite-part accumulation so that the hot path stays untouched.
    // Throws on the first non-zero non-(0,0) pole bin.
    if (check_divergences) {
        check_divergences_pass(ctx, input, var_idx, table, zw_tab,
                                introduce_algebraic_letters,
                                remaining_var_indices);
    }

    // Build result RegulatorSym, dropping zero-coef entries.
    RegulatorSym out;
    out.reserve(polesInf.terms.size());
    for (auto& t : polesInf.terms) {
        if (!t.coef.is_zero()) {
            out.push_back(std::move(t));
        }
    }
    // Phase-B telemetry: record the final-canonicalize cost.
    const auto t_canon0 = std::chrono::steady_clock::now();
    RegulatorSym canon = canonicalize_regulator_sym(out);
    const auto t_canon1 = std::chrono::steady_clock::now();
    g_integration_step_canon_s +=
        std::chrono::duration<double>(t_canon1 - t_canon0).count();
    // HF_ADDPF_PROBE (additive-parfrac Phase 0): one JSONL summary per
    // step, then reset, so the per-step attribution lines up with the
    // step-trace timers. Default-OFF.
    if (addpf_probe::enabled())
        addpf_probe::emit_and_reset(
            var_idx < ctx.vars().size() ? ctx.vars()[var_idx].c_str()
                                        : "?");
    return canon;
}

// Rat-valued overload — thin wrapper that promotes each entry's Rat
// coef into a trivial SymCoef (via SymCoef::from_rat) and delegates
// to the SymCoef overload above. Kept for callers at the driver
// boundary (the top-level integrator input is always Rat).
RegulatorSym integration_step(const PolyCtx& ctx,
                                const ShuffleList& input,
                                size_t var_idx,
                                const MzvReductionTable& table,
                                std::shared_ptr<ZWTable> zw_tab,
                                bool check_divergences,
                                bool introduce_algebraic_letters,
                                const std::vector<size_t>&
                                  remaining_var_indices) {
    ShuffleListSym promoted;
    promoted.reserve(input.size());
    for (const auto& e : input) {
        promoted.push_back(ShuffleEntrySym{
            SymCoef::from_rat(e.coef), e.shuffle});
    }
    return integration_step(ctx, promoted, var_idx, table, zw_tab,
                             check_divergences, introduce_algebraic_letters,
                             remaining_var_indices);
}

RegulatorSym integration_step_sym(const PolyCtx& ctx,
                                    const ShuffleListSym& input,
                                    size_t var_idx,
                                    const MzvReductionTable& table,
                                    std::shared_ptr<ZWTable> zw_tab,
                                    bool check_divergences,
                                    bool introduce_algebraic_letters,
                                    const std::vector<size_t>&
                                      remaining_var_indices) {
    // Bug #6 lift: `integration_step` now returns RegulatorSym directly,
    // so `base` carries SymCoef coefficients from the start. The
    // positive-letter closure runs on top-level keys that hit the
    // Phase-5e deferred stubs (i.e. keys produced before Fragment P2
    // replaces reglim_word's positive-letter return-empty).
    RegulatorSym base = integration_step(ctx, input, var_idx, table, zw_tab,
                                           check_divergences,
                                           introduce_algebraic_letters,
                                           remaining_var_indices);
    return close_positive_letters_in_regulator_sym(
        ctx, base, ctx.vars()[var_idx], table);
}

// Rat-valued overload — delegates to the SymCoef one after promoting
// each entry's Rat coef. Kept so the driver's final-step call at the
// ShuffleList boundary doesn't need to do the promotion itself.
RegulatorSym integration_step_sym(const PolyCtx& ctx,
                                    const ShuffleList& input,
                                    size_t var_idx,
                                    const MzvReductionTable& table,
                                    std::shared_ptr<ZWTable> zw_tab,
                                    bool check_divergences,
                                    bool introduce_algebraic_letters,
                                    const std::vector<size_t>&
                                      remaining_var_indices) {
    ShuffleListSym promoted;
    promoted.reserve(input.size());
    for (const auto& e : input) {
        promoted.push_back(ShuffleEntrySym{
            SymCoef::from_rat(e.coef), e.shuffle});
    }
    return integration_step_sym(ctx, promoted, var_idx, table, zw_tab,
                                 check_divergences,
                                 introduce_algebraic_letters,
                                 remaining_var_indices);
}

}  // namespace hyperflint
