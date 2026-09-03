// mem_budget — process-global HF_MEM_BUDGET_MB fuse (issue #52 round 4,
// 2026-08-20).
//
// A memory analogue of the LR time budget (integrator/lr_search.cpp
// LrBudget): a default-OFF safety net that converts an imminent
// memory-exhaustion death (silent SIGSEGV once the kernel cannot map
// another page — the "exit 139 with empty stderr" signature of issue
// #52 round 4) into a structured {"budget_exceeded":true} response.
//
//   HF_MEM_BUDGET_MB   double, MB of peak RSS; 0 / unset / unparsable
//                      = UNLIMITED, and every check is then a relaxed
//                      atomic load — the default behavior is
//                      byte-identical to the pre-fuse engine (same hard
//                      requirement as LrBudget).
//
// Semantics: the budget is on getrusage(RUSAGE_SELF).ru_maxrss — the
// process-lifetime PEAK RSS, normalized (macOS reports bytes, Linux
// KiB; same normalization as diagnostics/step_trace_rss).  Peak, not
// current, deliberately: it is fork-free and monotonic (a fuse should
// stay tripped), and it never needs an allocation or a subprocess at
// the moment of extreme memory pressure — sample_rss()'s current-RSS
// half runs `ps` via popen(), and fork can fail exactly when the fuse
// must fire.
//
// OMP discipline (same as the narrow-ctx / nonlinear-den flags, R24
// chain 17): NEVER throw inside a parallel region (escape is
// implementation-defined; Apple-clang/libomp calls std::terminate).
// Workers call mem_budget_hit() — check + trip the process-global
// flag, no throw — and skip their remaining work; host code observes
// the flag after the implicit barrier via mem_budget_throw_if_hit().
// Serial sites may use check_mem_budget(), which throws immediately.
// Skipped work never leaks into a result: a tripped flag always
// reaches a post-barrier throw before the step's output is assembled.
#pragma once
#include <stdexcept>

namespace hyperflint { namespace runtime {

struct MemBudgetExceeded : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Re-read HF_MEM_BUDGET_MB and clear the trip flag.  Called once per
// bridge-op entry (single-threaded), mirroring LrBudget's per-entry
// reset, so a per-call env push takes effect per request even in a
// long-lived process (the LibraryLink dylib inside a Wolfram kernel).
void mem_budget_reset_from_env();

// No-throw check, safe inside OMP regions: returns true (and trips
// the global flag) when the budget is finite and peak RSS has reached
// it.  Off cost: one relaxed load.  Armed cost: one getrusage(2)
// syscall at most every ~100 ms (time-gated; a per-call-count stride
// was tried and rejected — sparse call sites then sample only once).
bool mem_budget_hit(const char* where);

// Host-side post-barrier observation: throw MemBudgetExceeded if any
// worker tripped the flag during the parallel region.
void mem_budget_throw_if_hit(const char* where);

// Serial-site check-and-throw.
void check_mem_budget(const char* where);

// Enable the watchdog's HARD stage (structured stderr line + _exit(97)
// after a 10 s grace with no graceful checkpoint).  CLI main ONLY:
// in the LibraryLink dylib an _exit would take down the host Wolfram
// kernel, so the dylib transport stays checkpoints-only.
void mem_budget_enable_hard_exit();

// Disarm the fuse for the remainder of the request.  Called by the
// handler the moment the engine returns a completed result, BEFORE
// response emission: a finished computation must never be hard-killed
// while its answer is being serialized (the peak-RSS event already
// happened; killing now would only discard the result).  Also set
// internally when a graceful verdict is being raised, so the hard
// stage cannot race the structured JSON path.
void mem_budget_disarm();

// RAII sampler thread for one bridge op: samples peak RSS every
// 250 ms while armed (budget > 0), trips the flag on breach so the
// next checkpoint exits gracefully, and (hard stage only) ends the
// process loudly-but-cleanly when no checkpoint arrives within the
// grace window.  Construct AFTER mem_budget_reset_from_env(); the
// destructor stops and joins the thread on every exit path.
class MemBudgetWatchdog {
public:
    MemBudgetWatchdog();
    ~MemBudgetWatchdog();
    MemBudgetWatchdog(const MemBudgetWatchdog&) = delete;
    MemBudgetWatchdog& operator=(const MemBudgetWatchdog&) = delete;
private:
    bool running_ = false;
};

}}  // namespace hyperflint::runtime
