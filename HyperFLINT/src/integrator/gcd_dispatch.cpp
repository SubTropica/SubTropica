// Phase 0.5 Item U Stage 2: libdispatch wrapper for the outer integration_step
// parallel-for.  See include/hyperflint/integrator/gcd_dispatch.hpp for the
// full design rationale.
//
// Concurrency-bounding design:
//   dispatch_apply on the global QOS_CLASS_USER_INITIATED queue can schedule
//   iterations on ANY available GCD worker thread, including efficiency cores.
//   On Apple M-series the thread pool can be larger than omp_get_max_threads().
//   To prevent out-of-bounds indexing into per-slot arrays (accum_t, timer
//   vectors, etc.), we bound concurrent execution to exactly max_slots via a
//   dispatch semaphore.  At most max_slots iterations run simultaneously, so
//   at most max_slots distinct slots are needed at once.
//
// Phase 1 Task 1.E (Item U TSan-attempt fix, 2026-05-09):
//   The original Stage 2 design assigned slots via a monotone counter mod
//   max_slots, with thread_local caching of (call_token, t_slot).  TSan
//   under HF_USE_GCD=1 OMP=2 surfaced thousands of races on per-slot
//   storages (accum_t[slot], gcd_cofactors_storage[slot], etc.) at the
//   same byte address from two concurrent workers.  Root cause: the
//   counter-mod scheme assigns the same slot N to the 1st and the
//   (max_slots+1)th worker that enters; the semaphore allows up to
//   max_slots concurrent iterations, but it does NOT couple to the
//   counter, so out-of-order signal/wait can leave the 1st-slot-N
//   worker still running when the (max_slots+1)th-slot-N worker
//   acquires the semaphore.  The fix: replace the counter+modulo
//   resolver with a bitmask-based free-slot pool.  Each iteration
//   acquires the lowest free slot via CAS, runs the body, and releases
//   the slot back to the pool.  This guarantees at most one
//   concurrent worker per slot - no slot collision, no race.
//   Thread_local caching (per-call (call_token, t_slot)) is removed
//   since slots are now bound to ITERATIONS, not WORKER THREADS:
//   the same worker may hold different slots across consecutive
//   iterations, but no two concurrent iterations share a slot.
//
// Phase 3 section B Lever U (chunked parallelism, 2026-05-10):
//   Coalesce HF_GCD_CHUNK_SIZE adjacent integrator entries into one
//   libdispatch body invocation. The body acquires ONE slot, processes
//   chunk_size entries serially under that slot, then releases the slot.
//   Per-chunk: 1 x dispatch_semaphore_wait, 1 x acquire_slot_from_mask
//   CAS, chunk_size x body() calls, 1 x release_slot_to_mask, 1 x
//   dispatch_semaphore_signal. Per-slot storage reuse across chunked
//   entries mirrors the OMP fallback's static partition; no new TSan
//   exposure (memo section 4.2 + 4.7 invariant).
//
//   Knob: HF_GCD_CHUNK_SIZE env-var, integer >= 1. Default-OFF (unset
//   or "1") => chunk_size=1, byte-identical to the Phase 1 Task 1.E
//   implementation (one body() per dispatch_apply iteration; loop
//   degenerates to single-entry).
//
//   FOLD-2 small-n guard (iter-7 binding adversarial-reviewer agentId
//   a208448c15242e550): if n < chunk_size, demote chunk to 1 to
//   eliminate the worst-case load imbalance for n in [4, chunk_size)
//   (n=4, chunk=4, max_slots=13 -> 1 chunk -> 12 idle workers).
//
//   Probe-mode (HF_GCD_CHUNK_PROBE=1): per-chunk JSONL line on stderr
//   tagged record_type=gcd_chunk, with per-chunk wall, sema-wait wall,
//   slot-acquisition wall, body-summed wall, chunk_idx, chunk_size,
//   slot, n_entries. Format mirrors HF_STEP_TRACE JSONL convention
//   (integration_step.cpp:2034). Off in production; consumed by
//   iter-9 chunk-size sweep aggregator. Probe-on tst2 wall budget cap
//   = 35 s per FOLD-R1 + memo section 7 (~18 % overhead vs 29.65 s
//   unsampled baseline; below this cap the per-chunk timestamps are
//   captured in full).
//
//   Design memo (FINAL after iter-7 9 folds):
//     notes/hf_finite_field_program/phase3_combined/
//       lever_u_chunked_parallelism/design.md

