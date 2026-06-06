// LinearFactors -- port of HyperIntica's LinearFactors[p, var].
//
// Factor a polynomial in a designated "target" variable, and return a
// list of { multiplicity, pole-location } pairs for each degree-1
// factor in that variable. Degree-1 factors are the ones the
// HyperInt/HyperIntica pipeline can handle directly; higher-degree
// factors are reported separately in Phase 2 so the caller knows they
// exist. Algebraic-extension handling (Wm/Wp introduction for degree-2)
// lives in Phase 7.

#pragma once

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"

#include <memory>
#include <vector>

namespace hyperflint {

class ZWTable;  // fwd-decl; full def at hyperflint/core/zw_table.hpp.

struct LinearFactor {
    long multiplicity;
    // Pole location: the root c such that the factor is (var - c), up
    // to an overall constant pulled into `constant` below.  For a
    // factor (lc*var + c0) of multiplicity m, the pole is -c0/lc, which
    // in general is a Rat over the remaining variables (not just a
    // polynomial).
    Rat pole;
};

struct NonlinearFactor {
    long multiplicity;
    Poly polynomial;       // the factor itself, as it came from factor()
    long degree_in_var;    // >= 2
};

struct LinearFactorization {
    // Leading rational constant (the "content" in FactorList's first slot)
    // plus any constant-in-var factors absorbed.
    std::string constant;

    // Linear-in-var factors, listed in factor order.
    std::vector<LinearFactor>    linear;

