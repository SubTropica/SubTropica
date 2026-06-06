// narrow_ctx_flag — runtime infrastructure for the Step 1
// sentinel-tolerance design (R24 rev 2).
//
// Background.  The HF integrator's `apply_mzv_reductions` and
// `to_mzv_one_word` paths construct mzv-symbol Rat values at
// runtime via `Rat::parse(ctx, ...)`.  When ctx is the wide MZV
// var list the parse always succeeds, but under the narrow-ctx
// lever (HF_NARROW_CTX=1) some mzv names may be absent and the
// parse throws `std::runtime_error`.  From inside an OMP parallel
// region that exception escape is implementation-defined and in
// practice calls `std::terminate`.
//
// The R24 rev 2 design replaces the throwing parse with
// `Rat::parse_or_none` plus a process-global atomic flag set on
// missing-var.  After the parallel region's implicit barrier, the
// host reads the flag and (if set) throws `NarrowCtxTooNarrow`
// from the host thread, which the bridge handler catches and emits
// as a structured-error JSON `narrow_ctx_insufficient: true`.
// `STHyperFlint` (Mma) detects that flag and re-issues the
// `RunProcess` call with `HF_NARROW_CTX=0`.
//
// This TU exposes:
//   - `g_narrow_ctx_too_narrow` — the atomic flag itself.
//   - `reset_narrow_ctx_flag()` — clear at handler entry.
//   - `narrow_ctx_was_too_narrow()` — observe (relaxed atomic load).
//   - `tolerance_enabled()` — cached env-gate (HF_PARSE_TOLERANT).
//
// At default (HF_PARSE_TOLERANT unset / "0") the entire
// sentinel-tolerance machinery is dead code.  No production
// callsite invokes the new APIs in this chain (16); chain 17+
// wires them into `parse_rhs_cached`, `to_mzv_one_word`, the
// OMP-region try/catch wrappers, and the handler-side catch.
//
// Reviewer history: r24_step1_redesign_rev2.md (R25 GO-WITH-CHANGES).

#pragma once

#include <atomic>

namespace hyperflint {

// Atomic flag: set by `parse_or_none` failures inside the OMP
// region; observed by host code after the region's implicit barrier.
// `std::memory_order_relaxed` is sufficient because the OMP barrier
// (created by `#pragma omp parallel for`'s implicit barrier or
// explicit `#pragma omp barrier`) provides happens-before between
// the worker write and the host read.
extern std::atomic<bool> g_narrow_ctx_too_narrow;

// Clear the flag.  Call at hyperflint_sym handler entry so the
// per-call state starts clean; otherwise a flag set by a previous
// failed call could falsely poison the next call (relevant under
// LibraryLink in-process transport).
void reset_narrow_ctx_flag();

// Observe the flag.  Returns true iff some thread set it during
// the most recent OMP region.  Read-relaxed.
bool narrow_ctx_was_too_narrow();

// Cached env-gate: reads HF_PARSE_TOLERANT exactly once at first
// call (static-local once-init, thread-safe under C++11 magic
// statics) and returns the cached bool on every subsequent call.
//
// Production callers must use this rather than re-reading
// `getenv("HF_PARSE_TOLERANT")` on every iteration of an OMP loop:
// per-iteration getenv is non-trivial overhead AND races with any
// runtime `setenv` calls (none in this design but defensive).
bool tolerance_enabled();

}  // namespace hyperflint
