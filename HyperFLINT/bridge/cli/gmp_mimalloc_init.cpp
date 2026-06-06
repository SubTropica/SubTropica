// Phase-1.2 retrofit (2026-05-05): redirect GMP+FLINT allocations to
// mimalloc on the CLI binary.
//
// The Path-A diagnostic v2 measured mimalloc peak_commit = 390 MB while
// process RSS = 2426 MB on parity-1 ord_1 face_1 of 3l3pt — i.e. ~16% of
// the RSS is mimalloc-managed and ~2 GB is in non-mimalloc memory.
// Audit showed:
//
// - HyperFLINT/CMakeLists.txt:399-400 force-loads libmimalloc.a into the
//   CLI, which overrides malloc/free in the CLI's own symbol table.
// - But macOS uses two-level namespace: libgmp.10.dylib /
//   libflint.22.0.dylib / libmpfr.6.dylib were built with malloc/free
//   bound to libsystem_malloc.dylib at their build time. Force-loading
//   mimalloc into the CLI does NOT propagate into pre-bound dylib
//   references. GMP/FLINT continue to call libsystem_malloc.
// - GMP and FLINT each indirect every malloc through a function pointer
//   that can be rebound at runtime via mp_set_memory_functions /
//   __flint_set_all_memory_functions. We rebind both to mimalloc.
//
// The retrofit applies to the CLI binary only. The LibraryLink dylib
// (HyperFLINT/dist/macos-arm64/libhyperflint_librarylink.dylib) uses a
// different code path and is unaffected.
//
// Safety:
// - Audit (Explore-agent verified 2026-05-05) confirmed no namespace-
//   scope GMP/FLINT objects exist in libhyperflint.a; no static-init
//   landmines. Calling at the very first line of main() is safe.
// - hf_init_mimalloc_for_gmp_flint is idempotent and silently no-ops
//   if mimalloc isn't linked (HF_MIMALLOC=OFF) — weak-extern pattern
//   already proven by mi_collect / mi_process_info in
//   src/integrator/hyper_int.cpp:31-36.
// - Pre-condition: FLINT thread pool not yet spawned. Asserted via
//   flint_get_num_threads() == 1 before the rebind.

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "hyperflint/runtime/env_flags.hpp"  // §T7 third chunk: HF_FLAG_MI_INIT_VERBOSE

#include <gmp.h>

extern "C" {
#include <flint/flint.h>
}

// Weak externs of mimalloc allocator entries. When the CLI binary is
// linked with -force_load,libmimalloc.a, these resolve to mimalloc's
// strong symbols. When mimalloc is absent (HF_MIMALLOC=OFF), they
// resolve to nullptr at runtime and the init silently no-ops.
extern "C" {
__attribute__((weak)) void* mi_malloc(size_t size);
__attribute__((weak)) void* mi_realloc(void* p, size_t newsize);
__attribute__((weak)) void  mi_free(void* p);
__attribute__((weak)) void* mi_calloc(size_t count, size_t size);
__attribute__((weak)) void* mi_aligned_alloc(size_t alignment, size_t size);
}

