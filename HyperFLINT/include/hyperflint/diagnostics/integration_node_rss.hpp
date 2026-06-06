// integration_node_rss — per-integration-node RSS sampler.
//
// Records peak-RSS deltas across individual integration nodes (letter-loop
// iterations / recursion depth levels) inside the HF integrator.  This is
// the Phase 0 task 0-4 sampler; the hook into integration_step.cpp is added
// in task 0-5.
//
// Activation.  Set the environment variable HF_INTEG_NODE_RSS to a positive
// integer N before launching HF.  The sampler enables itself and records
// nodes at depth >= N.  Unset or "0" → disabled (zero overhead).
//
//   HF_INTEG_NODE_RSS=1   records all depths
//   HF_INTEG_NODE_RSS=2   records depth >= 2 (plan default)
//
// Thread-safety.  The singleton is thread_local: each OMP thread gets its own
// instance and its own records vector.  No locking is needed and there are no
// races.  Aggregation across threads is done via snapshot_thread_records() /
// drain_aggregate() — Task 0-5 hooks added to integration_step.cpp call
// snapshot_thread_records() at the end of the OMP body, and drain_node_aggregate()
// is called once post-barrier to collect all threads' records for emission.
//
// Design note.  The `stack_` member supports nested enter_node/exit_node
// calls (depth N calling depth N+1 inside the same thread), which is the
// natural recursion pattern inside the integrator's per-letter loop.
//
// Namespace.  Lives in hyperflint (project convention; not hf::diag).

#pragma once
#include "hyperflint/diagnostics/step_trace_rss.hpp"
#include <mutex>
#include <vector>
#include <cstdint>

namespace hyperflint {

/// Record of one completed integration-node sample.
///
/// Phase 6 REVISED §6.D iter-13 (REQ-C from iter-11 advisory reviewer
/// agentId ab6fa5cda36908669 CONCERNS-FOLD, folded into
/// notes/.../probe_a_d_integration_tree/design.md §10.3): the four
/// trailing fields (entry_bytes_thread_local, entry_bytes_aggregate_omp,
/// lf_cache_bytes, reg_sym_bytes) are the BFS-only per-entry attribution
/// columns required by the §6.D probe.  Default-initialized to 0 so
/// callers that do not set them (or build modes where
/// HF_INTEG_NODE_RSS is disabled) see zeros, not garbage.
struct NodeRssRecord {
    int64_t node_id;           ///< Monotonically increasing per-sampler instance
    int     depth;             ///< Recursion depth at which the node was entered
    int     letter_id;         ///< Integration letter index (0-based)
    double  t_wall_s;          ///< Wall time for the node body (seconds)
    int64_t rss_current_kib;   ///< Running RSS at node entry (KiB); -1 on error
    int64_t rss_peak_kib_delta;///< Process-lifetime peak-RSS increase across the
                               ///< node body (KiB); negative means no new peak

