// HF FF Phase 5 REC-1 — mpz-pool tracker / FLINT-pool partition attribution
// instrumentation public API.
//
// Design contract:
//     notes/hf_finite_field_program/phase5_three_paths/rec1_mpz_pool_tracker/design.md
//     (iter-79 ζ + §iter-79 fold appendix; 686 LOC; agentId ad8eb1e23b38382f3
//     BINDING CONCERNS-FOLD 5-REQ-4-REC).
//
// Iter-82 (this file) scope: public API surface only (header).
// Implementation (mpz_pool_probe.cpp), TDD harness (test_rec1_mpz_pool_probe.cpp),
// CMake wiring, and main.cpp::main() wiring are DEFERRED to iter-83 under a
// pre-build BINDING adversarial-reviewer dispatch per iter-58/62/67/69/71/73/75/
// 78/79/81 precedent.
//
// Goal: measure the partition share between
//   - mpz-pool block storage (`H_alt3_share`; §C.e addressable; see REC-1 §4.3)
//   - mpz limb storage in retained free-list cells (`H_alt1_share`; §C.a
//     addressable; per REC-1 design §4.3 + REQ-3 range-check fold)
//   - mimalloc-attributed residency residual (`H_alt2_share`; §C.b addressable)
//   - non-mimalloc residual (stack, .text/.data, OS mappings; NOT addressable
//     by any allocator-side lever)
//   - unattributed residual within mimalloc-attributed traffic (sanity check)
// via labelled wraps on `flint_malloc` (block-size signature) and
// `__gmp_allocate_func` (limb-size classification).
//
// Env-gate scheme (per design.md §3.1 + §iter-79 fold appendix REQ-5):
//     HF_REC1_TRACK_MPZ_POOL=1                — master gate. When UNSET, every
//                                               wrap site short-circuits on the
//                                               `if (!hf_rec1_active) return ...;`
//                                               fast path.
//     HF_REC1_LABEL_HEADER_CHECK=1            — sub-flag for the §3.2.1
//                                               page-header structural-match
//                                               labelling check (REQ-5
//                                               OPTION (b)). Default-ON when
//                                               REC-1 active; explicit-OFF
//                                               falls back to size-pattern-only
//                                               labelling (~5 % false-positive
//                                               rate per design audit). Set to
//                                               0 only for stress diagnostics.
//     HF_REC1_OUT_DIR=<dir>                   — snapshot output directory.
//                                               Defaults to cwd.
//
// REQ-1 folds applied at iter-79-ζ (per iter-79-δ BINDING reviewer
// ad8eb1e23b38382f3):
//   - REQ-1 (LOAD-BEARING, FLINT API signature correction): all FLINT memory
//     function wraps use the 6-arg variant
//     `__flint_get/set_all_memory_functions(alloc, calloc, realloc, free,
//     aligned_alloc, aligned_free)`. The 4-arg variant
//     `__flint_set_memory_functions` UNCONDITIONALLY RESETS the aligned
//     allocator binding to FLINT defaults `_flint_aligned_alloc2 /
//     _flint_aligned_free2` (memory_manager.c:331-332) which would WIPE the
//     Phase 0.5 mimalloc-aligned binding installed at
//     `bridge/cli/gmp_mimalloc_init.cpp:146`. The 6-arg variant preserves the
//     aligned binding via explicit pass-through.
//   - REQ-2 (chain-ordering contract): install order at `main()` is Phase 0.5
//     retrofit (`hf_init_mimalloc_for_gmp_flint`) → `hf_probe_init()` →
//     `hf_rec1_init()`. REC-1 chains the GMP-allocator wrap through
//     dag_hashcons_probe's wrap (which itself chains through the Phase 0.5
//     retrofit). Falsifier-5 (REC-1 design §4.5) cross-validates REC-1's
//     `gmp_total_bytes_in_flight` against dag_hashcons_probe's
//     `gmp_labelled_bytes_in_flight` at the same snapshot point.
//   - REQ-3 (range-check labelling falsifier): the labelling-drift falsifier is
//     a range check `B × MPZ_MIN_ALLOC × 8 ≤ mpz_limb_bytes ≤ B × FLINT_MPZ_MAX_CACHE_LIMBS × 8`
//     not an equality, accounting for cell-promotion clamp behaviour (cells
//     in [2, 64]-limb range retain their allocation in the free-list).
//   - REQ-4 (mi_process_info residual partition): `H_alt2_share` uses
//     `mi_resident_bytes` from `mi_process_info` as the mimalloc-attributed
//     denominator, not raw `peak_rss` subtraction. The remaining
//     `non_mimalloc_residual_share` is the un-addressable bucket
//     (stack + .text/.data + OS mappings + non-mimalloc C++ structures).
//   - REQ-5 (80 % threshold ADVISORY + page-header signature check):
//     `mpz_block_bytes / flint_malloc_total ≥ 80 %` is ADVISORY at iter-83;
//     promotion to BINDING gated on iter-83+ Step REC-1-impl-3 verdict
//     measurement of false-positive rate < 1 %.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// FLINT's `flint_malloc` signatures and the mpz-pool block-size constants
// surface in the labelling wrappers; we pull the full FLINT public header
// rather than forward-declare per the dag_hashcons_probe.hpp precedent
// (FLINT 3 typedefs structs via unnamed-struct patterns that frustrate
// forward declarations on some builds).
#include <flint/flint.h>

