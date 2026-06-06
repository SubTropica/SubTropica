// structural_sharing_probe — §A.P / Lever 6.P byte-weighted structural-sharing
// potential probe.
//
// Phase 6 REVISED §6.P iter-17 production source (BINDING pre-build reviewer
// dispatched at iter-16 on this staged diff).
//
// Plan:        docs/superpowers/plans/2026-05-12-hf-phase-6-revised-five-lever-pool-aware.md
//              §A.P + REVIEWER P6' BINDING (byte-weighted activation gate).
// Design memo: notes/hf_finite_field_program/phase6_combined/probe_a_p_persistent_data/design.md
//              (§1 hypothesis, §1.1 activation gate, §2 probe design,
//               §2.1 monomial-level / factor-level intersection,
//               §2.2 FOLD-DC5 stratified sampling, §5 wall-regression bound).
//
// ===========================================================================
// What the probe does
// ===========================================================================
// At four named entry points in the HF integration pipeline,
//
//   * Rat::reduce_inplace     (src/core/rat.cpp:1527)
//   * Rat::add                (src/core/rat.cpp:2418)
//   * linear_factors          (src/algebra/linear_factors.cpp:1782)
//   * partial_fractions       (src/algebra/partial_fractions.cpp:444)
//
// the probe (when activated via HF_STRUCTURAL_SHARING_PROBE=1) snapshots the
// byte-size of the operand BEFORE the operation and the byte-size of the
// result AFTER, plus a structural-equivalence intersection measure
// (`unchanged_bytes`).  Records are accumulated in a thread-local buffer and
// drained at outer-step boundaries.  The iter-3+ aggregator (out-of-tree
// Python under notes/...probe_a_p_persistent_data/) computes the
// byte-weighted shareable fraction per the P6' formula:
//
//     Σ_call (frequency × shareable_bytes_per_call)
//     ─────────────────────────────────────────────  ≥ 0.30  → GO §6.P
//     Σ_call (frequency ×    total_bytes_per_call)
//
// ===========================================================================
// Env-gate convention (default-OFF; FOLD-D-DISCIPLINE-N byte-id discipline)
// ===========================================================================
//
//   HF_STRUCTURAL_SHARING_PROBE                 unset / "0" → disabled
//                                               "1"          → enabled
//
//   HF_STRUCTURAL_SHARING_PROBE_SAMPLE_RATE     unset / "0"  → full emission
//                                               N > 0        → emit 1-in-N
//                                                              (per-op,
//                                                              per-thread,
//                                                              FOLD-DC5)
//
// When `HF_STRUCTURAL_SHARING_PROBE` is unset or "0", the four
// `probe_<op>_instrumented()` predicates return `false`.  Each hook site
// short-circuits on `!probe_<op>_instrumented()` before any pre-snapshot
// allocation; the default-OFF cost is a single `std::getenv`-cached load
// + branch + tail-call.  Production binary byte-identity (FOLD-D-DISCIPLINE-N)
// holds under default-OFF.
//
// ===========================================================================
// Schema (Record)
// ===========================================================================
//
//   op               entry point tag; one of {"reduce_inplace", "add",
//                                              "linear_factors",
//                                              "partial_fractions"}
//   pre_bytes        operand byte size BEFORE the op (heap-walking estimate
//                    via Rat::total_bytes / Poly::total_bytes; no FLINT
//                    `to_string` round-trip; see design memo §2)
//   post_bytes       result byte size AFTER the op
//   unchanged_bytes  bytes that appear structurally in both pre and post
//                    (sound-not-complete monomial-level / factor-level
//                    intersection per design memo §2.1).  Negative-1
//                    sentinel = "not measured" (lf / pf iter-17 fallback;
//                    see §6.1 FIXME below).
//   frac_changed     (pre_bytes + post_bytes − 2·unchanged_bytes) /
//                    (pre_bytes + post_bytes); aggregator-side convenience
//                    (probe leaves at 0.0 — receiver computes).
//
// ===========================================================================
// Thread-safety
// ===========================================================================
// Each OMP thread accumulates records in a thread_local buffer (no shared
// lock on the hot path).  REQ-16.2 fold (a) (iter-16 BINDING reviewer
// ae98902a768f7f242): the iter-16 implementation used a global registry
// of raw thread_local-buffer pointers; that pattern has a dangling-pointer
// hazard under `OMP_DYNAMIC=true` / signal-triggered abort / library-
// induced thread teardown (the registry holds a pointer whose pointee
// is destroyed by its thread_local dtor).  The iter-17 fold mirrors the
// `IntegrationNodeRssSampler::snapshot_thread_records` pattern at
// src/diagnostics/integration_node_rss.cpp:234-249 — move-into-global-
// aggregate-under-mutex.  Concretely:
//
//   * Each thread accumulates records into a thread_local `g_local_records`
//     vector (no lock, hot path).
//   * Inside `emit()`, when the local buffer crosses
//     `kSnapshotThreshold` (4096 records), the thread calls
//     `snapshot_thread_records()` (internal) which moves the local
//     vector into a global `g_aggregate_records` under `g_drain_mutex`
//     and clears the local vector.
//   * `drain_records()` (master thread, post-OMP-barrier) calls
//     `snapshot_thread_records()` for the master's local buffer, then
//     swaps `g_aggregate_records` into the return vector under
//     `g_drain_mutex`.  Per-worker local buffers that have NOT yet
//     crossed the threshold are flushed at the master's drain only if
//     each worker has called `snapshot_thread_records()` at its
//     post-barrier sync point; if a worker holds records past the
//     drain, they appear in the NEXT drain batch (post-barrier
//     accounting; same semantics as `integration_node_rss`).
//   * `reset_records()` clears both `g_local_records` (master thread)
//     and `g_aggregate_records` under `g_drain_mutex`.
//
// Drain-precondition (mirror of integration_node_rss:86-94): callers
// MUST quiesce all emission paths before drain — typically by calling
// drain at the master thread post-OMP-barrier.  This matches the
// iter-13 REQ-13.1 memory-ordering discipline carry (BINDING reviewer
// aad9b0038dad42d4c).  No raw pointer to a `thread_local` storage
// duration object is ever held across a sync point.
//
// ===========================================================================
// Memo-key invariance (FOLD-M6 carry)
// ===========================================================================
// The probe sits OUTSIDE the operator-memo `try_lookup` window: pre/post
// snapshots and emit() do NOT touch the canonical-signature key, the cache
// shards, or the counter-replay machinery.  The memo's HIT-rate under
// HF_STRUCTURAL_SHARING_PROBE=1 must be identical to default-OFF for the
// same fixture; probe-side any perturbation is a BINDING bug (REVIEWER
// scope §6 of design memo).
//
// ===========================================================================
// ABI safety (REQ-13.3 carry pattern)
// ===========================================================================
// The Record struct is tail-extensible — future fields appended at the
// end of the struct will not break existing callers because no
// sizeof-comparison, memcpy, or offsetof pattern crosses this header.
// The probe accessors return scalars / std::vector<Record> by value; no
// pointer-into-Record exposure.
//
// ===========================================================================
// Lifecycle
// ===========================================================================
// On the iter-15 TDD test (test_integration_structural_sharing_probe.cpp),
// the four predicates must return `true` under HF_STRUCTURAL_SHARING_PROBE=1
// so all four subtests turn green.  iter-18+ extends each subtest to a
// Parts-2-3 sentinel-round-trip pattern (mirror of REQ-13.4 fold on §6.D).
//
// ===========================================================================

