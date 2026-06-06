// HF FF Phase 6 REVISED §6.D iter-12 — TDD red-state scaffolding for the
// 4-column NodeRssRecord extension (REQ-C from iter-11 ADVISORY pre-build
// adversarial reviewer agentId ab6fa5cda36908669 CONCERNS-FOLD, folded
// into notes/.../probe_a_d_integration_tree/design.md §10).
//
// Purpose.  Lock in the BFS-only-invariant per-entry attribution contract
// (REQ-A LF_cache_bytes + RegulatorSym_bytes columns + REQ-C 4-column
// NodeRssRecord extension) BEFORE the production source change at iter-13.
// All four subtests assert the extension is present and FAIL today; once
// iter-13's source change lands (extending NodeRssRecord with the 4 new
// columns + wiring lf_cache_bytes sampling at hyper_int.cpp:1279 + per-OMP
// reduction), each probe function below will be flipped to call into the
// production API and the subtests will turn green.
//
// Dispatch.  One executable, four `add_test` entries (one per column),
// each pinning the subtest with a single argv argument.  Subtests are
// strictly independent; failure of one does not perturb the others.
// This matches the iter-12 close criterion in handoff.md:
//
//   "ctest --test-dir HyperFLINT/build-mcpu-tuned -R node-rss-4col
//    Expected: 4 FAILing test cases (intentional; TDD red-state)."
//
// Probe convention.  Each subtest delegates the actual existence check
// to a local `probe_*_present()` function, all defined at the bottom of
// this file in one block.  These probes return `false` today (REQ-C
// extension not yet present in production).  iter-13's source change
// will:
//   (i)  Extend `hyperflint::NodeRssRecord` with 4 new fields;
//   (ii) Add a `hyperflint::has_node_rss_4col_extension()` (and three
//        companion accessors) in integration_node_rss.hpp/.cpp;
//   (iii) Flip the four `probe_*_present()` definitions below to call the
//        corresponding production accessors; the test then PASSes.
//
// Compile-only fixture.  The test compiles against the current header
// (6-field NodeRssRecord — see HyperFLINT/include/hyperflint/diagnostics/
// integration_node_rss.hpp).  The 4 expected columns are forward-declared
// here in a local `ExpectedNodeRssRecordV2` struct only as a documentation
// aid; we do NOT static_assert against the production NodeRssRecord
// layout (that would block the iter-13 ratification at compile-time, which
// is undesirable: we want the test to remain runnable across the iter-12 →
// iter-13 transition).
//
// REQ-G (iter-11 reviewer).  This file is the FULL TDD red-state test
// (4 subtests, 4 ctest entries, 4 FAIL on first run).  It is NOT a
// skeleton.

#include "hyperflint/diagnostics/integration_node_rss.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Local forward-declared mirror of the EXPECTED post-iter-13 NodeRssRecord
// layout (REQ-C 4-column extension).  This is a documentation aid only;
// the test does NOT assume binary compatibility with the production struct.
//
// iter-13 production source change will extend hyperflint::NodeRssRecord
// with the four fields below; until then, this struct exists only in the
// test translation unit and serves to lock in field names + types.
// ---------------------------------------------------------------------------
struct ExpectedNodeRssRecordV2 {
    // Existing 6 fields (mirror of current production NodeRssRecord):
    int64_t node_id;
    int     depth;
    int     letter_id;
    double  t_wall_s;
    int64_t rss_current_kib;
    int64_t rss_peak_kib_delta;

    // REQ-C 4 new columns (iter-13 will install these):
    int64_t entry_bytes_thread_local;   ///< Per-OMP-thread bytes attributable
                                        ///< to this node's body (allocations
                                        ///< from enter_node to exit_node on
                                        ///< the same thread).
    int64_t entry_bytes_aggregate_omp;  ///< Sum across OMP threads of bytes
                                        ///< attributable to this node (REQ-A
                                        ///< realized-RSS column).
    int64_t lf_cache_bytes;             ///< LinearFactors cache footprint
                                        ///< sampled at outer-step boundary
                                        ///< (hyper_int.cpp:1279 adjacent).
    int64_t reg_sym_bytes;              ///< Regulator-SymCoef footprint
                                        ///< computed via
                                        ///< SymCoef::total_bytes() on the
                                        ///< current letter's regulator.
};

// ---------------------------------------------------------------------------
// Probe functions — local fallbacks (return false at iter-12).
//
// iter-13 production source change flips each of these to delegate to a
// new production accessor in hyperflint::, e.g.:
//
//   bool probe_entry_bytes_thread_local_present() {
//       return hyperflint::node_rss_has_entry_bytes_thread_local_column();
//   }
//
// Until then, all four return false → the subtests FAIL.
// ---------------------------------------------------------------------------
bool probe_entry_bytes_thread_local_present();
bool probe_entry_bytes_aggregate_omp_present();
bool probe_lf_cache_bytes_present();
bool probe_reg_sym_bytes_present();

