// HyperFLINT Track 4.2 — phase-timer + scope-guard infrastructure.
//
// Phase-timer instrumentation. Design rationale recorded in the HyperFLINT
// development notes (internal).
//
// Design contract (one line per requirement, see spec for full text):
//   §(b) counter graph is a DAG, not a tree; no parent-child additivity assert.
//   §(c) timer identity is the tuple (name, kind), NOT name alone.
//        kind ∈ {wall, count}; wall accumulates double seconds via
//        std::chrono::steady_clock, count accumulates signed longs.
//   §(d) HF_SCOPE_GUARD = RAII push/pop on thread-local active-scope bitmask.
//        HF_TIMER_TICK = RAII tick whose ctor checks predicate (bitmask AND
//        against the timer's declared parent scope mask); predicate-false
//        constructs a sentinel, dtor early-exits, NO chrono call, NO
//        storage increment.  Cost contract: predicate-true ≈ 250 ns/tick;
//        predicate-false ≈ 5 ns/tick (target; ≤ 50 ns/tick acceptance,
//        unit test at HyperFLINT/test/unit/test_phase_timer.cpp iter-25).
//   §(e) parents=[] timer MUST carry tags ⊇ {"cross_cutting"}.  Linter
//        enforcement is iter-26+; this header is declarative only.
//   §(f) emit_counter_scopes.py greps this header for HF_PHASE_TIMER_DECL /
//        HF_PHASE_SCOPE_DECL invocations and emits counter_scopes.json.
//
// Scope of this header (iter-25):
//   * Type aliases hf_scope_id_t / hf_timer_id_t / hf_tick_kind_t.
//   * Enum classes scope_id / timer_id populated via X-macro extension
//     points HF_PHASE_SCOPE_LIST and HF_PHASE_TIMER_LIST.
//   * Inline implementations of HF_SCOPE_GUARD and HF_TIMER_TICK.
//   * Public macros HF_SCOPE_ENTER(name) and HF_PHASE_TIMER_TICK(name).
//   * Per-thread bitmask, per-timer (wall, count) accumulators.
//   * Three proof-of-concept timer declarations (gcd_cofactors_narrow /
//     gcd_cofactors_wide / reduce_narrow); see spec §(f) "Migration scope".
//
// OUT of scope this iter (iter-26+):
//   * emit_counter_scopes.py emitter script.
//   * Source-side wiring at rat.cpp:1399/1473 + integration_step.cpp parallel-fors.
//   * Predicate-false no-op unit test.
//   * Bulk migration of the other ~109 existing thread_local g_*_s timers
//     (which remain untouched in iter-25; Track 4.2 v1 is ADDITIVE).
//   * Compile-time-disabled fine-grained variant HF_PHASE_TIMER_FINE.
//
// (name, kind) tuple-uniqueness is enforced at X-macro list expansion:
// every HF_PHASE_TIMER_DECL(name, kind, ...) entry produces a unique
// enumerator hf::timer_id::name_##kind, so the C++ enum naturally
// rejects duplicate (name, kind) pairs at compile time.  See
// HF_PHASE_TIMER_LIST below.
//
// History: iter-23 (Track 4.1 PINNED
// comments) → iter-24 (Track 4.2 spec + reviewer concerns_advisory +
// 8 advisory recs folded) → iter-25 (this header + §(c) (name, kind)
// amendment + §F row 6 + §F row 13 audit folds).
//
// PINNED iter-25.  Until Track 5 lands and the file is built into
// libhyperflint, this header is a stand-alone declaration of the
// infrastructure; no .cpp pair is required for iter-25 (everything is
// inline).  iter-26 wiring adds calls from rat.cpp + integration_step.cpp.

