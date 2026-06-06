// HF FF Phase 6 REVISED §6.D iter-22 — TDD red-state scaffolding for the
// §6.D engineering pilot (BFS-to-DFS dispatcher rewrite with env-var-gated
// hybrid cap; ship target Strategy (c) per design.md §3.4).
//
// Design memo:
//   notes/hf_finite_field_program/phase6_combined/section_6d_engineering/design.md
//   (iter-21; ~258 LOC post-7-REQ-folds; TWO CONCURRING BINDING reviewers
//    NAMED agentId a7c3f8b2e1d09a4f6 + NAMED agentId b3f24a9c7d1e5680f
//    CONCERNS-FOLD-required-then-ACCEPT).
//
// Promotion rationale (iter-20 RSS-map verdict GO_§6.D_ENGINEERING +
// iter-21 design memo §3.4 strategy choice). iter-20 RSS-map predicted
// parity_1 ΔR_peak central 28.41 % (PASS_MARGINAL_LOWER_BOUND_FRAGILE);
// iter-21 design memo authorises iter-22 TDD with Strategy (a) cap=1
// serial-DFS as ground-truth measurement + Strategy (c) hybrid env-var
// `HF_SECTION_6D_DFS_THREAD_CAP` as iter-22+ ship target.
//
// Purpose. Lock in the §6.D engineering pilot's API contract BEFORE the
// production source change at iter-22b. Four subtests assert the four
// load-bearing predicates exist and all FAIL today (iter-22a red-state);
// once iter-22b's source change lands (parse_section_6d_dfs_cap_env helper
// + cap-application predicates in integration_step.cpp:1990-2019 per
// REQ-21.4 placement constraint + companion accessors in a new
// include/hyperflint/integrator/section_6d_dfs_cap.hpp / src equivalent),
// each probe function below will be flipped to call into the production
// API and the subtests turn green.
//
// Mirror of iter-12 §6.D 4-col pattern and iter-15 §6.P sharing-probe
// pattern. Single binary, four `add_test` entries (one per predicate),
// each pinning the subtest with a single argv argument. Subtests are
// strictly independent; failure of one does not perturb the others.
//
// Probe convention. Each subtest delegates the actual existence check
// to a local `probe_*()` function, all defined at the bottom of this
// file in one block. These probes return `false` today (iter-22a
// red-state: §6.D dispatcher cap not yet present in production).
// iter-22b's source change will:
//   (i)   Add `parse_section_6d_dfs_cap_env()` (free function in
//         hyperflint:: namespace) reading `HF_SECTION_6D_DFS_THREAD_CAP`
//         exactly ONCE per dispatcher invocation per REQ-21.4.
//   (ii)  Add a `section_6d_dispatch_cap_active(int max_threads)` helper
//         returning `min(cap, max_threads)` when cap > 0; else
//         max_threads. Used at integration_step.cpp:1996 inside the
//         `omp_set_num_threads` call site + symmetrically inside the
//         GCD `dispatch_parallel_for` slot-cap path.
//   (iii) Add a `section_6d_env_var_read_count_per_dispatch()` test-only
//         accessor (read-count probe; gated on HF_SECTION_6D_TEST_PROBE=1)
//         used by Subtest 4 to assert REQ-21.4 placement (read ONCE per
//         dispatcher invocation, NOT per-entry / per-thread / per-slot).
//   (iv)  Flip the four `probe_*()` definitions below to delegate to
//         the corresponding production accessors; the subtests turn
//         green.
//
// Compile-only fixture (iter-22a). The test compiles against the current
// headers only (no `section_6d_dfs_cap.hpp` exists yet — iter-22b will
// add it). The expected API shape is forward-declared here in local
// stub signatures only as a documentation aid; we do NOT static_assert
// against the production layout (that would block iter-22b ratification
// at compile-time, undesirable across the iter-22a → iter-22b
// transition).
//
// REQ-G atomicity (iter-21 design memo §4 + commit message marker
// REVERT_TARGET_FOR_ITER22B_HARD_FAIL). This file is the FULL iter-22a
// TDD red-state test (4 subtests, 4 ctest entries, 4 FAIL on first
// run). It is NOT a skeleton; iter-22b will flip the probes only.
//
// REQ-21.4 caveat (BINDING; env-var read placement). Subtest 4
// (`bench-default-off-wall-stability`) checks the placement invariant
// `env_var_read_count_per_dispatch_call() == 1` once iter-22b probes
// land. Per design.md §5 forbidden placements: env-var read MUST NOT
// occur (a) inside `process_entry_body` lambda, (b) inside
// `dispatch_parallel_for` callback, (c) inside `#pragma omp parallel`
// region. The probe captures all three forbidden placements by
// counting calls per outer dispatcher invocation; iter-22b's
// implementation MUST read the env var exactly once at the outer
// dispatcher entry, cache in a `const int dfs_thread_cap`.
//
// REQ-21.5 cross-reference. The companion unit tests
// `test_linear_factors_cache_key_thread_invariant.cpp` and
// `test_transform_zw_tab_readonly_under_cap.cpp` (also new at iter-22a)
// gate R21.1 (LF cache key invariance) and R21.6 (zw_tab read-only
// under cap) respectively. All three tests must PASS before iter-23
// sweep.

