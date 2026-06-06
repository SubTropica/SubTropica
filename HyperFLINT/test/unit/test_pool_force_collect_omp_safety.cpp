// HF FF Phase 6 §A.M iter-4 Option M.c OMP-safety unit test.
//
// BINDING adversarial-reviewer agentId a80aa1876bf43e818 CONCERNS-FOLD
// REQ-3 (pre-build authorization for iter-4 §A.M Option M.c source change).
//
// Tests the OMP-safety invariants of the Option M.c source change at
// HyperFLINT/src/integrator/hyper_int.cpp::run_mi_collect_option_mc_timed.
// Option M.c calls mi_heap_collect(heap, true) on every heap visited via
// mi_subproc_visit_heaps(mi_subproc_main(), …), from the main thread,
// between OMP parallel regions.
//
// REQ-3 invariants:
//
//   T1 determinism: 10 consecutive mi_subproc_visit_heaps() walks from
//      the main thread, with OMP_NUM_THREADS=13 active (workers have
//      already allocated + freed via #pragma omp parallel), must return
//      identical theaps_visited count across all 10 trials.  iter-39 §6
//      empirically observed theaps_visited=1 in 14/14 records on tst2;
//      T1 confirms this is stable across trials in a standalone test
//      fixture.
//
//   T2 per-heap collect safety: for each visited heap, invoke
//      mi_heap_collect(heap, true) from the main thread. No abort,
//      no segfault, no use-after-free. Caller MUST NOT be inside an
//      `#pragma omp parallel` region at the time of the call.
//
// Note on the empirical expectation (iter-39 §6):
//   In HF's OMP context, mi_subproc_visit_heaps(mi_subproc_main(), ...)
//   returns the SAME single heap (= mi_heap_main()) regardless of how
//   many OMP workers ran.  OMP worker theaps are NOT attached to the
//   main subproc because HF never calls mi_subproc_add_current_thread
//   from worker threads (mimalloc 3.3.1 brew header line 355 documents
//   the required "right after thread creation, before any allocation"
//   ordering that HF cannot guarantee).  Consequently this test will
//   typically observe theaps_visited == 1 across all 10 trials, which
//   is the EXPECTED behaviour.  The test asserts (a) the count is
//   IDENTICAL across all 10 trials and (b) the per-heap collect does
//   not crash; it does NOT assert a specific value of theaps_visited.

#include <mimalloc.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

// Per-trial state for the mi_subproc_visit_heaps walk.
struct VisitState {
    std::size_t theaps_visited = 0;
    bool collect_safety_failed = false;
};

// Visit callback that mirrors the production Option M.c visitor at
// HyperFLINT/src/integrator/hyper_int.cpp::option_mc_visit_cb.  Counts
// theaps_visited and calls mi_heap_collect(heap, true) per visited heap.
// Returns true to continue subproc traversal.
bool visit_and_collect_cb(mi_heap_t* heap, void* arg) {
    auto* s = static_cast<VisitState*>(arg);
    s->theaps_visited += 1;
    if (heap == nullptr) {
        // Should never happen per mimalloc heap-visit semantics, but
        // defense-in-depth: if a null heap pointer were passed, treat
        // it as a collect-safety failure rather than dereferencing.
        s->collect_safety_failed = true;
        return false;
    }
    // The load-bearing call: if mi_heap_collect on a non-owning thread
    // were UB in mimalloc 3.3.1, this would crash here.  iter-4 reviewer
    // REQ-3 invariant T2.
    mi_heap_collect(heap, /*force=*/true);
    return true;
}

