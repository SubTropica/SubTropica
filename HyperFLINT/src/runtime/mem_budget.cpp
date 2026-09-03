// mem_budget — implementation.  See include/hyperflint/runtime/
// mem_budget.hpp for the full contract (issue #52 round 4; adversarial
// diff review 2026-08-21 folded: message published via g_msg_ready
// release/acquire, disarm handshake between the graceful and hard
// stages, integer-only formatting on the hard path, guarded thread
// lifecycle).

#include "hyperflint/runtime/mem_budget.hpp"
#include "hyperflint/runtime/env_flags.hpp"  // HF_FLAG_MEM_BUDGET_MB

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <sys/resource.h>
#include <unistd.h>

namespace hyperflint { namespace runtime {
namespace {

// 0 = unlimited.  Reset per bridge-op entry from HF_MEM_BUDGET_MB.
std::atomic<double>  g_budget_mb{0.0};
std::atomic<bool>    g_tripped{false};
// Message handshake (review S1): the CAS winner composes into a LOCAL
// buffer, copies into g_trip_msg, THEN publishes via g_msg_ready
// (release).  Readers use g_trip_msg only after an acquire load of
// g_msg_ready sees true — otherwise they fall back to the generic
// text.  This closes the race where an observer saw g_tripped==true
// while the winner's snprintf was still writing.
std::atomic<bool>    g_msg_ready{false};
char g_trip_msg[256] = {0};
// Disarm handshake (review S2/S3): once true, the watchdog neither
// samples nor hard-exits, and checkpoints stop throwing.  Set by
// mem_budget_disarm() when the engine has returned a completed result
// (a finished computation must not be killed during emission), and by
// mem_budget_throw_if_hit() when the graceful verdict is being raised
// (the hard stage must not race the structured JSON path).
std::atomic<bool>    g_disarmed{false};
// Checkpoint sampling gate: at most one getrusage per ~100 ms.  A
// counter stride was tried first and REJECTED: a region with fewer
// calls than the stride samples only once (T2 on the issue-52
// fast-blower payload never tripped); a time gate is responsive
// regardless of call count (steady_clock read ~20 ns).
std::atomic<int64_t> g_last_sample_ns{0};

// Watchdog machinery.  Checkpoints fire only where the computation
// passes a call site; a single long-running entry (the issue-52
// detonator payloads) can allocate tens of GB between call sites.
// When armed, one sampler thread checks every 250 ms; on breach it
// trips the flag (a checkpoint within the grace window still produces
// the graceful {"budget_exceeded":true} JSON) and, where the HARD
// stage is enabled, ends the process cleanly-and-loudly after the
// grace period: one structured line on fd 2 via write(2), then
// _exit(97).  Hard stage is OPT-IN FROM THE CLI MAIN ONLY: in the
// LibraryLink dylib an _exit would take down the host Wolfram kernel.
std::atomic<bool> g_hard_exit_enabled{false};
std::atomic<bool> g_watchdog_stop{false};
std::thread       g_watchdog;
std::mutex        g_watchdog_mtx;   // guards g_watchdog start (review N2)

constexpr int    kHardExitCode = 97;   // decoded by SubTropica.wl
constexpr double kGraceSeconds = 10.0;
constexpr auto   kSamplePeriod = std::chrono::milliseconds(250);

double peak_rss_mb_now() {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return -1.0;
#if defined(__APPLE__)
    // macOS: ru_maxrss is in BYTES (same normalization as
    // diagnostics/step_trace_rss).
    return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
#else
    // Linux: ru_maxrss is in KiB.
    return static_cast<double>(ru.ru_maxrss) / 1024.0;
#endif
}

// Trip the flag; the CAS winner composes and publishes the message.
// Safe from any thread; no allocation.
void trip(double peak, double budget, const char* where) {
    bool expected = false;
    if (g_tripped.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        char local[sizeof(g_trip_msg)];
        std::snprintf(local, sizeof(local),
            "memory budget exceeded: peak RSS %lld MB >= %lld MB budget "
            "(at %s); raise or unset HF_MEM_BUDGET_MB to allow more",
            static_cast<long long>(peak), static_cast<long long>(budget),
            where);
        std::memcpy(g_trip_msg, local, sizeof(g_trip_msg));
        g_msg_ready.store(true, std::memory_order_release);
    }
}

void watchdog_main() {
    using clock = std::chrono::steady_clock;
    clock::time_point breach_t{};
    while (!g_watchdog_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(kSamplePeriod);
        if (g_disarmed.load(std::memory_order_acquire)) continue;
        const double budget = g_budget_mb.load(std::memory_order_relaxed);
        if (budget <= 0.0) continue;
        const double peak = peak_rss_mb_now();
        if (peak < 0.0 || peak < budget) continue;
        trip(peak, budget, "watchdog");
        if (!g_hard_exit_enabled.load(std::memory_order_relaxed)) continue;
        if (breach_t == clock::time_point{}) breach_t = clock::now();
        const double since = std::chrono::duration<double>(
            clock::now() - breach_t).count();
        if (since < kGraceSeconds) continue;
        // Re-check the disarm handshake at the last moment: a graceful
        // checkpoint may have taken the verdict during this sleep.
        if (g_disarmed.load(std::memory_order_acquire)) continue;
        // Grace expired with no graceful checkpoint reached: end the
        // process loudly-but-cleanly.  write(2) + _exit(2) only, and
        // integer-only formatting (%f would allocate via __dtoa on
        // macOS — this path must survive memory pressure).
        char line[256];
        const int n = std::snprintf(line, sizeof(line),
            "HF_MEM_BUDGET_MB hard-stop: peak RSS %lld MB >= %lld MB "
            "budget and no graceful checkpoint within %d s; "
            "exiting %d instead of running the machine out of memory\n",
            static_cast<long long>(peak), static_cast<long long>(budget),
            static_cast<int>(kGraceSeconds), kHardExitCode);
        if (n > 0) {
            ssize_t ignored = write(2, line, static_cast<size_t>(n));
            (void)ignored;
        }
        _exit(kHardExitCode);
    }
}

}  // namespace

void mem_budget_reset_from_env() {
    double mb = 0.0;
    if (const char* e = HF_FLAG_MEM_BUDGET_MB) {
        char* end = nullptr;
        const double v = std::strtod(e, &end);
        // Reject non-finite values too ("inf" would arm a watchdog
        // that can never trip).
        if (end != e && std::isfinite(v) && v > 0.0) mb = v;
    }
    g_budget_mb.store(mb, std::memory_order_relaxed);
    g_last_sample_ns.store(0, std::memory_order_relaxed);
    g_msg_ready.store(false, std::memory_order_relaxed);
    g_trip_msg[0] = '\0';  // single-threaded op entry: plain write is fine
    g_disarmed.store(false, std::memory_order_relaxed);
    g_tripped.store(false, std::memory_order_release);
}

void mem_budget_enable_hard_exit() {
    g_hard_exit_enabled.store(true, std::memory_order_relaxed);
}

void mem_budget_disarm() {
    g_disarmed.store(true, std::memory_order_release);
}

bool mem_budget_hit(const char* where) {
    if (g_disarmed.load(std::memory_order_relaxed)) return false;
    if (g_tripped.load(std::memory_order_relaxed)) return true;
    const double budget = g_budget_mb.load(std::memory_order_relaxed);
    if (budget <= 0.0) return false;
    // Time-gate: sample at most every ~100 ms (racy CAS-free gate —
    // a rare duplicate sample across threads is harmless).
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t last = g_last_sample_ns.load(std::memory_order_relaxed);
    if (now_ns - last < 100'000'000) return false;
    g_last_sample_ns.store(now_ns, std::memory_order_relaxed);
    const double peak = peak_rss_mb_now();
    if (peak < 0.0 || peak < budget) return false;
    trip(peak, budget, where);
    return true;
}

void mem_budget_throw_if_hit(const char* where) {
    if (g_disarmed.load(std::memory_order_relaxed)) return;
    if (!g_tripped.load(std::memory_order_acquire)) return;
    // Take the graceful verdict: block the hard stage first (S2), so
    // the watchdog cannot _exit while the structured JSON is built.
    mem_budget_disarm();
    std::string msg = g_msg_ready.load(std::memory_order_acquire)
        ? std::string(g_trip_msg)
        : std::string("memory budget exceeded (at ") + where + ")";
    throw MemBudgetExceeded(msg);
}

void check_mem_budget(const char* where) {
    if (mem_budget_hit(where)) mem_budget_throw_if_hit(where);
}

MemBudgetWatchdog::MemBudgetWatchdog() {
    if (g_budget_mb.load(std::memory_order_relaxed) <= 0.0) return;
    std::lock_guard<std::mutex> lk(g_watchdog_mtx);
    if (g_watchdog.joinable()) return;  // one per process; ops are serial
    g_watchdog_stop.store(false, std::memory_order_relaxed);
    try {
        g_watchdog = std::thread(watchdog_main);
        running_ = true;
    } catch (...) {
        // Thread creation failed (resource pressure): fall back to
        // checkpoints-only silently — the fuse degrades, never breaks
        // the computation.
        running_ = false;
    }
}

MemBudgetWatchdog::~MemBudgetWatchdog() {
    if (!running_) return;
    g_watchdog_stop.store(true, std::memory_order_relaxed);
    try {
        if (g_watchdog.joinable()) g_watchdog.join();
    } catch (...) {
        // join() can throw std::system_error; never let a destructor
        // terminate the process over a diagnostics thread.
    }
}

}}  // namespace hyperflint::runtime
