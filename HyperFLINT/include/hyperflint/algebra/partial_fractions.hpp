// PartialFractions -- port of HyperIntica's PartialFractions[f, var].
//
// Given a rational function f (as a Rat) and a target variable, return:
//   { polynomial_part,  [ (pole_a, [c_a1, c_a2, ..., c_am]) for each pole a ] }
//
// where polynomial_part is the quotient when numerator degree >= denominator
// degree in var, and each c_aj is the partial-fraction coefficient of
// 1/(var - a)^j. The layout mirrors HyperIntica's output structure
// HyperIntica.wl:3210ff.
//
// Phase 2c scope:
//   - Linear poles (including higher multiplicities handled by the
//     standard derivative formula).
//   - Non-linear denominator factors: throws std::runtime_error.
//     Phase 7's algebraic-letter path will extend degree-2 factors to
//     Wm/Wp pairs; degree >= 3 remains unsupported until we add
//     number-field factoring (Antic).

#pragma once

#include "hyperflint/core/rat.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace hyperflint {

class ZWTable;  // fwd-decl; full def at hyperflint/core/zw_table.hpp.

struct PartialFractionPole {
    Rat pole;                  // pole location a, as a Rat in the other vars
    long multiplicity;         // m >= 1
    std::vector<Rat> coefs;    // c_1, c_2, ..., c_m  (length == multiplicity)
};

struct PartialFractionization {
    // Polynomial part (in var, with Rat-valued coefficients in the
    // other variables). We represent it as a Rat since in general its
    // coefficients are rational functions.
    Rat polynomial_part;

    // One entry per distinct pole, in the factor order of
    // fmpq_mpoly_factor.
    std::vector<PartialFractionPole> poles;
};

// Phase 7-vi-a: when `introduce_algebraic_letters` is true, degree-2
// irreducible factors in the denominator are split into Wm_<i>/Wp_<i>
// atom pairs via linear_factors's algebraic-letter branch. Default
// false preserves the 222-fixture suite exactly. When true, the caller
// must have pre-allocated the Wm/Wp/WmOverWp/sqrt_disc atom pool in
// the PolyCtx (build_algebraic_letter_var_list).
//
// Iter-52 C0c.1 (Increment β, sites 6 + 7 + 10 atomic ship): mandatory
// `std::shared_ptr<ZWTable> zw_tab` parameter (Option A; ratified at
// iter-50 MEMO §6 Q1 + iter-51 §6.5). Threaded through to the inner
// `linear_factors` call (replacing the iter-52a caller-side transient).
// Allowed to be null in default-OFF (HF_USE_SCALAR_REP=0) mode; the
// site-6 lambda body never dereferences it because
// `runtime::scalar_rep_enabled()` early-returns first. Production
// integration calls thread the persistent driver-entry ZWTable from
// `hyper_int.cpp:~463`; bridge/CLI handlers and tests construct fresh
// transients per-call.
PartialFractionization partial_fractions(
    const Rat& f, size_t var_idx,
    std::shared_ptr<ZWTable> zw_tab,
    bool introduce_algebraic_letters = false);

// Phase-d15 follow-up: per-primitive timer for the linear_factors call
// (FLINT fmpq_mpoly_factor) inside partial_fractions. Same per-thread
// accumulator vector pattern as integrate_ii's partial_fractions timer
// — see primitive.hpp / primitive.cpp for the model.
void   init_linear_factors_per_thread(int n_threads);
void   reset_linear_factors_per_thread();
double sum_linear_factors_per_thread();

// 2026-04-27 (Lever A — partial_fractions option remember): per-thread
// memoization mirroring HI's `option remember` on `partialFractions`.
// integration_step calls `bump_pf_cache_generation()` at step entry,
// which makes every worker's thread_local cache lazily invalidate on
// its next access. Same pattern as the linear_factors cache flush.
//
// Kill switch: `HF_DISABLE_PF_CACHE=1` forces the uncached path.
//
// The cache stores the input `(num, den)` Polys alongside every
// cached value and FLINT-equality-compares stored vs current input
// on every hit (correctness fix for FNV-1a collisions on content-
// only-different polynomials; see `partial_fractions.cpp` for the
// bisect history).  Three per-thread counters are exposed:
//   read_pf_cache_hits()       — input matched, cached value used.
//   read_pf_cache_misses()     — input not in cache, fresh recompute.
//   read_pf_cache_collisions() — input in cache slot but stored input
//                                differs (hash collision); fresh
//                                recompute returned, cache untouched.
// Callers aggregating cache effectiveness should treat
// `(misses + collisions)` as "wall paid for fresh compute".
void bump_pf_cache_generation();
long read_pf_cache_hits();
long read_pf_cache_misses();
long read_pf_cache_collisions();

// iter-17 (Track 0.4 probe; spec
// pfrac-row storage spec v4 (internal development notes, iter-14/16)):
// per-bucket-per-container heap-byte attribution for the
// pfrac-row storage lever. Default-OFF; env-gated via
// `HF_PF_STORAGE_STATS=1` (`HF_PF_STORAGE_STATS_THROTTLE` controls
// emit cadence per worker, default 1 = emit on every probe call;
// `HF_PF_STORAGE_DEBUG_ASSERTS=1` enables capacity-vs-payload audit
// hooks for spec v4 audit rows 5+6).
//
// Each call emits, per (cache, live) container, six `[pf_storage]`
// lines (one per bucket from `Poly::PolyByteBuckets`) plus one
// `n_cache_entries` line. Called from worker threads inside `#pragma
// omp parallel` regions in `integration_step` (pre_step/post_step
// scopes from `hyper_int.cpp`) and from inside `partial_fractions()`
// after the fresh-compute path (`after_pf` phase).
extern std::atomic<bool> g_pf_storage_stats_enabled;
extern std::atomic<bool> g_pf_storage_debug_asserts_enabled;
void emit_pf_storage_stats(long step, const char* var_name,
                           const char* phase,
                           const PartialFractionization* live_or_null);

}  // namespace hyperflint