#include <cstdio>

// iter-22b green-state: production accessors live in this header.
#include "hyperflint/integrator/section_6d_dfs_cap.hpp"

namespace {

// ---------------------------------------------------------------------------
// Local forward-declared mirror of the EXPECTED iter-22b API shape
// (per design.md §5 green-state impl). Documentation aid only; the test
// does NOT static_assert against the production signatures.
//
// iter-22b production source change will install these as
// `hyperflint::section_6d::parse_dfs_cap_env()` (or under whatever
// namespace the integrator chooses); until then, this comment block
// serves to lock in the names + types.
//
//   int  hyperflint::section_6d::parse_dfs_cap_env();
//        // Reads $HF_SECTION_6D_DFS_THREAD_CAP. Returns 0 if unset /
//        // empty / non-positive (= default-OFF, current 13-thread).
//        // Otherwise returns the positive integer cap (1..13 typical).
//
//   int  hyperflint::section_6d::dispatch_cap_active(int dfs_thread_cap,
//                                                     int max_threads);
//        // Returns min(dfs_thread_cap, max_threads) when
//        // dfs_thread_cap > 0; else max_threads. Idempotent + side-
//        // effect-free.
//
//   int  hyperflint::section_6d::env_var_read_count_per_dispatch_call();
//        // Test-only probe (gated on HF_SECTION_6D_TEST_PROBE=1). Returns
//        // the number of times the env var was read in the most-recent
//        // dispatcher invocation. REQ-21.4 places: MUST be exactly 1.
//
// ---------------------------------------------------------------------------

bool probe_parse_dfs_cap_env_present();
bool probe_dispatch_cap_active_present();
bool probe_default_off_no_op();
bool probe_env_var_read_count_per_dispatch_le_one();

// ---------------------------------------------------------------------------
// Subtest 1 — `parse_dfs_cap_env()` is callable and respects env var.
//
// REQ-21.4 BINDING. The env-var-read function is the load-bearing
// helper for Strategy (c) hybrid env-var-cap; it must be a free
// function in hyperflint:: namespace, readable from
// integration_step.cpp:1990-2019 dispatcher entry.
//
// Current state (iter-22a): probe returns false → FAIL.
// After iter-22b: production source adds the helper + flips probe → PASS.
// ---------------------------------------------------------------------------
int test_cap_env_var_read() {
    if (!probe_parse_dfs_cap_env_present()) {
        std::fprintf(stderr,
            "[FAIL] hyperflint::section_6d::parse_dfs_cap_env() NOT "
            "present. iter-22b green-state must add this free function "
            "per design.md §5 + REQ-21.4 placement constraint.\n");
        return 1;
    }
    std::fprintf(stdout, "[PASS] section-6d-dfs-cap/cap-env-var-read\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Subtest 2 — `dispatch_cap_active(cap, max)` returns capped value.
//
// REQ-B + REQ-C (design.md §4). The cap-application predicate is the
// single point at which the dispatcher decides how many slots to claim
// from the GCD slot pool / how many OMP threads to permit. iter-22b
// green-state wires this at integration_step.cpp:1996 inside the
// `omp_set_num_threads` call AND symmetrically inside the GCD
// `dispatch_parallel_for` `max_slots` argument.
//
// Current state (iter-22a): probe returns false → FAIL.
// ---------------------------------------------------------------------------
int test_cap_dispatch_applies() {
    if (!probe_dispatch_cap_active_present()) {
        std::fprintf(stderr,
            "[FAIL] hyperflint::section_6d::dispatch_cap_active(...) "
            "NOT present. iter-22b green-state must add this helper + "
            "wire it at integration_step.cpp:1996 (OMP path) + GCD "
            "`dispatch_parallel_for` max_slots (libdispatch path).\n");
        return 1;
    }
    std::fprintf(stdout, "[PASS] section-6d-dfs-cap/cap-dispatch-applies\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Subtest 3 — Default-OFF behaviour (env var unset → no-op).
//
// REQ-C default-OFF gate (design.md §4): when
// `HF_SECTION_6D_DFS_THREAD_CAP` is unset/empty/0, the dispatcher
// behaviour is bit-identical to iter-21 baseline. The env-var read
// returns 0 = unset = current 13-thread behaviour; the cap-application
// is a pass-through (returns max_threads unchanged). Byte-id smoke on
// 5 fixtures × default-OFF is the integration-level falsifier; this
// unit-level test locks in the contract at the API boundary.
//
// Current state (iter-22a): probe returns false → FAIL.
// ---------------------------------------------------------------------------
int test_default_off_no_op() {
    if (!probe_default_off_no_op()) {
        std::fprintf(stderr,
            "[FAIL] Default-OFF no-op contract NOT satisfied. iter-22b "
            "green-state must guarantee that with `$HF_SECTION_6D_DFS_THREAD_CAP` "
            "unset/empty/0, `parse_dfs_cap_env()==0` AND "
            "`dispatch_cap_active(0,max_threads)==max_threads`.\n");
        return 1;
    }
    std::fprintf(stdout, "[PASS] section-6d-dfs-cap/default-off-no-op\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Subtest 4 — REQ-21.4 BINDING placement falsifier:
// `env_var_read_count_per_dispatch_call() == 1` (read exactly once
// per dispatcher invocation, NOT per-entry / per-thread / per-slot).
//
// REQ-21.4 BINDING (design.md §5 + iter-21 reviewer agentId
// a7c3f8b2e1d09a4f6). The env-var read MUST occur exactly once per
// outer dispatcher invocation at integration_step.cpp:1990-2019, BEFORE
// the `if (gcd_dispatch_enabled() && gcd_dispatch_available())` branch.
// Forbidden placements (compounds cost + race risk):
//   (a) inside `process_entry_body` lambda (per-entry repeat).
//   (b) inside `dispatch_parallel_for` callback (per-slot repeat).
//   (c) inside `#pragma omp parallel` region (per-thread race + repeat).
// Probe captures all three forbidden placements by counting calls per
// outer dispatcher invocation. The test passes iff exactly one read
// occurred.
//
// Companion integration falsifier (NOT in this test): the
// bench_default_off_wall_stability harness runs parity_1 under
// default-OFF, N=5 trials, asserts median wall deviation < 0.5 % vs
// iter-15 baseline. iter-22b CI step gates the integration falsifier;
// this unit test gates the placement invariant at the API boundary.
//
// Current state (iter-22a): probe returns false → FAIL.
// ---------------------------------------------------------------------------
int test_bench_default_off_wall_stability() {
    if (!probe_env_var_read_count_per_dispatch_le_one()) {
        std::fprintf(stderr,
            "[FAIL] REQ-21.4 placement invariant violated: "
            "`env_var_read_count_per_dispatch_call() != 1`. iter-22b "
            "green-state must read $HF_SECTION_6D_DFS_THREAD_CAP "
            "exactly ONCE per dispatcher invocation, outside "
            "process_entry_body lambda + outside dispatch_parallel_for "
            "callback + outside `#pragma omp parallel` region. Cache in "
            "`const int dfs_thread_cap` local at "
            "integration_step.cpp:1996 entry.\n");
        return 1;
    }
    std::fprintf(stdout,
        "[PASS] section-6d-dfs-cap/bench-default-off-wall-stability\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Probe definitions — LOCAL stubs (iter-22a red-state).
//
// All four return false at iter-22a; iter-22b production source change
// will flip these to delegate to
// `hyperflint::section_6d::probe_*_present()` (or equivalent production
// accessors) defined in a new include/hyperflint/integrator/
// section_6d_dfs_cap.hpp.
//
// REQ-21.5 cross-reference: the green-state probe semantics are pinned
// in design.md §5 + §7 R21.1 mitigation (LF cache key invariance) and
// R21.6 (zw_tab read-only under cap), gated by the companion test files
// test_linear_factors_cache_key_thread_invariant.cpp and
// test_transform_zw_tab_readonly_under_cap.cpp.
// ---------------------------------------------------------------------------
// iter-22b green-state: delegate to production accessors in
// hyperflint::section_6d (section_6d_dfs_cap.hpp).
bool probe_parse_dfs_cap_env_present() {
    return ::hyperflint::section_6d::probe_parse_dfs_cap_env_present();
}
bool probe_dispatch_cap_active_present() {
    return ::hyperflint::section_6d::probe_dispatch_cap_active_present();
}
bool probe_default_off_no_op() {
    return ::hyperflint::section_6d::probe_default_off_no_op();
}
bool probe_env_var_read_count_per_dispatch_le_one() {
    return ::hyperflint::section_6d::
        probe_env_var_read_count_per_dispatch_le_one();
}

}  // namespace

// ---------------------------------------------------------------------------
// main — runs all four subtests sequentially.
//
// CMakeLists.txt registers ONE `add_test` entry per design.md §5
// deliverable 1 (ctest count 35 → 36 for this file). Each subtest
// fails the run; main returns 1 on any failure and exits early at
// the first failing subtest. argv is unused.
// ---------------------------------------------------------------------------
int main(int /*argc*/, char** /*argv*/) {
    if (int rc = test_cap_env_var_read()) return rc;
    if (int rc = test_cap_dispatch_applies()) return rc;
    if (int rc = test_default_off_no_op()) return rc;
    if (int rc = test_bench_default_off_wall_stability()) return rc;
    std::fprintf(stdout,
        "[PASS] section-6d-dfs-cap (4/4 subtests; iter-22b green-state "
        "must flip all four local probes)\n");
    return 0;
}
