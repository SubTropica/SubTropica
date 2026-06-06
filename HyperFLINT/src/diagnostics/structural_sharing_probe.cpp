// structural_sharing_probe — implementation.
//
// See include/hyperflint/diagnostics/structural_sharing_probe.hpp for the
// public contract and design-memo cross-references.
//
// ===========================================================================
// Implementation notes (iter-17 production source; REQ-16.1..REQ-16.4 folds
// applied per iter-16 BINDING reviewer ae98902a768f7f242)
// ===========================================================================
//
// 1.  Env-gate caching.  HF_STRUCTURAL_SHARING_PROBE is read via std::getenv
//     and cached in `g_master_enabled` (a **process-global**
//     `std::atomic<bool>`; REQ-16.1 fold corrects an iter-16 header
//     docstring drift that said "thread_local").  refresh_env_impl() is
//     called from `ensure_env_initialized()` only on first access per
//     process; thereafter the predicate reads the cached value with
//     `std::memory_order_relaxed`.  `reset_records()` also refreshes the
//     env var so callers can toggle the gate between fixtures in a
//     long-lived process.  This mirrors the integration_node_rss
//     enabled_ caching pattern.
//
// 2.  Stratified sampling.  Per-op call counters live in a thread_local
//     std::array<int64_t, 4> indexed by op_to_index().  should_emit_op()
//     increments the counter and returns `true` when (counter % N) == 0,
//     where N = sample_rate() (clamped to 1 = full emission).  Per-thread
//     counters → no shared lock on the hot path.
//
// 3.  Emit buffer.  Each thread accumulates Records in a thread_local
//     std::vector<Record>.  REQ-16.2 fold (a) (iter-16 BINDING reviewer
//     ae98902a768f7f242): when the local buffer reaches
//     `kSnapshotThreshold` (4096 records), emit() calls
//     `snapshot_thread_records()` (internal), which moves the buffer's
//     contents into a global `g_aggregate_records` under `g_drain_mutex`
//     and clears the local buffer.  drain_records() snapshots the
//     calling (master) thread's buffer, then swaps `g_aggregate_records`
//     into the return vector under the same mutex.  This pattern mirrors
//     `IntegrationNodeRssSampler::snapshot_thread_records`
//     (src/diagnostics/integration_node_rss.cpp:234-249) and avoids the
//     dangling-pointer hazard of the iter-16 raw-pointer registry: no
//     pointer-into-thread_local-storage is ever held across a sync point.
//
// 4.  Factor / fraction observation (REQ-16.3 fold).  Per-call, each LF /
//     PF probe site computes a 64-bit signature hash for every factor /
//     fraction and calls `observe_lf_factor_bytes(sig, bytes)` /
//     `observe_pf_fraction_bytes(sig, bytes)`.  The observe functions
//     acquire a global mutex, look up the signature in a process-global
//     `std::unordered_set<uint64_t>`, return `bytes` if already-seen
//     (caller adds to `unchanged_bytes`), insert and return 0 otherwise.
//     This implements the design-memo §2.1 factor-level structural-
//     equivalence intersection as a cumulative cross-call signal: any
//     factor reused across calls (regardless of whether the cache HIT
//     happened to route the value-restore) is counted as shareable.
//     FOLD-DC5 stratification (sample_rate >= 10 on LF/PF per design
//     memo §2.2) keeps the mutex acquisition rate bounded.
//     `reset_records()` clears both seen-sets at fixture boundary.
//
// 5.  FOLD-D-DISCIPLINE-N byte-id invariance.  Default-OFF
//     (HF_STRUCTURAL_SHARING_PROBE unset or "0"): the four predicates
//     return `false` and every hook site short-circuits before any
//     probe-side allocation.  Production binary byte-output on
//     tst0/tst1/tst2/findroots21_a under default-OFF must remain
//     sha-identical to the iter-15 close binary
//     580556dfa38118547a63cc8f4fd3150993d2bf98f95421387d9411bd769c5803.
//     iter-17 re-confirms after REQ-folds touch the source.
//
// 6.  Memo-key invariance.  The probe operates OUTSIDE the operator-memo
//     try_lookup window: the hook sites compute pre_bytes BEFORE memo
//     lookup, then run the entire memo path (HIT or MISS), then compute
//     post_bytes from the (now-mutated or returned) post-state.  The
//     canonical_signature key, the cache shards, and counter_replay are
//     not touched on the probe path.