#pragma once

#include <cstdint>
#include <vector>

namespace hyperflint {
namespace structural_sharing {

// ---------------------------------------------------------------------------
// Record schema — mirrors test/test_integration_structural_sharing_probe.cpp's
// `ExpectedStructuralSharingRecord` (iter-15 forward-declared documentation
// aid).  iter-17 lands the real type here in the hyperflint::structural_sharing
// namespace.
//
// Field order:
//   op              : tag (literal-string lifetime; not owned)
//   pre_bytes       : operand heap-byte estimate before op
//   post_bytes      : result heap-byte estimate after op
//   unchanged_bytes : sound monomial / factor-level intersection bytes,
//                     or -1 = "not measured" sentinel (iter-17 lf/pf fallback;
//                     see §6.1 FIXME in design memo §2.1).
//   frac_changed   : aggregator-side scalar, probe leaves at 0.0
// ---------------------------------------------------------------------------
struct Record {
    const char* op            = nullptr;  ///< literal-string lifetime
    int64_t     pre_bytes     = 0;
    int64_t     post_bytes    = 0;
    int64_t     unchanged_bytes = 0;
    double      frac_changed  = 0.0;
};

// ---------------------------------------------------------------------------
// Activation predicates.
//
// Each returns `true` iff HF_STRUCTURAL_SHARING_PROBE is set to a non-zero
// value AT THE TIME OF THE CALL.  The four entry-point predicates are
// distinct symbols so that future refinements (e.g. per-entry kill-switches
// HF_STRUCTURAL_SHARING_PROBE_RAT_ADD=0) can decouple them.  At iter-17 all
// four delegate to the same master gate `probe_master_enabled()`.
//
// Implementation note (REQ-16.1 fold; iter-16 BINDING reviewer
// ae98902a768f7f242): the master predicate caches the env-var read via a
// **process-global** `std::atomic<bool>` (init-once + relaxed reads; see
// impl cpp:102, 129).  `reset_records()` is the only safe in-process
// toggle of the gate after the first call; it re-reads the env var and
// is serialized by `g_drain_mutex` (cpp:206).  Callers MUST treat the
// predicate as "read at probe entry" — the env var is not re-read per
// call.  An earlier draft of this docstring said "thread_local
// atomic-bool"; that was a header/impl drift caught by the iter-16
// pre-build adversarial reviewer (REQ-16.1) and corrected here.  See
// FOLD-DC5 §2.2 carry: stratified sampling is independent of the env
// var, controlled by HF_STRUCTURAL_SHARING_PROBE_SAMPLE_RATE.
// ---------------------------------------------------------------------------
bool probe_reduce_inplace_instrumented();
bool probe_add_instrumented();
bool probe_linear_factors_instrumented();
bool probe_partial_fractions_instrumented();

// ---------------------------------------------------------------------------
// Stratified sampling (FOLD-DC5).
//
// Per-op stratification: each entry point has its own sample-rate read from
// the same env var (HF_STRUCTURAL_SHARING_PROBE_SAMPLE_RATE).  iter-17 lands
// the global rate; iter-18+ may refine to per-op rates (RAT_ADD_SAMPLE_RATE,
// LINEAR_FACTORS_SAMPLE_RATE, ...) if call-frequency skew distorts the
// byte-weighted aggregate.
//
// should_emit_op() returns `true` on every Nth call to the same op-tag from
// the same OMP thread, where N is sample_rate().  Counters are thread_local
// (no shared lock).  When sample_rate() == 0 OR <= 1, every call emits
// (full emission).
//
// IMPORTANT: should_emit_op() is the SAMPLING gate.  Callers MUST still
// check probe_<op>_instrumented() first — should_emit_op() does NOT re-read
// the master enable env var on each call.
// ---------------------------------------------------------------------------
int64_t sample_rate();                 ///< from HF_STRUCTURAL_SHARING_PROBE_SAMPLE_RATE
bool should_emit_op(const char* op);    ///< stratified-sampling gate

// ---------------------------------------------------------------------------
// Emission.
//
// Append a single Record to the calling thread's local buffer.  Caller
// computes pre_bytes / post_bytes / unchanged_bytes and passes them in.
// `frac_changed` is filled in at drain time.
//
// THREAD-SAFETY: no global lock on the hot path; appends to thread_local
// vector.  Caller MUST hold no other diagnostic lock when invoking emit().
//
// Caller is responsible for ensuring `op` has literal-string lifetime
// (the standard pattern: pass a string literal).
// ---------------------------------------------------------------------------
void emit(const char* op, int64_t pre_bytes, int64_t post_bytes,
          int64_t unchanged_bytes);

// ---------------------------------------------------------------------------
// Drain / reset.
//
// drain_records() acquires the global drain mutex, snapshots the master
// thread's local buffer into `g_aggregate_records`, swaps the aggregate
// into the return vector, and fills in `frac_changed` on each returned
// Record.  Worker threads' local buffers are NOT touched at drain time
// (REQ-16.2 fold (a) — they have already moved their records into the
// aggregate at threshold-triggered snapshots inside `emit()`, or they
// will at the next snapshot point).  The caller-side ordering is not
// specified (records from different threads interleave in the aggregate).
//
// reset_records() acquires the same mutex and clears both the master's
// `g_local_records` and the global `g_aggregate_records`.  Also re-reads
// the env var so callers can change the master gate / sample rate
// between fixtures in a long-lived process.  Worker threads' local
// buffers may retain records across reset_records() if they have not
// hit the snapshot threshold; this is the same trade-off
// `IntegrationNodeRssSampler::snapshot_thread_records` makes (records
// are bounded by the snapshot threshold, and accumulator-side resets
// are typically followed by a fixture-boundary OMP join).
//
// snapshot_thread_records() (internal; not part of public API) is called
// by `emit()` when `g_local_records.size() >= kSnapshotThreshold`; it
// acquires `g_drain_mutex`, moves the calling thread's local records
// into `g_aggregate_records`, and clears the local buffer.  Worker
// threads may invoke `snapshot_thread_records()` at OMP post-barrier
// sync points; the public API does not currently expose this hook but
// the threshold-triggered call inside `emit()` covers steady-state.
//
// THREAD-SAFETY: drain_records() and reset_records() are not safe to call
// concurrently with emit() on another thread.  Caller MUST quiesce all
// emission paths (typically: drain after OMP-barrier on master thread).
// This is the same precondition `integration_node_rss::drain_node_aggregate`
// requires (cpp:86-94).
// ---------------------------------------------------------------------------
void reset_records();
std::vector<Record> drain_records();

// ---------------------------------------------------------------------------
// REQ-16.3 fold (iter-16 BINDING reviewer ae98902a768f7f242): factor-level
// signature observation for `linear_factors` / `partial_fractions` probes.
//
// At each probe-emission site, the caller computes a 64-bit signature hash
// for every factor (LF) / fraction (PF) in the returned factorisation /
// decomposition, plus that factor's byte size, and calls
// `observe_lf_factor_bytes(signature, bytes)` / `observe_pf_fraction_bytes`.
// The function returns the number of bytes the caller should attribute to
// `unchanged_bytes` for THIS factor:
//
//   * If the signature has been observed on ANY thread BEFORE this call:
//     return `bytes` (the factor is byte-for-byte present in prior state
//     and could have been shared via `shared_ptr<const Poly>` /
//     `shared_ptr<const Rat>` path-copying).
//   * If the signature is new: return 0 (no prior state to share with;
//     this is the first observation), and insert the signature into the
//     global seen-set for future calls.
//
// Implementation: process-global `std::unordered_set<uint64_t>` guarded by
// a mutex (one mutex acquisition per factor per probe-emission; the
// emission is itself gated by FOLD-DC5 sample_rate, so the lock-acquisition
// rate is bounded by sample_rate × thread_count × factors_per_call).  This
// is the iter-17 equivalent of "factors in prior LF/PF-cache entries with
// the same input p canonical-signature" from REQ-16.3 fold (a): we measure
// the cumulative cross-call structural-sharing potential observable at
// signature granularity.
//
// Scope-by-key-attribute (R17.8 carry): the caller is responsible for
// mixing key-relevant attributes into the signature hash before calling
// (e.g., `compute_constant` for LF, `introduce_algebraic_letters` for both).
// This honors the cache-key-includes-`compute_constant` semantics so the
// probe does not collapse semantically distinct factorisations.
//
// `reset_records()` clears both seen-sets so fixture boundaries do not
// leak state across runs of the same long-lived process.
// ---------------------------------------------------------------------------
std::int64_t observe_lf_factor_bytes(std::uint64_t signature_hash,
                                     std::int64_t bytes);
std::int64_t observe_pf_fraction_bytes(std::uint64_t signature_hash,
                                       std::int64_t bytes);

}  // namespace structural_sharing
}  // namespace hyperflint
