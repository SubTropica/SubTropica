// Phase 5d: IntegrateII.
//
// Given a wordlist  sum_i  coef_i * Hlog[var, word_i]  and a variable,
// return its primitive as a new wordlist — the anti-derivative of the
// same shape so that
//
//     differentiate_wordlist(integrate_ii(wl, var), var) == wl.
//
// Mirrors HyperIntica.wl:4448 and HyperInt.mpl:736 (integrateInplace).
//
// Algorithm:
//   queue = input
//   while queue nonempty:
//     {coef, word} = pop queue
//     parfr = PartialFractions[coef, var]
//     polynomial_part -> term-by-term antiderivative + IBP correction
//     pole_part[n=1]  -> prepend pole to word (hyperlog primitive)
//     pole_part[n>=2] -> rational primitive + IBP correction
//
// The anti-derivative is pinned at var=0 (lower-limit 0 convention),
// matching HyperIntica's  int(parfr[1], var=0..var)  in Maple and
// HyperIntica's  CoefficientList-based  Sum[cs[[k]] var^k / k, {k,...}].
//
// Non-linear denominator factors in the input coefficient cause
// partial_fractions to throw; we wrap that in IntegrateIIFailed so the
// CLI emits "failed":true rather than a stack trace. Phase 7's
// algebraic-letter path will extend this to degree-2 via Wm/Wp.

#pragma once

#include "hyperflint/core/rat.hpp"
#include "hyperflint/symbols/word.hpp"

#include <memory>
#include <stdexcept>

namespace hyperflint {

class ZWTable;  // fwd-decl; full def at hyperflint/core/zw_table.hpp.

class IntegrateIIFailed : public std::runtime_error {
public:
    IntegrateIIFailed() : std::runtime_error("IntegrateII: $Failed") {}
    explicit IntegrateIIFailed(const std::string& msg)
        : std::runtime_error("IntegrateII: " + msg) {}
};

// Phase 7-vi-a: when `introduce_algebraic_letters` is true, the inner
// partial_fractions call splits deg-2 irreducible denominators into
// Wm/Wp pairs. Default false preserves existing behavior. The pole-
// accumulation and IBP loop are agnostic to letter type — a Wm/Wp
// pole is just another Rat.
//
// Iter-52 C0c.1 Increment β: mandatory `std::shared_ptr<ZWTable> zw_tab`
// parameter (Option A; ratified at iter-50 MEMO §6 Q1 + iter-51 §6.5).
// Threaded through to the inner `partial_fractions` call.
Wordlist integrate_ii(const PolyCtx& ctx, const Wordlist& wl, size_t var_idx,
                      std::shared_ptr<ZWTable> zw_tab,
                      bool introduce_algebraic_letters = false);

// Phase-d15 follow-up: per-primitive timer for the partial_fractions
// call inside integrate_ii. Uses a file-scope per-thread vector so the
// cost is correctly attributed even when integrate_ii is called from
// inside integration_step's OMP parallel-for region.
//   init  — call once before the parallel region; sets the vector size.
//   reset — call once before the parallel region; zeros all per-thread slots.
//   sum   — call once after the parallel region; returns sum across threads.
void   init_partial_fractions_per_thread(int n_threads);
void   reset_partial_fractions_per_thread();
double sum_partial_fractions_per_thread();

// Per-call breakdown of the integrate_ii body outside partial_fractions.
// All four timers and the bump-calls counter follow the same per-thread
// vector pattern as partial_fractions above. Used to attribute the
// "53 % non-pf wall block" identified by the 2026-04-26 sanity matrix.
//   bump_lookup_s — content_key construction + unordered_map::find
//   bump_addto_s  — Rat::operator+ on hit / emplace on miss inside `bump`
//   push_ibp_s    — Rat division -p/(var-w[0]) + queue.push_back
//   antideriv_s   — polynomial-part antiderivative loop
//   bump_calls    — total calls into the `bump` lambda
void   init_ii_sub_timers_per_thread(int n_threads);
void   reset_ii_sub_timers_per_thread();
double sum_bump_lookup_s_per_thread();
double sum_bump_addto_s_per_thread();
double sum_push_ibp_s_per_thread();
double sum_antideriv_s_per_thread();
long   sum_bump_calls_per_thread();
// 2026-04-27 Lever-1 extended: split bump_addto_s into the two
// branches of `bump`'s if/else.  bump_addto_s = bump_emplace_s +
// bump_rat_add_s; bump_calls = (new-row push count) + bump_rat_add_calls.
double sum_bump_emplace_s_per_thread();
double sum_bump_rat_add_s_per_thread();
long   sum_bump_rat_add_calls_per_thread();

// 2026-04-29 (Probe 3 — HF/Maple investigation): split the
// previously-unaccounted ~1.9 kCPU-s of integrate_ii body on
// 3l3pt parity-1 ord_1_face_1 step 7 (= integrate_ii_s minus
// partial_fractions_s minus the existing bump/push_ibp/antideriv
// sub-buckets). Three target sites in the outer queue loop:
//   ii_queue_copy_s    — line 324: const WordlistTerm w = queue[q_idx];
//                          (deep-copy of two fmpq_mpoly + Word per iter).
//   ii_pole_arith_s    — lines 361-364: var_minus_pole = var_rat - pole.pole;
//                          vmp_pow = .pow(n-1); cn / (vmp_pow * (1-n)).
//                          Three Rat ops with canonicalisation.
//   ii_pole_word_ctor_s — lines 370-374: Word construction on n==1
//                          poles (push_back of pole.pole + word letters).
// Surfaced in HF_STEP_TRACE JSON under those names. See
// notes/2026-04-29_hf-vs-maple-investigation.md §5 Probe 3.
double sum_ii_queue_copy_s_per_thread();
double sum_ii_pole_arith_s_per_thread();
double sum_ii_pole_word_ctor_s_per_thread();
// Phase-0 P1' pre-gating (2026-04-26): per-invocation distinct-denominator
// counters, summed across integrate_ii invocations on this thread.
//   pf_calls_in_loop : sum over invocations of (loop iterations that hit
//                       the linear_factors call inside partial_fractions)
//   pf_unique_dens   : sum over invocations of (distinct den.to_string()
//                       keys seen in this invocation's queue)
// Bucketing-collapsible-fraction = (pf_calls_in_loop - pf_unique_dens)
//                                  / pf_calls_in_loop.
long   sum_pf_calls_in_loop_per_thread();
long   sum_pf_unique_dens_per_thread();
// Phase-0b RatAccumulator pre-gating (2026-04-26): summed `rows.size()`
// at end of each integrate_ii invocation. Combined with the existing
// `bump_calls` counter, bounds the RatAccumulator (lazy-canonicalising
// sum) ceiling: fraction of bumps that target an existing row and would
// be collapsed by deferred canonicalization is
//   (bump_calls − bump_unique_rows) / bump_calls.
long   sum_bump_unique_rows_per_thread();

}  // namespace hyperflint
