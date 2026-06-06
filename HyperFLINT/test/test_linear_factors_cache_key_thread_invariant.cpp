// HF FF Phase 6 REVISED §6.D iter-22 — REQ-21.5 R21.1 mitigation:
// TDD red-state guard against LF cache key invariance regression.
//
// Design memo:
//   notes/hf_finite_field_program/phase6_combined/section_6d_engineering/design.md
//   §7 R21.1 (LF cache key invariance under DFS reordering) + REQ-21.5
//   three-layer mitigation (byte-id smoke + this test + sibling
//   test_transform_zw_tab_readonly_under_cap.cpp).
//
// Rationale.
//   Second BINDING reviewer (NAMED agentId b3f24a9c7d1e5680f) audited
//   `linear_factors_cache_key()` at HyperFLINT/src/algebra/linear_factors.cpp:761-766
//   and confirmed the function is purely structural: it returns
//   `poly_struct_hash(p, var_idx, introduce_al, sqf, compute_constant)`,
//   which is keyed only on the polynomial + variable + flags — NO OMP
//   thread id, NO counter, NO thread-local state in the key. Therefore
//   `cache_key(ctx_A) == cache_key(ctx_B)` for two contexts differing
//   only in OMP thread id is a TAUTOLOGY at HEAD (iter-21 commit
//   f45332fa5).
//
//   Per the second reviewer's recommendation, this test is kept as a
//   defense-in-depth REGRESSION GUARD: it locks in the invariant
//   "cache_key is thread-id-independent" so a future refactor that
//   introduces a thread-id, omp_get_thread_num(), or other thread-
//   local state into the cache key path is caught at ctest. R21.1's
//   §6.D worry (BFS-to-DFS visit reorder invalidates cache key) is
//   structurally false today; if it ever becomes true, this test
//   fires.
//
// Purpose. Lock in the cache-key thread invariance contract at the API
// boundary. The probe `probe_cache_key_thread_invariant()` returns
// false today (iter-22a red-state); iter-22b production source change
// exposes `linear_factors_cache_key()` (or a test-only invariant
// accessor) via a public header and flips the probe to actually
// compute two keys and compare.
//
// Mirror of iter-12 / iter-15 probe-stub pattern. Single binary, ONE
// ctest entry (mirrors design.md §5 deliverable 2: ctest count 36 →
// 37). The subtest delegates to `probe_cache_key_thread_invariant()`
// defined at the bottom of this file.
//
// REQ-G atomicity (iter-21 design memo §4). iter-22b's source change
// (which exposes the production accessor + flips the probe below) is
// committed atomically alongside the §6.D dispatcher rewrite.

#include <cstdio>

// iter-22b green-state: production accessor lives in section_6d_dfs_cap.hpp.
#include "hyperflint/integrator/section_6d_dfs_cap.hpp"

namespace {

bool probe_cache_key_thread_invariant();

// ---------------------------------------------------------------------------
// Subtest — Cache-key thread invariance.
//
// REQ-21.5 R21.1 mitigation (defense-in-depth regression guard).
// The probe at iter-22b green-state will:
//   (i)  Construct two minimal `Poly` contexts ctx_A, ctx_B that differ
//        only in OMP thread id (verified via `omp_get_thread_num()`).
//   (ii) Compute `linear_factors_cache_key(p, var, false, false, false)`
//        in both threads.
//   (iii) Compare the returned LFCacheKey pairs; equal → PASS.
//
// Current state (iter-22a): probe returns false → FAIL.
// ---------------------------------------------------------------------------
int test_cache_key_thread_invariant() {
    if (!probe_cache_key_thread_invariant()) {
        std::fprintf(stderr,
            "[FAIL] linear_factors_cache_key() thread-invariance "
            "probe not yet wired. iter-22b green-state must expose "
            "`hyperflint::linear_factors_cache_key()` (currently "
            "unnamed-namespace-internal at src/algebra/linear_factors.cpp:761) "
            "or a test-only `cache_key_thread_invariant_probe()` "
            "accessor + flip this probe to compute two keys across "
            "two OMP threads and compare.\n");
        return 1;
    }
    std::fprintf(stdout, "[PASS] lf-cache-key-thread-invariant\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Probe definition — LOCAL stub (iter-22a red-state).
//
// iter-22b production source change will flip this to delegate to
// `hyperflint::probe_lf_cache_key_thread_invariant()` defined in a new
// public header. Second reviewer's audit of linear_factors.cpp:761-766
// confirms the function is purely structural (no thread-id-keyed
// state), so the green-state probe will PASS immediately on first
// invocation.
// ---------------------------------------------------------------------------
// iter-22b green-state: delegate to production accessor.
bool probe_cache_key_thread_invariant() {
    return ::hyperflint::section_6d::probe_lf_cache_key_thread_invariant();
}

}  // namespace

// ---------------------------------------------------------------------------
// main — single-subtest, no argv dispatch.
//
// CMakeLists.txt registers ONE `add_test` entry pointing at this binary;
// the ctest count increments from 36 to 37 per design.md §5 deliverable 2.
// ---------------------------------------------------------------------------
int main(int /*argc*/, char** /*argv*/) {
    return test_cache_key_thread_invariant();
}