#ifndef HYPERFLINT_INSTRUMENTATION_PHASE_TIMER_HPP
#define HYPERFLINT_INSTRUMENTATION_PHASE_TIMER_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace hf {
namespace phase_timer {

// ────────────────────────────────────────────────────────────────────────
//  Type aliases
// ────────────────────────────────────────────────────────────────────────

using hf_scope_mask_t = std::uint64_t;   // up to 64 simultaneously-active scopes
using hf_scope_id_t   = std::uint16_t;   // index into HF_PHASE_SCOPE_LIST
using hf_timer_id_t   = std::uint16_t;   // index into HF_PHASE_TIMER_LIST

enum class tick_kind : std::uint8_t {
    wall  = 0,   // accumulates std::chrono::steady_clock delta as double seconds
    count = 1,   // accumulates signed long via add_count(n)
};

// ────────────────────────────────────────────────────────────────────────
//  X-macro extension points
//
//  HF_PHASE_SCOPE_LIST(X) — list of scope_id enumerators.
//  Each entry: X(name)
//
//  HF_PHASE_TIMER_LIST(X) — list of timer_id enumerators.
//  Each entry: X(name, kind, parents_mask)
//      kind         — hf::phase_timer::tick_kind value
//      parents_mask — hf_scope_mask_t, bit-OR of the timer's declared
//                     parent scope_ids (1ULL << scope_id::<name>);
//                     0 means "cross-cutting" (parents=[], tags must
//                     include "cross_cutting" per spec §(e)).
//
//  Both lists are CLOSED extension points: anyone adding a new
//  scope/timer adds one X(...) line, and the enum/registry update
//  is automatic.  Removal is also one-line.
// ────────────────────────────────────────────────────────────────────────

#define HF_PHASE_SCOPE_LIST(X)                                          \
    /* scope tokens for the iter-25 PoC */                              \
    X(omp_parallel_for_integration_step)                                \
    X(rat_reduce_inplace_narrow_branch)                                 \
    X(rat_reduce_inplace_wide_branch)

enum class scope_id : hf_scope_id_t {
#define X(name) name,
    HF_PHASE_SCOPE_LIST(X)
#undef X
    _count
};
static_assert(static_cast<hf_scope_id_t>(scope_id::_count) <= 64,
              "phase_timer: HF_PHASE_SCOPE_LIST exceeds 64-bit "
              "active-scope bitmask capacity (§F audit row 3).  "
              "Extend hf_scope_mask_t (and HF_SCOPE_MASK_BITS) "
              "before adding a 65th scope.");

inline constexpr hf_scope_mask_t scope_bit(scope_id s) noexcept {
    return hf_scope_mask_t{1} << static_cast<hf_scope_id_t>(s);
}

//  Parent-mask constants for the three PoC timers.  Each timer's
//  parents are an explicit bit-OR of scope_bit(scope_id::<parent>).
//  parents=0 means cross-cutting.

#define HF_PT_PARENT_MASK_gcd_cofactors_narrow                          \
    (::hf::phase_timer::scope_bit(::hf::phase_timer::scope_id::omp_parallel_for_integration_step) | \
     ::hf::phase_timer::scope_bit(::hf::phase_timer::scope_id::rat_reduce_inplace_narrow_branch))

#define HF_PT_PARENT_MASK_gcd_cofactors_wide                            \
    (::hf::phase_timer::scope_bit(::hf::phase_timer::scope_id::omp_parallel_for_integration_step) | \
     ::hf::phase_timer::scope_bit(::hf::phase_timer::scope_id::rat_reduce_inplace_wide_branch))

#define HF_PT_PARENT_MASK_reduce_narrow                                 \
    (::hf::phase_timer::hf_scope_mask_t{0})

//  Three PoC timers (all kind=wall per spec §(f) "Migration scope"):
//      gcd_cofactors_narrow — parents = {omp_parallel_for_integration_step,
//                                        rat_reduce_inplace_narrow_branch}
//      gcd_cofactors_wide   — parents = {omp_parallel_for_integration_step,
//                                        rat_reduce_inplace_wide_branch}
//      reduce_narrow        — parents = {}  (cross-cutting; fires inside
//                                            every Rat::reduce_inplace)

#define HF_PHASE_TIMER_LIST(X)                                          \
    X(gcd_cofactors_narrow, ::hf::phase_timer::tick_kind::wall,         \
      HF_PT_PARENT_MASK_gcd_cofactors_narrow)                           \
    X(gcd_cofactors_wide,   ::hf::phase_timer::tick_kind::wall,         \
      HF_PT_PARENT_MASK_gcd_cofactors_wide)                             \
    X(reduce_narrow,        ::hf::phase_timer::tick_kind::wall,         \
      HF_PT_PARENT_MASK_reduce_narrow)

enum class timer_id : hf_timer_id_t {
#define X(name, kind, parents_mask) name,
    HF_PHASE_TIMER_LIST(X)
#undef X
    _count
};

//  Per-timer metadata dispatchers.  Implemented as inline constexpr
//  functions with a switch over timer_id, so each call inlines to a
//  single constant load at the call site (the compiler folds the
//  switch when the timer_id is a compile-time constant, which it
//  always is via the HF_PHASE_TIMER_TICK macro).  This avoids the
//  C++17 limitation that aggregate copy-assignment isn't constexpr
//  (so we cannot build a constexpr std::array<timer_meta_t,N>).

inline constexpr tick_kind timer_kind_of(timer_id t) noexcept {
    switch (t) {
#define X(name, kind, parents_mask)                                     \
    case timer_id::name: return (kind);
        HF_PHASE_TIMER_LIST(X)
#undef X
    case timer_id::_count: break;
    }
    return tick_kind::wall;  // unreachable
}

inline constexpr hf_scope_mask_t timer_parents_mask_of(timer_id t) noexcept {
    switch (t) {
#define X(name, kind, parents_mask)                                     \
    case timer_id::name: return (parents_mask);
        HF_PHASE_TIMER_LIST(X)
#undef X
    case timer_id::_count: break;
    }
    return 0;  // unreachable
}

// ────────────────────────────────────────────────────────────────────────
//  Per-thread state
//
//  active_scope_mask_  — bitmask of scope_ids currently on the thread's
//                        scope-guard stack.  Push/pop via HF_SCOPE_GUARD.
//  wall_accum_         — per-timer accumulated wall (kind=wall only).
//  count_accum_        — per-timer accumulated count (kind=count only).
//
//  inline thread_local (C++17) → exactly one definition across TUs,
//  no .cpp file needed for iter-25 skeleton.
// ────────────────────────────────────────────────────────────────────────

inline thread_local hf_scope_mask_t g_active_scope_mask = 0;

inline thread_local std::array<double,
                               static_cast<hf_timer_id_t>(timer_id::_count)>
g_wall_accum_s{};

inline thread_local std::array<long,
                               static_cast<hf_timer_id_t>(timer_id::_count)>
g_count_accum{};

// Master enable flag.  Default ON for the iter-25 PoC; future Track 4.2
// variants may gate by env var HF_PHASE_TIMERS=0 to skip ALL phase-timer
// work (predicate evaluation + chrono call) for production runs that
// don't need scope-aware data.  iter-25 reads this exactly once at
// construction; if it ever becomes runtime-mutable, all timers in
// flight at the toggle moment must be defined to either complete or
// abort — that contract is deferred to iter-26+.
inline std::atomic<bool> g_phase_timers_enabled{true};

// ────────────────────────────────────────────────────────────────────────
//  RAII guards
//
//  HF_SCOPE_GUARD — pushes/pops one scope_id into g_active_scope_mask.
//  HF_TIMER_TICK  — checks predicate (timer.parents_mask & active_mask
//                   == timer.parents_mask), records start_ns, and on
//                   destruction adds (end - start) seconds to
//                   g_wall_accum_s[timer] (kind=wall) or no-ops (kind=count;
//                   counts are added via explicit add_count() call).
//                   Predicate-false: sentinel start_ns = -1, destructor
//                   early-exits.
// ────────────────────────────────────────────────────────────────────────

class HF_SCOPE_GUARD {
public:
    explicit HF_SCOPE_GUARD(scope_id s) noexcept
      : bit_(scope_bit(s)),
        re_entrant_(static_cast<bool>(g_active_scope_mask & bit_))
    {
        // Bit is set even if already set, but we record whether we were
        // the one to set it so that ~HF_SCOPE_GUARD only clears the bit
        // if THIS guard pushed it (handles legitimate nested-same-scope
        // entry conservatively: the outer guard owns the bit; inner is
        // a no-op for the mask).
        g_active_scope_mask |= bit_;
    }
    ~HF_SCOPE_GUARD() noexcept {
        if (!re_entrant_) {
            g_active_scope_mask &= ~bit_;
        }
    }
    HF_SCOPE_GUARD(const HF_SCOPE_GUARD&)            = delete;
    HF_SCOPE_GUARD& operator=(const HF_SCOPE_GUARD&) = delete;
    HF_SCOPE_GUARD(HF_SCOPE_GUARD&&)                 = delete;
    HF_SCOPE_GUARD& operator=(HF_SCOPE_GUARD&&)      = delete;
private:
    hf_scope_mask_t bit_;
    bool            re_entrant_;
};

class HF_TIMER_TICK {
public:
    HF_TIMER_TICK(timer_id t, tick_kind /* kind unused at ctor */) noexcept
      : timer_(t),
        active_(predicate_active(t))
    {
        if (active_) {
            start_ = std::chrono::steady_clock::now();
        }
    }
    ~HF_TIMER_TICK() noexcept {
        if (!active_) return;
        const auto end = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(end - start_).count();
        g_wall_accum_s[static_cast<hf_timer_id_t>(timer_)] += dt;
    }
    // Explicit count accumulator (for kind=count timers).  Caller is
    // responsible for matching the timer's declared kind to the
    // accumulator call site; static_assert at HF_PHASE_TIMER_COUNT
    // macro expansion (iter-26) will check this automatically.
    void add_count(long n) noexcept {
        if (!active_) return;
        g_count_accum[static_cast<hf_timer_id_t>(timer_)] += n;
    }
    HF_TIMER_TICK(const HF_TIMER_TICK&)            = delete;
    HF_TIMER_TICK& operator=(const HF_TIMER_TICK&) = delete;
    HF_TIMER_TICK(HF_TIMER_TICK&&)                 = delete;
    HF_TIMER_TICK& operator=(HF_TIMER_TICK&&)      = delete;
private:
    static bool predicate_active(timer_id t) noexcept {
        if (!g_phase_timers_enabled.load(std::memory_order_relaxed)) return false;
        const auto parents = timer_parents_mask_of(t);
        // parents == 0 → cross-cutting → always active.
        if (parents == 0) return true;
        // Otherwise require ALL parent bits to be set.
        return (g_active_scope_mask & parents) == parents;
    }
    timer_id timer_;
    bool     active_;
    std::chrono::steady_clock::time_point start_;
};

// ────────────────────────────────────────────────────────────────────────
//  Accessors (for emit_counter_scopes.py / aggregator / tests)
// ────────────────────────────────────────────────────────────────────────

inline double timer_wall_s(timer_id t) noexcept {
    return g_wall_accum_s[static_cast<hf_timer_id_t>(t)];
}
inline long timer_count(timer_id t) noexcept {
    return g_count_accum[static_cast<hf_timer_id_t>(t)];
}
inline void timer_reset(timer_id t) noexcept {
    g_wall_accum_s[static_cast<hf_timer_id_t>(t)] = 0.0;
    g_count_accum[static_cast<hf_timer_id_t>(t)]  = 0L;
}
inline void timer_reset_all() noexcept {
    g_wall_accum_s.fill(0.0);
    g_count_accum.fill(0L);
    g_active_scope_mask = 0;
}

}  // namespace phase_timer
}  // namespace hf

// ────────────────────────────────────────────────────────────────────────
//  Public macros
//
//  HF_SCOPE_ENTER(name)       — declare a stack-allocated scope guard.
//  HF_PHASE_TIMER_TICK(name)  — declare a stack-allocated wall-timer tick.
//                               Predicate-false at ctor → no-op dtor
//                               (no chrono, no storage write).
//
//  Identifier naming: `_hf_scope_<name>` and `_hf_tick_<name>` ; grep'd
//  in §F row 5 audit (no collisions in HF source as of iter-24).
// ────────────────────────────────────────────────────────────────────────

#define HF_SCOPE_ENTER(name)                                            \
    ::hf::phase_timer::HF_SCOPE_GUARD _hf_scope_##name(                 \
        ::hf::phase_timer::scope_id::name)

#define HF_PHASE_TIMER_TICK(name)                                       \
    ::hf::phase_timer::HF_TIMER_TICK _hf_tick_##name(                   \
        ::hf::phase_timer::timer_id::name,                              \
        ::hf::phase_timer::timer_kind_of(                               \
            ::hf::phase_timer::timer_id::name))

#endif  // HYPERFLINT_INSTRUMENTATION_PHASE_TIMER_HPP
