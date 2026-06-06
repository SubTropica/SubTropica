// HyperFLINT Track 4.2 iter-27 §F row 14 close: predicate-false no-op
// unit test for hf::phase_timer infrastructure.
//
// Design contract:
//     Phase-timer instrumentation spec (internal development notes).
//     §(d) "Predicate-false: sentinel start_ns = -1, destructor early-exits,
//           NO chrono call, NO storage increment.  Cost contract: predicate-
//           false ≈ 5 ns/tick (target; ≤ 50 ns/tick acceptance)."
//     Predicate-false no-op unit test deliverable.
//
// What this test exercises:
//   * Choose a NON-cross-cutting timer (gcd_cofactors_narrow).  Its
//     parents_mask requires BOTH omp_parallel_for_integration_step AND
//     rat_reduce_inplace_narrow_branch scopes to be active.  The test
//     enters NEITHER scope, so the predicate is structurally false.
//   * Assertion (a) — STORAGE INVARIANCE: after timer_reset_all(), run
//     10^6 HF_PHASE_TIMER_TICK(gcd_cofactors_narrow) predicate-false ticks.
//     timer_wall_s(timer_id::gcd_cofactors_narrow) MUST equal 0.0
//     bit-identically (no chrono call, no storage write).
//   * Assertion (b) — LATENCY CONTRACT: same 10^6-tick loop wrapped in
//     std::chrono::steady_clock; ns_per_tick = elapsed / 10^6 MUST be
//     <= 50 ns/tick.  Spec target is 5 ns/tick; 10x slack covers
//     branch-predictor warmup + chrono-overhead variance + non-M4 dev
//     hardware where the test may run.
//   * Assertion (c) — POSITIVE CONTROL: enter both required scopes,
//     run a single predicate-TRUE tick, verify timer_wall_s grows
//     above 0 (proves the predicate evaluator's TRUE branch works on
//     this build, so a 0.0 in (a) is genuinely from the FALSE branch
//     and not from a broken accumulator).
//
// ctest registers this under name `phase-timer-predicate-false-no-op`
// without any HF_* env vars; the master enable flag g_phase_timers_enabled
// defaults to true, so the predicate evaluator runs and (with no scopes
// entered) returns false on non-cross-cutting timers.

#include "hyperflint/instrumentation/phase_timer.hpp"

#include <chrono>
#include <cstdio>
#include <cstdint>

using namespace hf::phase_timer;

static int g_failures = 0;

static void check(bool cond, const char* label) {
    if (cond) {
        std::printf("[PASS] %s\n", label);
    } else {
        std::printf("[FAIL] %s\n", label);
        ++g_failures;
    }
}

