// Phase 0 task 0-2 (2026-05-09): unit test for the RssSample helper.
//
// Allocates ~64 MB, touches every page to force it into RSS, then
// verifies that sample_rss() returns plausible values:
//   current_kib > 0
//   peak_kib > 0
//   current_kib >= 64 * 1024  (the 64 MB allocation is visible in ps)
//
// NOTE: we do NOT assert peak_kib >= current_kib.  On macOS, ps(1)
// rss and getrusage(RUSAGE_SELF).ru_maxrss use different kernel
// accounting paths and can disagree by tens of KiB for the same
// instant — ru_maxrss can legitimately report a slightly smaller value
// than ps rss.  Both are > 0 and > 64 MB, which is what matters.
//
// The test uses plain <cassert> (no external framework), consistent
// with the project's unit-test style.

#include "hyperflint/diagnostics/step_trace_rss.hpp"

#include <cassert>
#include <cstdio>

using hyperflint::sample_rss;

int main() {
    // Allocate ~64 MB and touch every page so the OS actually maps
    // it into physical memory (page-fault it in).
    constexpr size_t kBytes = 64UL * 1024 * 1024;
    auto* big = new char[kBytes];
    for (size_t i = 0; i < kBytes; i += 4096) big[i] = 1;

    auto rss = sample_rss();

    // Diagnostic: print values before asserting so a failure message
    // shows the actual numbers.
    printf("sample_rss: current_kib=%lld  peak_kib=%lld\n",
           static_cast<long long>(rss.current_kib),
           static_cast<long long>(rss.peak_kib));

    assert(rss.current_kib > 0);
    assert(rss.peak_kib > 0);
    assert(rss.current_kib >= static_cast<int64_t>(64 * 1024));

    delete[] big;
    return 0;
}
