#pragma once
// Phase 0.5 Item U Stage 2: Apple libdispatch opt-in for the outer
// integration_step parallel-for.
//
// On Apple platforms this wraps dispatch_apply on the
// QOS_CLASS_USER_INITIATED global queue. On non-Apple platforms
// dispatch_parallel_for falls back to a serial loop; the OMP path
// in integration_step.cpp is selected via the #if defined(__APPLE__)
// guard so this fallback is only reached in testing.
//
// Gate: env-var HF_USE_GCD=1. Unset or "0" -> OMP path. Phase 1 Task 1.E
// flipped this default to ON (HF_USE_GCD=1 is now the production
// default; HF_USE_GCD=0 selects the OMP fallback).
//
// Slot resolver design (Phase 1 Task 1.E bitmask free-slot pool):
//   dispatch_apply workers do not have a stable thread index in [0, N).
//   Each chunk acquires a slot via CAS on a per-call atomic uint64_t
//   bitmask: lowest set bit (free slot) is flipped to 0 on acquire,
//   re-or'd back on release. The dispatch semaphore (initial value
//   max_slots) bounds concurrent body invocations; the bitmask
//   resolves which of those concurrent invocations gets which slot.
//   Slots are bound to ITERATIONS (chunks), not to worker threads:
//   the same worker may hold different slots across consecutive
//   chunks, but no two concurrent chunks share a slot. See
//   gcd_dispatch.cpp top-of-file rationale + commit 81ef8a0df.
//
// Phase 3 section B Lever U (chunked parallelism, 2026-05-10):
//   Two env-var knobs control chunked execution and probe-mode
//   instrumentation, both default-OFF.
//
//   HF_GCD_CHUNK_SIZE=N (integer >= 1; unset or "1" -> chunk_size=1
//     byte-identical to Phase 1 Task 1.E implementation): coalesce N
//     adjacent integrator entries into one libdispatch body invocation
//     under one slot acquire/release. Reduces per-iteration semaphore
//     and bitmask sync ops by factor N, targeting the 51.5 % cumulative
//     kernel_sync (libsystem) LEAF surface measured at Phase 2
//     section B.1 sample(1) on tst2 (notes/hf_finite_field_program/
//     phase2_investigation/{verdict.md,hot_spot_table.md,step_trace/}).
//     FOLD-2 small-n guard: if n < chunk_size, demote chunk to 1 to
//     avoid n_chunks < max_slots load imbalance.
//
//   HF_GCD_CHUNK_PROBE=1: emit one JSONL line per chunk on stderr
//     tagged record_type=gcd_chunk (chunk_idx, chunk_size, slot,
//     chunk_wall_s, sema_wait_s, slot_acq_s, body_sum_s, n_entries).
//     Format mirrors HF_STEP_TRACE JSONL convention used at
//     integration_step.cpp:2034. Default-OFF: no emission, no
//     measurement, zero overhead.
//
//   Default-OFF (both env-vars unset) is bit-byte-identical to
//   commit c6c295907 (Phase 3 section A.7 closure; iter-2 4/4 sha-id
//   PASS gate baseline). Iter-9 chunk-size sweep + iter-10 production
//   N=5 paired A/B at the chosen best chunk_size are the next
//   benchmark milestones; design memo at notes/hf_finite_field_program/
//   phase3_combined/lever_u_chunked_parallelism/design.md (FINAL after
//   iter-7 9 folds applied per binding adversarial-reviewer agentId
//   a208448c15242e550).
//
// Recursion safety:
//   If the same thread re-enters dispatch_parallel_for (i.e. nested
//   calls), the inner call gets its own per-call free_mask + token on
//   the stack; slot acquisition in the inner call is independent of
//   the outer call's bitmask. Integration_step does not recurse under
//   HF_USE_GCD=1 (commit 81ef8a0df + Phase 3 section A.6 PASS audit
//   confirm), so nested dispatch is not exercised in practice.

#include <cstddef>
#include <functional>

namespace hyperflint {
namespace integrator {

// Parallel-for using libdispatch (Apple) or serial fallback (non-Apple).
//
// Iterates entry_i in [begin, end). The body is called as body(entry_i, slot)
// where slot is in [0, max_slots) and is stable for the duration of this call
// on a per-thread basis (same worker thread always gets the same slot within
// one dispatch_parallel_for invocation).
//
// max_slots must equal the size of any per-slot array the body indexes into
// (e.g. accum_t.size() in integration_step).
void dispatch_parallel_for(size_t begin, size_t end, int max_slots,
                            const std::function<void(size_t, int)>& body);

// Returns true on __APPLE__, false elsewhere.
bool gcd_dispatch_available();

// Returns true iff env-var HF_USE_GCD is set to "1".  Cached per-thread in a
// thread_local on first call (no synchronization needed).
//
// Used by integration_step's outer entry-loop to switch between OMP and GCD
// dispatch paths at runtime.  Phase 0.5 Item U Stage 2 + Phase 1 Task 1.E
// (FOLD-4 from binding adversarial-reviewer agentId aea96b1612fb87428).
bool gcd_dispatch_enabled();

// Slot resolver helpers — exposed for unit testing.
//
// new_call_token(): returns a new monotonically increasing token.
// acquire_slot(call_token, max_slots): returns a stable slot in [0, max_slots)
//   for the calling thread within the current call identified by call_token.
//   If this thread has not yet acquired a slot under call_token, it atomically
//   claims the next available slot via a shared counter. Subsequent calls with
//   the same call_token return the cached slot without an atomic op.
int new_call_token();
int acquire_slot(int call_token, int max_slots);

}  // namespace integrator
}  // namespace hyperflint