#include "hyperflint/diagnostics/structural_sharing_probe.hpp"
#include "hyperflint/diagnostics/env_flags.hpp"  // iter-81 §T7 twelfth chunk: HF_FLAG_STRUCTURAL_SHARING_PROBE / HF_FLAG_STRUCTURAL_SHARING_PROBE_SAMPLE_RATE

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace hyperflint {
namespace structural_sharing {

namespace {

// ---------------------------------------------------------------------------
// Op index — keep in sync with op_tag() table below.
//
// Four entry points; the index is used to address per-op thread_local
// counters for stratified sampling.
// ---------------------------------------------------------------------------
enum OpIndex {
    OP_REDUCE_INPLACE     = 0,
    OP_ADD                = 1,
    OP_LINEAR_FACTORS     = 2,
    OP_PARTIAL_FRACTIONS  = 3,
    OP_COUNT              = 4,
};

inline int op_to_index(const char* op) {
    // Pointer-equality fast path for literal-string tags (the production
    // hook sites always pass a string literal; std::strcmp is the fallback
    // for callers that compose the tag dynamically).
    if (op == nullptr) return -1;
    if (std::strcmp(op, "reduce_inplace") == 0)    return OP_REDUCE_INPLACE;
    if (std::strcmp(op, "add") == 0)               return OP_ADD;
    if (std::strcmp(op, "linear_factors") == 0)    return OP_LINEAR_FACTORS;
    if (std::strcmp(op, "partial_fractions") == 0) return OP_PARTIAL_FRACTIONS;
    return -1;
}

// ---------------------------------------------------------------------------
// Master env gate (HF_STRUCTURAL_SHARING_PROBE).
//
// Cached in g_master_enabled (a process-global atomic; loaded relaxed
// because the value is set once at process start by the user's env, and
// any subsequent change via reset_records() is serialized by the drain
// mutex).  REQ-16.1 fold (iter-16 BINDING reviewer ae98902a768f7f242):
// the previous header docstring said "thread_local atomic-bool"; the
// impl was always process-global and the docstring is corrected.
// ---------------------------------------------------------------------------
std::atomic<bool>  g_master_enabled{false};
std::atomic<bool>  g_env_initialized{false};
std::atomic<int64_t> g_sample_rate{0};   // 0 = full emission

void refresh_env_impl() {
    const char* s = HF_FLAG_STRUCTURAL_SHARING_PROBE;
    bool enabled = (s != nullptr && std::strlen(s) > 0 &&
                    std::strcmp(s, "0") != 0);
    g_master_enabled.store(enabled, std::memory_order_relaxed);

    const char* r = HF_FLAG_STRUCTURAL_SHARING_PROBE_SAMPLE_RATE;
    int64_t rate = 0;
    if (r != nullptr && std::strlen(r) > 0) {
        rate = static_cast<int64_t>(std::atoll(r));
        if (rate < 0) rate = 0;
    }
    g_sample_rate.store(rate, std::memory_order_relaxed);

    g_env_initialized.store(true, std::memory_order_release);
}

inline void ensure_env_initialized() {
    if (!g_env_initialized.load(std::memory_order_acquire)) {
        refresh_env_impl();
    }
}

inline bool master_enabled() {
    ensure_env_initialized();
    return g_master_enabled.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Per-thread emit buffer + global aggregate (REQ-16.2 fold (a); iter-16
// BINDING reviewer ae98902a768f7f242).
//
// Pattern transplant from integration_node_rss.cpp:234-249
// (`IntegrationNodeRssSampler::snapshot_thread_records`).  Each thread
// owns a `g_local_records` vector; periodically (or on drain) it moves
// the records into `g_aggregate_records` under `g_drain_mutex` and
// clears the local buffer.  The aggregate is the only thing
// `drain_records()` returns; per-thread locals are never accessed from
// another thread (no raw-pointer-to-thread_local in a global registry).
//
// Threshold-triggered snapshot: when `g_local_records.size() >=
// kSnapshotThreshold` inside emit(), the thread takes the mutex once,
// moves its buffer into the aggregate, and continues.  At
// kSnapshotThreshold = 4096 records of ~40 bytes each (~160 KiB), the
// mutex contention is bounded by once per ~thousands of probe calls
// per thread.
// ---------------------------------------------------------------------------
constexpr std::size_t kSnapshotThreshold = 4096;

std::mutex                 g_drain_mutex;
std::vector<Record>        g_aggregate_records;

thread_local std::vector<Record> g_local_records;

// snapshot_thread_records — internal helper.  Moves the calling thread's
// `g_local_records` into the global `g_aggregate_records` under
// `g_drain_mutex`, then clears the local vector.  Mirror of
// integration_node_rss.cpp::IntegrationNodeRssSampler::snapshot_thread_records.
void snapshot_thread_records() {
    if (g_local_records.empty()) return;
    std::lock_guard<std::mutex> lk(g_drain_mutex);
    g_aggregate_records.insert(
        g_aggregate_records.end(),
        std::make_move_iterator(g_local_records.begin()),
        std::make_move_iterator(g_local_records.end()));
    g_local_records.clear();
}

// ---------------------------------------------------------------------------
// Per-thread per-op sampling counters (FOLD-DC5).
// ---------------------------------------------------------------------------
thread_local std::array<int64_t, OP_COUNT> g_local_op_counter = {0, 0, 0, 0};

// ---------------------------------------------------------------------------
// Per-process LF / PF factor-signature seen-sets (REQ-16.3 fold; iter-16
// BINDING reviewer ae98902a768f7f242).
//
// Process-global `std::unordered_set<uint64_t>` for each of LF and PF;
// guarded by `g_factor_set_mutex`.  Each `observe_*_bytes` call acquires
// the mutex once and either inserts the signature (returning 0, "first
// observation") or looks it up (returning `bytes`, "previously seen, count
// as shareable").  At sample_rate >= 10 (design memo §2.2), the mutex
// acquisition rate is bounded by (sample_rate × thread_count ×
// factors_per_call)^-1.
//
// `reset_records()` clears both sets so fixture boundaries do not leak
// state.  The sets are inside the anonymous namespace; the public API
// (`observe_lf_factor_bytes` / `observe_pf_fraction_bytes`) is the only
// route to them.
// ---------------------------------------------------------------------------
std::mutex                         g_factor_set_mutex;
std::unordered_set<std::uint64_t>  g_lf_seen_factors;
std::unordered_set<std::uint64_t>  g_pf_seen_fractions;

}  // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================

bool probe_reduce_inplace_instrumented()    { return master_enabled(); }
bool probe_add_instrumented()               { return master_enabled(); }
bool probe_linear_factors_instrumented()    { return master_enabled(); }
bool probe_partial_fractions_instrumented() { return master_enabled(); }

int64_t sample_rate() {
    ensure_env_initialized();
    return g_sample_rate.load(std::memory_order_relaxed);
}

bool should_emit_op(const char* op) {
    const int idx = op_to_index(op);
    if (idx < 0) return false;

    // Stratified sampling: full emission when rate <= 1, else 1-in-rate.
    int64_t rate = sample_rate();
    if (rate <= 1) return true;

    int64_t n = ++g_local_op_counter[idx];
    return (n % rate) == 0;
}

void emit(const char* op, int64_t pre_bytes, int64_t post_bytes,
          int64_t unchanged_bytes) {
    Record rec;
    rec.op              = op;
    rec.pre_bytes       = pre_bytes;
    rec.post_bytes      = post_bytes;
    rec.unchanged_bytes = unchanged_bytes;
    rec.frac_changed    = 0.0;  // filled at drain
    g_local_records.push_back(rec);
    if (g_local_records.size() >= kSnapshotThreshold) {
        snapshot_thread_records();
    }
}

void reset_records() {
    // Snapshot the calling thread's local records into the aggregate so
    // they are visible to the cross-fixture clear, then drop the
    // aggregate + seen-sets.  REQ-16.2 fold (a): we do NOT iterate
    // worker threads' g_local_records (no raw-pointer registry); the
    // caller is responsible for quiescing emission (post-OMP-barrier
    // master-only invocation, as documented at hpp:drain_records).
    snapshot_thread_records();
    std::lock_guard<std::mutex> lk(g_drain_mutex);
    g_aggregate_records.clear();
    {
        std::lock_guard<std::mutex> flk(g_factor_set_mutex);
        g_lf_seen_factors.clear();
        g_pf_seen_fractions.clear();
    }
    // Re-read env so callers can toggle gate / sample rate between
    // fixtures in a long-lived process (mirror of
    // IntegrationNodeRssSampler::refresh_env on reset()).
    refresh_env_impl();
}

std::vector<Record> drain_records() {
    std::vector<Record> out;
    // First, snapshot the calling thread's local records (typically the
    // master thread post-OMP-barrier).  Worker threads with un-snapshotted
    // records pay the deferred-drain penalty (records appear in the next
    // drain batch).  This is the same trade-off
    // `integration_node_rss::drain_node_aggregate` makes (cpp:86-94).
    snapshot_thread_records();
    {
        std::lock_guard<std::mutex> lk(g_drain_mutex);
        out.swap(g_aggregate_records);
    }
    // Fill frac_changed:
    //   = (pre + post − 2·unchanged) / (pre + post)
    // Skip records where unchanged_bytes is the "-1 not measured" sentinel.
    for (auto& r : out) {
        if (r.unchanged_bytes < 0) { r.frac_changed = 0.0; continue; }
        const double denom = static_cast<double>(r.pre_bytes + r.post_bytes);
        if (denom > 0.0) {
            r.frac_changed =
                (denom - 2.0 * static_cast<double>(r.unchanged_bytes)) / denom;
        } else {
            r.frac_changed = 0.0;
        }
    }
    return out;
}

std::int64_t observe_lf_factor_bytes(std::uint64_t signature_hash,
                                     std::int64_t bytes) {
    std::lock_guard<std::mutex> lk(g_factor_set_mutex);
    auto ins = g_lf_seen_factors.insert(signature_hash);
    // ins.second == true  → newly inserted (first observation; not shareable)
    // ins.second == false → already present  (previously observed; shareable)
    return ins.second ? 0 : bytes;
}

std::int64_t observe_pf_fraction_bytes(std::uint64_t signature_hash,
                                       std::int64_t bytes) {
    std::lock_guard<std::mutex> lk(g_factor_set_mutex);
    auto ins = g_pf_seen_fractions.insert(signature_hash);
    return ins.second ? 0 : bytes;
}

}  // namespace structural_sharing
}  // namespace hyperflint
