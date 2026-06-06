// HF FF Phase 5 §C.b Step C.b-impl-1 TDD scaffolding.
//
// Authored iter-59-γ as 6 SKIP-placeholder sub-tests; ENHANCED iter-89
// (path-default per iter-88-α §C.b verdict.md §6.1) to convert 4 of the
// 6 SKIPs into REAL exercises of mimalloc 3.3.1 primitives that the
// §C.b production code (iter-90+ Step C.b-impl-2) will depend on. The
// remaining 2 SKIPs (sha-id cross-arm + falsification gate) remain
// placeholders since they require the §C.b production binary + the
// A/B harness (iter-N+ Step C.b-impl-3 scope).
//
// Build pattern: each test_* returns 0 on PASS / 1 on FAIL; main()
// aggregates and returns rc. The test target now links mimalloc-static
// (CMakeLists.txt:651-654 + 699) so the mi_* primitives are exercised
// in-process under the same allocator instance the §C.b production
// path will use.
//
// References:
//   notes/hf_finite_field_program/phase5_three_paths/lever_c_b_mimalloc_per_arena/design.md
//     §3.1   PRIMARY default runtime libdispatch RAII install/restore wiring
//     §3.4   libdispatch process-wide pool reuse defense (iter-58 REQ-6)
//     §4.5   mi_collect(false) vs (true) choice (iter-58 REQ-5)
//     §5     falsification gate (post §C.d FAIL PASS_PARTIAL band 5-15%)
//     §6.2   TDD test file plan (iter-58 fold appendix)
//   notes/hf_finite_field_program/phase5_three_paths/lever_c_b_mimalloc_per_arena/mimalloc_version_probe.md
//   notes/hf_finite_field_program/phase5_three_paths/lever_c_b_mimalloc_per_arena/verdict.md (iter-88-α PRELIMINARY DRAFT)
//
// iter-89 PHANTOM-API discoveries (load-bearing for iter-90+ impl):
//
//   (1) Name skew (caught at compile-time by header inspection):
//       The §C.b design memo §3.1 + iter-58 fold appendix REQ-2 code-
//       snippets cite `mi_heap_set_default(mi_heap_t*)` and
//       `mi_heap_get_default()`. These names DO NOT EXIST in mimalloc
//       3.3.1 (verified via header inspection: 0 grep hits). The v3+
//       first-class theaps API uses the `mi_theap_*` prefix on the
//       default-redirect pair.
//
//   (2) Implementation gap (caught at link-time by iter-89 build):
//       The header declares `mi_theap_t* mi_theap_set_default(mi_theap_t*)`
//       (mimalloc 3.3.1 mimalloc.h:375) but the brew prebuilt
//       libmimalloc.a does NOT export the symbol — verified via
//       `nm /opt/homebrew/lib/libmimalloc.a | grep mi_theap_set_default`
//       (returns 0 hits). Only `mi_theap_get_default` is exported.
//       Calling `mi_theap_set_default` produces a linker error
//       `Undefined symbols for architecture arm64: _mi_theap_set_default`.
//
//   Consequence for §C.b:
//       The §C.b RAII guard pattern at design.md §3.1 (POST iter-58 fold
//       REQ-2) relies on installing a worker theap as the per-libdispatch-
//       worker default malloc target; the install step REQUIRES
//       `mi_theap_set_default`. Without this symbol, the entire §C.b
//       per-region theap install/restore pattern is BLOCKED at the
//       binary level. The iter-57-ε.2 BINDING reviewer (agentId
//       `af9f467e94d9938ac`) accepted FOLD-CR4(a) v3+ path soundness
//       without verifying the symbol export; this is a missed
//       phantom-API attribution that iter-89 TDD scaffolding catches.
//
//   Iter-90+ design rework options (NOT pursued at iter-89):
//       (a) Build mimalloc from source with the theap-default-set API
//           enabled (custom build flag; not currently in the build
//           recipe; out of allowlist scope).
//       (b) Wait for a mimalloc release that ships the missing symbol.
//       (c) Use per-heap explicit allocation (`mi_heap_malloc(worker,
//           sz)`) throughout HF's hot-path callsites in lieu of
//           default-redirect; much higher LOC delta, defeats the
//           drop-in design.
//       (d) Sunset §C.b per design.md §7.4 FUNDAMENTAL_FLAW disposition.
//
//   Test 3 below ("install_restore_unavailable") REVERTED TO SKIP at
//   iter-89 and documents the unavailability finding. Re-enabling it
//   requires either (a) above or a mimalloc upgrade that exports the
//   missing symbol.

#include <cstddef>
#include <cstdio>
#include <iostream>
#include <vector>

#include <mimalloc.h>