namespace hyperflint {

// Fast-path active flag. Read by every labelled wrap as the first instruction.
// Set once at process start by `hf_rec1_init()`. Externally visible so the
// wrap functions (defined in mpz_pool_probe.cpp at iter-83+) can early-out
// without a function call.
//
// iter-84 REC-3 fold (BINDING reviewer aca17701072051b8f): promoted from
// `bool` to `std::atomic<bool>`. Rationale: the flag is set by `hf_rec1_init`
// (single-threaded, before OMP starts) and read by every labelled wrap from
// arbitrary OMP threads. Under the pre-REC-3 plain-bool form, the store and
// the loads were technically a data race per the C++ memory model even
// though the relaxed load is benign in practice. The atomic form makes the
// invariant explicit and adds no measurable cost (relaxed loads compile to
// a plain `mov`). Implicit `operator bool()` conversion preserves the
// boolean-context API at call sites.
extern std::atomic<bool> hf_rec1_active;

// Sub-flag for the §3.2.1 page-header signature check (REQ-5 OPTION (b)).
// Default-ON when `hf_rec1_active` is set; explicit-OFF via
// `HF_REC1_LABEL_HEADER_CHECK=0` falls back to size-pattern-only labelling.
//
// iter-84 REC-3 fold companion: also promoted to `std::atomic<bool>` for the
// same reason as `hf_rec1_active`.
extern std::atomic<bool> hf_rec1_label_header_check_active;

// Three-stage initialisation (per REC-1 design §6.1 + REQ-2 chain-ordering
// contract):
//   1. `hf_init_mimalloc_for_gmp_flint()` — Phase 0.5 retrofit at
//      `bridge/cli/gmp_mimalloc_init.cpp:114-149`. Installs mimalloc
//      bindings on BOTH GMP (`mp_set_memory_functions`) and FLINT
//      (`__flint_set_all_memory_functions`).
//   2. `hf_probe_init()` — Phase 5 §A.1 dag-hashcons probe. Wraps the GMP
//      allocator triple if `HF_DAG_HASHCONS_PROBE=1`. (No-op otherwise.)
//   3. `hf_rec1_init()` — THIS function. Wraps the FLINT memory function
//      6-tuple via `__flint_get/set_all_memory_functions` (REQ-1). Wraps
//      the GMP allocator triple via `mp_get/set_memory_functions` (chains
//      through dag_hashcons_probe's wraps if active, else through Phase 0.5
//      retrofit). Reads env flags; sets `hf_rec1_active`.
//   4. Application code (FLINT init, OMP setup, etc.).
//
// REQ-2 ORDERING IS BINDING. Calling `hf_rec1_init()` before `hf_probe_init()`
// would reverse the GMP-allocator chain (dag_hashcons would chain through
// REC-1 instead of vice versa), and dag_hashcons's
// `g_gmp_labelled_bytes_in_flight` would receive double-classified amounts.
//
// REC-1 init refuses to install (and logs a clear diagnostic) if it detects
// that the Phase 0.5 retrofit aligned binding has been wiped (defense-in-depth
// per REQ-1 fold: assert
// `g_prev_flint_aligned_alloc != NULL && g_prev_flint_aligned_alloc != _flint_aligned_alloc2`).
// This mirrors the iter-57-ε.1 §C.a REQ-1 "REFUSES branch" protocol.
//
// If `HF_REC1_TRACK_MPZ_POOL` is unset, `hf_rec1_init()` is a no-op: the
// FLINT memory-function and GMP-allocator wraps are skipped, `hf_rec1_active`
// stays false, and the OFF-path budget (§3.5: ≤ 2 % RSS / ≤ 5 % wall) is
// structurally honoured.
void hf_rec1_init();

// Snapshot the current REC-1 partition counters and emit a JSON file at
// `<HF_REC1_OUT_DIR>/rec1_mpz_pool_<fixture>.json` (or `<cwd>/...` if
// `HF_REC1_OUT_DIR` is unset). Called from CLI per-fixture exit hooks at
// peak-RSS checkpoint and at end-of-fixture. Safe to call from any thread;
// relaxed atomic loads on the counters.
//
// `fixture_id` is a short string identifying the fixture (e.g. "tst0", "tst2",
// "parity_1", "findroots21_a"). It is included as a top-level key in the
// emitted JSON.
//
// `checkpoint_tag` is a short string identifying the checkpoint (e.g.
// "peak_rss", "fixture_exit", "manual"). It is included as a top-level key.
//
// When `hf_rec1_active` is false, this is a no-op.
void hf_rec1_snapshot(const char* fixture_id, const char* checkpoint_tag);

// End-of-run aggregation: emit the cross-fixture aggregate
// `rec1_mpz_pool_aggregate.json` consolidating per-fixture snapshots from this
// process. Registered via `std::atexit` from `hf_rec1_init()` when active.
// Idempotent: subsequent calls are no-ops.
void hf_rec1_finalize();

// REC-1 partition snapshot struct exposed to tests and to internal callers
// that want to consult the counters without going through the JSON emit path.
// Bytes are signed int64 to allow defense-in-depth detection of double-free
// underflow (negative value = bug; positive only = clean).
//
// All fields are snapshotted with relaxed atomic loads; values are consistent
// at the snapshot point but may diverge under concurrent traffic.
struct HfRec1PartitionSnapshot {
    // Labelled FLINT mpz-pool block traffic (272-KiB signature; per design §1.1
    // + REC-1 fold REC-1 cells-per-block 16352). Per-block, the labelling
    // wrap increments at `flint_malloc(block_size)` and decrements at
    // `flint_free(block_ptr)` matched by stored tag.
    int64_t mpz_block_bytes_in_flight   = 0;
    int64_t labelled_block_count        = 0;

