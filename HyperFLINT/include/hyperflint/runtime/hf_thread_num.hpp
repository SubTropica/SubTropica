#pragma once
// Phase 0.5 Item U Stage 2: portable thread-number helper.
//
// Inside an OMP parallel region, hf_get_thread_num() returns omp_get_thread_num().
// Inside a GCD dispatch_apply block (HF_USE_GCD=1), the GCD dispatcher sets
// a thread_local slot override before calling the body; hf_get_thread_num()
// returns that override so all per-thread array indexing in callees
// (primitive.cpp, rat.cpp, poly.cpp, partial_fractions.cpp, linear_factors.cpp)
// receives a valid slot index even when omp_get_thread_num() would return 0.
//
// Usage:
//   - Call hf_set_thread_num(slot) from the GCD lambda BEFORE invoking the body.
//   - Call hf_clear_thread_num() AFTER the body (or just let the thread_local
//     be overwritten on the next iteration — the slot is always set before use).
//   - Replace omp_get_thread_num() with hf_get_thread_num() at every per-thread
//     array indexing site that is called from within the outer OMP/GCD for-loop.
//
// Invariant: hf_set_thread_num must be called before any call to hf_get_thread_num
// from within a GCD worker.  In OMP mode the override is never set, so the
// function falls through to omp_get_thread_num().

#ifdef HF_HAVE_OPENMP
#include <omp.h>
#endif

namespace hyperflint {
namespace runtime {

// Thread_local slot override.  -1 = not set (OMP path).
// Set by the GCD lambda in integration_step.cpp; read by hf_get_thread_num().
extern thread_local int g_hf_thread_num_override;

// Returns the slot for the current thread.
// GCD path: returns g_hf_thread_num_override (set by GCD dispatcher).
// OMP path: returns omp_get_thread_num() (override is -1).
// No-OMP path: returns 0.
inline int hf_get_thread_num() {
    if (g_hf_thread_num_override >= 0) return g_hf_thread_num_override;
#ifdef HF_HAVE_OPENMP
    return omp_get_thread_num();
#else
    return 0;
#endif
}

// Set the per-thread slot override (called from GCD lambda before body).
inline void hf_set_thread_num(int slot) {
    g_hf_thread_num_override = slot;
}

// Clear the override (sets back to -1; optional, call after body if needed).
inline void hf_clear_thread_num() {
    g_hf_thread_num_override = -1;
}

}  // namespace runtime
}  // namespace hyperflint
