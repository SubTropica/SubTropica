// Phase 5f: HyperFLINT driver — port of HyperIntica.wl:4913.

#include "hyperflint/integrator/hyper_int.hpp"

#include "hyperflint/integrator/env_flags.hpp"  // iter-77 Track-probe-ctx
#include "hyperflint/integrator/integration_step.hpp"
#include "hyperflint/reduce/mzv_reduce.hpp"  // substitute_var_rat
#include "hyperflint/runtime/narrow_ctx_flag.hpp"  // R24v2 narrow-ctx defense
#include "hyperflint/runtime/rat_split_verify.hpp"  // Phase A commit (3) verifier
#include "hyperflint/runtime/scalar_rep.hpp"  // Phase B B7 env-gate
#include "hyperflint/runtime/scs_roundtrip.hpp"  // Phase B B7 entry/exit adapters
#include "hyperflint/runtime/trace_gate.hpp"  // iter-85 §T7: canonical step_trace_enabled() (replaced local anon-ns duplicate at L853)
#include "hyperflint/algebra/linear_factors.hpp"  // 2026-05-05 Probe 3a
#include "hyperflint/algebra/partial_fractions.hpp"  // iter-17 pf_storage probe
#include "hyperflint/algebra/poly_struct_hash.hpp"  // 2026-05-05 Probe 1-UB
#include "hyperflint/core/operator_memo.hpp"  // §E iter-7: evict_post_step_hook
#include "hyperflint/diagnostics/step_trace_rss.hpp"  // Phase 0 task 0-3: per-step RSS
#include "hyperflint/diagnostics/integration_node_rss.hpp"  // §6.D iter-13: outer-step context setter
#include "hyperflint/runtime/env_flags.hpp"  // §T7 third chunk: HF_FLAG_MI_* macros

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// HF_MI_STATS=1 enables per-step mimalloc diagnostics. We declare
// the mimalloc symbols as weak externs so this TU links cleanly in
// builds that don't include mimalloc (the LibraryLink dylib does
// not link it; only the CLI binary does via -force_load). When the
// symbols resolve to null at runtime the diagnostic is silently
// disabled. (dlsym(RTLD_DEFAULT, ...) won't work because the
// statically-linked mimalloc symbols are file-local in the
// executable's symbol table.)
extern "C" {
__attribute__((weak)) void mi_collect(int /*force*/);
__attribute__((weak)) void mi_process_info(size_t*, size_t*, size_t*,
                                             size_t*, size_t*,
                                             size_t*, size_t*, size_t*);

// iter-38 REC-1 instrumentation: per-size-class breakdown of live
// allocations via mimalloc's heap-area visitor. Used to populate
// FOLD-M10 column at payload-vs-backing tier (Phase 5 plan §A.1).
// HF_MI_HEAP_VISIT=1 env-gated, defaults off to preserve byte-id.
//
// ABI per mimalloc v3.x public header (mimalloc.h). The mi_heap_t
// struct is opaque; only the pointer is passed through. mi_heap_area_t
// layout is fixed in v3.x ABI (see /opt/homebrew/opt/mimalloc/include/
// mimalloc.h ~ lines 350-360 of v3.3.1; mirrored here so we do not
// include the brew header).
//
// Symbol availability (verified via nm at iter-38 Phase 38-α):
//   _mi_heap_visit_blocks   PRESENT  (0x1000054a0 in baseline binary)
//   _mi_heap_main           PRESENT  (0x100009b24 in baseline binary)
//   _mi_heap_get_default    ABSENT   from the v3.3.1 brew libmimalloc.a
//                                     static archive's symbol table — the
//                                     public-API name was either inlined
//                                     in mimalloc.h or renamed to
//                                     _mi_theap_get_default (an internal
//                                     thread-heap helper). We rely on
//                                     mi_heap_main() as the heap source;
//                                     this returns the global main-heap
//                                     pointer, which is sufficient for
//                                     FOLD-M10 size-class accounting in
//                                     the integrator path where the
//                                     dominant allocations originate on
//                                     the main thread (OMP workers donate
//                                     abandoned blocks back to main on
//                                     parallel-region exit).
struct mi_heap_s;
struct mi_heap_area_s {
    void*  blocks;
    size_t reserved;
    size_t committed;
    size_t used;
    size_t block_size;
    size_t full_block_size;
    int    heap_tag;
};
typedef struct mi_heap_s      mi_heap_t;
typedef struct mi_heap_area_s mi_heap_area_t;
typedef bool (mi_block_visit_fun)(const mi_heap_t*, const mi_heap_area_t*,
                                    void*, size_t, void*);

__attribute__((weak)) mi_heap_t* mi_heap_main(void);
__attribute__((weak)) bool mi_heap_visit_blocks(const mi_heap_t* heap,
                                                  bool visit_blocks,
                                                  mi_block_visit_fun* visitor,
                                                  void* arg);

// iter-39 REC-1 cross-theap instrumentation: extend the iter-38
// main-heap-only visitor to iterate ALL heaps in the main subproc via
// mi_subproc_visit_heaps(mi_subproc_main(), ...). Each visited heap
// receives a sub-callback that invokes mi_heap_visit_blocks(heap,
// visit_blocks=false, ...) for area-level accumulation, with a SHARED
// bucket accumulator across all heaps. HF_MI_HEAP_VISIT_ALL=1 env-gated,
// independent of HF_MI_HEAP_VISIT (both default off, byte-id preserved).
//
// ABI per brew mimalloc v3.3.1 header (Subprocesses section line 344-358;
// see HyperFLINT development notes (internal) for the local header mirror). The type
// mi_subproc_id_t is just a void*; mi_subproc_main() returns the main
// subproc handle. Heap visitor signature: bool (mi_heap_t*, void*) —
// note non-const heap pointer, unlike the area/block visitor.
//
// Symbol availability (verified via nm at iter-39 Phase 39-α on the
// post-iter-38 baseline binary HyperFLINT/build-mcpu-tuned/hyperflint
// sha256 2102ed05df9349...):
//   _mi_subproc_main         PRESENT  (0x100009a9c)
//   _mi_subproc_visit_heaps  PRESENT  (0x100009f04)
//   _mi_subproc_current      PRESENT  (0x100009aa8)  unused; main preferred
//   _mi_heap_visit_blocks    PRESENT  (0x1000054a0)  already wired
//
// CORRIGENDUM CR-1 (discovered iter-39 Phase 39-α): iter-38's parser/
// aggregate interpreted area->used as bytes. The brew header v3.3.1
// (line 303) documents it as "number of allocated blocks", NOT bytes.
// Cross-bucket sanity confirms: b10_le=1024 with used=15340 / n=663
// areas gives 23 blocks/area (plausible for 1KB-block 64KB pages); a
// bytes interpretation gives 23 B/area (nonsensical). Live bytes per
// bucket = used × block_size. Iter-38's "b10 0.04% live" reads as
// "~38% live" under the corrected interpretation, weakening the
// "Class-1 FLINT-mpoly attribution is speculative" inference. The
// iter-39 cross-theap emission below reports BOTH the raw block count
// (was: misnamed area_used_sum) AND the computed live_bytes via
// used × block_size, so downstream parsers can use either with full
// transparency.
typedef bool (mi_heap_visit_fun)(mi_heap_t* heap, void* arg);
__attribute__((weak)) void* mi_subproc_main(void);
__attribute__((weak)) bool mi_subproc_visit_heaps(void* subproc,
                                                    mi_heap_visit_fun* visitor,
                                                    void* arg);

// iter-41 REC-1 (Option A) abandoned-heap visitor: extension of the
// iter-39 cross-theap walker. The iter-39
// mi_subproc_visit_heaps(mi_subproc_main(), ...) path returned SAME ONE
// HEAP as mi_heap_main() in HF's OMP context (empirically refuted at
// iter-39 §6; ratified by iter-40 BINDING reviewer ae9d8df32f6342372
// disposition CONCERNS_FOLD_4_REQ_3_REC_OPTION_A_AUTHORIZED_WITH_CAVEATS).
// The abandoned-heap path operates on a DIFFERENT registry — mimalloc's
// abandoned-segments pool — which OMP worker theaps populate when the
// worker pthread exits OR when the OMP runtime retires the worker's
// heap (the latter is the open empirical question for iter-41-γ).
//
// Requires mi_option_visit_abandoned=1 set programmatically before the
// visitor call (default is 0). Enum value 28 in mimalloc v3.3.1 ABI
// (counted from mi_option_show_errors=0 in the brew header
// enum mi_option_e starting at line 437; cross-verified at runtime via
// mi_option_get readback).
//
// ABI per brew header v3.3.1:
//   line 312  bool mi_heap_visit_abandoned_blocks(mi_heap_t*, bool,
//                                                  mi_block_visit_fun*, void*)
//   line 502  long mi_option_get(mi_option_t)
//   line 505  void mi_option_set(mi_option_t, long)
//   line 468  mi_option_visit_abandoned  // enum index 28
//
// Symbol availability (verified iter-41-α stand-alone fixture
// nm scan of brew libmimalloc.a v3.3.1; iter-41-β binary
// re-verified post-rebuild via nm):
//   _mi_heap_visit_abandoned_blocks  PRESENT
//   _mi_option_set                   PRESENT  (0x10000b9fc iter-39 binary)
//   _mi_option_get                   PRESENT  (iter-39 binary has
//                                              _mi_option_set_default and
//                                              _mi_option_set_enabled and
//                                              _mi_option_set_enabled_default
//                                              already linked; _mi_option_get
//                                              is in the same TU)
__attribute__((weak)) bool mi_heap_visit_abandoned_blocks(
                                       const mi_heap_t* heap,
                                       bool visit_blocks,
                                       mi_block_visit_fun* visitor,
                                       void* arg);
__attribute__((weak)) void mi_option_set(int option, long value);
__attribute__((weak)) long mi_option_get(int option);

// HF FF Phase 6 §A.M iter-4 Option M.c: per-heap forced collect via the
// mi_subproc_visit_heaps walker. Visitor calls mi_heap_collect(heap, true)
// on every visited heap. Symbol verified PRESENT in brew libmimalloc.a
// 3.3.1 (`nm` precondition at iter-4 cold-start; offset 0x154
// `T _mi_heap_collect`). Prototype per brew header v3.3.1 line 237:
//   void mi_heap_collect(mi_heap_t* heap, bool force);
__attribute__((weak)) void mi_heap_collect(mi_heap_t* heap, bool force);
}