    // ---- iter-11 REQ-C 4-column extension (iter-13 production source) -----
    //
    // iter-13 BINDING reviewer aad9b0038dad42d4c REQ-13.2 / REQ-13.5
    // CONCERNS-FOLD: entry_bytes_thread_local BYTE-RESTATES the per-thread
    // peak-RSS delta, NOT an allocator-instrumented byte count.  These
    // are not the same quantity:
    //
    //   * When the node body allocates ≤ current process peak (no new
    //     high-water-mark), this column reads 0 even if the body in fact
    //     allocated and released a non-trivial working set.
    //
    //   * When several OMP threads concurrently grow the peak, only the
    //     thread that "won the race to peak" sees a positive delta; the
    //     others see 0 or negative deltas.  Under an i.i.d. peak-crossing
    //     model with N node-entries per thread and OMP=13, the fraction
    //     of records with a positive delta is HarmonicNumber(13 N) / (13 N),
    //     i.e. ~ log(13 N) / (13 N) for large N; concretely
    //     ~ 0.6% on N = 100 (~ 99.4% read zero) and ~ 0.08% on N = 1000
    //     (~ 99.92% read zero).  The column degenerates to ~all-zeros on
    //     heavy fixtures (tst3-class) where N is large.
    //
    //   * macOS arm64 lazy-decommit lags allocation by seconds, further
    //     biasing this column low on heavy fixtures.
    //
    // The REQ-E §10.5 ceiling and REQ-B §10.2 Pareto frontier (design.md)
    // therefore use a LOWER BOUND on per-thread byte attribution; an
    // allocator-instrumented per-thread counter (mimalloc per-heap
    // peak_bytes or accum_t[tid]-style wiring through
    // integration_step.cpp) is deferred to iter-15+ if §6.D advances past
    // PASS_PARTIAL on iter-14 data.  iter-14 data is sufficient to FAIL
    // the §6.D gate but NOT to PASS it; a PASS verdict requires iter-15
    // recompute under allocator-instrumented data.
    int64_t entry_bytes_thread_local  = 0; ///< Per-OMP-thread bytes attributable
                                           ///< to this node's body.  Populated
                                           ///< in exit_node() as a byte-restated
                                           ///< view of rss_peak_kib_delta
                                           ///< (rss_peak_kib_delta * 1024 when
                                           ///< the delta is positive; 0 otherwise).
                                           ///< See data-fidelity caveat above.
    int64_t entry_bytes_aggregate_omp = 0; ///< Sum across all records in the
                                           ///< current drain batch of
                                           ///< entry_bytes_thread_local.
                                           ///< INVARIANT per drain batch:
                                           ///< stamped identically on every
                                           ///< record produced by one
                                           ///< drain_node_aggregate() call.
                                           ///< NOT a per-node attribute —
                                           ///< treat as a step-level scalar
                                           ///< (iter-13 design-pass invariant).
                                           ///< NOTE: this is `sum over the
                                           ///< drain batch`, not the
                                           ///< `temporal-max H_in_flight peak`
                                           ///< that REQ-A originally
                                           ///< requested — true temporal-max
                                           ///< deferred to iter-15+
                                           ///< (iter-13 BINDING reviewer
                                           ///< aad9b0038dad42d4c deferred-
                                           ///< approximation caveat).
    int64_t lf_cache_bytes            = 0; ///< LinearFactors cache footprint
                                           ///< (sum of Poly::total_bytes()
                                           ///< over cached state via
                                           ///< linear_factors_cache_residency())
                                           ///< sampled at outer-step boundary,
                                           ///< stamped on every record produced
                                           ///< during that step (iter-11 REQ-A
                                           ///< + REC-3).
    int64_t reg_sym_bytes             = 0; ///< Inter-step regulator-state
                                           ///< SymCoef total_bytes sampled at
                                           ///< outer-step boundary on the
                                           ///< pre-step `current`
                                           ///< (ShuffleListSym) variable in
                                           ///< hyperflint_sym(); on step k ≥ 1
                                           ///< this is the materialization of
                                           ///< step-(k-1)'s RegulatorSym via
                                           ///< regulator_sym_as_shuffle_list_sym
                                           ///< (i.e. the "persistent inter-step
                                           ///< state" of design.md §10.3).  On
                                           ///< step 0 it is the original input.
                                           ///< Computed as
                                           ///< Σ t.coef.total_bytes() for t in
                                           ///< `current`; SymCoef carries the
                                           ///< same total_bytes() accessor at
                                           ///< symcoef.hpp:141 across all
                                           ///< coefficient flavors.  iter-13
                                           ///< BINDING reviewer aad9b0038dad42d4c
                                           ///< REQ-13.2 column-name vs
                                           ///< computed-quantity drift
                                           ///< clarified here.
};

// ---------------------------------------------------------------------------
// REQ-C 4-column accessor helpers (iter-13).
//
// Each returns true unconditionally; the test
// test_integration_node_rss_4col.cpp flips its probe_*_present() bodies to
// delegate to these accessors so the four iter-12 red-state subtests turn
// green once the production source ladder is in place.  Accessors are
// kept as free functions (not member functions) so callers do not need
// access to the singleton instance.
// ---------------------------------------------------------------------------
bool node_rss_has_entry_bytes_thread_local_column();
bool node_rss_has_entry_bytes_aggregate_omp_column();
bool node_rss_has_lf_cache_bytes_column();
bool node_rss_has_reg_sym_bytes_column();

/// Per-integration-node RSS sampler (singleton).
///
/// Singleton — one instance per OMP thread (thread_local storage).
/// Each thread maintains its own records vector; aggregation across
/// threads is the responsibility of the caller.
///
/// Typical usage (task 0-5 will insert these calls):
///
///   auto& s = hyperflint::IntegrationNodeRssSampler::instance();
///   s.enter_node(depth, letter_id);
///   ... do integration work ...
///   s.exit_node(depth);
///
/// After the run, call records() to retrieve the accumulated data.
class IntegrationNodeRssSampler {
public:
    /// Return the singleton instance.
    static IntegrationNodeRssSampler& instance();