    // Cumulative FLINT `flint_malloc` + `flint_calloc` traffic, all sizes.
    // CUMULATIVE-NOT-IN-FLIGHT semantics: incremented at every alloc; NOT
    // decremented at free (FLINT's free signature is `void(void*)` — no size,
    // and tracking every flint_malloc ptr in a map would blow the tag-map up
    // to O(10^4–10^6) entries on heavy fixtures, well beyond REC-1's
    // diagnostic-only scope). Renamed from `flint_malloc_total_bytes_in_flight`
    // at iter-83-δ REQ-A (BINDING fold; agentId `aca17701072051b8f`) to make
    // the cumulative semantics explicit.
    //
    // The §5.1 ADVISORY `mpz_block_bytes / flint_malloc_total_cumulative` ratio
    // is only meaningful AT PEAK-RSS CHECKPOINT (where `mpz_block_bytes_in_flight`
    // is at its peak) AND interpreted as "ratio of peak-in-flight block bytes
    // over cumulative FLINT traffic so far". The §5.1 threshold (≥ 80 %) is
    // structurally biased downward by this denominator drift and the
    // RECONSIDER-AT-IMPL clause in §iter-79 fold REQ-5 governs eventual
    // re-promotion at impl-2/impl-3 if a tag-map-tracked in-flight variant is
    // added.
    int64_t flint_malloc_total_bytes_cumulative = 0;

    // Labelled GMP limb-storage traffic (size-classified as
    // `n × FLINT_BITS / 8` bytes for cells with `n` limbs; clamp range
    // `[MPZ_MIN_ALLOC × 8, FLINT_MPZ_MAX_CACHE_LIMBS × 8] = [16, 512]` bytes
    // per REQ-3 range-check fold).
    int64_t mpz_limb_bytes_in_flight    = 0;
    int64_t labelled_limb_alloc_count   = 0;

    // GMP "other" traffic (sizes outside the limb classification range).
    int64_t gmp_other_bytes_in_flight   = 0;

    // Total GMP allocator traffic, all sizes. Cross-check value: at the same
    // snapshot point, this must equal dag_hashcons_probe's
    // `gmp_labelled_bytes_in_flight` (REQ-2 falsifier-5 self-consistency).
    int64_t gmp_total_bytes_in_flight   = 0;