namespace hyperflint {

namespace {

bool mi_stats_enabled() {
    const char* s = HF_FLAG_MI_STATS;
    if (!(s && s[0] && s[0] != '0')) return false;
    return mi_collect != nullptr && mi_process_info != nullptr;
}

void emit_mi_stats(size_t step, const std::string& var_name,
                    const char* phase) {
    if (!mi_process_info) return;
    size_t elapsed_msecs = 0, user_msecs = 0, system_msecs = 0;
    size_t current_rss = 0, peak_rss = 0;
    size_t current_commit = 0, peak_commit = 0, page_faults = 0;
    mi_process_info(&elapsed_msecs, &user_msecs, &system_msecs,
                    &current_rss, &peak_rss,
                    &current_commit, &peak_commit, &page_faults);
    // Note: mimalloc reports current_commit/peak_commit as
    // SIZE_MAX-ish nonsense when the value is "unknown" — print only
    // when it looks sane (< 1 PB) so we don't spew garbage.
    auto sane_mb = [](size_t v) -> double {
        return (v > (size_t(1) << 50)) ? -1.0 : v / 1048576.0;
    };
    std::fprintf(stderr,
        "[mi] step=%zu var=%s phase=%s rss=%.1fMB peak_rss=%.1fMB "
        "commit=%.1fMB peak_commit=%.1fMB page_faults=%zu\n",
        step, var_name.c_str(), phase,
        current_rss / 1048576.0, peak_rss / 1048576.0,
        sane_mb(current_commit), sane_mb(peak_commit),
        page_faults);
    std::fflush(stderr);
}

void run_mi_collect() {
    if (mi_collect) mi_collect(/*force=*/1);
}

// HF FF Phase 6 §A.M iter-3 probe gate. Three modes:
//   • HF_MI_COLLECT_AT_STEP unset → DefaultOn: preserve the pre-iter-3
//     behavior where `HF_MI_STATS=1` unconditionally calls mi_collect(true)
//     at every step boundary (backward-compat for iter-37/41/86/87 baselines).
//   • HF_MI_COLLECT_AT_STEP=0 → ExplicitOff: emit snapshots but skip
//     mi_collect(true) at step boundaries. This is the §A.M activation-gate
//     baseline arm — the no-forced-collect reference RSS.
//   • HF_MI_COLLECT_AT_STEP=1 → ExplicitOn: identical to DefaultOn but
//     explicit; pairs with ExplicitOff for an AB comparison.
// Resolved at every call (cheap getenv on already-throttled probe path);
// no per-process caching, matching the existing HF_MI_STATS pattern.
enum class MiCollectAtStepMode { kDefaultOn, kExplicitOff, kExplicitOn };
MiCollectAtStepMode mi_collect_at_step_mode() {
    const char* s = HF_FLAG_MI_COLLECT_AT_STEP;
    if (!s || !s[0]) return MiCollectAtStepMode::kDefaultOn;
    return (s[0] == '0') ? MiCollectAtStepMode::kExplicitOff
                         : MiCollectAtStepMode::kExplicitOn;
}
bool mi_collect_at_step_enabled() {
    return mi_collect_at_step_mode() != MiCollectAtStepMode::kExplicitOff;
}

// HF FF Phase 6 §A.M iter-3 probe: time mi_collect(true) and emit a
// [mi] collect_cost line for the wall-regression gate (§A.M memo §5).
void run_mi_collect_timed(size_t step, const std::string& var_name,
                            const char* phase) {
    if (!mi_collect) return;
    const auto t0 = std::chrono::steady_clock::now();
    mi_collect(/*force=*/1);
    const auto t1 = std::chrono::steady_clock::now();
    const auto collect_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    std::fprintf(stderr,
        "[mi] step=%zu var=%s phase=%s collect_ns=%lld\n",
        step, var_name.c_str(), phase, static_cast<long long>(collect_ns));
    std::fflush(stderr);
}

// HF FF Phase 6 §A.M iter-4 Option M.c — empirical CONTROL arm per BINDING
// adversarial-reviewer agentId a80aa1876bf43e818 CONCERNS-FOLD REQ-1.b.
//
// Calls mi_heap_collect(heap, true) on every heap visited via
// mi_subproc_visit_heaps(mi_subproc_main(), …). This is the design memo §2.2
// "main + per-worker via mi_subproc_visit_heaps" option, originally proposed
// as a way to reach OMP-worker subproc heaps that Option M.a (main-thread
// mi_collect) cannot.
//
// LOAD-BEARING ITER-39 §6 RECONCILIATION (REQ-1 binding citation):
//
//   notes/hf_finite_field_program/phase4_memory_audit/lever_t3_xcode_harvest/
//   iter39_cross_theap/aggregate_iter39.md §6 EMPIRICALLY REFUTED the design
//   memo §2.2 expectation. In HF's OMP context with OMP_NUM_THREADS=13,
//   `mi_subproc_visit_heaps(mi_subproc_main(), …)` returned theaps_visited=1
//   in 14/14 records on tst2 — i.e. it visits only the main heap
//   (= mi_heap_main()). OMP worker theaps are NOT attached to the main
//   subproc because HF never calls `mi_subproc_add_current_thread(...)`
//   from worker threads (the brew header v3.3.1 line 355 documents the call
//   requires "right after thread creation, before any allocation" ordering
//   that HF cannot guarantee under OMP or libdispatch).
//
//   Consequently, Option M.c implemented here is FUNCTIONALLY EQUIVALENT
//   to Option M.a (both collect only the main heap). It is shipped as an
//   empirical CONTROL arm to confirm — via per-step commit-delta equivalence
//   between Option M.a and Option M.c snapshots (REQ-4.a falsifier) — that
//   the iter-39 §6 instrumented-walk refutation GENERALIZES to active
//   mi_heap_collect. If confirmed, §A.M closes
//   YELLOW/PARTIAL_GATE_STRUCTURALLY_UNREACHABLE and iter-5 fires the §C.e
//   revival hook (Phase 5 §F FAIL_FINAL preservation: FLINT_USES_TLS drop
//   or main-thread cleanup primitive).
//
// OPTIMISTIC CEILING (REQ-2 binding acknowledgement; phantom-optimism
// removed): even a hypothetical "Option M.c'" using
// mi_heap_visit_abandoned_blocks + de-dup + mi_collect_reduce(0) (no public
// API in mimalloc 3.3.1 to collect abandoned heaps directly) — combined
// with Option M.a — has an RSS-reduction upper bound on tst2 max_rss of
// +5.4 percentage points (ratio 0.946), still YELLOW (above 0.90 gate,
// above 0.92 falsifier-NEGATIVE band). NO mi_collect-family lever
// structurally reaches the 10 percentage-point activation gate; the
// kernel-view-RSS ceiling is set by macOS arm64 MADV_FREE_REUSABLE
// lazy-decommit (lessons_learned[10]) + §E operator-memo cache residual.
//
// Env-gated via `HF_MI_COLLECT_OPTION_M_C`:
//   * unset / "0" → Option M.a (delegate to existing run_mi_collect_timed)
//   * "1"         → Option M.c (this function)
// Default OFF preserves iter-3/iter-86/iter-87 baselines bit-for-bit;
// production HF_MI_STATS-unset code path is unchanged.

enum class MiCollectOptionMcMode { kOff, kOn };
MiCollectOptionMcMode mi_collect_option_mc_mode() {
    const char* s = HF_FLAG_MI_COLLECT_OPTION_M_C;
    if (!s || !s[0] || s[0] == '0') return MiCollectOptionMcMode::kOff;
    return MiCollectOptionMcMode::kOn;
}
bool mi_collect_option_mc_enabled() {
    return mi_collect_option_mc_mode() == MiCollectOptionMcMode::kOn
        && mi_subproc_main != nullptr
        && mi_subproc_visit_heaps != nullptr
        && mi_heap_collect != nullptr;
}

// Visitor state for Option M.c. Tracks theaps_visited (load-bearing for the
// iter-39 §6 generalization signal — must be 1 in HF/OMP context per
// REQ-4.a falsifier) and accumulated per-heap mi_heap_collect cost.
struct OptionMcVisitState {
    size_t theaps_visited = 0;
    long long total_collect_ns = 0;
};

bool option_mc_visit_cb(mi_heap_t* heap, void* arg) {
    auto* s = static_cast<OptionMcVisitState*>(arg);
    s->theaps_visited += 1;
    const auto t0 = std::chrono::steady_clock::now();
    mi_heap_collect(heap, /*force=*/true);
    const auto t1 = std::chrono::steady_clock::now();
    s->total_collect_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return true;  // continue subproc traversal (per mimalloc heap-visit-fun
                  // semantics: true = visit next heap; false = stop early).
}

void run_mi_collect_option_mc_timed(size_t step,
                                      const std::string& var_name,
                                      const char* phase) {
    // Caller is expected to gate via mi_collect_option_mc_enabled(); defense
    // in depth here in case a future call site introduces a path that
    // bypasses the env-var check.
    if (!mi_collect_option_mc_enabled()) return;
    void* sp = mi_subproc_main();
    if (!sp) return;
    OptionMcVisitState state;
    const auto t_walk0 = std::chrono::steady_clock::now();
    mi_subproc_visit_heaps(sp, &option_mc_visit_cb, &state);
    const auto t_walk1 = std::chrono::steady_clock::now();
    const auto walk_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t_walk1 - t_walk0).count();
    std::fprintf(stderr,
        "[mi] step=%zu var=%s phase=%s option=Mc theaps_visited=%zu "
        "collect_ns=%lld walk_ns=%lld\n",
        step, var_name.c_str(), phase, state.theaps_visited,
        static_cast<long long>(state.total_collect_ns),
        static_cast<long long>(walk_ns));
    std::fflush(stderr);
}

// Iter-4 dispatch wrapper. Routes the two run_mi_collect_timed call sites
// (main-loop body @ line ~1088 and final-step @ line ~1178) to Option M.a
// (existing run_mi_collect_timed) or Option M.c (new
// run_mi_collect_option_mc_timed) based on env var. Default OFF
// (HF_MI_COLLECT_OPTION_M_C unset) preserves iter-3 bit-identical behavior.
void run_mi_collect_at_step_dispatch(size_t step,
                                       const std::string& var_name,
                                       const char* phase) {
    if (mi_collect_option_mc_enabled()) {
        run_mi_collect_option_mc_timed(step, var_name, phase);
    } else {
        run_mi_collect_timed(step, var_name, phase);
    }
}

// iter-38 REC-1 instrumentation: per-size-class heap-area visitor.
// Bucketed by log2(block_size); 26 buckets cover 1B → 16MB+ overflow.
// Areas with block_size==0 are skipped (mimalloc sentinel).
//
// FOLD-M10 column mapping (Phase 5 plan §A.1):
//   ≤ 256 B            → Class-2 (GMP-backing limb arrays)
//   256 B – 16 KB      → Class-1 (FLINT-mpoly small coefficient buffers)
//   16 KB – 1 MB       → Class-1 (FLINT-mpoly medium working buffers)
//   ≥ 1 MB             → Class-1 or Class-3 (large mpoly OR FLINT-ctx arena)
//
// We pass visit_blocks=false so the visitor receives one callback per
// MIMALLOC AREA (page-aggregated; uniform block_size per area), not per
// block. This is ~10⁴× cheaper than block-level traversal and gives
// the size-class distribution we need for FOLD-M10 mapping.
bool mi_heap_visit_enabled() {
    const char* s = HF_FLAG_MI_HEAP_VISIT;
    if (!(s && s[0] && s[0] != '0')) return false;
    return mi_heap_visit_blocks != nullptr && mi_heap_main != nullptr;
}

struct HfHeapBuckets {
    static constexpr size_t N = 26;
    size_t total_used[N] = {0};
    size_t total_committed[N] = {0};
    size_t total_reserved[N] = {0};
    size_t area_count[N] = {0};
    size_t area_reserved_sum = 0;
    size_t area_committed_sum = 0;
    size_t area_used_sum = 0;
    // iter-43-β REC-2 disjoint-audit: when non-null, the callback hashes
    // each visited `area->blocks` pointer into this set. Caller decides
    // which set to bind (main vs abandoned) and reports the intersection
    // after both visitors have run at the same step+phase.
    std::unordered_set<const void*>* audit_set = nullptr;
};

bool hf_heap_visit_cb(const mi_heap_t* /*heap*/,
                       const mi_heap_area_t* area,
                       void* /*block*/, size_t /*block_size*/,
                       void* arg) {
    auto* b = static_cast<HfHeapBuckets*>(arg);
    if (b->audit_set && area->blocks) {
        b->audit_set->insert(area->blocks);
    }
    b->area_reserved_sum  += area->reserved;
    b->area_committed_sum += area->committed;
    b->area_used_sum      += area->used;
    const size_t sz = area->block_size;
    if (sz == 0) return true;  // skip sentinel areas
    size_t bucket = 0;
    while ((size_t(1) << bucket) < sz && bucket < HfHeapBuckets::N - 1) {
        ++bucket;
    }
    b->total_used[bucket]      += area->used;
    b->total_committed[bucket] += area->committed;
    b->total_reserved[bucket]  += area->reserved;
    b->area_count[bucket]      += 1;
    return true;  // continue traversal
}

// iter-43-β REC-2 area-pointer disjoint-audit. When
// HF_MI_HEAP_VISIT_DISJOINT_AUDIT=1 (default off) is set in addition to
// HF_MI_HEAP_VISIT=1 + HF_MI_HEAP_VISIT_ABANDONED=1, the main-heap and
// abandoned-heap visitors each populate a thread-local set of visited
// `area->blocks` pointers via the HfHeapBuckets::audit_set hook.
// emit_heap_visit_disjoint_audit_intersect() (defined below) reports
// the intersection count and clears both sets, called after a matched
// main+abandoned emit pair at the same step+phase. intersect=0 across
// all records firms up the §3 disjointness claim that the two visitors
// report disjoint memory regions; intersect>0 falsifies it and is a
// load-bearing pre-disposition for the §5 sum-coverage interpretation.
//
// Hashing the `area->blocks` start-of-region pointer (not the `area`
// snapshot pointer itself, which mimalloc may reuse across callbacks)
// is the load-bearing identity for region equality. Per the locally-
// mirrored mimalloc v3.3.1 header struct mi_heap_area_s the `blocks`
// field is a `void*` at offset 0 (line 71-79 above).
thread_local std::unordered_set<const void*> g_main_areas_seen;
thread_local std::unordered_set<const void*> g_abandoned_areas_seen;

bool mi_heap_visit_disjoint_audit_enabled() {
    const char* s = HF_FLAG_MI_HEAP_VISIT_DISJOINT_AUDIT;
    if (!(s && s[0] && s[0] != '0')) return false;
    return mi_heap_visit_blocks != nullptr
        && mi_heap_visit_abandoned_blocks != nullptr;
}

void emit_heap_visit_buckets(size_t step, const std::string& var_name,
                              const char* phase) {
    if (!mi_heap_visit_blocks) return;
    const mi_heap_t* h = nullptr;
    if (mi_heap_main) h = mi_heap_main();
    if (!h) return;
    HfHeapBuckets bk;
    if (mi_heap_visit_disjoint_audit_enabled()) {
        bk.audit_set = &g_main_areas_seen;
    }
    mi_heap_visit_blocks(h, /*visit_blocks=*/false,
                         &hf_heap_visit_cb, &bk);
    std::fprintf(stderr,
        "[mi_heap_visit] step=%zu var=%s phase=%s "
        "area_reserved=%zu area_committed=%zu area_used=%zu",
        step, var_name.c_str(), phase,
        bk.area_reserved_sum, bk.area_committed_sum, bk.area_used_sum);
    for (size_t i = 0; i < HfHeapBuckets::N; ++i) {
        if (bk.area_count[i] == 0) continue;
        std::fprintf(stderr,
            " b%zu_le=%zu(used=%zu,cmt=%zu,rsv=%zu,n=%zu)",
            i, size_t(1) << i,
            bk.total_used[i], bk.total_committed[i],
            bk.total_reserved[i], bk.area_count[i]);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

// iter-39 cross-theap walker (Phase 39-β). Iterates ALL heaps in the
// main subproc and accumulates the same per-size-class buckets across
// all theaps. Output line tag is `[mi_heap_visit_all_theaps]` (the
// iter-38 line tag `[mi_heap_visit]` stays main-heap-only). Emits both
// raw block counts (the `used` field per mimalloc ABI) AND computed
// `live_bytes` per CR-1 corrigendum (Σ used × block_size). Downstream
// parsers can read either; the iter-38 parser_heap_visit.py continues
// to apply to the unchanged [mi_heap_visit] line.
bool mi_heap_visit_all_enabled() {
    const char* s = HF_FLAG_MI_HEAP_VISIT_ALL;
    if (!(s && s[0] && s[0] != '0')) return false;
    return mi_subproc_main != nullptr
        && mi_subproc_visit_heaps != nullptr
        && mi_heap_visit_blocks != nullptr;
}

struct HfHeapCrossState {
    HfHeapBuckets buckets;
    size_t theaps_visited = 0;
};

// Sub-callback invoked once per heap visited by mi_subproc_visit_heaps.
// For each heap, dispatches the existing area visitor to fill the
// SHARED bucket accumulator. The non-const heap pointer decays to const
// at the call boundary into mi_heap_visit_blocks (which declares const).
bool hf_subproc_heap_visit_cb(mi_heap_t* heap, void* arg) {
    auto* s = static_cast<HfHeapCrossState*>(arg);
    s->theaps_visited += 1;
    mi_heap_visit_blocks(heap, /*visit_blocks=*/false,
                         &hf_heap_visit_cb, &s->buckets);
    return true;  // continue subproc traversal
}

void emit_heap_visit_buckets_all_theaps(size_t step,
                                          const std::string& var_name,
                                          const char* phase) {
    if (!mi_subproc_main || !mi_subproc_visit_heaps
        || !mi_heap_visit_blocks) return;
    void* sp = mi_subproc_main();
    if (!sp) return;
    HfHeapCrossState s;
    mi_subproc_visit_heaps(sp, &hf_subproc_heap_visit_cb, &s);
    // Compute live_bytes per CR-1 corrigendum (Σ used × block_size).
    // Bucket label i corresponds to ceil(log2(block_size)); we use
    // (1 << i) as the bucket's upper bound on block_size, which
    // OVER-ESTIMATES bytes_live for non-power-of-2 actual block sizes
    // in the bucket. Compare against the per-bucket cmt field for a
    // sanity check (bytes_live should be ≤ cmt in every bucket).
    size_t live_bytes_total = 0;
    for (size_t i = 0; i < HfHeapBuckets::N; ++i) {
        live_bytes_total += s.buckets.total_used[i] * (size_t(1) << i);
    }
    std::fprintf(stderr,
        "[mi_heap_visit_all_theaps] step=%zu var=%s phase=%s "
        "theaps=%zu area_reserved=%zu area_committed=%zu "
        "block_count_total=%zu live_bytes_total=%zu",
        step, var_name.c_str(), phase,
        s.theaps_visited,
        s.buckets.area_reserved_sum,
        s.buckets.area_committed_sum,
        s.buckets.area_used_sum,
        live_bytes_total);
    for (size_t i = 0; i < HfHeapBuckets::N; ++i) {
        if (s.buckets.area_count[i] == 0) continue;
        const size_t bsz = size_t(1) << i;
        std::fprintf(stderr,
            " b%zu_le=%zu(blocks=%zu,live_B=%zu,cmt=%zu,rsv=%zu,n=%zu)",
            i, bsz,
            s.buckets.total_used[i],
            s.buckets.total_used[i] * bsz,
            s.buckets.total_committed[i],
            s.buckets.total_reserved[i],
            s.buckets.area_count[i]);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

// iter-41 REC-1 (Option A; iter-40 BINDING reviewer ae9d8df32f6342372
// REQ-2 ratified) abandoned-heap emitter. Mirrors
// emit_heap_visit_buckets_all_theaps but uses
// mi_heap_visit_abandoned_blocks(mi_heap_main(), ...) instead of
// iterating mi_subproc_visit_heaps(mi_subproc_main(), ...). Line tag is
// [mi_heap_visit_abandoned] to disambiguate from iter-38
// [mi_heap_visit] (main-heap-only) and iter-39
// [mi_heap_visit_all_theaps]. Default-off; gated by
// HF_MI_HEAP_VISIT_ABANDONED=1 env-var AND symbol availability AND
// successful mi_option_visit_abandoned=1 setup (mi_option_get readback
// must confirm).
//
// CR-1 corrigendum (iter-39): area->used is "number of allocated blocks"
// per brew header v3.3.1 line 303. live_bytes per bucket = used ×
// block_size; reported alongside raw block counts so downstream parsers
// can use either with full transparency.
constexpr int kMimallocOptionVisitAbandoned_v3p3p1 = 28;

bool mi_heap_visit_abandoned_enabled() {
    const char* s = HF_FLAG_MI_HEAP_VISIT_ABANDONED;
    if (!(s && s[0] && s[0] != '0')) return false;
    return mi_heap_visit_abandoned_blocks != nullptr
        && mi_heap_main != nullptr
        && mi_option_set != nullptr;
}

// Programmatically enable the abandoned-block visitor by setting
// mi_option_visit_abandoned=1 (default is 0; without this set, the
// visitor will silently report zero blocks). Idempotent and once-flag-
// guarded. Returns true iff the option was confirmed set via
// mi_option_get readback (or if mi_option_get is unavailable but
// mi_option_set is — assumed-success fallback for that case);
// returns false on enum-value mismatch or symbol absence, in which case
// callers MUST NOT invoke mi_heap_visit_abandoned_blocks (the visitor
// will report zeros and pollute the bucket aggregation).
bool ensure_mi_visit_abandoned_enabled() {
    static bool tried = false;
    static bool ok = false;
    if (tried) return ok;
    tried = true;
    if (!mi_option_set) return false;
    mi_option_set(kMimallocOptionVisitAbandoned_v3p3p1, 1);
    if (mi_option_get != nullptr) {
        const long v = mi_option_get(kMimallocOptionVisitAbandoned_v3p3p1);
        if (v != 1) {
            std::fprintf(stderr,
                "[mi_heap_visit_abandoned] WARN "
                "mi_option_get(enum=%d) returned %ld (expected 1); "
                "abandoned visitor disabled (ABI drift?)\n",
                kMimallocOptionVisitAbandoned_v3p3p1, v);
            std::fflush(stderr);
            return false;
        }
    }
    ok = true;
    return true;
}

void emit_heap_visit_buckets_abandoned(size_t step,
                                        const std::string& var_name,
                                        const char* phase) {
    if (!mi_heap_visit_abandoned_blocks || !mi_heap_main) return;
    if (!ensure_mi_visit_abandoned_enabled()) return;
    const mi_heap_t* h = mi_heap_main();
    if (!h) return;
    HfHeapBuckets bk;
    if (mi_heap_visit_disjoint_audit_enabled()) {
        bk.audit_set = &g_abandoned_areas_seen;
    }
    mi_heap_visit_abandoned_blocks(h, /*visit_blocks=*/false,
                                    &hf_heap_visit_cb, &bk);
    size_t live_bytes_total = 0;
    for (size_t i = 0; i < HfHeapBuckets::N; ++i) {
        live_bytes_total += bk.total_used[i] * (size_t(1) << i);
    }
    std::fprintf(stderr,
        "[mi_heap_visit_abandoned] step=%zu var=%s phase=%s "
        "area_reserved=%zu area_committed=%zu "
        "block_count_total=%zu live_bytes_total=%zu",
        step, var_name.c_str(), phase,
        bk.area_reserved_sum, bk.area_committed_sum,
        bk.area_used_sum, live_bytes_total);
    for (size_t i = 0; i < HfHeapBuckets::N; ++i) {
        if (bk.area_count[i] == 0) continue;
        const size_t bsz = size_t(1) << i;
        std::fprintf(stderr,
            " b%zu_le=%zu(blocks=%zu,live_B=%zu,cmt=%zu,rsv=%zu,n=%zu)",
            i, bsz,
            bk.total_used[i],
            bk.total_used[i] * bsz,
            bk.total_committed[i],
            bk.total_reserved[i],
            bk.area_count[i]);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

// iter-43-β REC-2 disjoint-audit intersection report. Called after a
// matched main+abandoned emit pair at the same step+phase; reports
// |main ∩ abandoned| and clears both TLS sets. See declaration of
// g_main_areas_seen / g_abandoned_areas_seen above (before
// emit_heap_visit_buckets) for context.
void emit_heap_visit_disjoint_audit_intersect(size_t step,
                                                const std::string& var_name,
                                                const char* phase) {
    size_t intersect = 0;
    for (const void* p : g_main_areas_seen) {
        if (g_abandoned_areas_seen.count(p)) ++intersect;
    }
    std::fprintf(stderr,
        "[mi_heap_visit_disjoint_audit] step=%zu var=%s phase=%s "
        "main_areas=%zu abandoned_areas=%zu intersect=%zu\n",
        step, var_name.c_str(), phase,
        g_main_areas_seen.size(), g_abandoned_areas_seen.size(),
        intersect);
    std::fflush(stderr);
    g_main_areas_seen.clear();
    g_abandoned_areas_seen.clear();
}

// 2026-05-05 (path-A diagnostic): per-step probe writing JSON to stderr
// when HF_PROBE_PARITY1=1 in env. Walks the persistent ShuffleListSym
// `current` carried between integration_step calls, plus the LF cache
// residency. Reports total bytes, distinct-Poly bytes (DAG-share LB
// via poly_struct_hash_raw), narrow- vs wide-ctx residency split.
//
// Sample point: post-step assignment in the hyperflint_sym outer loop
// (immediately after `current = regulator_sym_as_shuffle_list_sym(...)`).
// Walks the persistent state — does NOT walk thread-local OMP slots,
// in-flight stack frames, or transient FLINT allocations. The walked
// state is the dominant residency contributor on parity-1 (the H2/H3
// audits showed queue + bump_addto residency is <100 MB).
bool parity1_probe_enabled() {
    const char* s = HF_FLAG_PROBE_PARITY1;
    return s && s[0] && s[0] != '0';
}

void emit_parity1_probe(size_t step,
                         const std::string& var_name,
                         const ShuffleListSym& current) {
    if (!parity1_probe_enabled()) return;

    // Walk every Poly through the SymCoef's monomial prefactors.
    size_t total_bytes      = 0;
    size_t distinct_bytes   = 0;
    size_t poly_count       = 0;
    size_t entry_count      = current.size();
    size_t monomial_count   = 0;
    std::map<const PolyCtx*, size_t> bytes_by_ctx;
    std::map<const PolyCtx*, size_t> count_by_ctx;
    std::unordered_set<std::pair<uint64_t, uint64_t>, PairU64Hash> seen;

    auto visit_poly = [&](const Poly& p) {
        poly_count += 1;
        const size_t b = p.total_bytes();
        total_bytes += b;
        bytes_by_ctx[&p.ctx()] += b;
        count_by_ctx[&p.ctx()] += 1;
        // poly_struct_hash_raw — content-only hash (NOT the LF cache key).
        auto [h1, h2] = poly_struct_hash_seed();
        poly_struct_hash_raw(h1, h2, p);
        if (seen.insert({h1, h2}).second) {
            distinct_bytes += b;
        }
    };
    for (const auto& e : current) {
        for (const auto& mon : e.coef.terms()) {
            monomial_count += 1;
            visit_poly(mon.prefactor.num());
            visit_poly(mon.prefactor.den());
        }
    }
    const size_t shared_bytes = total_bytes - distinct_bytes;
    LFCacheResidency lfr = linear_factors_cache_residency();

    // Identify largest ctx by bytes (likely the wide one). Output the
    // top-2 ctxs so the report can do the narrow-vs-wide split.
    struct CtxRow { const PolyCtx* p; size_t bytes; size_t cnt; size_t nvars; };
    std::vector<CtxRow> ctxs;
    for (const auto& kv : bytes_by_ctx) {
        ctxs.push_back({kv.first, kv.second, count_by_ctx[kv.first],
                        kv.first ? kv.first->vars().size() : 0});
    }
    std::sort(ctxs.begin(), ctxs.end(),
              [](const CtxRow& a, const CtxRow& b){ return a.bytes > b.bytes; });

    std::fprintf(stderr,
        "{\"hf_parity1_probe\":true"
        ",\"step\":%zu"
        ",\"var\":\"%s\""
        ",\"entries\":%zu"
        ",\"monomials\":%zu"
        ",\"polys\":%zu"
        ",\"total_bytes\":%zu"
        ",\"distinct_bytes\":%zu"
        ",\"shared_bytes\":%zu"
        ",\"dag_share_LB_bytes\":%zu"
        ",\"lf_cache_entries\":%zu"
        ",\"lf_cache_total_bytes\":%zu"
        ",\"lf_cache_polys\":%zu"
        ",\"lf_cache_nonlin_bytes\":%zu"
        ",\"lf_cache_pole_rats_bytes\":%zu"
        ",\"n_distinct_ctxs\":%zu",
        step, var_name.c_str(),
        entry_count, monomial_count, poly_count,
        total_bytes, distinct_bytes, shared_bytes, shared_bytes,
        lfr.entry_count, lfr.total_bytes, lfr.poly_count,
        lfr.nonlin_polys, lfr.pole_rats,
        ctxs.size());
    // Emit top-2 ctxs.
    for (size_t i = 0; i < ctxs.size() && i < 2; ++i) {
        std::fprintf(stderr,
            ",\"ctx%zu_nvars\":%zu"
            ",\"ctx%zu_bytes\":%zu"
            ",\"ctx%zu_polys\":%zu",
            i, ctxs[i].nvars,
            i, ctxs[i].bytes,
            i, ctxs[i].cnt);
    }
    std::fprintf(stderr, "}\n");
    std::fflush(stderr);
}

// SymCoef-preserving conversion: RegulatorSym → ShuffleListSym.
// Mirrors the 1-for-1 mapping of RegTermSym → ShuffleEntrySym
// (each RegKey is a list of words, exactly the shape ShuffleEntry
// wants). Used between intermediate integration steps in
// `hyperflint_sym` so that I*Pi*delta[var] residues from Fragment
// P2's positive-letter closure survive into the next step.
// Take by rvalue ref + move per-element coef/key into the output.
// Saves the duplicate residency that previously held both `r` and
// `out` alive simultaneously inside the function body — the
// regulator's coefs are large SymCoefs on heavy fixtures (~525 MB
// per side on Smirnov tst3), so the pre-move copy doubled the
// transient peak.
ShuffleListSym regulator_sym_as_shuffle_list_sym(RegulatorSym&& r) {
    ShuffleListSym out;
    out.reserve(r.size());
    for (auto& t : r) {
        out.push_back(ShuffleEntrySym{std::move(t.coef), std::move(t.key)});
    }
    return out;
}

// Reinterpret a ShuffleList as a RegulatorSym (used only when
// var_indices is empty — we return the input in RegulatorSym form so
// callers get a consistent output type).
RegulatorSym shuffle_list_as_regulator_sym(const ShuffleList& s) {
    RegulatorSym out;
    out.reserve(s.size());
    for (const auto& e : s) {
        out.push_back(RegTermSym{SymCoef::from_rat(e.coef), e.shuffle});
    }
    return out;
}

}  // namespace

// Phase 6d-v-iv driver. Bug #6 lift: every step produces a
// RegulatorSym; SymCoef-valued intermediates flow through subsequent
// steps as outer scalar factors on their contributions (see the
// SymCoef overload of `integration_step`). This is needed for
// integrands (e.g. Smirnov tst1) whose Fragment P2 positive-letter
// closure fires at a non-final step: residues like I*Pi*delta[var]
// must survive into the remaining integrations.
namespace {

// Diagnostic: per-step state-size metrics emitted to stderr when
// HF_STEP_TRACE is set in the environment. Lines are JSON for easy
// post-processing. Added for the Phase-A HF/Maple tst2-gap
// investigation (docs/hf_vs_hyperint_tst2_diagnostic.md). iter-85 §T7
// fifteenth chunk: the local uncached `step_trace_enabled()` duplicate
// of the canonical gate (`hyperflint::step_trace_enabled` from
// `runtime/trace_gate.hpp`) was deleted; unqualified
// `step_trace_enabled()` at L1128 now binds to the canonical cached
// (magic-statics) inline gate.

// ShuffleListSym metrics: (n_entries, total_words, total_letters,
// total_monomials_in_coefs).
struct InputMetrics {
    size_t n_entries = 0;
    size_t total_words = 0;
    size_t total_letters = 0;
    size_t total_monomials = 0;
};

InputMetrics measure(const ShuffleListSym& input) {
    InputMetrics m;
    m.n_entries = input.size();
    for (const auto& e : input) {
        m.total_words += e.shuffle.size();
        for (const auto& w : e.shuffle) m.total_letters += w.size();
        m.total_monomials += e.coef.terms().size();
    }
    return m;
}

struct OutputMetrics {
    size_t n_terms = 0;       // # distinct RegKeys
    size_t total_words = 0;   // sum of words across all keys
    size_t total_letters = 0; // sum of letters across all key words
    size_t total_monomials = 0; // sum of coef monomials across all terms
};

OutputMetrics measure(const RegulatorSym& out) {
    OutputMetrics m;
    m.n_terms = out.size();
    for (const auto& t : out) {
        m.total_words += t.key.size();
        for (const auto& w : t.key) m.total_letters += w.size();
        m.total_monomials += t.coef.terms().size();
    }
    return m;
}

void emit_step_trace(size_t step, const std::string& var_name,
                      const InputMetrics& in, const OutputMetrics& out,
                      double wall_s, bool include_sub_timers) {
    std::cerr << "{\"hf_step_trace\":true"
              << ",\"step\":" << step
              << ",\"var\":\"" << var_name << "\""
              << ",\"in_entries\":" << in.n_entries
              << ",\"in_words\":" << in.total_words
              << ",\"in_letters\":" << in.total_letters
              << ",\"in_monomials\":" << in.total_monomials
              << ",\"out_terms\":" << out.n_terms
              << ",\"out_words\":" << out.total_words
              << ",\"out_letters\":" << out.total_letters
              << ",\"out_monomials\":" << out.total_monomials
              << ",\"wall_s\":" << wall_s
              << ",\"istep_canon_s\":" << read_integration_step_canon_s()
              << ",\"transform_shuffle_s\":" << read_transform_shuffle_s()
              << ",\"integrate_ii_s\":"      << read_integrate_ii_s()
              << ",\"loop_residual_s\":"     << read_loop_residual_s()
              << ",\"partial_fractions_s\":" << read_partial_fractions_s()
              << ",\"linear_factors_s\":"    << read_linear_factors_s()
              << ",\"lf_flint_factor_s\":"   << read_lf_flint_factor_s()
              << ",\"lf_cache_hits\":"       << read_lf_cache_hits()
              << ",\"lf_cache_misses\":"     << read_lf_cache_misses()
              << ",\"lf_flint_deg1_s\":"     << read_lf_flint_deg1_s()
              << ",\"lf_flint_deg2_s\":"     << read_lf_flint_deg2_s()
              << ",\"lf_miss_deg1\":"        << read_lf_miss_deg1()
              << ",\"lf_miss_deg2\":"        << read_lf_miss_deg2()
              << ",\"lf_d3p_all_linear_count\":"   << read_lf_d3p_all_linear_count()
              << ",\"lf_d3p_all_linear_s\":"       << read_lf_d3p_all_linear_s()
              << ",\"lf_d3p_has_nonlinear_count\":"<< read_lf_d3p_has_nonlinear_count()
              << ",\"lf_d3p_has_nonlinear_s\":"    << read_lf_d3p_has_nonlinear_s()
              << ",\"lf_d3p_squarefree_count\":"   << read_lf_d3p_squarefree_count()
              << ",\"lf_d3p_squarefree_s\":"       << read_lf_d3p_squarefree_s()
              << ",\"lf_d3p_repeated_count\":"     << read_lf_d3p_repeated_count()
              << ",\"lf_d3p_repeated_s\":"         << read_lf_d3p_repeated_s()
              << ",\"lf_sqf_total_s\":"            << read_lf_sqf_total_s()
              << ",\"lf_sqf_decomp_s\":"           << read_lf_sqf_decomp_s()
              << ",\"lf_sqf_inner_factor_s\":"     << read_lf_sqf_inner_factor_s()
              << ",\"lf_sqf_calls\":"              << read_lf_sqf_calls()
              << ",\"lf_sqf_inner_factor_calls\":" << read_lf_sqf_inner_factor_calls()
              << ",\"lf_sqf_bailouts\":"           << read_lf_sqf_bailouts()
              << ",\"bump_lookup_s\":"             << read_bump_lookup_s()
              << ",\"bump_addto_s\":"              << read_bump_addto_s()
              << ",\"push_ibp_s\":"                << read_push_ibp_s()
              << ",\"antideriv_s\":"               << read_antideriv_s()
              << ",\"bump_calls\":"                << read_bump_calls()
              << ",\"bump_emplace_s\":"            << read_bump_emplace_s()
              << ",\"bump_rat_add_s\":"            << read_bump_rat_add_s()
              << ",\"bump_rat_add_calls\":"        << read_bump_rat_add_calls()
              << ",\"pf_calls_in_loop\":"          << read_pf_calls_in_loop()
              << ",\"pf_unique_dens\":"            << read_pf_unique_dens()
              << ",\"bump_unique_rows\":"          << read_bump_unique_rows()
              << ",\"lf_lock_held_s\":"            << read_lf_lock_held_s()
              << ",\"lf_lock_wait_s\":"            << read_lf_lock_wait_s()
              << ",\"lf_cache_key_build_s\":"      << read_lf_cache_key_build_s()
              << ",\"lf_post_transplant_s\":"        << read_lf_post_transplant_s()
              << ",\"lf_post_rat_ctor_s\":"          << read_lf_post_rat_ctor_s()
              << ",\"lf_post_constant_to_string_s\":"<< read_lf_post_constant_to_string_s()
              << ",\"lf_post_clone_from_raw_s\":"    << read_lf_post_clone_from_raw_s()
              << ",\"ii_queue_copy_s\":"           << read_ii_queue_copy_s()
              << ",\"ii_pole_arith_s\":"           << read_ii_pole_arith_s()
              << ",\"ii_pole_word_ctor_s\":"       << read_ii_pole_word_ctor_s()
              << ",\"omp_collect_into_flat_s\":"     << read_omp_collect_into_flat_s()
              << ",\"omp_merge_sort_s\":"            << read_omp_merge_sort_s()
              << ",\"omp_parallel_canonicalize_s\":" << read_omp_parallel_canonicalize_s()
              << ",\"omp_serial_assembly_s\":"       << read_omp_serial_assembly_s()
              << ",\"reduce_narrow_s\":"           << read_reduce_narrow_s()
              << ",\"reduce_wide_s\":"             << read_reduce_wide_s()
              << ",\"reduce_narrow_calls\":"       << read_reduce_narrow_calls()
              << ",\"reduce_wide_calls\":"         << read_reduce_wide_calls()
              << ",\"reduce_zero_calls\":"         << read_reduce_zero_calls()
              << ",\"gcd_cofactors_s\":"           << read_gcd_cofactors_s()
              << ",\"gcd_cofactors_calls\":"       << read_gcd_cofactors_calls()
              << ",\"rn_used_vars_s\":"            << read_rn_used_vars_s()
              << ",\"rn_setup_s\":"                << read_rn_setup_s()
              << ",\"rn_post_s\":"                 << read_rn_post_s()
              << ",\"rat_mul_calls\":"             << read_rat_mul_calls()
              << ",\"rat_sub_calls\":"             << read_rat_sub_calls()
              << ",\"rat_div_calls\":"             << read_rat_div_calls()
              << ",\"rat_add_polymul_s\":"         << read_rat_add_polymul_s()
              << ",\"rat_add_polyadd_s\":"         << read_rat_add_polyadd_s()
              << ",\"rat_add_calls\":"             << read_rat_add_calls()
              << ",\"rat_add_legacy_wall_s\":"     << read_rat_add_legacy_wall_s()
              << ",\"rat_add_via_qu_wall_s\":"     << read_rat_add_via_qu_wall_s()
              << ",\"rat_add_legacy_calls\":"      << read_rat_add_legacy_calls()
              << ",\"rat_add_via_qu_calls\":"      << read_rat_add_via_qu_calls()
              << ",\"mul_narrow_s\":"              << read_mul_narrow_s()
              << ",\"mul_wide_s\":"                << read_mul_wide_s()
              << ",\"mul_narrow_calls\":"          << read_mul_narrow_calls()
              << ",\"mul_wide_calls\":"            << read_mul_wide_calls()
              << ",\"mul_gated_calls\":"           << read_mul_gated_calls()
              << ",\"nbin_lalb_count\":["
                << read_nbin_lalb_count(0) << "," << read_nbin_lalb_count(1) << ","
                << read_nbin_lalb_count(2) << "," << read_nbin_lalb_count(3) << ","
                << read_nbin_lalb_count(4) << "," << read_nbin_lalb_count(5) << "]"
              << ",\"nbin_lalb_us\":["
                << read_nbin_lalb_us(0) << "," << read_nbin_lalb_us(1) << ","
                << read_nbin_lalb_us(2) << "," << read_nbin_lalb_us(3) << ","
                << read_nbin_lalb_us(4) << "," << read_nbin_lalb_us(5) << "]"
              << ",\"nbin_lalb_max\":["
                << read_nbin_lalb_max(0) << "," << read_nbin_lalb_max(1) << ","
                << read_nbin_lalb_max(2) << "," << read_nbin_lalb_max(3) << ","
                << read_nbin_lalb_max(4) << "," << read_nbin_lalb_max(5) << "]"
              << ",\"nbin_u_count\":["
                << read_nbin_u_count(0) << "," << read_nbin_u_count(1) << ","
                << read_nbin_u_count(2) << "," << read_nbin_u_count(3) << ","
                << read_nbin_u_count(4) << "]"
              << ",\"nbin_u_us\":["
                << read_nbin_u_us(0) << "," << read_nbin_u_us(1) << ","
                << read_nbin_u_us(2) << "," << read_nbin_u_us(3) << ","
                << read_nbin_u_us(4) << "]"
              << ",\"bucket_canon_regkey_s\":"     << read_bucket_canon_regkey_s()
              << ",\"bucket_struct_hash_s\":"      << read_bucket_struct_hash_s()
              << ",\"bucket_index_find_s\":"       << read_bucket_index_find_s()
              << ",\"bucket_symcoef_add_s\":"      << read_bucket_symcoef_add_s()
              << ",\"bucket_emplace_s\":"          << read_bucket_emplace_s()
              << ",\"bucket_collision_calls\":"      << read_bucket_collision_calls()
              << ",\"bucket_collision_pre_terms\":"  << read_bucket_collision_pre_terms()
              << ",\"bucket_collision_post_terms\":" << read_bucket_collision_post_terms()
              << ",\"merge_in_terms\":"            << read_merge_in_terms()
              << ",\"merge_out_terms\":"           << read_merge_out_terms()
              << ",\"merge_slots\":"               << read_merge_slots()
              << ",\"pole_zero_expand_s\":"        << read_pole_zero_expand_s()
              << ",\"pole_inf_expand_s\":"         << read_pole_inf_expand_s()
              << ",\"reduce_nterm_calls\":"      << read_reduce_nterm_calls()
              << ",\"reduce_nterm_pre_total\":"  << read_reduce_nterm_pre_total()
              << ",\"reduce_nterm_post_total\":" << read_reduce_nterm_post_total()
              << ",\"reduce_nterm_pre_max\":"    << read_reduce_nterm_pre_max()
              << ",\"reduce_nterm_post_max\":"   << read_reduce_nterm_post_max()
              << ",\"reduce_wide_smallfall_calls\":" << read_reduce_wide_smallfall_calls()
              << ",\"pie_substitute_var_reciprocal_s\":" << read_pie_substitute_var_reciprocal_s()
              << ",\"pie_series_expansion_s\":"          << read_pie_series_expansion_s()
              << ",\"pie_expand_inf_word_in_ctx_s\":"    << read_pie_expand_inf_word_in_ctx_s()
              << ",\"pie_rat_var0_coef_s\":"             << read_pie_rat_var0_coef_s()
              << ",\"bucket_bump_s\":"             << read_bucket_bump_s()
              << ",\"omp_parallel_wall_s\":"       << read_omp_parallel_wall_s()
              << ",\"omp_post_merge_s\":"          << read_omp_post_merge_s()
              << ",\"entry_max_per_thread_s\":"    << read_entry_max_per_thread_s()
              << ",\"entry_min_per_thread_s\":"    << read_entry_min_per_thread_s()
              << ",\"pm_canon_max_per_thread_s\":" << read_pm_canon_max_per_thread_s()
              << ",\"pm_canon_min_per_thread_s\":" << read_pm_canon_min_per_thread_s()
              << ",\"pm_canon_sum_per_thread_s\":" << read_pm_canon_sum_per_thread_s()
              << ",\"pf_cache_hits\":"             << read_pf_cache_hits_step()
              << ",\"pf_cache_misses\":"           << read_pf_cache_misses_step()
              << ",\"pf_cache_collisions\":"       << read_pf_cache_collisions_step();
    if (include_sub_timers) {
        std::cerr
            << ",\"closure_body_s\":" << read_closure_body_s()
            << ",\"closure_canon_s\":" << read_closure_canon_s();
    }
    // Phase 0 task 0-3: sample RSS at every step-trace emit so downstream
    // analyzers can correlate memory footprint with step index and integrand size.
    // Placed last so the ps(1) subprocess fork does not perturb any earlier timer reads.
    {
        auto rss = hyperflint::sample_rss();
        std::cerr << ",\"rss_current_kib\":" << rss.current_kib
                  << ",\"rss_peak_kib\":"    << rss.peak_kib;
    }
    std::cerr << "}\n";
    std::cerr.flush();  // crash-resilience: ensure each JSONL line reaches the reader
}

}  // namespace

RegulatorSym hyperflint_sym(const PolyCtx& ctx,
                              const ShuffleList& input,
                              const std::vector<size_t>& var_indices,
                              const MzvReductionTable& table,
                              bool introduce_algebraic_letters,
                              bool check_divergences,
                              const std::vector<size_t>& spectator_var_indices) {
    // Phase-B B7 (design v2 §3.5a row 7 + §4.2 commit (7)). Two
    // driver-level adapter sites: (entry) split user input -- per-Rat
    // round-trip through SymCoef::from_rat -> SymCoefSplit::from_rat ->
    // SymCoefSplit::as_rat -> SymCoef::as_rat on every input ShuffleList
    // coef before SymCoef-promotion; (exit) reconstitute output --
    // per-RegulatorSym round-trip through the same chain on every
    // SymMonomial-prefactor of the returned RegulatorSym (and the early-
    // return RegulatorSym when var_indices is empty). Under
    // runtime::scalar_rep_enabled() (the HF_USE_SCALAR_REP env-gate),
    // both adapters perform the round-trip via the shared helpers
    // runtime::roundtrip_rat_through_scs / roundtrip_regulator_through_scs.
    // At B-stages on Smirnov, the W-side variable set is empty by
    // hypothesis (b1_scoping_memo.md R2; design v2 §4.4a Note 2), so
    // both round-trips are canonically no-ops and the data is byte-exact
    // preserved. Mirrors the B1.c / B2 / B3 / B4 / B5 / B6.b dispatch
    // pattern.
    //
    // Iter-41 C0a partial (sites 8+9 only): the per-call ZWTable
    // allocations that previously lived inside each lambda body have
    // been hoisted to a single allocation at driver-entry. Both adapter
    // lambdas now capture this single zw_tab by reference. Sites 1-7
    // (lambdas in transform.cpp / break_up_contour.cpp / linear_factors.cpp /
    // primitive.cpp / integration_step.cpp) require an API thread-through
    // and are deferred to iter-42+ (sites 1, 4) and iter-45+ (sites 6, 7
    // C0c.1 atomic). Allocation guarded by scalar_rep_enabled() so that
    // default-OFF behavior is byte-identical to pre-C0a (no allocation
    // happens when the env-gate is unset).
    std::shared_ptr<ZWTable> zw_tab;
    if (runtime::scalar_rep_enabled()) {
        zw_tab = std::make_shared<ZWTable>(ctx);
    }
    auto apply_v1_roundtrip_rat = [&](const Rat& coef,
                                        const char* tag) -> Rat {
        if (!runtime::scalar_rep_enabled()) return coef;
        return runtime::roundtrip_rat_through_scs(coef, ctx, zw_tab, tag);
    };
    auto apply_v1_roundtrip_regulator = [&](RegulatorSym reg,
                                              const char* tag) -> RegulatorSym {
        if (!runtime::scalar_rep_enabled()) return reg;
        return runtime::roundtrip_regulator_through_scs(reg, ctx, zw_tab, tag);
    };

    if (var_indices.empty()) {
        return apply_v1_roundtrip_regulator(
            canonicalize_regulator_sym(
                shuffle_list_as_regulator_sym(input)),
            "hyper_int/exit_reconstitute_empty");
    }

    // Promote the initial Rat input once; subsequent steps keep the
    // ShuffleListSym representation end-to-end. Phase-B B7 entry adapter:
    // per-Rat round-trip on every coef before SymCoef::from_rat.
    ShuffleListSym current;
    current.reserve(input.size());
    for (const auto& e : input) {
        current.push_back(ShuffleEntrySym{
            SymCoef::from_rat(
                apply_v1_roundtrip_rat(e.coef, "hyper_int/entry_split")),
            e.shuffle});
    }

    const bool trace = step_trace_enabled();
    const bool mi_stats = mi_stats_enabled();
    const bool mi_heap_visit = mi_heap_visit_enabled();
    const bool mi_heap_visit_all = mi_heap_visit_all_enabled();
    const bool mi_heap_visit_abandoned =
        mi_heap_visit_abandoned_enabled();
    const bool mi_heap_visit_disjoint =
        mi_heap_visit && mi_heap_visit_abandoned
        && mi_heap_visit_disjoint_audit_enabled();

    // HF_PROGRESS (2026-06-04): one-line stderr heartbeat per step so
    // long runs are not a black box. Default-OFF; zero hot-path cost.
    static const bool hf_progress = [] {
        const char* e = std::getenv("HF_PROGRESS");
        return e && e[0] == '1';
    }();
    for (size_t step = 0; step + 1 < var_indices.size(); ++step) {
        if (hf_progress) {
            std::fprintf(stderr,
                "[hf-progress] step %zu/%zu: integrating %s (entries=%zu)\n",
                step + 1, var_indices.size(),
                ctx.vars()[var_indices[step]].c_str(), current.size());
            std::fflush(stderr);
        }
        // Phase 5e-iii follow-up: pass remaining var_indices to the
        // divergence check for full-fibration zero testing.
        // DP.3 spectator projection (2026-06-03): the zero test must
        // also fibrate over the never-integrated user variables, or
        // cross-letter cancellations that are functions of the free
        // kinematic parameters stay invisible (false-positive
        // "divergent" on generic convergent inputs). See hyper_int.hpp.
        std::vector<size_t> remaining(
            var_indices.begin() + step + 1, var_indices.end());
        remaining.insert(remaining.end(),
                         spectator_var_indices.begin(),
                         spectator_var_indices.end());
        InputMetrics in_m;
        std::chrono::steady_clock::time_point t0;
        if (trace) {
            in_m = measure(current);
            reset_step_sub_timers();
            t0 = std::chrono::steady_clock::now();
        }
        if (mi_stats) {
            // §A.M iter-3 probe: pre-step snapshot establishes the
            // start-of-step RSS reference for the (post_step - pre_step)
            // delta = step-resident bytes computation.
            emit_mi_stats(step, ctx.vars()[var_indices[step]], "pre_step");
            if (mi_heap_visit) {
                emit_heap_visit_buckets(step,
                    ctx.vars()[var_indices[step]], "pre_step");
            }
            if (mi_heap_visit_all) {
                emit_heap_visit_buckets_all_theaps(step,
                    ctx.vars()[var_indices[step]], "pre_step");
            }
            if (mi_heap_visit_abandoned) {
                emit_heap_visit_buckets_abandoned(step,
                    ctx.vars()[var_indices[step]], "pre_step");
            }
            if (mi_heap_visit_disjoint) {
                emit_heap_visit_disjoint_audit_intersect(step,
                    ctx.vars()[var_indices[step]], "pre_step");
            }
        }
        // iter-17 (Track 0.4 pfrac-row storage probe).
        // Independent of HF_MI_STATS; gate is HF_PF_STORAGE_STATS.
        // Note: this fires on the main thread; only main-thread
        // `g_pf_cache` is sampled.  Per-worker coverage is supplied by
        // the `after_pf` probe inside `partial_fractions()` itself.
        if (g_pf_storage_stats_enabled.load(std::memory_order_relaxed)) {
            emit_pf_storage_stats(static_cast<long>(step),
                                   ctx.vars()[var_indices[step]].c_str(),
                                   "pre_step",
                                   nullptr);
        }
        // §6.D iter-13 (REQ-A + REC-3 from iter-11 advisory reviewer
        // ab6fa5cda36908669 CONCERNS-FOLD): stamp the outer-step
        // boundary state onto the per-node sampler so that every
        // NodeRssRecord produced by integration_step's OMP body
        // carries the LinearFactors-cache footprint and regulator-
        // SymCoef footprint at this step's entry.  Fast-path: when
        // HF_INTEG_NODE_RSS is disabled the sampler short-circuits
        // and the two atomic stores are dead writes (no observable
        // impact, ~tens of ns).  Sampling at outer-step boundary,
        // not per node, is per REC-3 (the per-shard mutex-walking
        // residency function is O(N_polys) and would dominate inner-
        // node wall otherwise).
        {
            int64_t lf_bytes = 0;
            int64_t rs_bytes = 0;
            if (hyperflint::IntegrationNodeRssSampler::instance().enabled()) {
                lf_bytes = static_cast<int64_t>(
                    hyperflint::linear_factors_cache_residency().total_bytes);
                for (const auto& t : current) {
                    rs_bytes += static_cast<int64_t>(t.coef.total_bytes());
                }
            }
            hyperflint::set_outer_step_context(lf_bytes, rs_bytes);
        }

        // Iter-52 C0c.1 Increment β: thread the persistent driver-entry
        // `zw_tab` (allocated at :463-466 under scalar_rep_enabled()) into
        // integration_step. From there it cascades to transform_shuffle,
        // integrate_ii, partial_fractions, linear_factors (site 6 lambda
        // body kill) and to the post-OMP-merge canon-slot lambda (site 7
        // lambda body kill, Protocol A per-thread + master merge_into per
        // design v2 §3.6a STEPS 1-3). When scalar_rep_enabled() is false,
        // `zw_tab` is null and every dereferencing site short-circuits via
        // its own runtime::scalar_rep_enabled() guard.
        RegulatorSym reg = integration_step(ctx, current, var_indices[step],
                                              table, zw_tab,
                                              check_divergences,
                                              introduce_algebraic_letters,
                                              remaining);
        // Phase A commit (3): HF_RAT_SPLIT_VERIFY round-trip check.
        // No-op when the env gate is unset; aborts on bit-divergence
        // when set.
        verify_regulator_sym_rat_split(reg, ctx, var_indices,
                                       "hyperflint_sym/integration_step");
        if (trace) {
            const auto t1 = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(t1 - t0).count();
            emit_step_trace(step, ctx.vars()[var_indices[step]],
                            in_m, measure(reg), dt,
                            /*include_sub_timers=*/false);
        }
        if (mi_stats) {
            emit_mi_stats(step, ctx.vars()[var_indices[step]], "post_step");
            if (mi_heap_visit) {
                emit_heap_visit_buckets(step,
                    ctx.vars()[var_indices[step]], "post_step");
            }
            if (mi_heap_visit_all) {
                emit_heap_visit_buckets_all_theaps(step,
                    ctx.vars()[var_indices[step]], "post_step");
            }
            if (mi_heap_visit_abandoned) {
                emit_heap_visit_buckets_abandoned(step,
                    ctx.vars()[var_indices[step]], "post_step");
            }
            if (mi_heap_visit_disjoint) {
                emit_heap_visit_disjoint_audit_intersect(step,
                    ctx.vars()[var_indices[step]], "post_step");
            }
            if (g_pf_storage_stats_enabled.load(std::memory_order_relaxed)) {
                emit_pf_storage_stats(static_cast<long>(step),
                                       ctx.vars()[var_indices[step]].c_str(),
                                       "post_step",
                                       nullptr);
            }
            // §A.M iter-3: mi_collect(true) is now gated by
            // HF_MI_COLLECT_AT_STEP. Default (env unset) preserves the
            // pre-iter-3 always-collect behaviour for backward compat with
            // iter-37/41/86/87 baselines; HF_MI_COLLECT_AT_STEP=0
            // selects the §A.M activation-gate baseline arm.
            // §A.M iter-4: dispatch route through
            // run_mi_collect_at_step_dispatch — Option M.a (default) or
            // Option M.c (HF_MI_COLLECT_OPTION_M_C=1).
            if (mi_collect_at_step_enabled()) {
                run_mi_collect_at_step_dispatch(step,
                                       ctx.vars()[var_indices[step]],
                                       "collect_step_cost");
                emit_mi_stats(step, ctx.vars()[var_indices[step]],
                              "post_collect");
                if (mi_heap_visit) {
                    emit_heap_visit_buckets(step,
                        ctx.vars()[var_indices[step]], "post_collect");
                }
                if (mi_heap_visit_all) {
                    emit_heap_visit_buckets_all_theaps(step,
                        ctx.vars()[var_indices[step]], "post_collect");
                }
                if (mi_heap_visit_abandoned) {
                    emit_heap_visit_buckets_abandoned(step,
                        ctx.vars()[var_indices[step]], "post_collect");
                }
                if (mi_heap_visit_disjoint) {
                    emit_heap_visit_disjoint_audit_intersect(step,
                        ctx.vars()[var_indices[step]], "post_collect");
                }
            }
        }
        current = regulator_sym_as_shuffle_list_sym(std::move(reg));
        if (mi_stats) {
            emit_mi_stats(step, ctx.vars()[var_indices[step]], "post_assign");
            if (mi_heap_visit) {
                emit_heap_visit_buckets(step,
                    ctx.vars()[var_indices[step]], "post_assign");
            }
            if (mi_heap_visit_all) {
                emit_heap_visit_buckets_all_theaps(step,
                    ctx.vars()[var_indices[step]], "post_assign");
            }
            if (mi_heap_visit_abandoned) {
                emit_heap_visit_buckets_abandoned(step,
                    ctx.vars()[var_indices[step]], "post_assign");
            }
            if (mi_heap_visit_disjoint) {
                emit_heap_visit_disjoint_audit_intersect(step,
                    ctx.vars()[var_indices[step]], "post_assign");
            }
        }
        // 2026-05-05 (path-A diagnostic): walk persistent state after step.
        emit_parity1_probe(step, ctx.vars()[var_indices[step]], current);

        // §E iter-7 (Phase 6 REVISED lever 5 / iter-6 design.md): post-step
        // RSS-pressure LRU eviction. Hook fires AFTER the assignment back
        // to `current` and after all per-step diagnostics, in the outer
        // loop's serial scope (post-OMP-region implicit barrier inside
        // integration_step). Fast-path no-op when HF_OP_MEMO_EVICT_ON_RSS
        // is OFF (default). See:
        //   - notes/.../lever_e_op_memo_eviction_rss_pressure/design.md §6
        //   - notes/.../lever_e_op_memo_eviction_rss_pressure/determinism_audit.md
        //   - iter-6 BINDING reviewer agentId ac8bfbb26a2038b96 REQ-3
        //     (hook-location correction: this site is the outer-loop step
        //     boundary, NOT integration_step.cpp:1388/1393 which fire at
        //     function entry of `integration_step()`).
        operator_memo::evict_post_step_hook();
    }
    // Final step: no remaining vars (empty list).
    InputMetrics in_m;
    std::chrono::steady_clock::time_point t0;
    if (trace) {
        in_m = measure(current);
        reset_step_sub_timers();
        t0 = std::chrono::steady_clock::now();
    }
    // §6.D iter-13: stamp the outer-step boundary state for the final
    // step's records too.  Same fast-path guard on the sampler enabled
    // flag — disabled (default-OFF) path is two atomic stores of 0.
    {
        int64_t lf_bytes = 0;
        int64_t rs_bytes = 0;
        if (hyperflint::IntegrationNodeRssSampler::instance().enabled()) {
            lf_bytes = static_cast<int64_t>(
                hyperflint::linear_factors_cache_residency().total_bytes);
            for (const auto& t : current) {
                rs_bytes += static_cast<int64_t>(t.coef.total_bytes());
            }
        }
        hyperflint::set_outer_step_context(lf_bytes, rs_bytes);
    }
    if (hf_progress) {
        std::fprintf(stderr,
            "[hf-progress] final step %zu/%zu: integrating %s (entries=%zu)\n",
            var_indices.size(), var_indices.size(),
            ctx.vars()[var_indices.back()].c_str(), current.size());
        std::fflush(stderr);
    }
    RegulatorSym final_reg = integration_step_sym(
        ctx, current, var_indices.back(),
        table, zw_tab, check_divergences,
        introduce_algebraic_letters,
        /*remaining_var_indices=*/spectator_var_indices);
    // Phase A commit (3): HF_RAT_SPLIT_VERIFY on the final-step output.
    verify_regulator_sym_rat_split(final_reg, ctx, var_indices,
                                   "hyperflint_sym/integration_step_sym");
    if (trace) {
        const auto t1 = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(t1 - t0).count();
        emit_step_trace(var_indices.size() - 1,
                        ctx.vars()[var_indices.back()],
                        in_m, measure(final_reg), dt,
                        /*include_sub_timers=*/true);
    }
    if (mi_stats) {
        emit_mi_stats(var_indices.size() - 1,
                      ctx.vars()[var_indices.back()], "post_final_step");
        if (mi_heap_visit) {
            emit_heap_visit_buckets(var_indices.size() - 1,
                ctx.vars()[var_indices.back()], "post_final_step");
        }
        if (mi_heap_visit_all) {
            emit_heap_visit_buckets_all_theaps(var_indices.size() - 1,
                ctx.vars()[var_indices.back()], "post_final_step");
        }
        if (mi_heap_visit_abandoned) {
            emit_heap_visit_buckets_abandoned(var_indices.size() - 1,
                ctx.vars()[var_indices.back()], "post_final_step");
        }
        if (mi_heap_visit_disjoint) {
            emit_heap_visit_disjoint_audit_intersect(var_indices.size() - 1,
                ctx.vars()[var_indices.back()], "post_final_step");
        }
        // §A.M iter-3: same gating semantics as the main-loop branch.
        // §A.M iter-4: dispatch route through
        // run_mi_collect_at_step_dispatch — Option M.a (default) or
        // Option M.c (HF_MI_COLLECT_OPTION_M_C=1).
        if (mi_collect_at_step_enabled()) {
            run_mi_collect_at_step_dispatch(var_indices.size() - 1,
                                   ctx.vars()[var_indices.back()],
                                   "collect_final_step_cost");
            emit_mi_stats(var_indices.size() - 1,
                          ctx.vars()[var_indices.back()],
                          "post_final_collect");
            if (mi_heap_visit) {
                emit_heap_visit_buckets(var_indices.size() - 1,
                    ctx.vars()[var_indices.back()], "post_final_collect");
            }
            if (mi_heap_visit_all) {
                emit_heap_visit_buckets_all_theaps(var_indices.size() - 1,
                    ctx.vars()[var_indices.back()], "post_final_collect");
            }
            if (mi_heap_visit_abandoned) {
                emit_heap_visit_buckets_abandoned(var_indices.size() - 1,
                    ctx.vars()[var_indices.back()], "post_final_collect");
            }
            if (mi_heap_visit_disjoint) {
                emit_heap_visit_disjoint_audit_intersect(var_indices.size() - 1,
                    ctx.vars()[var_indices.back()], "post_final_collect");
            }
        }
    }
    // R24 rev 2 / chain 17 — defense-in-depth post-OMP flag check.
    // The inner `integration_step`/`integration_step_sym` calls each
    // throw `NarrowCtxTooNarrow` if their parallel-region observed a
    // missing var, so this is a belt-and-braces guard catching any
    // path that sets the flag outside those parallel regions (e.g.
    // a future `to_mzv_one_word` callsite reached from a serial
    // helper).  Default-off path: relaxed-load no-op.
    if (narrow_ctx_was_too_narrow()) {
        throw NarrowCtxTooNarrow{"hyperflint_sym"};
    }
    // Phase-B B7 exit adapter: per-RegulatorSym round-trip on the
    // driver-level return.
    return apply_v1_roundtrip_regulator(std::move(final_reg),
                                          "hyper_int/exit_reconstitute");
}

// -------------- Phase 5f-iii: interval rescaling --------------

namespace {

enum class Bound { FINITE, POS_INF, NEG_INF };

Bound classify_bound(const std::string& s) {
    if (s == "Infinity"  || s == "+Infinity" || s == "oo")  return Bound::POS_INF;
    if (s == "-Infinity" || s == "-oo")                      return Bound::NEG_INF;
    return Bound::FINITE;
}

// Substitute `var_idx -> sub_expr` in every Rat (all letters + coef)
// of one ShuffleEntry; multiply the result's coef by `jac`. The
// substitution and the Jacobian together realize one case of the
// Mma HyperInt interval rescaling logic (HyperIntica.wl:4365-4380).
ShuffleEntry rescale_entry(const PolyCtx& ctx,
                            const ShuffleEntry& e,
                            size_t var_idx,
                            const Rat& sub_expr,
                            const Rat& jac) {
    std::vector<Word> new_shuffle;
    new_shuffle.reserve(e.shuffle.size());
    for (const auto& w : e.shuffle) {
        Word nw;
        nw.letters.reserve(w.letters.size());
        for (const auto& l : w.letters) {
            nw.letters.push_back(substitute_var_rat(ctx, l, var_idx, sub_expr));
        }
        new_shuffle.push_back(std::move(nw));
    }
    Rat new_coef = substitute_var_rat(ctx, e.coef, var_idx, sub_expr);
    return ShuffleEntry{new_coef * jac, std::move(new_shuffle)};
}

}  // namespace

ShuffleList rescale_interval(const PolyCtx& ctx,
                              const ShuffleList& input,
                              size_t var_idx,
                              const std::string& from_str,
                              const std::string& to_str) {
    const Bound from_kind = classify_bound(from_str);
    const Bound to_kind   = classify_bound(to_str);

    // a == b: zero integral.
    if (from_kind == Bound::FINITE && to_kind == Bound::FINITE &&
        from_str == to_str) {
        return ShuffleList{};
    }
    // Default case: already [0, ∞).
    if (from_kind == Bound::FINITE && from_str == "0" &&
        to_kind == Bound::POS_INF) {
        return input;
    }

    const Poly var_poly = Poly::gen(ctx, var_idx);
    const Rat  var_rat(var_poly);
    const Rat  one  = Rat::one_of(ctx);
    const Rat  mone = Rat::from_int(ctx, -1);

    // [-∞, ∞]: F(var) + F(-var), both over [0, ∞).
    if (from_kind == Bound::NEG_INF && to_kind == Bound::POS_INF) {
        const Rat neg_var = -var_rat;
        ShuffleList out;
        out.reserve(input.size() * 2);
        for (const auto& e : input) out.push_back(e);
        for (const auto& e : input)
            out.push_back(rescale_entry(ctx, e, var_idx, neg_var, one));
        return out;
    }

    // [a, ∞]: var -> a + var, Jacobian 1.
    if (from_kind == Bound::FINITE && to_kind == Bound::POS_INF) {
        const Rat a = Rat::parse(ctx, from_str);
        const Rat sub = a + var_rat;
        ShuffleList out;
        out.reserve(input.size());
        for (const auto& e : input)
            out.push_back(rescale_entry(ctx, e, var_idx, sub, one));
        return out;
    }

    // [-∞, b]: var -> b - var, Jacobian -1.
    if (from_kind == Bound::NEG_INF && to_kind == Bound::FINITE) {
        const Rat b = Rat::parse(ctx, to_str);
        const Rat sub = b - var_rat;
        ShuffleList out;
        out.reserve(input.size());
        for (const auto& e : input)
            out.push_back(rescale_entry(ctx, e, var_idx, sub, mone));
        return out;
    }

    // [a, -∞]: var -> a - var, Jacobian -1. (Backwards bounds.)
    if (from_kind == Bound::FINITE && to_kind == Bound::NEG_INF) {
        const Rat a = Rat::parse(ctx, from_str);
        const Rat sub = a - var_rat;
        ShuffleList out;
        out.reserve(input.size());
        for (const auto& e : input)
            out.push_back(rescale_entry(ctx, e, var_idx, sub, mone));
        return out;
    }

    // [∞, b]: var -> b + var, Jacobian -1.
    if (from_kind == Bound::POS_INF && to_kind == Bound::FINITE) {
        const Rat b = Rat::parse(ctx, to_str);
        const Rat sub = b + var_rat;
        ShuffleList out;
        out.reserve(input.size());
        for (const auto& e : input)
            out.push_back(rescale_entry(ctx, e, var_idx, sub, mone));
        return out;
    }

    // [a, b] both finite: var -> (a + b·var)/(1 + var);
    // Jacobian (b - a)/(1 + var)².
    if (from_kind == Bound::FINITE && to_kind == Bound::FINITE) {
        const Rat a = Rat::parse(ctx, from_str);
        const Rat b = Rat::parse(ctx, to_str);
        const Rat one_plus_var = one + var_rat;
        const Rat sub = (a + b * var_rat) / one_plus_var;
        const Rat jac = (b - a) / (one_plus_var * one_plus_var);
        ShuffleList out;
        out.reserve(input.size());
        for (const auto& e : input)
            out.push_back(rescale_entry(ctx, e, var_idx, sub, jac));
        return out;
    }

    throw std::runtime_error(
        "rescale_interval: unsupported bound combination (from='" +
        from_str + "', to='" + to_str + "')");
}

}  // namespace hyperflint
