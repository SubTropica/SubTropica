// HF FF Phase 6 REVISED §6.D iter-22 — REQ-21.5 R21.6 mitigation:
// TDD red-state guard against zw_tab read-only contract violation
// under cap.
//
// Design memo:
//   notes/hf_finite_field_program/phase6_combined/section_6d_engineering/design.md
//   §7 R21.6 (zw_tab shared via outer-scope shared_ptr under DFS) +
//   REQ-21.5 three-layer mitigation (byte-id smoke + sibling
//   test_linear_factors_cache_key_thread_invariant.cpp + this test).
//
// Rationale.
//   iter-65 / iter-66 source-changes (commits 0883aeb66, 7dd21d981,
//   12f1e8cc0) widened `zw_tab` via outer-scope `shared_ptr<ZWTable>`
//   shared across OMP threads at multiple sites:
//     site 1: bridge/cli/main.cpp + transform.cpp::reglim_word ABI
//             (iter-66 cascade widened the parameter; CLI direct +
//              integration-pipeline transitive callers updated).
//     sites 2/3/4/5/7: lambda-inner allocations replaced by outer-
//             scope reference captures; `HF_SCALAR_REP_REQUIRE_PERSISTENT`
//             defense-in-depth abort guards added.
//   The contract is: zw_tab is constructed ONCE at outer dispatcher
//   entry; the parallel region treats it as READ-ONLY; only the
//   outer-scope owner writes. Under §6.D Strategy (c) with cap = N
//   (3 ≤ N ≤ 6), the read-only invariant must continue to hold even
//   though fewer threads share the same zw_tab.
//
//   R21.6 specifically worries that under cap = N ≠ 13, a write-path
//   the iter-65/66 audit missed could surface (e.g., a lazy memo
//   write under cap=1 single-thread visit order that was previously
//   masked by parallel-thread interleaving). REQ-21.5 R21.6 mitigation:
//   this test asserts the zw_tab read-only contract holds under cap = 3
//   (representative non-trivial cap value).
//
// Purpose. Lock in the zw_tab read-only invariance contract at the API
// boundary. The probe `probe_zw_tab_readonly_invariant_under_cap()`
// returns false today (iter-22a red-state); iter-22b production source
// change adds either (a) a shared_ptr ref-count invariant probe that
// asserts the count is constant across `apply_v1_roundtrip` inner-body
// invocations under cap=3, OR (b) an AddressSanitizer write-watch on
// the underlying ZWTable storage that fires zero writes under cap=3.
// Per design.md §7 R21.6, (a) is the fallback if (b) is too invasive.
//
// Mirror of iter-12 / iter-15 / iter-22a §6.D-cap probe-stub pattern.
// Single binary, ONE ctest entry (mirrors design.md §5 deliverable 2:
// ctest count 37 → 38).
//
// REQ-G atomicity (iter-21 design memo §4). iter-22b's source change
// (which exposes the production accessor + flips the probe below) is
// committed atomically alongside the §6.D dispatcher rewrite.
//
// REQ-21.4 cross-reference. The companion test
// test_integration_section_6d_dfs_cap.cpp Subtest 4
// (`bench-default-off-wall-stability`) gates REQ-21.4 env-var read
// placement. R21.6 + REQ-21.4 + REQ-21.5 LF-cache-key together
// constitute the three-layer R21 mitigation per design.md §7.

#include <cstdio>

// iter-22b green-state: production accessor lives in section_6d_dfs_cap.hpp.
#include "hyperflint/integrator/section_6d_dfs_cap.hpp"

namespace {

bool probe_zw_tab_readonly_invariant_under_cap();

// ---------------------------------------------------------------------------
// Subtest — zw_tab read-only invariance under cap.
//
// REQ-21.5 R21.6 mitigation (cap = 3 representative non-trivial).
// The probe at iter-22b green-state will (option a):
//   (i)   Construct a `shared_ptr<ZWTable> zw_tab` at outer scope.
//   (ii)  Record `zw_tab.use_count()` pre-dispatch.
//   (iii) Invoke `apply_v1_roundtrip()` with cap = 3 (3 OMP threads
//         capture zw_tab via reference per the iter-65/66 ABI cascade).
//   (iv)  Re-record `zw_tab.use_count()` post-dispatch.
//   (v)   Assert pre == post (no write created a new owner / no copy
//         broke the read-only contract).
// Option (b) AddressSanitizer write-watch is the design.md §7 R21.6
// fallback if option (a) ref-count invariance proves insufficient
// (e.g., a copy-on-write inside ZWTable would not bump use_count of
// the outer pointer).
//
// Current state (iter-22a): probe returns false → FAIL.
// ---------------------------------------------------------------------------
int test_zw_tab_readonly_under_cap() {
    if (!probe_zw_tab_readonly_invariant_under_cap()) {
        std::fprintf(stderr,
            "[FAIL] zw_tab read-only-under-cap invariance probe not "
            "yet wired. iter-22b green-state must expose either (a) a "
            "shared_ptr ref-count invariant probe or (b) an "
            "AddressSanitizer write-watch on ZWTable internal storage "
            "+ flip this probe to drive `apply_v1_roundtrip` under "
            "cap=3 + assert zero new owners / writes per design.md §7 "
            "R21.6.\n");
        return 1;
    }
    std::fprintf(stdout, "[PASS] zw-tab-readonly-under-cap\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Probe definition — LOCAL stub (iter-22a red-state).
//
// iter-22b production source change will flip this to delegate to
// `hyperflint::probe_zw_tab_readonly_invariant_under_cap()` defined in
// a new public header.
// ---------------------------------------------------------------------------
// iter-22b green-state: delegate to production accessor.
bool probe_zw_tab_readonly_invariant_under_cap() {
    return ::hyperflint::section_6d::
        probe_zw_tab_readonly_invariant_under_cap();
}

}  // namespace

// ---------------------------------------------------------------------------
// main — single-subtest, no argv dispatch.
//
// CMakeLists.txt registers ONE `add_test` entry pointing at this binary;
// the ctest count increments from 37 to 38 per design.md §5 deliverable 2.
// ---------------------------------------------------------------------------
int main(int /*argc*/, char** /*argv*/) {
    return test_zw_tab_readonly_under_cap();
}
