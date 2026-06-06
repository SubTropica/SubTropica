#pragma once
// HF FF Phase 6 REVISED §6.D iter-22b — env-var-gated thread-cap helpers
// for the §6.D engineering pilot (BFS-to-DFS dispatcher rewrite).
//
// Design memo:
//   notes/hf_finite_field_program/phase6_combined/section_6d_engineering/
//     design.md (iter-21; ~258 LOC post 7 REQ-folds; TWO CONCURRING
//     BINDING reviewers NAMED agentId a7c3f8b2e1d09a4f6 + NAMED agentId
//     b3f24a9c7d1e5680f CONCERNS-FOLD-required-then-ACCEPT).
//
// Scope. Provides two production helpers consumed by the §6.D dispatcher
// at HyperFLINT/src/integrator/integration_step.cpp:1990-2019:
//
//   (1) parse_dfs_cap_env()         — reads $HF_SECTION_6D_DFS_THREAD_CAP
//                                     ONCE per call; returns 0 when unset
//                                     (default-OFF; current 13-thread
//                                     behaviour) or the parsed positive
//                                     integer cap (1..13 typical).
//                                     Caches result via static int after
//                                     first read; subsequent calls reuse
//                                     the cache. Per-call cost: one
//                                     thread_local int load + one branch
//                                     after the first call.
//
//   (2) dispatch_cap_active(cap, max) — returns min(cap, max) when cap > 0;
//                                       else max. Side-effect-free and
//                                       idempotent.
//
// Plus a small set of probe accessors used by the iter-22a TDD red-state
// tests at HyperFLINT/test/test_integration_section_6d_dfs_cap.cpp +
// test_linear_factors_cache_key_thread_invariant.cpp +
// test_transform_zw_tab_readonly_under_cap.cpp. iter-22b flips those
// tests' local `probe_*()` stubs to delegate to the accessors declared
// here.
//
// REQ-21.4 BINDING placement constraint. The dispatcher at
// integration_step.cpp:1990-2019 MUST invoke parse_dfs_cap_env() exactly
// ONCE per dispatcher invocation, BEFORE the
// `if (gcd_dispatch_enabled() && gcd_dispatch_available())` branch.
// Forbidden placements (compounds cost + race risk):
//   (a) inside `process_entry_body` lambda (per-entry repeat).
//   (b) inside `dispatch_parallel_for` callback (per-slot repeat).
//   (c) inside `#pragma omp parallel` region (per-thread race + repeat).
//
// REQ-C wall regression (design.md §4). Default-OFF wall regression
// must be ≤ +2 % on tst2 + parity_1 vs iter-15 baseline; the
// parse_dfs_cap_env + dispatch_cap_active pair contributes one
// thread_local int load + two integer comparisons + one int branch at
// the dispatcher entry. Empirically (iter-22b spot-check, deferred to
// iter-22b-β) expected ≤ +0.1 % wall overhead at default-OFF.

#include <cstddef>

namespace hyperflint {
namespace section_6d {

// ---------------------------------------------------------------------------
// (1) parse_dfs_cap_env — env-var reader for HF_SECTION_6D_DFS_THREAD_CAP.
//
// Returns:
//   0           — env var unset / empty / non-positive / parse failed
//                 (= default-OFF; dispatcher retains 13-thread behaviour).
//   N (1..13)   — positive integer parsed from env var
//                 (= opt-in Strategy (c) hybrid cap; dispatcher will cap
//                  the OMP thread count or GCD slot count to N).
//
// Implementation contract. The function caches the result via a static
// int after the first read; subsequent calls are O(1) and read no env
// var. Per REQ-21.4, the dispatcher should call this ONCE per invocation
// and pass the value into the OMP / GCD branch as a `const int`.
//
// Side effects. Increments a thread_local call counter
// `s_env_var_read_count` on every call. The counter is read back via
// `env_var_read_count_per_dispatch_call()` after `reset_env_var_read_count()`
// in the iter-22a Subtest 4 falsifier (REQ-21.4 placement invariant).
int parse_dfs_cap_env();

// ---------------------------------------------------------------------------
// (2) dispatch_cap_active — cap-application predicate.
//
// Returns:
//   min(dfs_thread_cap, max_threads) when dfs_thread_cap > 0.
//   max_threads                       otherwise.
//
// The dispatcher passes the result as `omp_set_num_threads(...)` (OMP
// path) and as the `max_slots` argument to `dispatch_parallel_for` (GCD
// path). Idempotent + side-effect-free.
int dispatch_cap_active(int dfs_thread_cap, int max_threads);

// ---------------------------------------------------------------------------
// Test probe accessors (consumed by the iter-22a TDD red-state tests).
// ---------------------------------------------------------------------------

// Returns true; locks in the existence + linkage of `parse_dfs_cap_env`.
// iter-22b green-state: returns true. Test Subtest 1.
bool probe_parse_dfs_cap_env_present();

// Returns true; locks in the existence + linkage of `dispatch_cap_active`.
// iter-22b green-state: returns true. Test Subtest 2.
bool probe_dispatch_cap_active_present();

// Returns true iff parse_dfs_cap_env() returns 0 AND
// dispatch_cap_active(0, 13) == 13 (default-OFF no-op contract).
// Test Subtest 3.
bool probe_default_off_no_op();

// Returns true iff after one call to parse_dfs_cap_env(), the
// thread-local call counter reads exactly 1. REQ-21.4 BINDING placement
// invariant falsifier. Test Subtest 4.
bool probe_env_var_read_count_per_dispatch_le_one();

// Returns the current value of the thread_local call counter that
// `parse_dfs_cap_env()` increments. Test-only diagnostic; resets via
// `reset_env_var_read_count()`.
int env_var_read_count_per_dispatch_call();

// Resets the thread_local call counter to 0. Test-only.
void reset_env_var_read_count();

// ---------------------------------------------------------------------------
// REQ-21.5 mitigation probe accessors (R21.1 LF cache key + R21.6
// zw_tab read-only). Defense-in-depth regression guards per design.md §7.
// ---------------------------------------------------------------------------

// Returns true; locks in the invariant
// "linear_factors_cache_key() is thread-id-independent" per second
// BINDING reviewer's audit of src/algebra/linear_factors.cpp:761-766.
// The function is purely structural at HEAD (poly_struct_hash + flags).
// This probe is a TAUTOLOGICAL doc / regression guard: if a future
// refactor introduces thread-local state into the cache key path, this
// header's contract becomes false and the iter-22b commit author must
// either (a) rewrite this probe to construct two contexts in two OMP
// threads + compare keys (the original design.md §7 R21.1 mitigation),
// OR (b) refuse the cache-key refactor.
bool probe_lf_cache_key_thread_invariant();

// Returns true; locks in the invariant "zw_tab is read-only inside the
// parallel region" per iter-65/iter-66 cascade (commits 0883aeb66,
// 7dd21d981, 12f1e8cc0) + HF_SCALAR_REP_REQUIRE_PERSISTENT abort
// guards at sites 1/2/3/4/5/7. Defense-in-depth doc / regression guard;
// iter-22b commit author MUST either (a) write a shared_ptr ref-count
// invariant probe + drive `apply_v1_roundtrip` under cap=3 (the
// original design.md §7 R21.6 option (a) mitigation), OR (b) confirm
// the iter-65/66 abort guards remain in place. Currently (b) is
// satisfied at HEAD (commit f45332fa5 iter-21 close).
bool probe_zw_tab_readonly_invariant_under_cap();

}  // namespace section_6d
}  // namespace hyperflint
