// Phase 5e: IntegrationStep.
//
// Input: ShuffleList [(scalar, [word_1, word_2, ...]), ...] representing
//          sum_i scalar_i * Hlog_{word_1 ⧢ word_2 ⧢ ...}(var, ...).
// Output: Regulator  [(coef, reg_key), ...] — the regulator-wordlist of
//          the integral from 0 to ∞ after pole accumulation.
//
// Mirrors HyperIntica.wl:4717. Phase 5e-ii ports the
// CheckDivergences=False path: we only accumulate the (logpower=0,
// power=0) finite part; the Phase-6 positive-letter analytic
// continuation and TestZeroFunction divergence check are deferred.
//
// Algorithm outline:
//   for each entry in input:
//     ts = transform_shuffle(entry.shuffle, var)         (Phase 5c)
//     for each (shuf, reg) pair in ts:
//       primitive = integrate_ii(scalar_mul(shuf, entry.coef), var)  (Phase 5d)
//       for each term in primitive:
//         — Zero expansion: subtract to polesZero[(0,0,L)].
//         — Infinity expansion: add to polesInf[(0,0,L)].
//   polesInf[L] += polesZero[L] for shared L.
//   emit a Regulator from polesInf.
//
// See HyperIntica.wl:4717 for the reference implementation; internal
// helper identities (expand_zero_word, regzero_word, etc.) come from
// Phases 4a and 5a.

#pragma once

#include "hyperflint/integrator/transform.hpp"
#include "hyperflint/reduce/break_up_contour.hpp"   // RegulatorSym
#include "hyperflint/reduce/mzv_reduce.hpp"         // MzvReductionTable

#include <memory>
#include <stdexcept>

