// step_trace_rss — lightweight per-step RSS sampler.
//
// Provides two RSS measurements per sample point:
//   current_kib  — running RSS at sample time (via `ps -o rss=`; can
//                  decrease between samples as pages are released)
//   peak_kib     — process-lifetime monotonic peak RSS (via
//                  getrusage(RUSAGE_SELF).ru_maxrss)
//
// Intended use.  Phase 0 tasks 0-3 and 0-4 call sample_rss() at ~10
// points per HF run (step boundaries, per-integration-node entry/exit).
// The popen cost is ~50-150 µs/call and is negligible at that cadence.
// Higher-frequency sampling (Phase 1+) may want the faster macOS-native
// task_info(TASK_BASIC_INFO) path; that is out of scope for Phase 0.
//
// macOS vs Linux.  getrusage(2) reports ru_maxrss in bytes on macOS
// and in KiB on Linux; sample_rss() normalises to KiB on both platforms.
//
// Namespace.  Lives in hyperflint (project convention; not hf::diag).

#pragma once
#include <cstdint>

namespace hyperflint {

/// Memory usage at a single sample point.
struct RssSample {
    int64_t current_kib;  ///< Running RSS at sample time (KiB); -1 on error
    int64_t peak_kib;     ///< Process-lifetime peak RSS (KiB); -1 on error
};

/// Sample the current process RSS.
/// Thread-safe (each call spawns its own ps subprocess).
RssSample sample_rss();

}  // namespace hyperflint