    /// Clear all records and re-read env vars.  Call at the start of
    /// each test or integration run to get a clean slate.
    void reset();

    /// Whether the sampler is currently active.
    bool enabled() const { return enabled_; }

    /// Minimum depth (inclusive) at which nodes are recorded.
    int depth_threshold() const { return depth_threshold_; }

    /// Mark the entry of an integration node.
    ///
    /// Records the current wall time and peak RSS so exit_node can
    /// compute deltas.  No-op if !enabled() or depth < depth_threshold().
    ///
    /// @param depth      Recursion depth of this node.
    /// @param letter_id  Integration letter index (0-based).
    void enter_node(int depth, int letter_id);

    /// Mark the exit of an integration node.
    ///
    /// Pops the most recently pushed entry from the per-call stack and
    /// fills the corresponding NodeRssRecord with wall time and peak-RSS
    /// delta.  No-op if !enabled(), depth < depth_threshold(), or the
    /// stack is empty (degenerate: exit without matching enter).
    ///
    /// @param depth  Must match the depth passed to the corresponding
    ///               enter_node call (used only for the threshold guard;
    ///               the actual stack is LIFO so mismatched depths are
    ///               not detected beyond the guard).
    void exit_node(int depth);

    /// Retrieve all completed node records.
    /// Returns a const reference to the internal vector; valid until the
    /// next call to reset() or enter_node()/exit_node().
    const std::vector<NodeRssRecord>& records() const { return records_; }

    /// Re-read HF_INTEG_NODE_RSS from the environment.
    ///
    /// Called automatically by reset() and the constructor.  Exposed
    /// publicly so tests can change the env var between sampler.reset()
    /// calls without constructing a fresh object.
    void refresh_env();

    /// Snapshot this thread's completed records into the process-wide
    /// aggregate.  Call from inside the OMP parallel body, after the last
    /// exit_node() for this iteration, to hand the thread's records off for
    /// cross-thread collection.  Thread-safe (uses an internal mutex).
    ///
    /// After the call the thread's records_ vector is cleared so the same
    /// thread can begin a fresh set of nodes in the next OMP iteration
    /// without accumulating unboundedly.
    void snapshot_thread_records();

private:
    IntegrationNodeRssSampler() { refresh_env(); }

    bool    enabled_          = false;
    int     depth_threshold_  = 2;
    int64_t next_node_id_     = 0;

    std::vector<NodeRssRecord> records_;

