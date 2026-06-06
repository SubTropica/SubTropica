// HF MZV-rewrite C0c.1 (iter-52c-Tests): OMP-determinism
// unit test for Protocol A's combined->master 2-stage canonical merge.
//
// Design ref:
//   notes/hf_mzv_rewrite_design_2026-05-05/design.md
//     §3.6a (cross-merge protocol; canonical-content sort with handle-
//            relabel-deterministic master assignment),
//     §5.4  ("OMP determinism test (folds adversarial H2 + physics
//            ZWTable cross-merge re-hash)").
//
// Iter-51 reviewer item A4 (advisory; internal review):
// "iter-52 atomic commit MUST include or exercise OMP=1 vs OMP=2 vs
// OMP=13 sha256 bit-identity test on Smirnov tst0/tst1 under
// SCALAR_REP=1.  Mirror design.md:1029 §5.4 test_omp_determinism.cpp.
// ~30 LOC test surface."
//
// Iter-54 audit AUDIT_A2_U1_U3.md §1 nailed the load-bearing invariant:
// the canonical-emission writer's sort keys (SymMonomial::power_key,
// Word::content_key, regkey_content_key) encode ONLY integer powers +
// integration-variable names + Rat polynomial string content; ZWTable
// handle indices NEVER appear in any sort key.  Therefore the END-TO-END
// determinism property reduces to the SINGLE structural property tested
// here: under Protocol A's 2-stage merge, the master ZWTable's handle
// assignment is INVARIANT under the partition order of stage-1 secondary
// tables.
//
// This unit test exercises that invariant directly, mirroring the
// production code path at integration_step.cpp::apply_v1_roundtrip_symcoef
// (lines ~2354-2440 in the iter-52b cascade): build N "per-thread" ZWTables
// in different intern orders and partition orders, run the two-stage
// merge twice with stage-1 ordering permuted, and assert the master
// table's entry-by-handle byte image is byte-identical across the two
// runs.
//
// The Smirnov tst0/tst1 sha256-identity assertion in §5.4 is exercised
// at the bash-gate level (run_smirnov_phase_b_gate.sh) under
// HF_USE_SCALAR_REP=1 with OMP_NUM_THREADS={1,2,13} as part of the iter-55
// step 1b A1 evidence.  This unit test guards the underlying ZWTable
// invariant on every commit, so a future regression in the merge
// protocol will surface here BEFORE the heavier bash gate runs.

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/zw_table.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace hyperflint;