int main(int /*argc*/, char** /*argv*/) {
    // Reset state at start so the test is independent of any prior state
    // (which there shouldn't be inside a fresh process, but defensive).
    timer_reset_all();
    check(g_active_scope_mask == 0,
          "preamble: g_active_scope_mask is zero after timer_reset_all");
    check(timer_wall_s(timer_id::gcd_cofactors_narrow) == 0.0,
          "preamble: timer_wall_s(gcd_cofactors_narrow) == 0.0 after reset");

    constexpr long N = 1'000'000;

    // ────────────────────────────────────────────────────────────────────
    // Assertion (a) — STORAGE INVARIANCE under predicate-false.
    // ────────────────────────────────────────────────────────────────────
    //
    // The predicate for gcd_cofactors_narrow is
    //     (active_mask & parents) == parents
    // with parents = bit(omp_parallel_for_integration_step) |
    //                bit(rat_reduce_inplace_narrow_branch).
    // We enter NEITHER scope, so the predicate is false on every tick.
    //
    // Spec §(d): predicate-false ctor sets active_=false; dtor early-exits
    // BEFORE the chrono::now() call AND BEFORE the wall_accum_ increment.
    // Therefore timer_wall_s MUST stay bit-identical to 0.0.

    for (long i = 0; i < N; ++i) {
        HF_PHASE_TIMER_TICK(gcd_cofactors_narrow);
        // suppress unused-variable warning on the RAII guard
        (void)_hf_tick_gcd_cofactors_narrow;
    }

    const double wall_after_loop = timer_wall_s(timer_id::gcd_cofactors_narrow);
    check(wall_after_loop == 0.0,
          "(a) STORAGE INVARIANCE: timer_wall_s(gcd_cofactors_narrow) "
          "== 0.0 bit-identically after 1,000,000 predicate-false ticks");

    // ────────────────────────────────────────────────────────────────────
    // Assertion (b) — LATENCY CONTRACT under predicate-false.
    // ────────────────────────────────────────────────────────────────────
    //
    // Re-run the loop bracketed by steady_clock; compute ns_per_tick.
    // Spec §(d) target = 5 ns/tick; acceptance gate = 50 ns/tick (10x
    // slack).  The asm volatile barrier prevents the compiler from
    // hoisting the entire RAII construction out of the loop.

    timer_reset_all();   // defensive; predicate-false should not have
                         // moved storage, but reset re-zeroes everything
                         // including the scope mask.

    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < N; ++i) {
        HF_PHASE_TIMER_TICK(gcd_cofactors_narrow);
        (void)_hf_tick_gcd_cofactors_narrow;
        // Memory barrier — keeps the compiler from hoisting RAII
        // construction/destruction out of the loop body.
        asm volatile("" ::: "memory");
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double elapsed_ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double ns_per_tick = elapsed_ns / static_cast<double>(N);

    std::printf("[INFO] predicate-false ns_per_tick = %.2f  "
                "(spec target 5 ns; acceptance 50 ns)\n", ns_per_tick);
    check(ns_per_tick <= 50.0,
          "(b) LATENCY CONTRACT: ns_per_tick <= 50 ns under predicate-false");

    // The wall accumulator MUST still be 0.0 after this second loop too,
    // for the same predicate-false reason.
    const double wall_after_timed = timer_wall_s(timer_id::gcd_cofactors_narrow);
    check(wall_after_timed == 0.0,
          "(b-corollary) timer_wall_s stays at 0.0 after the latency-timed "
          "predicate-false loop");

    // ────────────────────────────────────────────────────────────────────
    // Assertion (c) — POSITIVE CONTROL (predicate-TRUE branch works).
    // ────────────────────────────────────────────────────────────────────
    //
    // Without this control, a broken wall_accum_ that silently no-ops on
    // BOTH true and false branches would still pass (a) and (b).  We
    // enter both parent scopes and run a single tick that does a small
    // amount of work; the accumulator MUST grow above 0.0.

    timer_reset_all();
    {
        HF_SCOPE_ENTER(omp_parallel_for_integration_step);
        HF_SCOPE_ENTER(rat_reduce_inplace_narrow_branch);
        check((g_active_scope_mask & HF_PT_PARENT_MASK_gcd_cofactors_narrow)
                  == HF_PT_PARENT_MASK_gcd_cofactors_narrow,
              "(c-preamble) parent scopes set in active_scope_mask");
        {
            HF_PHASE_TIMER_TICK(gcd_cofactors_narrow);
            // Burn a small but non-zero amount of wall.  10us nominal,
            // any positive amount suffices for the strict-positive check.
            const auto end = std::chrono::steady_clock::now()
                             + std::chrono::microseconds(10);
            while (std::chrono::steady_clock::now() < end) {
                asm volatile("" ::: "memory");
            }
        }
    }
    const double wall_after_positive =
        timer_wall_s(timer_id::gcd_cofactors_narrow);
    check(wall_after_positive > 0.0,
          "(c) POSITIVE CONTROL: timer_wall_s > 0 after one predicate-TRUE "
          "tick with both parent scopes active (proves the TRUE branch is "
          "wired and the FALSE-branch 0.0 in (a) is meaningful)");
    check(g_active_scope_mask == 0,
          "(c-coda) HF_SCOPE_GUARD dtors restored g_active_scope_mask to 0");

    if (g_failures > 0) {
        std::printf("=== FAIL: %d check(s) failed ===\n", g_failures);
        return 1;
    }
    std::printf("=== PASS: all phase_timer predicate-false checks passed ===\n");
    return 0;
}