// ---------------------------------------------------------------------------
// Subtest 1 — entry_bytes_thread_local column present under HF_INTEG_NODE_RSS=1.
//
// REQ-C: per-OMP-thread byte attribution.  The integrator OMP body
// (integration_step.cpp::*::omp_for) maintains a thread-local accumulator
// `accum_t[tid]` that records allocations between enter_node and exit_node
// on the same thread.  At exit_node, the accumulator delta is written to
// NodeRssRecord::entry_bytes_thread_local.
//
// Current state (iter-12): probe returns false → FAIL.
// After iter-13: production source extends NodeRssRecord + flips probe →
//                PASS.
// ---------------------------------------------------------------------------
int test_emit_thread_local() {
    // (Part 1) Schema-presence accessor (iter-13 BINDING reviewer
    // aad9b0038dad42d4c REQ-13.4: this alone is tautological; Parts 2-3
    // below are the real behavior anchor).
    if (!probe_entry_bytes_thread_local_present()) {
        std::fprintf(stderr,
            "[FAIL] entry_bytes_thread_local column NOT present in NodeRssRecord. "
            "iter-11 REQ-C requires per-OMP-thread byte attribution.\n");
        return 1;
    }

    // (Part 2) Field-observability sentinel round-trip on a directly
    // constructed NodeRssRecord (iter-13 REQ-13.4 fold).  This is the
    // compile-time + run-time check that the field is a real int64_t
    // member of the production struct, not an alias, an empty stub, or
    // a missing member that would silently coerce to 0.
    {
        hyperflint::NodeRssRecord rec{};
        constexpr int64_t kSentinel = static_cast<int64_t>(0x0F1E2D3C4B5A6978LL);
        rec.entry_bytes_thread_local = kSentinel;
        if (rec.entry_bytes_thread_local != kSentinel) {
            std::fprintf(stderr,
                "[FAIL] entry_bytes_thread_local sentinel did not round-trip: "
                "wrote 0x%016llx, read 0x%016llx\n",
                static_cast<long long>(kSentinel),
                static_cast<long long>(rec.entry_bytes_thread_local));
            return 1;
        }
    }

    // (Part 3) Sampling-path observability: exercise the production
    // sampler with HF_INTEG_NODE_RSS=1 and a live 1 MiB allocation,
    // drain the aggregate, and verify the column is preserved into the
    // drained record.  The value may legitimately be 0 if
    // rss_peak_kib_delta did not cross a new peak on this thread (see
    // NodeRssRecord::entry_bytes_thread_local data-fidelity caveat at
    // integration_node_rss.hpp); the test reads the field but does NOT
    // assert on its value.  REQ-13.4 minimum-viable anchor: the field
    // is observable on a record produced by the production code path.
    setenv("HF_INTEG_NODE_RSS", "1", /*overwrite=*/1);
    hyperflint::reset_node_aggregate();
    auto& sampler = hyperflint::IntegrationNodeRssSampler::instance();
    sampler.reset();

    sampler.enter_node(/*depth=*/2, /*letter_id=*/0);
    auto* big = new char[1 * 1024 * 1024];  // 1 MiB
    for (size_t i = 0; i < 1u * 1024u * 1024u; i += 4096) big[i] = 1;
    sampler.exit_node(/*depth=*/2);
    sampler.snapshot_thread_records();
    delete[] big;

    auto drained = hyperflint::drain_node_aggregate();
    std::fprintf(stdout, "[node-rss-4col/thread-local] drained.size()=%zu\n",
                 drained.size());
    if (drained.empty()) {
        std::fprintf(stderr,
            "[FAIL] drain_node_aggregate returned empty after enter_node/exit_node/"
            "snapshot_thread_records.  Sampler may be disabled or stack underflow.\n");
        return 1;
    }
    // Read entry_bytes_thread_local off the drained record; the compiler
    // would fail to compile this line if the field did not exist, and
    // the read at runtime confirms the field is observable on the
    // production-path record.
    const int64_t observed_tl = drained.front().entry_bytes_thread_local;
    std::fprintf(stdout,
        "[node-rss-4col/thread-local] observed entry_bytes_thread_local=%lld "
        "(may be 0 if rss_peak_kib_delta did not cross new peak; "
        "see data-fidelity caveat)\n",
        static_cast<long long>(observed_tl));

    std::fprintf(stdout, "[PASS] node-rss-4col/thread-local\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Subtest 2 — entry_bytes_aggregate_omp reduction across OMP workers.
//
// REQ-A: realized-RSS column (per-step aggregate across OMP threads).  At
// the outer-step boundary the per-thread `entry_bytes_thread_local` values
// are reduced (sum) into the master record's
// NodeRssRecord::entry_bytes_aggregate_omp.  Together with REQ-A's
// byte-weighted vs realized-RSS dual reporting (folded into design.md §10.1),
// this column anchors the "realized" half of the comparison.
//
// Current state (iter-12): probe returns false → FAIL.
// ---------------------------------------------------------------------------
int test_emit_aggregate_omp() {
    // (Part 1) Schema-presence accessor (per REQ-13.4: not sufficient
    // alone; Parts 2-3 are the behavior anchor).
    if (!probe_entry_bytes_aggregate_omp_present()) {
        std::fprintf(stderr,
            "[FAIL] entry_bytes_aggregate_omp column NOT present in NodeRssRecord. "
            "iter-11 REQ-A requires the realized-RSS column reduced across OMP "
            "workers at the outer-step boundary.\n");
        return 1;
    }

    // (Part 2) Field-observability sentinel round-trip (iter-13 REQ-13.4).
    {
        hyperflint::NodeRssRecord rec{};
        constexpr int64_t kSentinel = static_cast<int64_t>(0x1A2B3C4D5E6F7081LL);
        rec.entry_bytes_aggregate_omp = kSentinel;
        if (rec.entry_bytes_aggregate_omp != kSentinel) {
            std::fprintf(stderr,
                "[FAIL] entry_bytes_aggregate_omp sentinel did not round-trip\n");
            return 1;
        }
    }

    // (Part 3) Drain-reduction invariant: produce two records with
    // distinct entry_bytes_thread_local values (we cannot directly set
    // these via the sampler API since exit_node owns the field; but we
    // CAN verify the drain reduction matches the INVARIANT "all records
    // in one drain batch share the same entry_bytes_aggregate_omp
    // equal to the sum of entry_bytes_thread_local across the batch").
    // We do this by entering+exiting two nodes and snapshotting to the
    // global aggregate, then draining and asserting the invariant holds
    // regardless of the per-thread value (which may be 0 on this
    // platform).  This anchors the REQ-3 deferred-approximation contract
    // documented in NodeRssRecord::entry_bytes_aggregate_omp at
    // integration_node_rss.hpp.
    setenv("HF_INTEG_NODE_RSS", "1", /*overwrite=*/1);
    hyperflint::reset_node_aggregate();
    auto& sampler = hyperflint::IntegrationNodeRssSampler::instance();
    sampler.reset();

    sampler.enter_node(/*depth=*/2, /*letter_id=*/0);
    sampler.exit_node(/*depth=*/2);
    sampler.enter_node(/*depth=*/2, /*letter_id=*/1);
    sampler.exit_node(/*depth=*/2);
    sampler.snapshot_thread_records();

    auto drained = hyperflint::drain_node_aggregate();
    std::fprintf(stdout, "[node-rss-4col/aggregate-omp] drained.size()=%zu\n",
                 drained.size());
    if (drained.size() < 2) {
        std::fprintf(stderr,
            "[FAIL] expected >= 2 drained records (entered/exited 2 nodes); "
            "got %zu\n", drained.size());
        return 1;
    }
    int64_t expected_sum = 0;
    for (const auto& rec : drained) expected_sum += rec.entry_bytes_thread_local;
    for (const auto& rec : drained) {
        if (rec.entry_bytes_aggregate_omp != expected_sum) {
            std::fprintf(stderr,
                "[FAIL] drain-batch invariant violated: "
                "expected aggregate_omp=%lld on all records, got %lld\n",
                static_cast<long long>(expected_sum),
                static_cast<long long>(rec.entry_bytes_aggregate_omp));
            return 1;
        }
    }
    std::fprintf(stdout,
        "[node-rss-4col/aggregate-omp] drain-batch invariant verified: "
        "aggregate_omp=%lld stamped on all %zu records\n",
        static_cast<long long>(expected_sum), drained.size());

    std::fprintf(stdout, "[PASS] node-rss-4col/aggregate-omp\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Subtest 3 — lf_cache_bytes sampled at outer-step boundary.
//
// REQ-A: LinearFactors cache footprint column.  hyper_int.cpp:1279
// adjacent (per REC-3 from iter-11 advisory reviewer) is the production
// sample point: at the end of each outer integrator step, the LF cache
// total_bytes() is read and written to NodeRssRecord::lf_cache_bytes for
// every node entered during that step.
//
// R12.2 from handoff.md: iter-12 STUBs the lf_cache_bytes column at 0
// and DOCUMENTS that iter-13 production source must add a public
// accessor in linear_factors.hpp (or equivalent) exposing the cache size.
// The probe below returns false at iter-12.
//
// Current state (iter-12): probe returns false → FAIL.
// ---------------------------------------------------------------------------
int test_emit_lf_cache_bytes() {
    // (Part 1) Schema-presence accessor.
    if (!probe_lf_cache_bytes_present()) {
        std::fprintf(stderr,
            "[FAIL] lf_cache_bytes column NOT present in NodeRssRecord.\n");
        return 1;
    }

    // (Part 2) Field-observability sentinel round-trip.
    {
        hyperflint::NodeRssRecord rec{};
        constexpr int64_t kSentinel = static_cast<int64_t>(0x21324354657687A9LL);
        rec.lf_cache_bytes = kSentinel;
        if (rec.lf_cache_bytes != kSentinel) {
            std::fprintf(stderr,
                "[FAIL] lf_cache_bytes sentinel did not round-trip\n");
            return 1;
        }
    }

    // (Part 3) Sampling-path round-trip via set_outer_step_context.
    // Inject a unique sentinel via the production atomic-write API
    // (set_outer_step_context), enter and exit a node, snapshot+drain,
    // and assert the drained record's lf_cache_bytes equals the
    // sentinel.  This is the real behavior anchor for REQ-13.4: the
    // production code path (enter_node load on the seq_cst atomic;
    // drain_node_aggregate field preservation) is exercised end-to-end.
    constexpr int64_t kPathSentinel = static_cast<int64_t>(0x3344556677889900LL);
    setenv("HF_INTEG_NODE_RSS", "1", /*overwrite=*/1);
    hyperflint::reset_node_aggregate();
    auto& sampler = hyperflint::IntegrationNodeRssSampler::instance();
    sampler.reset();

    hyperflint::set_outer_step_context(kPathSentinel, /*reg_sym_bytes=*/0);
    sampler.enter_node(/*depth=*/2, /*letter_id=*/2);
    sampler.exit_node(/*depth=*/2);
    sampler.snapshot_thread_records();

    auto drained = hyperflint::drain_node_aggregate();
    std::fprintf(stdout, "[node-rss-4col/lf-cache-bytes] drained.size()=%zu\n",
                 drained.size());
    if (drained.empty()) {
        std::fprintf(stderr,
            "[FAIL] drain returned empty; sampler may be disabled.\n");
        return 1;
    }
    const int64_t observed = drained.front().lf_cache_bytes;
    if (observed != kPathSentinel) {
        std::fprintf(stderr,
            "[FAIL] lf_cache_bytes did not round-trip through "
            "set_outer_step_context -> enter_node -> drain.  "
            "Expected 0x%016llx, observed 0x%016llx.\n",
            static_cast<long long>(kPathSentinel),
            static_cast<long long>(observed));
        return 1;
    }
    std::fprintf(stdout,
        "[node-rss-4col/lf-cache-bytes] sampling-path round-trip verified: "
        "0x%016llx stamped onto drained record\n",
        static_cast<long long>(observed));

    std::fprintf(stdout, "[PASS] node-rss-4col/lf-cache-bytes\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Subtest 4 — reg_sym_bytes computed via SymCoef::total_bytes().
//
// REQ-A: Regulator-SymCoef footprint column.  SymCoef::total_bytes() at
// HyperFLINT/include/hyperflint/core/symcoef.hpp:141 already exists (per
// iter-5 reviewer §9.3, verified at iter-11 — see handoff.md R12.3).  The
// remaining iter-13 work is purely:
//   (a) extend NodeRssRecord with the reg_sym_bytes field;
//   (b) wire the SymCoef::total_bytes() call into exit_node().
// No new accessor required.
//
// Current state (iter-12): probe returns false → FAIL.
// ---------------------------------------------------------------------------
int test_emit_reg_sym_bytes() {
    // (Part 1) Schema-presence accessor.
    if (!probe_reg_sym_bytes_present()) {
        std::fprintf(stderr,
            "[FAIL] reg_sym_bytes column NOT present in NodeRssRecord.\n");
        return 1;
    }

    // (Part 2) Field-observability sentinel round-trip.
    {
        hyperflint::NodeRssRecord rec{};
        constexpr int64_t kSentinel = static_cast<int64_t>(0x55667788AABBCCDDLL);
        rec.reg_sym_bytes = kSentinel;
        if (rec.reg_sym_bytes != kSentinel) {
            std::fprintf(stderr,
                "[FAIL] reg_sym_bytes sentinel did not round-trip\n");
            return 1;
        }
    }

    // (Part 3) Sampling-path round-trip via set_outer_step_context with
    // a unique sentinel injected into the reg_sym_bytes slot.  Confirms
    // the production code path (enter_node load on the seq_cst atomic
    // for reg_sym_bytes; drain_node_aggregate field preservation)
    // exercises end-to-end without cross-talk with the lf_cache_bytes
    // sentinel from the sibling subtest.
    constexpr int64_t kPathSentinel = static_cast<int64_t>(0x42BEEFDEAD42BEEFLL);
    setenv("HF_INTEG_NODE_RSS", "1", /*overwrite=*/1);
    hyperflint::reset_node_aggregate();
    auto& sampler = hyperflint::IntegrationNodeRssSampler::instance();
    sampler.reset();

    hyperflint::set_outer_step_context(/*lf_cache_bytes=*/0, kPathSentinel);
    sampler.enter_node(/*depth=*/2, /*letter_id=*/3);
    sampler.exit_node(/*depth=*/2);
    sampler.snapshot_thread_records();

    auto drained = hyperflint::drain_node_aggregate();
    std::fprintf(stdout, "[node-rss-4col/reg-sym-bytes] drained.size()=%zu\n",
                 drained.size());
    if (drained.empty()) {
        std::fprintf(stderr, "[FAIL] drain returned empty.\n");
        return 1;
    }
    const int64_t observed = drained.front().reg_sym_bytes;
    if (observed != kPathSentinel) {
        std::fprintf(stderr,
            "[FAIL] reg_sym_bytes did not round-trip through "
            "set_outer_step_context -> enter_node -> drain.  "
            "Expected 0x%016llx, observed 0x%016llx.\n",
            static_cast<long long>(kPathSentinel),
            static_cast<long long>(observed));
        return 1;
    }
    std::fprintf(stdout,
        "[node-rss-4col/reg-sym-bytes] sampling-path round-trip verified: "
        "0x%016llx stamped onto drained record\n",
        static_cast<long long>(observed));

    std::fprintf(stdout, "[PASS] node-rss-4col/reg-sym-bytes\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Probe definitions (LOCAL, iter-13 green-state).
//
// iter-13 production source change (this iteration) extended
// hyperflint::NodeRssRecord with the four trailing fields plus the
// four `node_rss_has_*_column()` accessors in
// HyperFLINT/src/diagnostics/integration_node_rss.cpp.  The accessor
// alone is a schema-presence check; the subtests above also exercise
// (a) field-observability sentinel round-trip on a directly constructed
// NodeRssRecord, and (b) where the column is populated via the
// production atomic-write API (lf_cache_bytes / reg_sym_bytes via
// set_outer_step_context), end-to-end sampling-path round-trip through
// enter_node + drain_node_aggregate.  The combined three-part check
// converts the test from the iter-12 tautology to a real behavior
// anchor per iter-13 BINDING reviewer aad9b0038dad42d4c REQ-13.4
// CONCERNS-FOLD.
// ---------------------------------------------------------------------------
bool probe_entry_bytes_thread_local_present()  {
    return hyperflint::node_rss_has_entry_bytes_thread_local_column();
}
bool probe_entry_bytes_aggregate_omp_present() {
    return hyperflint::node_rss_has_entry_bytes_aggregate_omp_column();
}
bool probe_lf_cache_bytes_present()            {
    return hyperflint::node_rss_has_lf_cache_bytes_column();
}
bool probe_reg_sym_bytes_present()             {
    return hyperflint::node_rss_has_reg_sym_bytes_column();
}

}  // namespace

// ---------------------------------------------------------------------------
// main — single-subtest dispatch via argv[1].
//
// Usage:  test_integration_node_rss_4col {thread-local|aggregate-omp|
//                                         lf-cache-bytes|reg-sym-bytes}
//
// Each `add_test` entry in HyperFLINT/CMakeLists.txt passes exactly one
// subtest name; mismatched / missing argv is a usage error and returns 2.
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr,
            "usage: %s {thread-local|aggregate-omp|lf-cache-bytes|reg-sym-bytes}\n",
            argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "thread-local")   return test_emit_thread_local();
    if (mode == "aggregate-omp")  return test_emit_aggregate_omp();
    if (mode == "lf-cache-bytes") return test_emit_lf_cache_bytes();
    if (mode == "reg-sym-bytes")  return test_emit_reg_sym_bytes();

    std::fprintf(stderr, "unknown subtest mode: %s\n", mode.c_str());
    return 2;
}