namespace {

// GMP's allocator API:
//   void *(*alloc)(size_t)
//   void *(*realloc)(void*, size_t old_size, size_t new_size)
//   void  (*free)(void*, size_t)
// The size argument to free is informative for size-class allocators
// (which mimalloc is); we pass it through but mi_free finds the size
// itself via segment metadata, so the n parameter is unused.
void* gmp_alloc(size_t n) {
    return mi_malloc(n);
}
void* gmp_realloc(void* p, size_t /*old*/, size_t newsz) {
    return mi_realloc(p, newsz);
}
void gmp_free(void* p, size_t /*n*/) {
    mi_free(p);
}

// FLINT's plain allocator API:
//   void *(*alloc)(size_t)
//   void *(*calloc)(size_t, size_t)
//   void *(*realloc)(void*, size_t)
//   void  (*free)(void*)
// Adapter names avoid collision with FLINT's public flint_alloc /
// flint_calloc / flint_realloc / flint_free symbols (extern "C", in
// scope through <flint/flint.h>). Suffix `_mi` distinguishes them.
void* flint_alloc_mi(size_t n) {
    return mi_malloc(n);
}
void* flint_calloc_mi(size_t count, size_t size) {
    return mi_calloc(count, size);
}
void* flint_realloc_mi(void* p, size_t newsz) {
    return mi_realloc(p, newsz);
}
void flint_free_mi(void* p) {
    mi_free(p);
}

// FLINT's aligned-alloc API (used by fft_small in the dynamic Homebrew
// libflint for large multiplications):
//   void *(*aligned_alloc)(size_t alignment, size_t size)
//   void  (*aligned_free)(void*)
// Note: aligned_free has signature (void*) — no alignment parameter.
// mi_free correctly handles pointers from mi_aligned_alloc on macOS
// (mimalloc tracks alignment in segment metadata), so we wrap with
// mi_free, NOT mi_free_aligned (which would require the alignment).
void* flint_aligned_alloc_mi(size_t alignment, size_t size) {
    return mi_aligned_alloc(alignment, size);
}
void flint_aligned_free_mi(void* p) {
    mi_free(p);
}

}  // namespace

// HF FF Phase 5 REC-1 (iter-83): Phase 0.5 retrofit completion flag. Used by
// mpz_pool_probe.cpp's REC-1 init via a weak extern to detect whether the
// retrofit ran before REC-1 init; if not, REC-1 refuses install. The flag is
// set to true at the END of hf_init_mimalloc_for_gmp_flint() so a partial
// retrofit (e.g. aborted by the assert at line 142) does NOT advertise itself
// as complete.
extern "C" bool hf_phase05_retrofit_done = false;

extern "C" void hf_init_mimalloc_for_gmp_flint(void) {
    // Silent no-op if mimalloc isn't linked (HF_MIMALLOC=OFF or
    // mimalloc not found at cmake time).
    if (mi_malloc == nullptr || mi_realloc == nullptr ||
        mi_free   == nullptr || mi_calloc  == nullptr ||
        mi_aligned_alloc == nullptr) {
        if (HF_FLAG_MI_INIT_VERBOSE) {
            std::fprintf(stderr,
                "[hf_init_mimalloc] mimalloc symbols absent; "
                "GMP/FLINT continue to use libsystem_malloc.\n");
            std::fflush(stderr);
        }
        return;
    }

    // Pre-condition: FLINT's thread pool must not yet be spawned.
    // If it were, worker threads would already hold xzm-allocated
    // mpz blocks that would later be freed via mi_free (crash).
    // HF only calls flint_set_num_threads() inside hyperflint_sym
    // (handlers.cpp), well after main() starts, so this assertion
    // protects against future regressions.
    if (flint_get_num_threads() != 1) {
        std::fprintf(stderr,
            "[hf_init_mimalloc] FATAL: FLINT thread pool already "
            "initialized (num_threads=%d) before mimalloc retrofit. "
            "Allocator swap would race with worker TLS pools. "
            "Move hf_init_mimalloc_for_gmp_flint() earlier in main().\n",
            flint_get_num_threads());
        std::abort();
    }

    mp_set_memory_functions(gmp_alloc, gmp_realloc, gmp_free);
    __flint_set_all_memory_functions(
        flint_alloc_mi, flint_calloc_mi,
        flint_realloc_mi, flint_free_mi,
        flint_aligned_alloc_mi, flint_aligned_free_mi);

    if (HF_FLAG_MI_INIT_VERBOSE) {
        std::fprintf(stderr,
            "[hf_init_mimalloc] GMP+FLINT (incl. aligned-alloc) "
            "redirected to mimalloc.\n");
        std::fflush(stderr);
    }

    // REC-1 (iter-83) defense-in-depth flag. Set LAST so a partial / aborted
    // retrofit (the assert at line 142, or an early return through the
    // weak-symbol check at line 117) does NOT advertise as complete to
    // downstream weak-extern consumers.
    hf_phase05_retrofit_done = true;
}