namespace {

bool g_pass = true;

void check(bool cond, const std::string& tag) {
    std::cout << "[" << (cond ? "OK " : "FAIL") << "] " << tag << "\n";
    if (!cond) g_pass = false;
}

// Build a "per-thread" ZWTable with the given content (intern order is
// the vector order; this mirrors the per-thread interning produced by
// each OMP worker in apply_v1_roundtrip_symcoef under HF_USE_SCALAR_REP=1).
ZWTable build_per_thread(const PolyCtx& F,
                         const std::vector<std::string>& content) {
    ZWTable t(F);
    for (const auto& s : content) {
        t.intern(Poly(F, s), ZWTable::Intent::NumIntent);
    }
    return t;
}

// Run Protocol A 2-stage canonical merge:
//   stage 1: combined = empty;  for each per_thread tab: combined.merge_into(tab)
//   stage 2: master   = empty;  master.merge_into(combined)
// Returns the master table's full entry sequence (as polynomial strings,
// indexed by handle 0..size-1) for byte-image comparison.
std::vector<std::string>
run_protocol_a_2stage(const PolyCtx& F,
                      std::vector<ZWTable>& per_thread,
                      const std::vector<size_t>& stage1_order) {
    ZWTable combined(F);
    for (size_t idx : stage1_order) {
        combined.merge_into(per_thread[idx]);
    }
    ZWTable master(F);
    master.merge_into(combined);

    std::vector<std::string> img;
    img.reserve(master.size());
    for (size_t i = 0; i < master.size(); ++i) {
        img.push_back(master.get(static_cast<ZWHandle>(i)).to_string());
    }
    return img;
}

// =====================================================================
// T1: Protocol A 2-stage canonical merge is invariant under stage-1
// partition order.
//
// Models the OMP=N case where N per-thread ZWTables are merged into a
// `combined` table in arbitrary tid order (stage 1), then `combined` is
// merged into the function-parameter `master` table (stage 2).  The
// master's entry-by-handle byte image must be partition-order-INDEPENDENT;
// this is the property that backs design §3.6a / §5.4.
// =====================================================================
void test_protocol_a_two_stage_determinism() {
    PolyCtx F({"x", "y", "s", "mm", "Wm_1", "Wp_1"});

    // Three "per-thread" tables, each with a different (overlapping)
    // content slice and a different intern order. The union of
    // content across the three is 7 distinct polynomials.
    std::vector<std::vector<std::string>> per_thread_content = {
        {"x + s",          "y*s",         "Wm_1 + 1",          "x*y"},
        {"x*y",            "mm + Wp_1",   "y*s",               "x + y"},
        {"x + y",          "Wm_1 + 1",    "x + s",             "mm + Wp_1"},
    };

    auto build_all = [&]() {
        std::vector<ZWTable> v;
        v.reserve(per_thread_content.size());
        for (const auto& c : per_thread_content) {
            v.push_back(build_per_thread(F, c));
        }
        return v;
    };

    // Run A: stage-1 partition order = [0, 1, 2] (mimicking OMP_NUM_THREADS=3
    // tid-canonical order).
    auto pt_a = build_all();
    auto img_a = run_protocol_a_2stage(F, pt_a, {0, 1, 2});

    // Run B: stage-1 partition order = [2, 0, 1] (mimicking OMP scheduler
    // dispatching threads in non-canonical order).
    auto pt_b = build_all();
    auto img_b = run_protocol_a_2stage(F, pt_b, {2, 0, 1});

    // Run C: stage-1 partition order = [1, 2, 0] (third permutation; A6
    // says master assignment depends only on the SET of unique contents
    // in the union, not the partition order).
    auto pt_c = build_all();
    auto img_c = run_protocol_a_2stage(F, pt_c, {1, 2, 0});

    check(img_a.size() == img_b.size(),
          "T1 protocol-a: master.size() identical across partition perms (A vs B)");
    check(img_a.size() == img_c.size(),
          "T1 protocol-a: master.size() identical across partition perms (A vs C)");

    bool ab_match = (img_a == img_b);
    bool ac_match = (img_a == img_c);
    check(ab_match,
          "T1 protocol-a: master entry-by-handle byte image invariant under stage-1 perm (A vs B)");
    check(ac_match,
          "T1 protocol-a: master entry-by-handle byte image invariant under stage-1 perm (A vs C)");

    if (!ab_match || !ac_match) {
        const size_t n = std::min({img_a.size(), img_b.size(), img_c.size()});
        for (size_t i = 0; i < n; ++i) {
            if ((!ab_match && img_a[i] != img_b[i]) ||
                (!ac_match && img_a[i] != img_c[i])) {
                std::cout << "    diff at handle " << i
                          << ": A=" << img_a[i]
                          << "  B=" << img_b[i]
                          << "  C=" << img_c[i] << "\n";
            }
        }
    }
}

// =====================================================================
// T2: Protocol A 2-stage canonical merge is invariant under intra-thread
// intern order.
//
// Models the case where the OMP scheduler dispatches the same workload
// across threads but the thread-local intern order varies (e.g., a
// different std::unordered_map iteration order produces a different
// intern sequence inside each per-thread loop).  Per A6 + iter-54 audit
// §1.4, the master's handle assignment must depend only on the UNIQUE
// CONTENT SET, not the intern order inside each per-thread secondary.
// =====================================================================
void test_intra_thread_intern_order_invariance() {
    PolyCtx F({"x", "y", "s", "mm"});

    std::vector<std::string> content_set = {
        "x + s",  "y*s",  "x*y",  "mm + 1", "x + y",
    };

    // pt_x interns in forward order; pt_y in reverse order.
    auto pt_x_fwd = build_per_thread(F, content_set);
    std::vector<std::string> rev = content_set;
    std::reverse(rev.begin(), rev.end());
    auto pt_y_rev = build_per_thread(F, rev);

    // Drive both through Protocol A as if they were the SOLE per-thread
    // secondary (single-thread case under HF_USE_SCALAR_REP=1).
    std::vector<ZWTable> v_fwd; v_fwd.push_back(std::move(pt_x_fwd));
    std::vector<ZWTable> v_rev; v_rev.push_back(std::move(pt_y_rev));

    auto img_fwd = run_protocol_a_2stage(F, v_fwd, {0});
    auto img_rev = run_protocol_a_2stage(F, v_rev, {0});

    check(img_fwd == img_rev,
          "T2 protocol-a: master byte image invariant under intra-thread intern reverse");
    if (img_fwd != img_rev) {
        const size_t n = std::min(img_fwd.size(), img_rev.size());
        for (size_t i = 0; i < n; ++i) {
            if (img_fwd[i] != img_rev[i]) {
                std::cout << "    diff at handle " << i
                          << ": fwd=" << img_fwd[i]
                          << "  rev=" << img_rev[i] << "\n";
            }
        }
    }
}

// =====================================================================
// T3: master state under Protocol A is identical to the state that
// would be produced by a SINGLE-thread run (one secondary containing
// the full union content set).
//
// This is the Protocol A "value-equivalence under thread-count
// variation" property: OMP_NUM_THREADS=1 and OMP_NUM_THREADS=N must
// produce indistinguishable master ZWTables.  Per design §5.4 the same
// property at the writer (RegulatorSym sha256) level is exercised by
// the bash gate; this test guards the ZWTable layer that carries it.
// =====================================================================
void test_omp1_vs_ompN_master_equivalence() {
    PolyCtx F({"x", "y", "s", "Wm_1", "Wp_1"});

    // OMP=1 path: a single secondary holding the full union.
    std::vector<std::string> union_content = {
        "x + s", "y*s", "x*y", "Wm_1 + 1", "x + y", "Wp_1 - 1",
    };
    auto pt_omp1 = build_per_thread(F, union_content);
    std::vector<ZWTable> v_omp1; v_omp1.push_back(std::move(pt_omp1));
    auto img_omp1 = run_protocol_a_2stage(F, v_omp1, {0});

    // OMP=3 path: three per-thread secondaries with disjoint slices.
    std::vector<std::vector<std::string>> slices = {
        {"x + s",       "y*s"},
        {"x*y",         "Wm_1 + 1"},
        {"x + y",       "Wp_1 - 1"},
    };
    std::vector<ZWTable> pt_omp3;
    pt_omp3.reserve(slices.size());
    for (const auto& s : slices) pt_omp3.push_back(build_per_thread(F, s));
    auto img_omp3 = run_protocol_a_2stage(F, pt_omp3, {0, 1, 2});

    check(img_omp1.size() == img_omp3.size(),
          "T3 omp1-vs-ompN: master.size() identical (single-secondary vs 3-secondary union)");
    check(img_omp1 == img_omp3,
          "T3 omp1-vs-ompN: master byte image identical across thread-count variation");
    if (img_omp1 != img_omp3) {
        const size_t n = std::min(img_omp1.size(), img_omp3.size());
        for (size_t i = 0; i < n; ++i) {
            if (img_omp1[i] != img_omp3[i]) {
                std::cout << "    diff at handle " << i
                          << ": omp1=" << img_omp1[i]
                          << "  omp3=" << img_omp3[i] << "\n";
            }
        }
    }
}

}  // namespace

int main() {
    test_protocol_a_two_stage_determinism();   // T1 (§3.6a / §5.4 partition perm)
    test_intra_thread_intern_order_invariance(); // T2 (intra-thread intern perm)
    test_omp1_vs_ompN_master_equivalence();    // T3 (OMP=1 vs OMP=N master)

    std::cout << "\n[summary] " << (g_pass ? "PASS" : "FAIL") << "\n";
    return g_pass ? 0 : 1;
}
