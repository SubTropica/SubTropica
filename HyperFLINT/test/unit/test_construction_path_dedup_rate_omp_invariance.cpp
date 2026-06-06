// HF FF Phase 5 §A.1 iter-50: REQ-2 BINDING CI test for the Probe A1
// construction-path dedup-rate (§5 of the design memo).
//
// Design contract:
//     notes/hf_finite_field_program/phase5_three_paths/probe_a1_dag_hashcons/design.md §5.2
//     "REQ-2 (BINDING, correctness-cliff under concurrency): sharded
//      std::shared_mutex-guarded seen-set ... CI test (DEFERRED to iter-50)
//      asserts OMP=1 vs OMP=13 dedup-rate agreement within ±0.5 pp."
//
// What this test exercises:
//   * A deterministic workload of 1000 Poly constructions over a fixed
//     PolyCtx of 5 variables, with 100 distinct expressions (so each
//     expression repeats ~10 times on average and the expected dedup rate
//     is ~90%).
//   * Phase A: omp_set_num_threads(1). Serial construction loop. Snapshot
//     final value-layer hit + miss counters; compute dedup_rate_omp1.
//   * `hf_probe_reset_dedup_state()` to clear the seen-set + counters.
//   * Phase B: omp_set_num_threads(13). Parallel-for construction. Snapshot
//     counters again; compute dedup_rate_omp13.
//   * Assert |dedup_rate_omp1 - dedup_rate_omp13| ≤ 0.005 (±0.5 pp).
//
// REQ-2 is a correctness-cliff requirement: if the seen-set is racy, two
// threads inserting the same key would both report MISS, inflating the
// dedup rate's numerator denominator divergence. The 0.5 pp tolerance
// allows for legitimate FP noise in the `double` division but rejects any
// shard-collision-driven count drift.
//
// ctest registers this under name `construction-path-dedup-rate-omp-
// invariance` with HF_DAG_HASHCONS_PROBE=1 so the probe is active.

#include "hyperflint/instrumentation/dag_hashcons_probe.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"

#ifdef HF_HAVE_OPENMP
#include <omp.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace hyperflint;

static int g_failures = 0;

static void check(bool cond, const char* label) {
    if (cond) {
        std::printf("[PASS] %s\n", label);
    } else {
        std::printf("[FAIL] %s\n", label);
        ++g_failures;
    }
}

// Build a fixed list of 100 algebraically distinct expressions over the
// shared 5-variable PolyCtx (vars: x0..x4). Mix of monomials, sums, and
// constants so canonical-bits hashes are well-spread across the shards.
// IMPORTANT: keep the expressions string-identical across phases so the
// canonical-bits hash is bit-identical; otherwise dedup_rate would
// diverge for legitimate reasons.
static std::vector<std::string> make_expression_pool() {
    std::vector<std::string> e;
    e.reserve(100);
    for (int i = 0; i < 25; ++i) {
        e.push_back("x0^" + std::to_string(i + 1));
    }
    for (int i = 0; i < 25; ++i) {
        e.push_back("x1^" + std::to_string(i + 1) + " + " +
                    std::to_string(i + 1));
    }
    for (int i = 0; i < 25; ++i) {
        e.push_back("x2*x3 - " + std::to_string(i + 1) + "*x4");
    }
    for (int i = 0; i < 25; ++i) {
        e.push_back("x0*x1 + " + std::to_string(2 * (i + 1)) + "*x2 - x3");
    }
    return e;
}

// Deterministic permutation: pos `k` in [0, 1000) maps to expression index
// `(k * 7919) % 100` in the pool. The constant is prime so the mapping
// covers every pool entry approximately uniformly (= ~10 repeats each).
static inline int pool_index_for_position(int k, int pool_size) {
    return ((unsigned)k * 7919u) % (unsigned)pool_size;
}

static double dedup_rate_from_snapshot(const HfProbeDedupSnapshot& s) {
    const uint64_t denom = s.value_layer_hits + s.value_layer_misses;
    return (denom == 0) ? 0.0
        : (double)s.value_layer_hits / (double)denom;
}