#include "hyperflint/integrator/gcd_dispatch.hpp"
// Iter-95 §T7 twenty-fifth chunk Track-gcd-chunk: macro-layer
// indirection moved into hyperflint::integrator::env_flags;
// see include/hyperflint/integrator/env_flags.hpp for the
// fifth-extension narrative (n=4: master gate, verbose-mode
// diagnostic, chunk-size knob, chunk-probe JSONL emitter).
#include "hyperflint/integrator/env_flags.hpp"

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hyperflint {
namespace integrator {

// ---------------------------------------------------------------------------
// Slot resolver — bitmask free-slot pool (Phase 1 Task 1.E fix)
// ---------------------------------------------------------------------------
//
// A per-call atomic uint64_t bitmask tracks which slots are FREE.  Bit s set
// means slot s is available; bit s clear means slot s is held by a worker.
// Acquire: CAS-loop to flip the lowest set bit to 0.  Release: atomic-or the
// bit back to 1.  The dispatch semaphore (initial value max_slots) bounds
// concurrent body invocations; the bitmask resolves which of those concurrent
// invocations gets which slot.
//
// max_slots <= 64 is enforced by static_assert/assert.  HF's OMP team is
// bounded by hardware concurrency (currently 14 on M4 Pro), so 64 is
// generous.  If max_slots > 64 ever becomes necessary, switch the bitmask
// to a fixed-size vector of std::atomic<uint64_t>.
//
// Acquire is wait-free in expectation: the semaphore guarantees at most
// max_slots concurrent waiters, and at least one of the max_slots bits is
// always set when a worker acquires the semaphore, so the CAS loop
// terminates in O(max_slots) tries.

// Global monotonic token generator (kept for diagnostics; not used by the
// new slot resolver).
static std::atomic<int> g_token_counter{1};

int new_call_token() {
    return g_token_counter.fetch_add(1, std::memory_order_relaxed);
}

// Acquire the lowest-numbered free slot from the bitmask.
// Spins until the CAS succeeds; the semaphore guarantees at least one bit
// is always free when this is called.
static int acquire_slot_from_mask(std::atomic<uint64_t>* free_mask) {
    uint64_t old_mask = free_mask->load(std::memory_order_acquire);
    while (true) {
        assert(old_mask != 0 && "semaphore should guarantee a free slot");
        // __builtin_ctzll gives the index of the lowest set bit.
        const int slot = __builtin_ctzll(old_mask);
        const uint64_t new_mask = old_mask & ~(uint64_t{1} << slot);
        if (free_mask->compare_exchange_weak(
                old_mask, new_mask,
                std::memory_order_acquire,
                std::memory_order_acquire)) {
            return slot;
        }
        // CAS failed; old_mask refreshed by compare_exchange_weak; retry.
    }
}

// Release a slot back to the pool.
static void release_slot_to_mask(std::atomic<uint64_t>* free_mask, int slot) {
    free_mask->fetch_or(uint64_t{1} << slot, std::memory_order_release);
}

// Public testing overload.  Uses a per-token bitmask reset on token change so
// the unit test can exercise slot assignment from a single thread.  Note
// that this stub does NOT release slots back; tests that rely on the new
// release-on-completion semantics must invoke dispatch_parallel_for itself.
int acquire_slot(int call_token, int max_slots) {
    static std::atomic<uint64_t> test_mask{0};
    static int last_seen_token = -1;
    if (last_seen_token != call_token) {
        const uint64_t init = (max_slots >= 64)
            ? ~uint64_t{0}
            : ((uint64_t{1} << max_slots) - 1);
        test_mask.store(init, std::memory_order_relaxed);
        last_seen_token = call_token;
    }
    return acquire_slot_from_mask(&test_mask);
}

// ---------------------------------------------------------------------------
// dispatch_parallel_for
// ---------------------------------------------------------------------------

bool gcd_dispatch_available() {
#ifdef __APPLE__
    return true;
#else
    return false;
#endif
}

bool gcd_dispatch_enabled() {
    // Phase 1 Task 1.E FOLD-4: cached per-thread reader of HF_USE_GCD.
    // -1 = unread, 0 = disabled, 1 = enabled.
    //
    // Phase 1-Verdict (2026-05-09): default-ON flip.  After Item U
    // slot-collision fix (commit 81ef8a0df) restored correctness and
    // delivered tst2 -2.18% wall vs hyperflint_v1, GCD dispatch becomes
    // the new default.  Set HF_USE_GCD=0 to opt back to the OMP path
    // (used for bit-byte sha-id checks against pre-flip binaries).
    thread_local int cached = -1;
    if (cached < 0) {
        // Iter-95 Track-gcd-chunk macro relocation: master gate
        // for the dispatch_apply parallel-for path; see
        // include/hyperflint/integrator/env_flags.hpp for the
        // call-site narrative (default-direction flipped to ON
        // per 2026-05-09 Phase 1-Verdict, opt-back-to-OMP knob).
        const char* v = HF_FLAG_USE_GCD;
        cached = (v && std::strcmp(v, "0") == 0) ? 0 : 1;
    }
    return cached == 1;
}

// Phase 1 Task 1.E FOLD-9: optional diagnostic to verify dispatch_apply is
// actually entered (vs silent OMP-fallthrough due to gate-logic bug).
// Iter-95 Track-gcd-chunk: see include/hyperflint/integrator/env_flags.hpp
// for the call-site narrative (default-OFF, exact-match-"1" opt-in).
static bool gcd_dispatch_verbose() {
    thread_local int cached = -1;
    if (cached < 0) {
        const char* v = HF_FLAG_USE_GCD_VERBOSE;
        cached = (v && std::strcmp(v, "1") == 0) ? 1 : 0;
    }
    return cached == 1;
}

// Phase 3 section B Lever U: chunk-size knob. Default-OFF (unset or "1")
// => chunk_size=1, byte-identical to Phase 1 Task 1.E implementation.
// Cached per-thread on first call (no synchronization needed). Per-call
// overhead at default-OFF = one thread_local int load + one branch
// (~2 ns on first call, ~1 ns on subsequent; FOLD-4 corrected accounting:
// the env read fires ONCE per dispatch_parallel_for invocation, not once
// per integrator entry; on tst2 with ~5 dispatch_parallel_for calls per
// run, total default-OFF overhead is ~10 ns across the entire run).
static int hf_gcd_chunk_size() {
    thread_local int cached = -1;
    if (cached < 0) {
        // Iter-95 Track-gcd-chunk: POSITIVE_INTEGER value-family
        // knob; see include/hyperflint/integrator/env_flags.hpp
        // for the default-unset⇒1 byte-identical-to-Phase-1-Task-1.E
        // call-site narrative.
        const char* v = HF_FLAG_GCD_CHUNK_SIZE;
        int parsed = 1;
        if (v && *v) {
            parsed = std::atoi(v);
            if (parsed < 1) parsed = 1;
        }
        cached = parsed;
    }
    return cached;
}

// Phase 3 section B Lever U: probe-mode gate. When HF_GCD_CHUNK_PROBE=1,
// every chunk emits a JSONL line on stderr containing per-chunk wall,
// sema-wait wall, slot-acquisition wall, body-summed wall, chunk_idx,
// chunk_size, slot, n_entries. Iter-9 chunk-size sweep aggregator
// consumes these JSONLs. Default-OFF: no emission, no measurement, zero
// overhead beyond the per-chunk thread_local read + branch. Format
// mirrors HF_STEP_TRACE JSONL convention used at
// integration_step.cpp:2034.
static bool hf_gcd_chunk_probe_enabled() {
    thread_local int cached = -1;
    if (cached < 0) {
        // Iter-95 Track-gcd-chunk: per-chunk JSONL emitter gate;
        // see include/hyperflint/integrator/env_flags.hpp for
        // the default-OFF exact-match-"1" opt-in call-site
        // narrative.
        const char* v = HF_FLAG_GCD_CHUNK_PROBE;
        cached = (v && std::strcmp(v, "1") == 0) ? 1 : 0;
    }
    return cached == 1;
}

void dispatch_parallel_for(size_t begin, size_t end, int max_slots,
                            const std::function<void(size_t, int)>& body) {
    if (begin >= end) return;
    const size_t n = end - begin;

    if (gcd_dispatch_verbose()) {
        std::fprintf(stderr,
            "GCD dispatch_apply n=%zu max_slots=%d\n", n, max_slots);
    }

    // Phase 1 Task 1.E (Item U TSan-attempt fix, 2026-05-09): max_slots is
    // bounded by HF's OMP team width (omp_get_max_threads(), 14 on M4 Pro);
    // 64 is a generous ceiling that fits a single uint64_t bitmask.  If a
    // larger team width ever becomes possible, promote `free_mask` to
    // std::vector<std::atomic<uint64_t>>.
    assert(max_slots > 0 && max_slots <= 64 &&
           "dispatch_parallel_for max_slots must be in [1, 64]");

#ifdef __APPLE__
    // Phase 3 section B Lever U: chunk-size resolution + FOLD-2 small-n
    // guard. At default-OFF (HF_GCD_CHUNK_SIZE unset) chunk=1 and
    // n_chunks=n -> identical iteration count to the Phase 1 Task 1.E
    // implementation. FOLD-2: if n < chunk, demote chunk to 1 (avoids the
    // worst-case n_chunks=1 load imbalance, e.g., n=4, chunk=4,
    // max_slots=13 -> 1 chunk -> 12 idle workers; design memo section
    // 3.1.5 small-n band table).
    size_t chunk = static_cast<size_t>(hf_gcd_chunk_size());
    if (chunk == 0) chunk = 1;
    if (n < chunk) chunk = 1;
    const size_t n_chunks = (n + chunk - 1) / chunk;

    // Per-call slot bitmask + call token on the stack.  Block captures the
    // address (plain pointer, copyable).  All max_slots bits start set
    // (every slot free).
    const uint64_t init_mask = (max_slots >= 64)
        ? ~uint64_t{0}
        : ((uint64_t{1} << max_slots) - 1);
    std::atomic<uint64_t> free_mask{init_mask};
    std::atomic<uint64_t>* free_mask_ptr = &free_mask;
    const int token = new_call_token();
    (void)token;  // reserved for diagnostics; resolver no longer keys on it

    // Semaphore limits the number of concurrently executing chunks (each
    // holding one slot for the duration of `chunk_size` body() calls) to
    // max_slots.  Together with the bitmask resolver, this guarantees that
    // every chunk runs with a unique-among-concurrent-chunks slot
    // index in [0, max_slots), and per-slot storages (accum_t[slot],
    // gcd_cofactors_storage[slot], etc.) are never touched concurrently.
    // Per-slot storages are REUSED across the chunk_size entries within
    // one chunk -- identical to the OMP fallback's
    // schedule(static, 1) per-thread static partition (memo section 4.2).
    dispatch_semaphore_t sem = dispatch_semaphore_create(max_slots);

    dispatch_queue_t q =
        dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);

    // Probe-mode capture for iter-9 chunk-size sweep. probe_on captured
    // by value in the block; chunk captured by value as size_t (chunk_int
    // is an int copy for JSONL emission).
    const bool probe_on = hf_gcd_chunk_probe_enabled();
    const int chunk_int = static_cast<int>(chunk);

    dispatch_apply(n_chunks, q, ^(size_t chunk_idx) {
        const auto t_chunk_in = probe_on
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};

        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        const auto t_after_sema = probe_on
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};

        const int slot = acquire_slot_from_mask(free_mask_ptr);

        const auto t_after_slot = probe_on
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};

        const size_t lo = chunk_idx * chunk;
        const size_t hi = std::min(lo + chunk, n);

        // FOLD-R4 (memo section 4.6): a body() exception leaks the held
        // slot back to the bitmask pool and the semaphore. Pre-existing
        // concern at chunk_size=1; chunking amplifies (N-j entries
        // abandoned vs 0) but does not introduce. HF integration loop
        // has no recovery path for body exceptions, so intentionally not
        // caught here. If a future iter introduces partial-failure
        // recovery, wrap the chunk loop in try/catch and release_slot +
        // dispatch_semaphore_signal in the catch path.
        double body_sum_s = 0.0;
        if (probe_on) {
            for (size_t idx = lo; idx < hi; ++idx) {
                const auto t_b0 = std::chrono::steady_clock::now();
                body(begin + idx, slot);
                const auto t_b1 = std::chrono::steady_clock::now();
                body_sum_s += std::chrono::duration<double>(
                    t_b1 - t_b0).count();
            }
        } else {
            for (size_t idx = lo; idx < hi; ++idx) {
                body(begin + idx, slot);
            }
        }

        release_slot_to_mask(free_mask_ptr, slot);

        if (probe_on) {
            const auto t_chunk_out = std::chrono::steady_clock::now();
            const double chunk_wall_s = std::chrono::duration<double>(
                t_chunk_out - t_chunk_in).count();
            const double sema_wait_s = std::chrono::duration<double>(
                t_after_sema - t_chunk_in).count();
            const double slot_acq_s = std::chrono::duration<double>(
                t_after_slot - t_after_sema).count();
            std::fprintf(stderr,
                "{\"record_type\":\"gcd_chunk\","
                "\"chunk_idx\":%zu,"
                "\"chunk_size\":%d,"
                "\"slot\":%d,"
                "\"chunk_wall_s\":%.9f,"
                "\"sema_wait_s\":%.9f,"
                "\"slot_acq_s\":%.9f,"
                "\"body_sum_s\":%.9f,"
                "\"n_entries\":%zu}\n",
                chunk_idx, chunk_int, slot,
                chunk_wall_s, sema_wait_s, slot_acq_s, body_sum_s,
                hi - lo);
        }

        dispatch_semaphore_signal(sem);
    });

    dispatch_release(sem);
#else
    // Non-Apple fallback: serial loop.  Slot 0 for all iterations.
    // Chunking is a no-op on the serial path (chunk_size partitioning
    // is irrelevant when n_workers=1); HF_GCD_CHUNK_SIZE has no effect.
    (void)max_slots;
    for (size_t i = 0; i < n; ++i) {
        body(begin + i, 0);
    }
#endif
}

}  // namespace integrator
}  // namespace hyperflint