    /// Per-call stack: (start_t_wall_s, start_peak_kib, record_idx) for each
    /// nested enter_node that has not yet been matched by an exit_node.
    /// record_idx is the index into records_ for the corresponding NodeRssRecord
    /// so that exit_node patches the correct record under nested calls.
    std::vector<std::tuple<double, int64_t, size_t>> stack_;
};

// ---------------------------------------------------------------------------
// Cross-thread aggregate helpers (Task 0-5).
// ---------------------------------------------------------------------------
//
// Usage pattern (integration_step.cpp OMP body):
//
//   // Inside the parallel-for body, after NodeScope destructor fires:
//   hyperflint::IntegrationNodeRssSampler::instance().snapshot_thread_records();
//
//   // After the OMP barrier (serial, master thread):
//   auto all_records = hyperflint::drain_node_aggregate();
//   for (const auto& rec : all_records) { /* emit */ }
//
// reset_node_aggregate() is called before each parallel region so that
// records from a prior integration_step call do not contaminate the next.

/// Set the outer-step context that subsequent enter_node() calls will
/// stamp into each NodeRssRecord (iter-11 REQ-A + REC-3 from reviewer
/// ab6fa5cda36908669, iter-13 REQ-13.1 seq_cst rebind from BINDING
/// reviewer aad9b0038dad42d4c).  The two values describe the integrator
/// state AT the boundary of the outer step the records are about to be
/// produced in:
///
///   lf_cache_bytes   sum of Poly::total_bytes() across the sharded
///                    linear_factors cache, computed by
///                    linear_factors_cache_residency() in
///                    src/algebra/linear_factors.cpp.
///   reg_sym_bytes    sum of SymCoef::total_bytes() across the terms
///                    of the integrator's `current` variable at
///                    hyper_int.cpp (ShuffleListSym carrying the
///                    persistent inter-step state; on steps k ≥ 1 this
///                    is the materialization of step-(k-1)'s
///                    RegulatorSym, on step 0 it is the original input —
///                    see NodeRssRecord::reg_sym_bytes documentation
///                    above for full semantics).
///
/// Thread-safety: writes are serial (master thread, before the OMP
/// region of integration_step); reads inside enter_node() are
/// concurrent.  Implemented via std::atomic<int64_t> with
/// std::memory_order_seq_cst on both store and load (iter-13 BINDING
/// reviewer aad9b0038dad42d4c REQ-13.1 CONCERNS-FOLD): seq_cst is
/// portable across compiler+OMP-runtime combinations and the per-step
/// cost is two atomic stores + N node-entry atomic loads, completely
/// negligible vs the integrator's per-node Rat work.  An earlier
/// design used `memory_order_relaxed` and appealed to the OMP barrier
/// for happens-before; relaxed is correct under common libomp but is
/// NOT portably guaranteed by the C++ Memory Model, and the worst-case
/// race ("LF-cache stamp for step k briefly reads step-(k-1) value")
/// is observable on tst3-class fixtures where the LF cache grows
/// monotonically and would distort the REQ-E §10.5 ceiling.
void set_outer_step_context(int64_t lf_cache_bytes,
                            int64_t reg_sym_bytes);

/// Clear the cross-thread aggregate without draining it.
/// Call before the OMP parallel region so each step starts clean.
///
/// iter-13 design-pass extension: this function also
/// resets the outer-step context atomics (lf_cache_bytes,
/// reg_sym_bytes) to 0 so that step-k reuse of stale step-(k-1)
/// values is impossible at fixture-restart boundaries (long-lived
/// processes that run multiple fixtures back-to-back).
void reset_node_aggregate();

/// Drain and return all records collected via snapshot_thread_records()
/// since the last reset_node_aggregate() call.
/// Thread-safe; intended to be called from the master thread after
/// the OMP barrier.
///
/// Precondition (iter-13 design-pass): drain_node_aggregate() MUST be
/// called serially, after the OMP region's implicit barrier.  Calling
/// from inside a
/// parallel region returns a partial-batch view with stale
/// entry_bytes_aggregate_omp; the function does not enforce the
/// precondition at runtime (assert-free path) because the call site
/// is integration_step.cpp:2032 (post-barrier, master thread).
std::vector<NodeRssRecord> drain_node_aggregate();

}  // namespace hyperflint