int main(int /*argc*/, char** /*argv*/) {
    // Step 1: probe init under HF_DAG_HASHCONS_PROBE=1.
    hf_probe_init();
    check(hf_probe_active,
          "hf_probe_active is TRUE after hf_probe_init under "
          "HF_DAG_HASHCONS_PROBE=1");
    if (!hf_probe_active) {
        std::printf("=== FAIL: probe not active, cannot run REQ-2 test ===\n");
        return 1;
    }

    // Step 2: shared PolyCtx.
    PolyCtx ctx({"x0", "x1", "x2", "x3", "x4"});
    const std::vector<std::string> pool = make_expression_pool();
    constexpr int kN = 1000;

    // Phase A: serial construction (OMP=1).
    hf_probe_reset_dedup_state();
#ifdef HF_HAVE_OPENMP
    omp_set_num_threads(1);
#endif
    {
        std::vector<Poly> sink;
        sink.reserve(kN);
        for (int k = 0; k < kN; ++k) {
            sink.emplace_back(ctx, pool[pool_index_for_position(k, (int)pool.size())]);
        }
        // sink destructors run on scope exit; that triggers value_destroy
        // emits but does NOT touch the dedup counters (destroy is not
        // tracked in the seen-set).
    }
    const HfProbeDedupSnapshot snap_omp1 = hf_probe_get_dedup_snapshot();
    const double dedup_rate_omp1 = dedup_rate_from_snapshot(snap_omp1);
    std::printf("Phase A (OMP=1): hits=%llu misses=%llu dedup_rate=%.6f\n",
                (unsigned long long)snap_omp1.value_layer_hits,
                (unsigned long long)snap_omp1.value_layer_misses,
                dedup_rate_omp1);

    // Step 3: reset seen-set + counters between phases.
    hf_probe_reset_dedup_state();
    {
        const HfProbeDedupSnapshot post_reset = hf_probe_get_dedup_snapshot();
        check(post_reset.value_layer_hits == 0
              && post_reset.value_layer_misses == 0,
              "hf_probe_reset_dedup_state clears value-layer counters");
    }

    // Phase B: parallel construction (OMP=13). Each thread builds an
    // independent vector to avoid container contention; the only shared
    // mutation point is the probe's sharded seen-set.
#ifdef HF_HAVE_OPENMP
    omp_set_num_threads(13);
    {
        std::vector<std::vector<Poly>> per_thread(13);
        // Reserve in single-thread region first so reserve() never races.
        for (auto& v : per_thread) v.reserve(kN / 13 + 1);
        #pragma omp parallel for schedule(static)
        for (int k = 0; k < kN; ++k) {
            const int tid = omp_get_thread_num();
            per_thread[tid].emplace_back(
                ctx, pool[pool_index_for_position(k, (int)pool.size())]);
        }
        // per_thread vectors destruct at scope exit.
    }
#else
    {
        std::vector<Poly> sink;
        sink.reserve(kN);
        for (int k = 0; k < kN; ++k) {
            sink.emplace_back(ctx, pool[pool_index_for_position(k, (int)pool.size())]);
        }
    }
#endif
    const HfProbeDedupSnapshot snap_omp13 = hf_probe_get_dedup_snapshot();
    const double dedup_rate_omp13 = dedup_rate_from_snapshot(snap_omp13);
    std::printf("Phase B (OMP=13): hits=%llu misses=%llu dedup_rate=%.6f\n",
                (unsigned long long)snap_omp13.value_layer_hits,
                (unsigned long long)snap_omp13.value_layer_misses,
                dedup_rate_omp13);

    // Step 4: invariance assertions.
    //
    // (a) The total emit count (hits + misses) should be the SAME in both
    //     phases (or close, accounting for the implicit-copies that move-
    //     elision usually elides but some compilers don't on older
    //     emplace_back paths). Allow ±5% drift in total emit count.
    const uint64_t total_omp1  = snap_omp1.value_layer_hits  + snap_omp1.value_layer_misses;
    const uint64_t total_omp13 = snap_omp13.value_layer_hits + snap_omp13.value_layer_misses;
    const double total_drift_pct = (total_omp1 == 0)
        ? 0.0
        : (std::abs((double)total_omp13 - (double)total_omp1)
            / (double)total_omp1);
    std::printf("Total emit drift: |omp13 - omp1| / omp1 = %.4f\n", total_drift_pct);
    check(total_drift_pct < 0.05,
          "Total emit count agrees between OMP=1 and OMP=13 within ±5%");

    // (b) The dedup rate agrees within ±0.5 pp (i.e., |Δ| ≤ 0.005).
    const double dedup_drift = std::abs(dedup_rate_omp1 - dedup_rate_omp13);
    std::printf("Dedup-rate drift: |omp1 - omp13| = %.6f (threshold 0.005)\n",
                dedup_drift);
    check(dedup_drift <= 0.005,
          "REQ-2 BINDING: construction-path dedup-rate agrees between "
          "OMP=1 and OMP=13 within ±0.5 pp");

    // (c) Sanity: dedup rate should be in (0, 1). At ~10 reps per expression
    //     across 100 distinct keys, the rate is around 0.90 (= 1 - 100/1000)
    //     but the exact value depends on per-leaf-ctor secondary emits.
    //     We just check it's a non-degenerate fraction.
    check(dedup_rate_omp1 > 0.0 && dedup_rate_omp1 < 1.0,
          "Phase-A dedup_rate is in (0, 1)");
    check(dedup_rate_omp13 > 0.0 && dedup_rate_omp13 < 1.0,
          "Phase-B dedup_rate is in (0, 1)");

    if (g_failures > 0) {
        std::printf("=== FAIL: %d check(s) failed ===\n", g_failures);
        return 1;
    }
    std::printf("=== PASS: all checks passed ===\n");
    return 0;
}
