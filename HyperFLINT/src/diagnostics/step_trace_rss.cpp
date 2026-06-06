// step_trace_rss — implementation.
// See include/hyperflint/diagnostics/step_trace_rss.hpp for design notes.

#include "hyperflint/diagnostics/step_trace_rss.hpp"

#include <sys/resource.h>  // getrusage, RUSAGE_SELF, struct rusage
#include <unistd.h>        // getpid
#include <cstdio>          // popen, pclose, snprintf
#include <cstdlib>         // (completeness; getpid is in unistd.h)

namespace hyperflint {

/// Read current RSS (KiB) via `ps -o rss=` for this process.
/// Returns -1 if popen fails or the output cannot be parsed.
/// Cost: one subprocess fork, ~50-150 µs on macOS.  Acceptable at
/// ~10 sample points per HF run (Phase 0 cadence).
static int64_t ps_rss_kib() {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "ps -o rss= -p %d", static_cast<int>(getpid()));
    FILE* f = popen(cmd, "r");
    if (!f) return -1;
    long kib = -1;
    if (fscanf(f, "%ld", &kib) != 1) kib = -1;
    (void)pclose(f);
    return static_cast<int64_t>(kib);
}

RssSample sample_rss() {
    RssSample s{-1, -1};  // explicit error sentinel before partial-success fills fields

    // Peak RSS via getrusage.
    // macOS reports ru_maxrss in bytes; Linux reports in KiB.
    // Both cases normalise to KiB here.
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
#ifdef __APPLE__
        s.peak_kib = static_cast<int64_t>(ru.ru_maxrss) / 1024;
#else
        s.peak_kib = static_cast<int64_t>(ru.ru_maxrss);
#endif
    } else {
        s.peak_kib = -1;
    }

    // Current (working set) RSS via ps.
    s.current_kib = ps_rss_kib();

    return s;
}

}  // namespace hyperflint