    // Factors of degree >= 2 in var.  Phase 2 reports them as-is;
    // Phase 7 will extend to Wm/Wp for deg == 2 when the algebraic-
    // letters path is enabled.
    std::vector<NonlinearFactor> nonlinear;
};

// Port of HyperIntica's LinearFactors[p, var]. The
// `introduce_algebraic_letters` flag mirrors Mma's
// `$HyperIntroduceAlgebraicLetters`: when true, an irreducible
// degree-2 factor `lc·x² + b·x + c` allocates a fresh
// AlgebraicLetterEntry and emits two LinearFactors with poles
// `Wm_<idx>` and `Wp_<idx>`. The Wm/Wp atom names must already be
// present in `p.ctx()`'s variable list (use
// build_algebraic_letter_var_list at PolyCtx construction time).
//
// Caller supplies the target variable by its index in p's PolyCtx.
//
// 2026-04-29 (axis-C-lf-constant-defer): `compute_constant` gates the
// `out.constant = X.to_string()` writes. Probe-2 measurement on
// 3l3pt parity-1 ord_1_face_1 pinned 354 of the 357 unaccounted CPU-s
// in `linear_factors_s` to those four to_string() calls (99.3% — see
// notes/2026-04-29_hf-vs-maple-investigation.md). The integration hot
// path (`partial_fractions`, `transform`) does NOT read `out.constant`
// and so defaults to `false` — the field is left default-constructed
// (empty string). The standalone bridge-CLI `linear_factors` op
// (`bridge/cli/main.cpp:handle_linear_factors`) needs the field for
// `compare.py` cross-tests and passes `true` explicitly.
//
// `compute_constant` is mixed into the cache key (via
// `linear_factors_cache_key`) so a cached entry produced under
// `compute_constant=false` cannot satisfy a later `=true` lookup, and
// vice-versa. Defensive: the cache is cleared per integration step
// today, but this future-proofs against any caller mixing modes
// inside one cache lifetime (e.g. LibraryLink-mode bridge calls).
// Iter-52 C0c.1 (sites 6 + 7 + 10): the per-call `make_shared<ZWTable>`
// allocation that previously lived inside this function's
// `apply_v1_roundtrip` cache-HIT lambda is hoisted to a caller-provided
// `std::shared_ptr<ZWTable>` (per design v2 §3.5a row 4 + §3.6a; iter-50
// MEMO §3-§6 site-inventory; iter-51 §6.5 ratification of Q1 = Option A
// = mandatory ABI break, consistent with iter-42 site-4 commit
// `5f51b7d28` precedent). The parameter is mandatory: callers must
// construct (or thread through) a `std::shared_ptr<ZWTable>` before
// calling. Bridge/test entry points construct a fresh transient
// per-call; the production integration chain threads the persistent
// driver-entry ZWTable allocated at `hyper_int.cpp:~463` through
// `partial_fractions` / `transform_word_impl` / `integrate_ii` /
// `integration_step` (the C0b.1/2/3/5 deferred sites carry the
// parameter without lambda-body change — "thread now, kill later").
LinearFactorization linear_factors(const Poly& p, size_t var_idx,
                                    std::shared_ptr<ZWTable> zw_tab,
                                    bool introduce_algebraic_letters = false,
                                    bool compute_constant = false);

// Drop the thread-local linear-factors cache. Call at the top of
// `integration_step` (mirrors HyperIntica.wl:4730's
// `$LinearFactorsCache = <||>`) to bound cache memory to a single
// integration step's factoring workload, and to ensure Polys in
// cache entries don't outlive their PolyCtx across steps with
// different contexts.
void clear_linear_factors_cache();

// 2026-05-05 (path-A diagnostic — Probe 3a). Walks all sharded LF cache
// maps under their per-shard mutexes; sums Poly::total_bytes() across
// every cached LinearFactorization's NonlinearFactor.polynomial AND
// every LinearFactor.pole's Rat (num+den Polys). Returns:
//   total_bytes  — sum of Poly::total_bytes() across cached state
//   entry_count  — total number of cache entries
//   poly_count   — total number of distinct Polys walked
// Default-OFF: only call this from probe code (held-time is O(N_polys)).
struct LFCacheResidency {
    size_t total_bytes  = 0;
    size_t entry_count  = 0;
    size_t poly_count   = 0;
    size_t nonlin_polys = 0;  // NonlinearFactor.polynomial bytes
    size_t pole_rats    = 0;  // LinearFactor.pole bytes
};
LFCacheResidency linear_factors_cache_residency();

// Phase-d15 deeper drill (round 3): split linear_factors wall into
// the FLINT `fmpq_mpoly_factor` call (`lf_flint_factor_s`) versus
// everything else (cache lookup mutex, narrow-ctx setup, post-
// processing, transplants). Plus per-call hit/miss counters so the
// driver can surface the cache-effectiveness alongside the timer.
//
// Thread-safety mirrors the partial_fractions per-thread storage:
// each worker writes to its own slot inside the OMP region; the
// driver `init_*` / `reset_*` before the parallel-for and `sum_*`
// after the implicit barrier.
void   init_lf_flint_factor_per_thread(int n_threads);
void   reset_lf_flint_factor_per_thread();
double sum_lf_flint_factor_per_thread();

void     init_lf_cache_hits_per_thread(int n_threads);
void     reset_lf_cache_hits_per_thread();
long     sum_lf_cache_hits_per_thread();

void     init_lf_cache_misses_per_thread(int n_threads);
void     reset_lf_cache_misses_per_thread();
long     sum_lf_cache_misses_per_thread();

// 2026-04-26 lock-contention proxy: per-thread accumulated held-time
// inside `g_linear_factors_mu` (sum across threads = wall-time the
// lock was held). Surfaced in HF_STEP_TRACE JSON as `lf_lock_held_s`.
// Sharded-mutex/Stage-2 GO threshold: held_s / wall_s > 0.30; NO-GO
// threshold: held_s / wall_s < 0.05.
void   init_lf_lock_held_per_thread(int n_threads);
void   reset_lf_lock_held_per_thread();
double sum_lf_lock_held_per_thread();

// iter-37 lock-acquire wait probe: per-thread accumulated wait
// time on shard mutex acquire (sibling of lf_lock_held_s, which
// covers held-time once acquired). Sum across threads = total
// queueing wall on shard mutexes. Default-OFF; env-gated under
// HF_LF_LOCK_WAIT_PROFILE=1. When the env-gate is off, the probe
// is one branch on a cached static int per lock_guard ctor.
// Surfaced in HF_STEP_TRACE JSON as `lf_lock_wait_s`.
//
// Reconciliation target (iter-37 MEMO):
//   linear_factors_s ≈ lf_lock_held_s + lf_lock_wait_s
//                    + lf_flint_factor_s + lf_cache_key_build_s
// (within ~1%; residual = post-FLINT extraction + cache HIT
//  return + sundry fast-path overhead).
void   init_lf_lock_wait_per_thread(int n_threads);
void   reset_lf_lock_wait_per_thread();
double sum_lf_lock_wait_per_thread();

// 2026-04-26 cache_key_build instrumentation: per-thread accumulated
// wall inside `linear_factors_cache_key()` + sqf-prefix concat. Direct
// measurement of the path the structural-hash lever targets. Surfaced
// in HF_STEP_TRACE JSON as `lf_cache_key_build_s`.
void   init_lf_cache_key_build_per_thread(int n_threads);
void   reset_lf_cache_key_build_per_thread();
double sum_lf_cache_key_build_per_thread();

// 2026-04-29 (Probe 2 — HF/Maple investigation): split the unaccounted
// post-FLINT-factor extraction in `linear_factors`. By direct algebra
// on the 3l3pt parity-1 ord_1_face_1 trace, ~289 CPU-s of step-7
// `linear_factors_s` lives in the lock-free section between
// fmpq_mpoly_factor return and cache_store_locked invocation. The four
// timers below split that residual into:
//   lf_post_transplant_s          : Poly::transplant calls (narrow→wide
//                                   pole/constant lifting, both branches).
//   lf_post_rat_ctor_s            : Rat pole(...) constructor + Rat::parse
//                                   (algebraic-letter path).
//   lf_post_constant_to_string_s  : *.to_string() on the running constant.
//   lf_post_clone_from_raw_s      : clone_from_raw() for FLINT factor list.
// Surfaced in HF_STEP_TRACE JSON under those names. Decision tree in
// notes/2026-04-29_hf-vs-maple-investigation.md §5 Probe 2.
void   init_lf_post_transplant_per_thread(int n_threads);
void   reset_lf_post_transplant_per_thread();
double sum_lf_post_transplant_per_thread();

void   init_lf_post_rat_ctor_per_thread(int n_threads);
void   reset_lf_post_rat_ctor_per_thread();
double sum_lf_post_rat_ctor_per_thread();

void   init_lf_post_constant_to_string_per_thread(int n_threads);
void   reset_lf_post_constant_to_string_per_thread();
double sum_lf_post_constant_to_string_per_thread();

void   init_lf_post_clone_from_raw_per_thread(int n_threads);
void   reset_lf_post_clone_from_raw_per_thread();
double sum_lf_post_clone_from_raw_per_thread();

// Phase-d15 deeper drill (round 4): degree-bucketed histogram of
// cache-miss inputs and the FLINT wall they consume. Buckets:
//   deg1     : input has degree-in-var == 1 (the shape-aware fast-
//              path target — emitting the linear factor directly
//              skips fmpq_mpoly_factor entirely).
//   deg2     : input has degree-in-var == 2 (Wm/Wp algebraic-letter
//              candidates when introduce_algebraic_letters=true).
// deg3+ wall and count are derivable as
//   lf_flint_factor_s - deg1_s - deg2_s,
//   lf_cache_misses   - miss_deg1 - miss_deg2.
void   init_lf_flint_deg1_per_thread(int n_threads);
void   reset_lf_flint_deg1_per_thread();
double sum_lf_flint_deg1_per_thread();

void   init_lf_flint_deg2_per_thread(int n_threads);
void   reset_lf_flint_deg2_per_thread();
double sum_lf_flint_deg2_per_thread();

void   init_lf_miss_deg1_per_thread(int n_threads);
void   reset_lf_miss_deg1_per_thread();
long   sum_lf_miss_deg1_per_thread();

void   init_lf_miss_deg2_per_thread(int n_threads);
void   reset_lf_miss_deg2_per_thread();
long   sum_lf_miss_deg2_per_thread();

// Phase-d15 deeper drill (round 5): classify deg-3+ FLINT misses by
// output shape. Settles whether the proposed mod-p irreducibility
// probe could ever help: probe shortcuts a deg-3+ call iff FLINT
// would have returned a single irreducible nonlinear factor, but
// `partial_fractions` (the dominant caller) throws on any nonlinear
// factor, so on a successful integration `has_nonlinear` should be
// effectively zero. `all_linear` is the case `partial_fractions`
// actually consumes, where the probe answers the wrong question.
void   init_lf_d3p_all_linear_count_per_thread(int n_threads);
void   reset_lf_d3p_all_linear_count_per_thread();
long   sum_lf_d3p_all_linear_count_per_thread();

void   init_lf_d3p_all_linear_s_per_thread(int n_threads);
void   reset_lf_d3p_all_linear_s_per_thread();
double sum_lf_d3p_all_linear_s_per_thread();

void   init_lf_d3p_has_nonlinear_count_per_thread(int n_threads);
void   reset_lf_d3p_has_nonlinear_count_per_thread();
long   sum_lf_d3p_has_nonlinear_count_per_thread();

void   init_lf_d3p_has_nonlinear_s_per_thread(int n_threads);
void   reset_lf_d3p_has_nonlinear_s_per_thread();
double sum_lf_d3p_has_nonlinear_s_per_thread();

// Phase-d15 deeper drill (round 6): within the deg-3+ all-linear
// case, split by whether the FLINT factorization had any factor
// of multiplicity > 1. Sizes the GCD-peel-off lever: if the inputs
// are mostly squarefree, GCD(f, df/dvar) yields no reduction and
// the peel-off is a net regression; if a substantial fraction has
// repeated roots, the squarefree-then-factor approach pays off.
void   init_lf_d3p_squarefree_count_per_thread(int n_threads);
void   reset_lf_d3p_squarefree_count_per_thread();
long   sum_lf_d3p_squarefree_count_per_thread();

void   init_lf_d3p_squarefree_s_per_thread(int n_threads);
void   reset_lf_d3p_squarefree_s_per_thread();
double sum_lf_d3p_squarefree_s_per_thread();

void   init_lf_d3p_repeated_count_per_thread(int n_threads);
void   reset_lf_d3p_repeated_count_per_thread();
long   sum_lf_d3p_repeated_count_per_thread();

void   init_lf_d3p_repeated_s_per_thread(int n_threads);
void   reset_lf_d3p_repeated_s_per_thread();
double sum_lf_d3p_repeated_s_per_thread();

// Phase-d15 deeper drill (round 7): squarefree-first path (HF_LF_SQF=1).
// Replaces the single fmpq_mpoly_factor on deg-3+ inputs with a
// factor_squarefree decomposition followed by per-base handling.
// Counters expose the wall split (total / decomp / inner_factor) and
// call counts so we can verify the lever pays off.
void   init_lf_sqf_total_s_per_thread(int n_threads);
void   reset_lf_sqf_total_s_per_thread();
double sum_lf_sqf_total_s_per_thread();

void   init_lf_sqf_decomp_s_per_thread(int n_threads);
void   reset_lf_sqf_decomp_s_per_thread();
double sum_lf_sqf_decomp_s_per_thread();

void   init_lf_sqf_inner_factor_s_per_thread(int n_threads);
void   reset_lf_sqf_inner_factor_s_per_thread();
double sum_lf_sqf_inner_factor_s_per_thread();

void   init_lf_sqf_calls_per_thread(int n_threads);
void   reset_lf_sqf_calls_per_thread();
long   sum_lf_sqf_calls_per_thread();

void   init_lf_sqf_inner_factor_calls_per_thread(int n_threads);
void   reset_lf_sqf_inner_factor_calls_per_thread();
long   sum_lf_sqf_inner_factor_calls_per_thread();

// Counts squarefree-first calls that bailed out to the standard FLINT
// path because the squarefree decomposition produced a base of degree
// >= 2 in the integration variable. Bailouts are strictly more
// expensive than baseline (we paid factor_squarefree, then fall
// through to a full fmpq_mpoly_factor that internally redoes the
// squarefree pass). Round-6 reported 100% repeated-multiplicity on
// pentagon-gauge and Smirnov tst* — but "all linear factors with
// repeated multiplicity" still yields a deg-1 squarefree base, while
// "two distinct roots with the same multiplicity (e.g. (x-a)²(x-b)²)"
// yields a deg-2 squarefree base and a bailout. This counter
// distinguishes the two regimes.
void   init_lf_sqf_bailouts_per_thread(int n_threads);
void   reset_lf_sqf_bailouts_per_thread();
long   sum_lf_sqf_bailouts_per_thread();

// ---------- HyperIntica-parity guard: forbidden-vars for Wm/Wp ----------
//
// HyperIntica refuses to introduce a Wm[i]/Wp[i] algebraic-letter pair
// whose definition still depends on a remaining (un-integrated) Feynman
// parameter.  The resulting partial integration would leave the result
// symbolically-dependent on a Feynman parameter that downstream numerics
// (ginsh, pySecDec) cannot substitute.  HF previously accepted such
// letters silently, returning a structurally-valid SeriesData that fails
// at verification time.
//
// Exposed as a thread-local RAII scope (rather than a new parameter on
// `linear_factors`) because the call chain
//   integration_step -> integrate_ii -> partial_fractions -> linear_factors
// is several layers deep, and the outermost integration loop is the only
// site that knows the remaining-var schedule.  Default (no scope active)
// preserves legacy behavior bit-for-bit.
//
// When an LFForbiddenVarsScope is active and `linear_factors` would
// otherwise allocate a Wm/Wp pair for a degree-2 factor in `var_idx`
// whose `base` polynomial uses any forbidden var (other than var_idx
// itself), the factor is instead pushed to `out.nonlinear`, signalling
// the caller that linear reducibility is not achievable in the
// polynomial-plus-{Wm,Wp} alphabet.
class LFForbiddenVarsScope {
public:
    explicit LFForbiddenVarsScope(const std::vector<size_t>& forbidden);
    ~LFForbiddenVarsScope();
    LFForbiddenVarsScope(const LFForbiddenVarsScope&) = delete;
    LFForbiddenVarsScope& operator=(const LFForbiddenVarsScope&) = delete;
};

// Read-only inspector --- primarily for tests / instrumentation.  Returns
// nullptr when no scope is active on the current thread.
const std::vector<size_t>* lf_current_forbidden_vars();

}  // namespace hyperflint