int test_omp_safety_determinism_and_collect() {
    constexpr int kNumTrials = 10;
#ifdef _OPENMP
    constexpr int kNumWorkers = 13;
    omp_set_num_threads(kNumWorkers);
#endif

    // Prime the OMP worker theaps by running a parallel block of
    // allocations + frees.  This mirrors the integration_step body's
    // OMP region pattern (alloc per worker, then release on region
    // exit).  After the region, workers are idle but their theaps
    // may still be live (libdispatch persistence) or abandoned
    // (pthread exit).  Either way, main-thread visit + collect must
    // not crash.
#ifdef _OPENMP
#pragma omp parallel
    {
        constexpr std::size_t kAllocsPerWorker = 128;
        constexpr std::size_t kBlockSize = 1024;
        std::vector<void*> blocks;
        blocks.reserve(kAllocsPerWorker);
        for (std::size_t i = 0; i < kAllocsPerWorker; ++i) {
            blocks.push_back(mi_malloc(kBlockSize));
        }
        for (void* p : blocks) {
            mi_free(p);
        }
    }
#else
    // No OpenMP at compile time — the test still exercises the
    // single-thread main-heap path so we get coverage in the unlikely
    // event HF_OPENMP=OFF.
    constexpr std::size_t kAllocs = 1024;
    constexpr std::size_t kBlockSize = 1024;
    std::vector<void*> blocks;
    blocks.reserve(kAllocs);
    for (std::size_t i = 0; i < kAllocs; ++i) {
        blocks.push_back(mi_malloc(kBlockSize));
    }
    for (void* p : blocks) {
        mi_free(p);
    }
#endif

    // Acquire main subproc handle.  Per nm-precondition probe
    // (libmimalloc.a 3.3.1, offset 0x438): _mi_subproc_main is exported.
    mi_subproc_id_t sp = mi_subproc_main();
    if (sp == nullptr) {
        std::cerr << "[FAIL] omp-safety-determinism-and-collect: "
                  << "mi_subproc_main() returned nullptr\n";
        return 1;
    }

    // T1 + T2: run 10 trials, each consisting of a mi_subproc_visit_heaps
    // walk with the visit-and-collect callback.  Record theaps_visited
    // per trial; assert identical across all trials.
    std::vector<std::size_t> theaps_visited_per_trial;
    theaps_visited_per_trial.reserve(kNumTrials);

    for (int trial = 0; trial < kNumTrials; ++trial) {
        VisitState state;
        const bool ok = mi_subproc_visit_heaps(sp, &visit_and_collect_cb,
                                                  &state);
        if (!ok) {
            std::cerr << "[FAIL] omp-safety-determinism-and-collect: "
                      << "mi_subproc_visit_heaps returned false at trial "
                      << trial << "\n";
            return 1;
        }
        if (state.collect_safety_failed) {
            std::cerr << "[FAIL] omp-safety-determinism-and-collect: "
                      << "T2 per-heap collect safety: visitor received "
                      << "nullptr heap pointer at trial " << trial << "\n";
            return 1;
        }
        theaps_visited_per_trial.push_back(state.theaps_visited);
    }

    // T1 determinism: all 10 trials must report identical theaps_visited.
    const std::size_t expected = theaps_visited_per_trial.front();
    for (int trial = 1; trial < kNumTrials; ++trial) {
        if (theaps_visited_per_trial[trial] != expected) {
            std::cerr << "[FAIL] omp-safety-determinism-and-collect: "
                      << "T1 determinism violation: trial 0 reported "
                      << expected << " heaps, trial " << trial
                      << " reported " << theaps_visited_per_trial[trial]
                      << "\n";
            return 1;
        }
    }

    std::cout << "[PASS] omp-safety-determinism-and-collect: "
              << kNumTrials << " trials all reported theaps_visited="
              << expected
              << " (expected = 1 in HF/OMP per iter-39 §6); "
              << "mi_heap_collect(heap, true) per visited heap completed "
              << "without abort/segfault\n";
    return 0;
}

// Secondary smoke test: confirm mi_subproc_main + mi_subproc_visit_heaps
// + mi_heap_collect are non-null at runtime (sanity check the binding
// reviewer's nm-precondition probe finding at iter-4 cold-start).
int test_option_mc_symbol_availability() {
    mi_subproc_id_t sp = mi_subproc_main();
    if (sp == nullptr) {
        std::cerr << "[FAIL] option-mc-symbol-availability: "
                  << "mi_subproc_main() returned nullptr at runtime\n";
        return 1;
    }
    // mi_subproc_visit_heaps and mi_heap_collect are exercised in T1+T2
    // above; if those PASSed, this implicitly confirms availability.
    std::cout << "[PASS] option-mc-symbol-availability: "
              << "mi_subproc_main()=" << sp
              << " (mi_subproc_visit_heaps + mi_heap_collect exercised "
              << "in determinism test)\n";
    return 0;
}

}  // namespace

int main() {
    int rc = 0;
    rc |= test_option_mc_symbol_availability();
    rc |= test_omp_safety_determinism_and_collect();
    if (rc == 0) {
        std::cout << "\n[PASS] HF FF Phase 6 §A.M iter-4 Option M.c "
                  << "OMP-safety unit test: all REQ-3 invariants confirmed\n";
    } else {
        std::cerr << "\n[FAIL] HF FF Phase 6 §A.M iter-4 Option M.c "
                  << "OMP-safety unit test: at least one REQ-3 invariant "
                  << "failed\n";
    }
    return rc;
}
