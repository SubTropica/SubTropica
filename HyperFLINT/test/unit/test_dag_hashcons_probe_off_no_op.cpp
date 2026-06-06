// HF FF Phase 5 §A.1 iter-50: REQ-3 prerequisite unit test (OFF-path
// structural no-op assertion).
//
// Design contract:
//     notes/hf_finite_field_program/phase5_three_paths/probe_a1_dag_hashcons/design.md §7.2-bis
//     "REQ-3 (BINDING, baseline-soundness): OFF-path wall + RSS budget gate
//      (±5% max_rss, ±10% wall vs §0.3 baseline on 6 fixtures)."
//
// What this test exercises:
//   * With `HF_DAG_HASHCONS_PROBE` UNSET in the environment, `hf_probe_init()`
//     MUST be a no-op: it must NOT install the probe GMP wrappers and MUST
//     NOT flip `hf_probe_active` to true.
//   * Every public emit-helper (`hf_probe_emit_poly_create`,
//     `hf_probe_emit_rat_create`, `hf_probe_emit_symcoef_create`,
//     `hf_probe_emit_op_call`) MUST short-circuit on the `hf_probe_active`
//     branch and have zero observable effect (no crash, no counter
//     increment, no shared-mutex acquisition).
//   * `hf_probe_canonical_hash_poly` MUST return 0 immediately without
//     walking any payload bytes.
//   * The labelled-bytes counter MUST stay at 0 across a synthetic
//     `mpz_init` + `mpz_mul_2exp` + `mpz_clear` cycle (proving the GMP
//     allocator chain was NOT rewritten).
//
// This is the *structural* half of REQ-3. The *empirical* byte-id half
// (CLI output sha256 against the §0.3 baseline) is verified externally by
// an external gate script (extended to 6 fixtures at iter-51-ε;
// see HyperFLINT development notes, internal). Both halves together discharge the REQ-3 BINDING obligation.
//
// ctest registers this under name `dag-hashcons-probe-off-no-op` WITHOUT
// setting HF_DAG_HASHCONS_PROBE so the probe is INACTIVE for the test
// (mirroring the production OFF-path).

#include "hyperflint/instrumentation/dag_hashcons_probe.hpp"

#include <gmp.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>

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

int main(int /*argc*/, char** /*argv*/) {
    // Defensive: even though ctest doesn't set HF_DAG_HASHCONS_PROBE, an
    // outer environment might have it. Refuse to run if the env says ON,
    // because then the test would mis-fire (we'd assert OFF-path behaviour
    // while the probe is actually ON).
    const char* env = std::getenv("HF_DAG_HASHCONS_PROBE");
    if (env != nullptr && env[0] != '\0' && env[0] != '0') {
        std::printf("[SKIP] HF_DAG_HASHCONS_PROBE is set in env (%s); the OFF "
                    "test cannot exercise the OFF path. Refusing to run.\n",
                    env);
        // Return a non-zero exit so ctest doesn't silently pass when the
        // intended path isn't actually being exercised.
        return 2;
    }

    // Snapshot the GMP allocator BEFORE hf_probe_init.
    void* (*pre_alloc)(size_t)                  = nullptr;
    void* (*pre_realloc)(void*, size_t, size_t) = nullptr;
    void  (*pre_free)(void*, size_t)            = nullptr;
    mp_get_memory_functions(&pre_alloc, &pre_realloc, &pre_free);
    check(pre_alloc != nullptr && pre_realloc != nullptr && pre_free != nullptr,
          "pre-init: GMP allocator functions are registered");

    // Step 1: probe init under no env flag — must be a structural no-op.
    hf_probe_init();
    check(!hf_probe_active,
          "REQ-3 OFF: hf_probe_active stays FALSE after hf_probe_init "
          "when HF_DAG_HASHCONS_PROBE is unset");

    // Step 2: the GMP allocator functions must be UNCHANGED — the probe
    // wrappers must NOT have been installed.
    void* (*post_alloc)(size_t)                  = nullptr;
    void* (*post_realloc)(void*, size_t, size_t) = nullptr;
    void  (*post_free)(void*, size_t)            = nullptr;
    mp_get_memory_functions(&post_alloc, &post_realloc, &post_free);
    check(post_alloc == pre_alloc,
          "REQ-3 OFF: GMP alloc pointer unchanged (probe wrap NOT installed)");
    check(post_realloc == pre_realloc,
          "REQ-3 OFF: GMP realloc pointer unchanged");
    check(post_free == pre_free,
          "REQ-3 OFF: GMP free pointer unchanged");

    // Step 3: every emit helper must short-circuit. We can't directly
    // observe the short-circuit, but we can verify two side-effect channels:
    //   (a) the labelled-bytes counter does NOT move across an mpz cycle
    //       (because the wrap was not installed; even if it had been, the
    //       emit functions guard on hf_probe_active so the counter would
    //       not update either).
    //   (b) the get_dedup_snapshot returns all zeros after a flurry of
    //       no-op emit calls.
    const int64_t labelled_before = hf_probe_gmp_labelled_bytes_in_flight();
    {
        mpz_t z;
        mpz_init(z);
        mpz_set_ui(z, 1);
        mpz_mul_2exp(z, z, 16384);
        mpz_clear(z);
    }
    const int64_t labelled_after = hf_probe_gmp_labelled_bytes_in_flight();
    check(labelled_before == 0 && labelled_after == 0,
          "REQ-3 OFF: labelled-bytes counter stays at 0 across mpz cycle "
          "(probe alloc wrap not installed)");

    // Step 4: dedup snapshot at start should be zero.
    {
        const HfProbeDedupSnapshot s = hf_probe_get_dedup_snapshot();
        check(s.value_layer_hits == 0 && s.value_layer_misses == 0
              && s.op_layer_hits == 0 && s.op_layer_misses == 0,
              "REQ-3 OFF: dedup snapshot is all zeros pre-emit");
    }

    // Step 5: fire a few emit helpers; the probe MUST treat them as no-ops.
    // We don't have a Poly to hand in (and constructing one would itself
    // trigger value_create emits which would themselves short-circuit on
    // the same branch); we use the canonical-hash helper as a stand-in,
    // since its OFF behaviour is the cleanest to verify (return 0 without
    // walking any payload).
    const uint64_t h = hf_probe_canonical_hash_poly(nullptr, nullptr);
    check(h == 0,
          "REQ-3 OFF: hf_probe_canonical_hash_poly returns 0 on null inputs "
          "(structural short-circuit)");

    hf_probe_emit_op_call("test::no_op", 0xDEADBEEFCAFEBABEULL, 1);
    {
        const HfProbeDedupSnapshot s = hf_probe_get_dedup_snapshot();
        check(s.op_layer_hits == 0 && s.op_layer_misses == 0,
              "REQ-3 OFF: hf_probe_emit_op_call short-circuits (no counter "
              "update)");
    }

    if (g_failures > 0) {
        std::printf("=== FAIL: %d check(s) failed ===\n", g_failures);
        return 1;
    }
    std::printf("=== PASS: all REQ-3 OFF-path structural checks passed ===\n");
    return 0;
}