namespace {

// ---------------------------------------------------------------------------
// Test 1: mimalloc version probe (REAL; iter-89).
// Asserts MI_MALLOC_VERSION == 30301 (compile-time) AND mi_version() ==
// 301 (runtime). The compile-time + runtime co-check defends against a
// linked-vs-header version skew (the iter-56-δ phantom-attributed
// "3.4.0" came from a strings(1) probe of an unrelated library; the
// compile-time header inspection is now the canonical probe per
// iter-58 REQ-1).
// ---------------------------------------------------------------------------
int test_mimalloc_version_probe_3_3_1() {
    int rc = 0;
#ifdef MI_MALLOC_VERSION
    if (MI_MALLOC_VERSION != 30301) {
        std::cerr << "[FAIL] mimalloc-version-probe-3-3-1: "
                  << "MI_MALLOC_VERSION (compile-time) = "
                  << MI_MALLOC_VERSION
                  << " expected 30301 (3.3.1)\n";
        rc = 1;
    }
#else
    std::cerr << "[FAIL] mimalloc-version-probe-3-3-1: "
              << "MI_MALLOC_VERSION not defined; mimalloc.h not visible\n";
    rc = 1;
#endif
    const int rt = mi_version();
    if (rt != 30301) {
        std::cerr << "[FAIL] mimalloc-version-probe-3-3-1: "
                  << "mi_version() (runtime) = " << rt
                  << " expected 30301 (mi_version() returns MI_MALLOC_VERSION as one integer)\n";
        rc = 1;
    }
    if (rc == 0) {
        std::cout << "[PASS] mimalloc-version-probe-3-3-1: "
                  << "MI_MALLOC_VERSION=30301 mi_version()=" << rt << "\n";
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Test 2: mi_heap_t lifecycle no-leak (REAL; iter-89).
// Allocates 1000 blocks of 1KiB each from a fresh mi_heap_new(); calls
// mi_heap_destroy(); verifies no crash. This exercises the load-bearing
// §C.b primitive `mi_heap_t* heap = mi_heap_new()` + bulk-free at
// `mi_heap_destroy()` without going through the (phantom) default-heap
// install API. Surfaces any mimalloc 3.3.1 v3+ heap regression early.
// ---------------------------------------------------------------------------
int test_mi_heap_lifecycle_no_leak() {
    constexpr std::size_t kNumBlocks = 1000;
    constexpr std::size_t kBlockSize = 1024;

    mi_heap_t* heap = mi_heap_new();
    if (heap == nullptr) {
        std::cerr << "[FAIL] mi-heap-lifecycle-no-leak: "
                  << "mi_heap_new() returned nullptr\n";
        return 1;
    }
    std::vector<void*> blocks;
    blocks.reserve(kNumBlocks);
    for (std::size_t i = 0; i < kNumBlocks; ++i) {
        void* p = mi_heap_malloc(heap, kBlockSize);
        if (p == nullptr) {
            std::cerr << "[FAIL] mi-heap-lifecycle-no-leak: "
                      << "mi_heap_malloc(heap, " << kBlockSize
                      << ") returned nullptr at i=" << i << "\n";
            mi_heap_destroy(heap);
            return 1;
        }
        blocks.push_back(p);
    }
    mi_heap_destroy(heap);
    std::cout << "[PASS] mi-heap-lifecycle-no-leak: "
              << kNumBlocks << " x " << kBlockSize
              << " B blocks bulk-freed via mi_heap_destroy()\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Test 3: mi_theap_get_default / mi_heap_theap soundness; document
// mi_theap_set_default unavailability in mimalloc 3.3.1 brew binary
// (PARTIAL-REAL + SKIP; iter-89; load-bearing for iter-90+ design).
//
// Per the iter-89 phantom-API discovery (see header comment), the brew
// libmimalloc.a 3.3.1 does NOT export `mi_theap_set_default` even
// though mimalloc.h declares it. The install/restore RAII pattern that
// §C.b design.md §3.1 mandates is therefore BLOCKED at the binary
// level. This test does what is possible at iter-89:
//   (a) verify `mi_theap_get_default()` returns a non-null pointer at
//       process startup (PASS — confirms the theap subsystem is at
//       least partially initialized);
//   (b) verify `mi_heap_new()` + `mi_heap_theap()` returns a non-null
//       theap (PASS — confirms the conversion API works);
//   (c) STOP before calling `mi_theap_set_default` (would not link);
//   (d) print a SKIP/REPORT line documenting the unavailability so
//       the test output explicitly surfaces this constraint to iter-90+
//       readers.
// ---------------------------------------------------------------------------
int test_mi_theap_partial_api_unavailable_set_default() {
    mi_theap_t* startup_default = mi_theap_get_default();
    if (startup_default == nullptr) {
        std::cerr << "[FAIL] mi-theap-partial-api: "
                  << "mi_theap_get_default() returned nullptr at startup; "
                  << "the v3+ theap subsystem is not initialized\n";
        return 1;
    }
    mi_heap_t* worker_heap = mi_heap_new();
    if (worker_heap == nullptr) {
        std::cerr << "[FAIL] mi-theap-partial-api: "
                  << "mi_heap_new() returned nullptr\n";
        return 1;
    }
    mi_theap_t* worker_theap = mi_heap_theap(worker_heap);
    if (worker_theap == nullptr) {
        std::cerr << "[FAIL] mi-theap-partial-api: "
                  << "mi_heap_theap(worker_heap) returned nullptr\n";
        mi_heap_destroy(worker_heap);
        return 1;
    }
    // DO NOT call mi_theap_set_default(worker_theap) — not exported in
    // libmimalloc.a 3.3.1 (brew). Re-enabling this path requires either
    // a custom mimalloc build with the theap-default-set API enabled
    // or a mimalloc release that exports the symbol.
    mi_heap_destroy(worker_heap);
    std::cout << "[REPORT] mi-theap-partial-api-unavailable-set-default: "
              << "mi_theap_get_default()=" << startup_default
              << " mi_heap_theap(new) non-null; "
              << "mi_theap_set_default NOT EXPORTED in libmimalloc.a 3.3.1 (brew); "
              << "§C.b RAII install/restore pattern BLOCKED at binary level. "
              << "See file header for iter-90+ rework options (a)-(d).\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Test 4: mi_collect(false) no-crash under mixed-size alloc pattern
// (REAL; iter-89). Allocates ~10MiB across mixed-size blocks via the
// default heap; calls mi_collect(false) (the heuristic-driven
// non-blocking purge per iter-58 REQ-5); frees; verifies no crash.
// This is the iter-89 minimum bar for the §C.b production wiring's
// mi_collect(false) sequencing decision (per design memo §4.5).
// ---------------------------------------------------------------------------
int test_mi_collect_false_no_crash() {
    constexpr int kNumBlocks = 4096;
    std::vector<void*> blocks;
    blocks.reserve(kNumBlocks);
    for (int i = 0; i < kNumBlocks; ++i) {
        const std::size_t sz =
            (i & 7) == 0 ? 8192 :
            (i & 3) == 0 ? 1024 :
            (i & 1) == 0 ?  256 : 64;
        void* p = mi_malloc(sz);
        if (p == nullptr) {
            std::cerr << "[FAIL] mi-collect-false-no-crash: "
                      << "mi_malloc(" << sz << ") returned nullptr at i="
                      << i << "\n";
            for (void* q : blocks) mi_free(q);
            return 1;
        }
        blocks.push_back(p);
    }
    // The load-bearing call: heuristic-driven non-blocking purge. Per
    // mimalloc 3.x docs, mi_collect(false) does NOT block on concurrent
    // allocators on other threads; this test runs single-threaded, so
    // we only check it does not crash / does not deadlock.
    mi_collect(false);
    for (void* p : blocks) mi_free(p);
    std::cout << "[PASS] mi-collect-false-no-crash: "
              << kNumBlocks << " mixed-size blocks allocated, "
              << "mi_collect(false) returned cleanly, blocks freed\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Test 5: byte-id smoke at OFF path (SKIP; iter-N+ Step C.b-impl-3
// scope). Requires the §C.b production binary (`hf_cb_region_guard.cpp`
// + env-gate at `bridge/cli/main.cpp:3192`) AND the iter-N+ A/B harness
// (`iter5N_ab.py`). Cannot be exercised at iter-89 since the production
// code does not exist yet.
// ---------------------------------------------------------------------------
int test_skip_byte_id_smoke_off_path() {
    std::cout << "[SKIP] byte-id-smoke-off-path "
                 "(iter-89 TDD scaffolding; iter-N+ Step C.b-impl-3 fills body)\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Test 6: falsification gate PASS_PARTIAL band (SKIP; iter-N+ Step
// C.b-impl-3 scope). Requires the production §C.b binary + the iter-N+
// A/B harness aggregate results.json. The gate predicate per §5.2 +
// §5.4 cannot be evaluated at iter-89.
// ---------------------------------------------------------------------------
int test_skip_falsification_gate_pass_partial_band() {
    std::cout << "[SKIP] falsification-gate-pass-partial-band "
                 "(iter-89 TDD scaffolding; iter-N+ Step C.b-impl-3 fills body)\n";
    return 0;
}

} // namespace

int main() {
    int rc = 0;
    rc |= test_mimalloc_version_probe_3_3_1();
    rc |= test_mi_heap_lifecycle_no_leak();
    rc |= test_mi_theap_partial_api_unavailable_set_default();
    rc |= test_mi_collect_false_no_crash();
    rc |= test_skip_byte_id_smoke_off_path();
    rc |= test_skip_falsification_gate_pass_partial_band();
    if (rc == 0) {
        std::cout << "[T-mi-arena] RESULT: PASS  "
                  << "(3 REAL primitives tests + 1 PARTIAL/REPORT + 2 SKIP placeholders; "
                  << "iter-89 phantom-API discovery: mi_theap_set_default missing from "
                  << "libmimalloc.a 3.3.1 brew binary; §C.b design rework REQUIRED)\n";
    } else {
        std::cout << "[T-mi-arena] RESULT: FAIL\n";
    }
    return rc;
}