    // mimalloc committed-bytes snapshot from `mi_process_info().peak_commit`
    // (REQ-4 fold: H_alt2 denominator). Weak-extern resolved at runtime;
    // if mimalloc is not the active allocator, set to -1.
    //
    // iter-84 REC-6 fold (BINDING reviewer aca17701072051b8f): renamed from
    // `mi_resident_bytes_peak`. The value is populated from `peak_com`
    // (`mi_process_info` 7th out-arg) which is mimalloc's "peak committed"
    // counter, not its RSS counter. Calling it "resident" was a misnomer that
    // would have caused confusion at impl-3 verdict authoring (peak_commit ≥
    // peak_rss generically since committed memory may not all be
    // resident). Aligned with mimalloc's own terminology.
    int64_t mi_committed_bytes_peak     = 0;

    // Process peak RSS in bytes (from `mach_task_self()` task_info /
    // `getrusage(RUSAGE_SELF).ru_maxrss`). Used as the denominator for
    // `H_alt1_share`, `H_alt3_share`, and `non_mimalloc_residual_share`.
    int64_t process_peak_rss_bytes      = 0;

    // ------------------------------------------------------------------
    // iter-84 page-header structural-check counters (§3.2.1 OPTION (b);
    // discharges REC-5 RECONSIDER-AT-IMPL precondition for §5.1 threshold
    // promotion ADVISORY → BINDING).
    //
    // At free time, when a tagged block is freed, REC-1 reads the
    // `fmpz_block_header_s` fields at the block base and at the first
    // aligned page; structural match increments `struct_match_pass_count`,
    // mismatch increments `struct_match_fail_count`. The ratio
    //     struct_match_fail / (struct_match_pass + struct_match_fail)
    // is the empirical false-positive rate of the size-pattern labelling.
    // Gated behind `HF_REC1_LABEL_HEADER_CHECK` (default-ON).
    //
    // Both counters are zero when the header check is OFF.
    // ------------------------------------------------------------------
    int64_t struct_match_pass_count     = 0;
    int64_t struct_match_fail_count     = 0;

    // ------------------------------------------------------------------
    // iter-86 Option (a) peak-tracking fields (schema 2 → 3). Each peak
    // field is the high-water mark of the corresponding *_in_flight
    // counter across the lifetime of the process, captured via a
    // compare-exchange CAS loop on every fetch_add at the wrap callsite
    // (see mpz_pool_probe.cpp `update_peak_to_max`). Invariants:
    //   peak ≥ current at all times (relaxed-load consistency under
    //     serial observation; under concurrent observation the invariant
    //     holds eventually but may transiently appear violated if a
    //     snapshot interleaves between a writer's fetch_add and its CAS
    //     retire — REC-2 iter-86 BINDING reviewer a6e7c42a949778ffa).
    //   peak is monotonically non-decreasing (free path never updates)
    // Rationale: iter-85 atexit-residual KEY FINDING showed that
    // aggregate-atexit snapshots capture POST-cleanup residual (= 0 for
    // mpz_block_bytes when FLINT's _fmpz_cleanup atexit handler has
    // drained the pool). Peak-tracking decouples the gate metric from
    // snapshot timing: any snapshot call (atexit, eval-end, manual)
    // sees the historic high-water, not the instantaneous in-flight.
    int64_t mpz_block_bytes_in_flight_peak   = 0;
    int64_t labelled_block_count_peak        = 0;
    int64_t mpz_limb_bytes_in_flight_peak    = 0;
    int64_t gmp_total_bytes_in_flight_peak   = 0;
};

// Diagnostic accessor returning the current REC-1 partition snapshot. Used by
// the iter-83+ TDD test cases:
//   - `TEST(Rec1, EnvGateOff)` — verify all fields zero when active=false.
//   - `TEST(Rec1, EnvGateOn_FlintMallocChain)` — verify
//     `flint_malloc_total_bytes_cumulative` tracks total FLINT traffic.
//   - `TEST(Rec1, LabellingDriftOnSyntheticBlocks)` — verify
//     `labelled_block_count` matches manually-issued block-sized allocations.
//   - `TEST(Rec1, PerSlotCounterUnderOMP)` — verify per-slot counter sum
//     equals serial-equivalent count under 13-thread OMP region.
HfRec1PartitionSnapshot hf_rec1_get_partition_snapshot();

// Per-thread accounting accessor (REC-1 design §3.3 + REQ-2 co-active
// semantics). Returns the per-OMP-slot block count for the calling thread, or
// the aggregate sum across slots if `omp_slot < 0`. iter-83+ TDD
// `TEST(Rec1, PerSlotCounterUnderOMP)` exercises this.
int64_t hf_rec1_get_slot_block_count(int omp_slot);

}  // namespace hyperflint