namespace hyperflint {

class ZWTable;  // fwd-decl; full def at hyperflint/core/zw_table.hpp.

class IntegrationStepFailed : public std::runtime_error {
public:
    IntegrationStepFailed()
        : std::runtime_error("IntegrationStep: $Failed") {}
    explicit IntegrationStepFailed(const std::string& m)
        : std::runtime_error("IntegrationStep: " + m) {}
};

// Phase 5e-iii: thrown by `integration_step` when `check_divergences`
// is true and the post-integration scan finds a non-zero residue in
// a non-(0, 0) {logpower, power} pole bin, indicating the input
// integral has a boundary divergence that doesn't cancel.
class HyperFLINTDivergentIntegral : public std::runtime_error {
public:
    HyperFLINTDivergentIntegral(const std::string& where,
                                  long logpower, long power)
        : std::runtime_error(
            "HyperFLINT: divergent integral detected at " + where +
            ", Log[var]^" + std::to_string(logpower) +
            " / var^" + std::to_string(power)) {}
};

// R24 rev 2 / chain-16 scaffolding: thrown post-OMP-region (host
// thread, after the implicit barrier) when
// `narrow_ctx_was_too_narrow()` reports any worker observed a
// missing variable during a `Rat::parse_or_none` call inside the
// parallel region.  The bridge handler catches this and emits the
// structured-error JSON `{"narrow_ctx_insufficient": true, ...}`
// which the Mma side detects to retry with HF_NARROW_CTX=0.
//
// Never throw from inside an OMP parallel region — only from host
// code after the region's implicit barrier (`#pragma omp parallel
// for`'s end).  The OpenMP standard makes exception escape from a
// parallel region implementation-defined; on Apple-clang/libomp it
// calls `std::terminate`.  See r24_step1_redesign_rev2.md for the
// full design.
//
// Wired in chain 17+: post-OMP flag check insertion in
// `integration_step.cpp` and `hyper_int.cpp::hyperflint_sym`,
// catch handler in `bridge/handlers.cpp::hyperflint_sym`.
class NarrowCtxTooNarrow : public std::runtime_error {
public:
    NarrowCtxTooNarrow()
        : std::runtime_error("HF: narrow ctx insufficient — "
                              "retry with wider ctx") {}
    explicit NarrowCtxTooNarrow(const std::string& where)
        : std::runtime_error("HF: narrow ctx insufficient at " +
                              where + " — retry with wider ctx") {}
};

// check_divergences = false (Phase 5e-ii default): skip divergence
// check, skip positive-letter analytic continuation.
//
// Phase 7-vi-a: `introduce_algebraic_letters` propagates through
// transform_shuffle → transform_word → linear_factors and
// integrate_ii → partial_fractions → linear_factors so that deg-2
// irreducible factors become Wm/Wp pairs. Default false preserves
// existing behavior.
// Bug #6 lift: returns RegulatorSym so positive-letter contour
// residues (I*Pi*delta[var]) that flow out of `reglim_word` can live
// in the per-step output. See docs/next_session_bug6_plan.md.
//
// Iter-52 C0c.1 Increment β: mandatory `std::shared_ptr<ZWTable> zw_tab`
// parameter (Option A; ratified at iter-50 MEMO §6 Q1 + iter-51 §6.5).
// Threaded through to transform_shuffle, integrate_ii, and the
// post-OMP-merge canon-slot lambda (site 7); the persistent
// driver-entry ZWTable from `hyper_int.cpp:~463` flows through here
// when `runtime::scalar_rep_enabled()`. Protocol A per design v2 §3.6a
// STEPS 1-3 governs site-7's per-thread allocation + post-barrier
// merge_into.
RegulatorSym integration_step(const PolyCtx& ctx,
                               const ShuffleList& input,
                               size_t var_idx,
                               const MzvReductionTable& table,
                               std::shared_ptr<ZWTable> zw_tab,
                               bool check_divergences = false,
                               bool introduce_algebraic_letters = false,
                               const std::vector<size_t>& remaining_var_indices
                                 = {});

// SymCoef-valued overload — used by hyperflint_sym to carry residues
// from Fragment P2's positive-letter closure through intermediate
// integration steps. Algebraically identical to the Rat-coef overload
// when every input coef is rat-pure. The main difference is that the
// entry's SymCoef factor is propagated to the output as a scalar
// multiplier instead of scaling the inner wordlist (which would
// require SymCoef-valued polynomial arithmetic inside integrate_ii).
//
// `remaining_var_indices` (default empty): when `check_divergences=true`,
// this is passed to `test_zero_function_sym`'s fibration-basis zero
// check. Empty list = base case only (algebraic per-term is_zero check);
// populated list = full fibration across the remaining variables of
// the multi-step integration. Drivers pass `var_indices[step+1:]`; the
// standalone CLI op passes an empty list (it doesn't know the full
// integration schedule).
//
// Iter-52 C0c.1 Increment β: same mandatory `std::shared_ptr<ZWTable>`
// parameter as the Rat overload.
RegulatorSym integration_step(const PolyCtx& ctx,
                               const ShuffleListSym& input,
                               size_t var_idx,
                               const MzvReductionTable& table,
                               std::shared_ptr<ZWTable> zw_tab,
                               bool check_divergences = false,
                               bool introduce_algebraic_letters = false,
                               const std::vector<size_t>& remaining_var_indices
                                 = {});

// Phase 6d-v-iii: IntegrationStep with positive-letter continuation.
//
// Runs the existing Rat-based integration_step, then post-processes
// the result: for each regulator key containing exactly one word with
// any positive-integer letter, call break_up_contour_sym with
// onAxis derived from that word's positive letters (paired with
// delta[var_name]), and re-bucket the SymCoef-valued result into the
// surviving (non-positive) regulator key.
//
// Mirrors HyperIntica.wl:4832-4856. Output: RegulatorSym (SymCoef
// coefficients carry Pi, I, Log[n], delta[var] literals).
//
// Iter-52 C0c.1 Increment β: same mandatory `std::shared_ptr<ZWTable>`
// parameter as integration_step.
RegulatorSym integration_step_sym(const PolyCtx& ctx,
                                    const ShuffleList& input,
                                    size_t var_idx,
                                    const MzvReductionTable& table,
                                    std::shared_ptr<ZWTable> zw_tab,
                                    bool check_divergences = false,
                                    bool introduce_algebraic_letters = false,
                                    const std::vector<size_t>&
                                      remaining_var_indices = {});

// SymCoef-valued overload, mirroring integration_step's SymCoef form.
RegulatorSym integration_step_sym(const PolyCtx& ctx,
                                    const ShuffleListSym& input,
                                    size_t var_idx,
                                    const MzvReductionTable& table,
                                    std::shared_ptr<ZWTable> zw_tab,
                                    bool check_divergences = false,
                                    bool introduce_algebraic_letters = false,
                                    const std::vector<size_t>&
                                      remaining_var_indices = {});

// Phase-B HF/Maple tst2-gap telemetry. Accessors for the per-thread
// sub-phase wall counters inside integration_step and
// close_positive_letters_in_regulator_sym. Zero when HF_STEP_TRACE is
// off (the driver doesn't reset them in that case). `reset_step_sub_timers`
// is called by the driver before each step; the three accessors return
// the accumulated seconds for that step's final canonicalize (the one
// inside integration_step), the closure body (close_positive_letters
// iteration), and the closure's final canonicalize.
void reset_step_sub_timers();
double read_integration_step_canon_s();
double read_closure_body_s();
double read_closure_canon_s();
// Phase-d15 follow-up: per-primitive timers for the main `integration_step`
// hot loop (NOT the check_divergences_pass nor fib_recurse_sym paths).
// Set under HF_STEP_TRACE; correct on the LibraryLink path and on CLI/OMP
// (per-thread accumulator vectors are summed into these globals after the
// parallel region — see integration_step.cpp post-loop block).
//   transform_shuffle_s : sum of wall around `transform_shuffle` calls (one per entry).
//   integrate_ii_s      : sum of wall around `integrate_ii` calls (one per (entry,monomial,sub)).
//   loop_residual_s     : entry-body total minus (transform_shuffle + integrate_ii)
//                          — covers expand_zero_word_in_ctx, series_expansion,
//                            log_zero_row_as_poly, polesZero/polesInf bump, etc.
double read_transform_shuffle_s();
double read_integrate_ii_s();
double read_loop_residual_s();
// Phase-d15 follow-up: subset-of-integrate_ii_s timer for the
// partial_fractions call in primitive.cpp's integrate_ii.
double read_partial_fractions_s();
// Phase-d15 deeper drill: subset-of-partial_fractions_s timer for the
// linear_factors (FLINT fmpq_mpoly_factor) call inside partial_fractions.
double read_linear_factors_s();
// Phase-d15 deeper drill (round 3): subset-of-linear_factors_s timer
// for just the FLINT `fmpq_mpoly_factor` call (covers both narrow and
// wide branches), plus per-step cache hit/miss counters. The implied
// "lookup + narrow-ctx setup + transplant" overhead inside linear_factors
// is `linear_factors_s - lf_flint_factor_s`.
double read_lf_flint_factor_s();
long   read_lf_cache_hits();
long   read_lf_cache_misses();
// Phase-d15 deeper drill (round 4): degree-bucketed split of the
// FLINT factor wall and miss count. deg3+ wall and count are
// derivable as `lf_flint_factor_s - deg1_s - deg2_s` and
// `lf_cache_misses - miss_deg1 - miss_deg2`.
double read_lf_flint_deg1_s();
double read_lf_flint_deg2_s();
long   read_lf_miss_deg1();
long   read_lf_miss_deg2();
// Phase-d15 deeper drill (round 5): deg-3+ output classifier. Splits
// the deg-3+ FLINT wall into "all-linear factors out" (the case
// partial_fractions consumes — irrelevant for an irreducibility probe)
// vs "has at least one nonlinear factor" (the case the probe would
// shortcut — expected ~0 since partial_fractions throws downstream).
long   read_lf_d3p_all_linear_count();
double read_lf_d3p_all_linear_s();
long   read_lf_d3p_has_nonlinear_count();
double read_lf_d3p_has_nonlinear_s();
// Phase-d15 deeper drill (round 6): split deg-3+ all-linear by whether
// any factor has multiplicity > 1. Sizes the GCD-peel-off lever:
// repeated_s near zero => GCD plan dead, inputs are already squarefree.
long   read_lf_d3p_squarefree_count();
double read_lf_d3p_squarefree_s();
long   read_lf_d3p_repeated_count();
double read_lf_d3p_repeated_s();
// Phase-d15 deeper drill (round 7): squarefree-first path counters.
// Active when HF_LF_SQF=1, all zeros otherwise.
double read_lf_sqf_total_s();
double read_lf_sqf_decomp_s();
double read_lf_sqf_inner_factor_s();
long   read_lf_sqf_calls();
long   read_lf_sqf_inner_factor_calls();
long   read_lf_sqf_bailouts();

// 2026-04-26 sanity-matrix follow-up: integrate_ii body sub-timers.
double read_bump_lookup_s();
double read_bump_addto_s();
double read_push_ibp_s();
double read_antideriv_s();
long   read_bump_calls();
// 2026-04-27 Lever-1 extended: split `bump_addto_s` into the new-row
// emplace branch and the existing-row Rat-add branch.
//   bump_emplace_s     : rows.push_back(Cell{w, c}) on miss
//   bump_rat_add_s     : rows[..].coef = rows[..].coef + c on hit
//   bump_rat_add_calls : hits (else-branch entries) =
//                          bump_calls - bump_emplace_calls
double read_bump_emplace_s();
double read_bump_rat_add_s();
long   read_bump_rat_add_calls();
// 2026-04-26 P1' pre-gating: per-`integrate_ii`-invocation distinct-
// denominator accounting. The two counters bound the bucketing-collapsible
// share of within-invocation linear_factors traffic. See
// primitive.hpp for definitions.
long   read_pf_calls_in_loop();
long   read_pf_unique_dens();
// 2026-04-26 RA pre-gating: per-invocation rows.size() at loop exit,
// summed across invocations on this step. Combined with `bump_calls`,
// bounds the RatAccumulator-collapsible share of `bump_addto_s`.
long   read_bump_unique_rows();
// 2026-04-26 lock-contention proxy: held-time inside g_linear_factors_mu
// (sum across threads). Compare to step wall_s for the contention ratio.
double read_lf_lock_held_s();
// iter-37 lock-acquire wait probe: queueing time before shard mutex
// is held (sibling of lf_lock_held_s; sum across threads). Default-OFF
// env-gated under HF_LF_LOCK_WAIT_PROFILE=1; reads zero when disabled.
double read_lf_lock_wait_s();
// 2026-04-26 cache_key_build direct measurement: per-step wall inside
// `linear_factors_cache_key()` + sqf-prefix concat (sum across threads).
double read_lf_cache_key_build_s();
// 2026-04-29 (Probe 2 — HF/Maple investigation): four sub-timers
// splitting the unaccounted post-FLINT-factor extraction inside
// `linear_factors`. Sum across threads = total per-step CPU. See
// linear_factors.hpp for the per-thread storage / init / reset / sum.
double read_lf_post_transplant_s();
double read_lf_post_rat_ctor_s();
double read_lf_post_constant_to_string_s();
double read_lf_post_clone_from_raw_s();
// 2026-04-29 (Probe 3 — HF/Maple investigation): three sub-timers
// attributing the integrate_ii body residual on step 7 outside
// partial_fractions / bump_addto / push_ibp / antideriv. See
// primitive.hpp for the comment block.
double read_ii_queue_copy_s();
double read_ii_pole_arith_s();
double read_ii_pole_word_ctor_s();
// 2026-04-30 (Probe 4 — axis-E omp_post_merge attribution): split the
// post-merge wall (97.5 wall-s aggregate) into its four sub-phases.
double read_omp_collect_into_flat_s();
double read_omp_merge_sort_s();
double read_omp_parallel_canonicalize_s();
double read_omp_serial_assembly_s();
// 2026-04-30 (Tier 1.4a η probe): per-step max/min/sum per-thread
// wall in the post-merge parallel-for. η = sum / (n_threads * max).
// If η < 0.7 the bottleneck is heavy-slot load imbalance, not the
// per-call canonicalize cost.
double read_pm_canon_max_per_thread_s();
double read_pm_canon_min_per_thread_s();
double read_pm_canon_sum_per_thread_s();
// 2026-04-27 (3l3pt profile-deepening): narrow-vs-wide branch of
// `reduce_inplace` (every Rat ctor and arithmetic op — NOT just
// Rat::add; renamed from rat_add_* after the reviewer correction).
//   reduce_narrow_s     : wall in narrow-ctx hoist (transplant +
//                           gcd_cofactors in narrow ctx + transplant back).
//   reduce_wide_s       : wall in wide-ctx fast path (gcd_cofactors
//                           directly in the input ctx).
//   reduce_narrow_calls : count of narrow-ctx path traversals.
//   reduce_wide_calls   : count of wide-ctx path traversals.
//   reduce_zero_calls   : degenerate 0/den short-circuits.
// Hypothesis: wide-ctx GCD dominates step-3 wall on 3l3pt.
// FALSIFIED 2026-04-27 — wide-ctx ≤ 0.4 s in every step. Narrow-
// ctx hoist is the dominant reducer cost.
double read_reduce_narrow_s();
double read_reduce_wide_s();
long   read_reduce_narrow_calls();
long   read_reduce_wide_calls();
long   read_reduce_zero_calls();
// 2026-05-02 (HF FLINT-pool experiment, re-scoped to GCD): isolates
// fmpq_mpoly_gcd_cofactors call wall from the surrounding transplant
// + canonicalization in reduce_inplace.
double read_gcd_cofactors_s();
long   read_gcd_cofactors_calls();
// 2026-05-02 (Phase-0-GCD follow-up): non-GCD sub-timers in the
// narrow path of reduce_inplace. See rat.cpp.
double read_rn_used_vars_s();
double read_rn_setup_s();
double read_rn_post_s();
long   read_rat_mul_calls();
long   read_rat_sub_calls();
long   read_rat_div_calls();
// 2026-04-27 Lever-1 extended: per-Poly-op timers inside
// `Rat::add` operator+, scoped to disambiguate the 125 s step-3
// "dark mass" between bump_addto_s and reduce_narrow_s.
//   rat_add_polymul_s : wall in the three wide-ctx Poly mults
//                         (num*b.den, b.num*den, den*b.den)
//   rat_add_polyadd_s : wall in the wide-ctx Poly addition
//                         ((num*b.den) + (b.num*den))
//   rat_add_calls     : count of Rat::add invocations
double read_rat_add_polymul_s();
double read_rat_add_polyadd_s();
long   read_rat_add_calls();
// 2026-05-03 (chain 20, PF3 prep): per-backend wall + calls at the
// Rat::add / operator+= dispatch fork. legacy = cross-mult+gcd path;
// via_qu = chain-11 add_via_q_underscore Tier A1 path.
double read_rat_add_legacy_wall_s();
double read_rat_add_via_qu_wall_s();
long   read_rat_add_legacy_calls();
long   read_rat_add_via_qu_calls();
// 2026-04-27 Avenue A: narrow-vs-wide path counters/timers for the
// narrow-ctx hoist in `Poly::mul`. Hypothesis: 3l3pt step-3
// rat_add_polymul_s = 165 s on a 30-var wide ctx; bump-internal mults
// touch 5-10 vars → narrow-ctx mult is ~2× faster at the
// packed-exponent level.  Reviewer-recalibrated savings estimate:
// 15-80 s on step 3 (median 35 s).
//   mul_narrow_s     : wall in narrow-ctx hoist (transplant +
//                        fmpq_mpoly_mul in narrow ctx + transplant back).
//   mul_wide_s       : wall in wide-ctx fast path (fmpq_mpoly_mul in
//                        the input ctx; size_gate cleared narrow path).
//   mul_narrow_calls : count of narrow-ctx path traversals.
//   mul_wide_calls   : count of wide-ctx path traversals (worth_narrowing
//                        was false — typically because used_count*4 >=
//                        nvars_wide).
//   mul_gated_calls  : count of size_gate-rejected calls (small polys
//                        where transplant would be net loss, len_total
//                        < kNarrowMulMinLen).
double read_mul_narrow_s();
double read_mul_wide_s();
long   read_mul_narrow_calls();
long   read_mul_wide_calls();
long   read_mul_gated_calls();
// Reviewer round 7 distribution counters.
// la·lb log bins:  i=0:<1k, i=1:<4k, i=2:<16k, i=3:<64k, i=4:<256k, i=5:≥256k
// U bins:          i=0:1, i=1:2, i=2:3, i=3:4-7, i=4:8+
long   read_nbin_lalb_count(int i);
double read_nbin_lalb_us(int i);
double read_nbin_lalb_max(int i);
long   read_nbin_u_count(int i);
double read_nbin_u_us(int i);
// 2026-04-27 (3l3pt profile-deepening): sub-timers inside
// PolesBucket::bump (file-local in integration_step.cpp).
// Decomposes bucket_bump_s into 5 per-line sub-costs.  On heavy
// 3l3pt steps bucket_bump_s is 95-99% of loop_residual_s; the
// dominant sub-line is presumably bucket_symcoef_add_s (the
// SymCoef::add → from_monomials → canonicalize → Rat::Rat chain
// on existing-row hits).  Used to confirm the attack target
// before any code change.
//   bucket_canon_regkey_s : canonicalize_regkey(key)
//   bucket_struct_hash_s  : regkey_struct_hash(canon)
//   bucket_index_find_s   : index.find(k)
//   bucket_symcoef_add_s  : terms[..].coef + coef (existing-row hit)
//   bucket_emplace_s      : new-row insert (index[k]=... + push_back)
double read_bucket_canon_regkey_s();
double read_bucket_struct_hash_s();
double read_bucket_index_find_s();
double read_bucket_symcoef_add_s();
double read_bucket_emplace_s();
// 2026-04-28 (Lever-D gate): PolesBucket collision counters + merge
// per-slot in/out monomial counts. Decision rule: ship Lever D iff
// mean_K (= bucket_collision_calls / merge_slots) >= 8 AND
// mean_post_terms / mean_pre_terms <= 1.3.
long   read_bucket_collision_calls();
long   read_bucket_collision_pre_terms();
long   read_bucket_collision_post_terms();
long   read_merge_in_terms();
long   read_merge_out_terms();
long   read_merge_slots();
// 2026-04-26 loop_residual_s drill: post-integrate_ii pole-expansion
// triage (the entry-body work outside transform_shuffle and integrate_ii).
//   pole_zero_expand_s : Zero-expansion block (expand_zero_word_in_ctx,
//                          log_zero_row_as_poly, series_expansion,
//                          rat_var0_coefficient, polesZero.bump).
//   pole_inf_expand_s  : Inf-expansion block (substitute_var_reciprocal,
//                          regzero_word_in_ctx, expand_inf_word_in_ctx,
//                          polesInf.bump).
// The two `expand` timers are mutually exclusive across primitives.
//   bucket_bump_s      : subset of (zero+inf) covering only the
//                          polesZero.bump / polesInf.bump calls.
double read_pole_zero_expand_s();
double read_pole_inf_expand_s();
// 2026-05-01 (Branch E pre-flight): sub-bucket leaves of pole_inf_expand_s.
// Sum across an OMP barrier inside integration_step. Together they
// account for the four named primitive calls inside the inf-expansion
// block; the remainder pole_inf_expand_s - sum(4) covers
// regzero_word_in_ctx, log_zero_row_as_poly, Rat::operator*,
// polesInf.bump, and loop overhead.
double read_pie_substitute_var_reciprocal_s();
double read_pie_series_expansion_s();
double read_pie_expand_inf_word_in_ctx_s();
double read_pie_rat_var0_coef_s();
// 2026-05-01 (Tier 3 Phase-0 lazy-pair diagnostic): per-step
// reduce_inplace nterm-blowup metrics.  Env-gated by
// HF_REDUCE_NTERM_LOG=1.  Reading guide:
//   avg_pre  = pre_total  / calls   (mean num+den nterms before GCD)
//   avg_post = post_total / calls   (mean num+den nterms after GCD)
//   shrink   = post / pre           (1 = GCD does nothing → lazy free;
//                                    << 1 = GCD shrinks → lazy blows up)
//   max_pre / max_post: single-call extremes per step.
long   read_reduce_nterm_calls();
long   read_reduce_nterm_pre_total();
long   read_reduce_nterm_post_total();
long   read_reduce_nterm_pre_max();
long   read_reduce_nterm_post_max();
// 2026-05-01 (Tier 3 refined lever): count of wide-ctx GCD calls
// that fell through specifically because of the raised size-gate
// threshold (HF_REDUCE_SIZE_GATE_MIN > 4).
long   read_reduce_wide_smallfall_calls();
double read_bucket_bump_s();
// 2026-04-26 (Lever-C scout): OMP/serial-merge diagnostic.
//   omp_parallel_wall_s : wall around `#pragma omp parallel for`.
//   omp_post_merge_s    : wall around the serial collect/canonicalize
//                          that runs after the implicit barrier.
// Combined with the existing `entry_per_thread`-derived totals
// (transform_shuffle_s + integrate_ii_s + loop_residual_s ≈
// sum(entry_per_thread)), the consumer derives:
//   eta = (sum_entry) / (n_threads * omp_parallel_wall_s)
//   barrier_idle_s = n_threads * omp_parallel_wall_s - sum_entry
double read_omp_parallel_wall_s();
double read_omp_post_merge_s();
// 2026-04-27 (Lever A): per-step partial_fractions cache
// hits/misses/collisions summed across worker threads (atomic-update
// inside an OMP team after the parallel region).
//   hit rate = hits / (hits + misses + collisions)
//   collisions = stored input != current input on hash hit (recompute,
//   cache untouched). Tracks the hash-collision-protection path added
//   2026-04-27 to defeat FNV-1a top-nibble collisions on content-only-
//   different polynomials.
long   read_pf_cache_hits_step();
long   read_pf_cache_misses_step();
long   read_pf_cache_collisions_step();
// Reviewer #13 Hole 3: per-step entry_per_thread spread. Compare
// against entry_min for load imbalance:
//   spread = entry_max - entry_min
//   if spread / entry_max > 0.3 → load imbalance, schedule(dynamic, k)
//   if spread / entry_max < 0.1 → balanced, schedule choice ~ wash
double read_entry_max_per_thread_s();
double read_entry_min_per_thread_s();

}  // namespace hyperflint
