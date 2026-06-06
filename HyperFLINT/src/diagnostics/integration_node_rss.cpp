// integration_node_rss — implementation.
// See include/hyperflint/diagnostics/integration_node_rss.hpp for design notes.

#include "hyperflint/diagnostics/integration_node_rss.hpp"

// iter-92 §T7 22nd chunk: Track-RSS-probe macro-layer LAND.  The env-var
// fetch in refresh_env() below routes through HF_FLAG_INTEG_NODE_RSS
// (declared in diagnostics/env_flags.hpp) rather than calling
// std::getenv directly, so the singleton diagnostics-domain env var
// joins Track-diagnostic-dump (iter-63) and Track-structural-sharing-
// probe (iter-81) under the shared §5.1 rule-1 diagnostics/ home.
// Refer to docs/env_flags.md §5.1 Track-RSS-probe LANDED row for the
// placement rationale and unset-direction semantics.
#include "hyperflint/diagnostics/env_flags.hpp"

#include <atomic>
#include <chrono>    // std::chrono::steady_clock
#include <cstdlib>   // std::getenv, std::atoi
#include <cstring>   // std::strlen, std::strcmp
#include <mutex>
#include <vector>

namespace hyperflint {

// ---------------------------------------------------------------------------
// Cross-thread aggregate (Task 0-5)
// ---------------------------------------------------------------------------

namespace {

/// Guards g_aggregate and g_aggregate_records.
std::mutex g_aggregate_mutex;

/// All records snapshotted from per-thread samplers since the last
/// reset_node_aggregate() call.
std::vector<NodeRssRecord> g_aggregate_records;

// ---------------------------------------------------------------------------
// Outer-step context (iter-11 REQ-A + REC-3, iter-13 REQ-13.1 ordering fold).
//
// Two atomics holding the integrator state at the boundary of the
// current outer step.  Written by the master thread (serial) via
// set_outer_step_context() before each integration_step() call; read
// concurrently by all OMP workers inside enter_node() to stamp the
// LF-cache and regulator-SymCoef footprints onto each NodeRssRecord.
//
// REQ-13.1 fold (iter-13 BINDING reviewer aad9b0038dad42d4c CONCERNS-FOLD):
// both store and load use std::memory_order_seq_cst.  An earlier draft
// used memory_order_relaxed with a hand-waved "OMP barrier provides
// happens-before" argument; relaxed is correct under common libomp
// implementations but is NOT portably guaranteed by the C++ Memory
// Model.  Worst-case race under relaxed: step-(k-1) LF-cache stamp
// briefly leaks into step-k records on fixtures where the LF cache
// grows monotonically (tst3-class), distorting the REQ-E §10.5
// ceiling computation.  seq_cst here is O(1) per outer step + O(1)
// per node, completely negligible vs the integrator's per-node
// Rat::total_bytes work.
// ---------------------------------------------------------------------------
std::atomic<int64_t> g_outer_step_lf_cache_bytes{0};
std::atomic<int64_t> g_outer_step_reg_sym_bytes{0};

}  // anonymous namespace

void set_outer_step_context(int64_t lf_cache_bytes,
                            int64_t reg_sym_bytes) {
    g_outer_step_lf_cache_bytes.store(lf_cache_bytes,
                                      std::memory_order_seq_cst);
    g_outer_step_reg_sym_bytes.store(reg_sym_bytes,
                                     std::memory_order_seq_cst);
}

// ---------------------------------------------------------------------------
// REQ-C 4-column accessors (iter-13).  Each returns true unconditionally
// once the production struct extension is in place.
// ---------------------------------------------------------------------------
bool node_rss_has_entry_bytes_thread_local_column()  { return true; }
bool node_rss_has_entry_bytes_aggregate_omp_column() { return true; }
bool node_rss_has_lf_cache_bytes_column()            { return true; }
bool node_rss_has_reg_sym_bytes_column()             { return true; }

void reset_node_aggregate() {
    std::lock_guard<std::mutex> lk(g_aggregate_mutex);
    g_aggregate_records.clear();
    // iter-13 design-pass (carried through iter-13 BINDING reviewer
    // aad9b0038dad42d4c REQ-13.1 seq_cst rebind): also reset the
    // outer-step context atomics so step-k reuse of stale step-(k-1)
    // values is impossible at fixture-restart boundaries (long-lived
    // processes that run multiple fixtures back-to-back).  seq_cst
    // stores so the reset is globally visible before the next
    // set_outer_step_context() call.
    g_outer_step_lf_cache_bytes.store(0, std::memory_order_seq_cst);
    g_outer_step_reg_sym_bytes .store(0, std::memory_order_seq_cst);
}

std::vector<NodeRssRecord> drain_node_aggregate() {
    // iter-13 design-pass precondition: drain_node_aggregate() MUST be
    // called serially, post-OMP-barrier (the call site is
    // integration_step.cpp:2032).  We do not enforce at runtime (no
    // assertion) — the mutex serializes drain against
    // snapshot_thread_records() but does NOT prevent an in-progress
    // OMP worker from appending a record AFTER the swap completes,
    // which would partition this drain batch's records across two
    // calls and produce a stale entry_bytes_aggregate_omp on the
    // first batch.  Caller MUST honor the post-barrier precondition.
    std::lock_guard<std::mutex> lk(g_aggregate_mutex);
    std::vector<NodeRssRecord> out;
    out.swap(g_aggregate_records);
    // iter-11 REQ-C (reviewer ab6fa5cda36908669): stamp
    // entry_bytes_aggregate_omp across all records produced in this
    // drain batch.  Each drain corresponds to one outer integration
    // step (called once from integration_step.cpp after the implicit
    // parallel-for barrier), so the sum across `out` is the realized-
    // RSS column reduced across OMP workers for the step.
    //
    // iter-13 design-pass deferred-approximation caveat:
    // entry_bytes_aggregate_omp is a `sum over the drain batch` of
    // per-thread byte-restated peak-RSS deltas, NOT the
    // `temporal-max H_in_flight peak` that REQ-A originally requested.
    // The two diverge when nodes do NOT temporally overlap (the sum
    // overcounts the in-flight peak by approximately
    // nodes_per_thread_per_step).  This column is a per-drain-batch
    // INVARIANT (same value on every record in `out`); downstream
    // analysis MUST treat it as a step-level scalar, not a per-node
    // observation.  True temporal-max is deferred to iter-15+ if §6.D
    // advances past PASS_PARTIAL on iter-14 data.
    int64_t sum_tl = 0;
    for (const auto& rec : out) sum_tl += rec.entry_bytes_thread_local;
    for (auto& rec : out)       rec.entry_bytes_aggregate_omp = sum_tl;
    return out;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Return the current wall time as seconds since an arbitrary epoch.
/// Uses std::chrono::steady_clock, matching the rest of the HF codebase
/// (see src/integrator/hyper_int.cpp and src/integrator/primitive.cpp).
static double now_wall_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// IntegrationNodeRssSampler
// ---------------------------------------------------------------------------

IntegrationNodeRssSampler& IntegrationNodeRssSampler::instance() {
    // thread_local: each OMP thread gets its own instance; no locking needed.
    // Construction is thread-safe by the C++11 thread_local guarantee.
    static thread_local IntegrationNodeRssSampler s;
    return s;
}

void IntegrationNodeRssSampler::reset() {
    records_.clear();
    stack_.clear();
    next_node_id_ = 0;
    refresh_env();
}

void IntegrationNodeRssSampler::refresh_env() {
    // iter-92: env-var fetch routed through the diagnostics/env_flags.hpp
    // macro layer (see file header).  Predicate semantics unchanged.
    const char* s = HF_FLAG_INTEG_NODE_RSS;
    if (s != nullptr && std::strlen(s) > 0 && std::strcmp(s, "0") != 0) {
        enabled_ = true;
        depth_threshold_ = std::atoi(s);
        if (depth_threshold_ < 1) depth_threshold_ = 2;
    } else {
        enabled_ = false;
        depth_threshold_ = 2;  // reset to safe default when disabled
    }
}

void IntegrationNodeRssSampler::enter_node(int depth, int letter_id) {
    if (!enabled_ || depth < depth_threshold_) return;

    const auto rss = sample_rss();
    // Record the index of the NodeRssRecord we are about to push, so that
    // exit_node can patch the correct record even under nested calls.
    const size_t record_idx = records_.size();
    stack_.emplace_back(now_wall_s(), rss.peak_kib, record_idx);

    NodeRssRecord rec{};
    rec.node_id         = next_node_id_++;
    rec.depth           = depth;
    rec.letter_id       = letter_id;
    rec.rss_current_kib = rss.current_kib;
    // t_wall_s and rss_peak_kib_delta filled by exit_node
    rec.t_wall_s            = 0.0;
    rec.rss_peak_kib_delta  = 0;
    // iter-11 reviewer ab6fa5cda36908669 REQ-A + REC-3, plus iter-13
    // BINDING reviewer aad9b0038dad42d4c REQ-13.1 (memory-ordering fold):
    // stamp the outer-step boundary state onto every record produced
    // during this step.  Both store (set_outer_step_context) and load
    // use std::memory_order_seq_cst — REQ-13.1 fold from iter-13 BINDING
    // reviewer aad9b0038dad42d4c CONCERNS-FOLD: an earlier draft used
    // memory_order_relaxed and appealed to the OMP barrier for happens-
    // before, but seq_cst is portably correct across compiler+OMP-runtime
    // combinations and the per-load cost is dominated by the surrounding
    // node-entry work.  entry_bytes_* are left at 0; exit_node patches
    // entry_bytes_thread_local and drain_node_aggregate patches
    // entry_bytes_aggregate_omp.
    rec.lf_cache_bytes =
        g_outer_step_lf_cache_bytes.load(std::memory_order_seq_cst);
    rec.reg_sym_bytes  =
        g_outer_step_reg_sym_bytes .load(std::memory_order_seq_cst);
    records_.push_back(rec);
}

void IntegrationNodeRssSampler::exit_node(int depth) {
    if (!enabled_ || depth < depth_threshold_ || stack_.empty()) return;

    const auto [t0, peak0, record_idx] = stack_.back();
    stack_.pop_back();

    const auto rss = sample_rss();

    // Patch the record identified at enter time, not records_.back().
    // Under nested calls (enter A → enter B → exit B → exit A) records_.back()
    // would still point at B's record when we are exiting A, silently leaving
    // A's t_wall_s and rss_peak_kib_delta at their zero sentinel values.
    if (record_idx < records_.size()) {
        auto& rec = records_[record_idx];
        rec.t_wall_s           = now_wall_s() - t0;
        rec.rss_peak_kib_delta = rss.peak_kib - peak0;
        // REQ-C iter-13: per-OMP-thread byte attribution = byte-restatement
        // of the per-thread peak-RSS delta when the delta is positive (the
        // node body actually grew the process peak on this thread).  When
        // the delta is zero or negative — the body did not push a new
        // peak — entry_bytes_thread_local is set to 0 so callers do not
        // mistake a no-growth node for one that allocated and immediately
        // released its working set.  drain_node_aggregate() further sums
        // these into entry_bytes_aggregate_omp at outer-step boundaries.
        if (rec.rss_peak_kib_delta > 0) {
            rec.entry_bytes_thread_local =
                static_cast<int64_t>(rec.rss_peak_kib_delta) *
                static_cast<int64_t>(1024);
        } else {
            rec.entry_bytes_thread_local = 0;
        }
    }
}

void IntegrationNodeRssSampler::snapshot_thread_records() {
    if (!enabled_ || records_.empty()) return;

    // Move this thread's completed records into the global aggregate under
    // the mutex, then clear the local vector so subsequent OMP iterations
    // on this thread start fresh.
    std::lock_guard<std::mutex> lk(g_aggregate_mutex);
    g_aggregate_records.insert(
        g_aggregate_records.end(),
        std::make_move_iterator(records_.begin()),
        std::make_move_iterator(records_.end()));
    records_.clear();
    // Reset the node_id counter so node_ids within a thread remain locally
    // monotone across OMP iterations; global uniqueness is not guaranteed and
    // not required (the consumer identifies records by their payload, not id).
    next_node_id_ = 0;
}

}  // namespace hyperflint
