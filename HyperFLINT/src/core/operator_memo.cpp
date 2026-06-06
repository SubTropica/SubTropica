// HF FF Phase 5 §E Step E.2-impl-2 (iter-60-β.4).
//
// Implementation of:
//   - Global cache state (`g_rat_add_cache`, `g_reduce_cache`,
//     `g_lf_cache_outer`, `g_pf_cache_outer`, `g_transform_shuffle_cache`).
//   - Explicit template instantiations of `OperatorMemo<KeyT, ValueT>` for
//     the four trivial-key boundaries shipped at iter-60 (RatAdd, Reduce, LF,
//     PF; TransformShuffle stays implicit pending iter-61 ValueT decision).
//   - Env-gate predicates (`master_enabled`, `rat_add_enabled`, ...) per
//     §iter-59-fold-REQ-3 + REQ-7 + §5.
//   - counter_replay shim namespace (5 per-op stubs per §iter-59-fold-REQ-5).
//   - `clear_between_fixtures()` hook (no-op when wraps not yet wired in;
//     the iter-N+ per-op wraps will populate the caches).
//
// Iter-60 deliberately does NOT call any of the new wrap entry-points from
// production code paths — those wraps are landed at iter-61 (per-op wraps
// at the 5 boundaries). The current TU is library-link-clean and exercised
// only by `test_operator_memo.cpp`.
//
// The ValueT for the iter-60 instantiations:
//   - rat_add        : `Rat`
//   - reduce         : `ReduceValue` (pair of Polys; defined here)
//   - lf             : `LinearFactorization` (from algebra/linear_factors.hpp)
//   - pf             : `PartialFractionization` (from algebra/partial_fractions.hpp)
//   - transform_shuf : deferred to iter-61 (TransformResultSym threading).

#include "hyperflint/core/operator_memo.hpp"
#include "hyperflint/core/canonical_signature.hpp"

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/runtime/hf_thread_num.hpp"
#include "hyperflint/algebra/linear_factors.hpp"
#include "hyperflint/algebra/partial_fractions.hpp"
#include "hyperflint/integrator/transform.hpp"  // iter-62-β.2: TransformResultSym
#include "hyperflint/runtime/env_flags.hpp"     // §T7 third chunk: HF_FLAG_MI_* macros
#include "hyperflint/core/env_flags.hpp"        // §T7 fourth chunk: HF_FLAG_NAME_OPERATOR_MEMO_* / HF_FLAG_NAME_OP_MEMO_* macros

#include <atomic>
#include <chrono>     // §E iter-9 REQ-9: per-cache dtor wall instrumentation
#include <cstdint>
#include <cstdio>     // §E iter-7: stderr trace, composability abort
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

// §E iter-7: physical-memory probe (REQ-5 sysctlbyname + sysconf
// fallback) and post-step RSS read (getrusage).
#if defined(__APPLE__)
#  include <sys/sysctl.h>   // sysctlbyname("hw.memsize", ...)
#endif
#include <sys/resource.h>   // getrusage(RUSAGE_SELF, ...)
#include <unistd.h>         // sysconf(_SC_PHYS_PAGES), sysconf(_SC_PAGESIZE)

namespace hyperflint {

// ---------------------------------------------------------------------------
// Per-op value types (only the ones whose ValueT isn't an existing public
// HF type with a copy ctor).
// ---------------------------------------------------------------------------

// reduce_inplace post-state cached as a (num, den) pair plus a kind
// classifier (iter-62-β.1 — extension over iter-60 ReduceValue).
//
// `kind` is set by the wrap site (rat.cpp::reduce_inplace) from the
// pre-impl input characterisation:
//   0 = zero    (num.is_zero() at entry)
//   1 = narrow  (else; the general case, may have run narrow OR wide
//                  fallback internally — the wrap captures only the
//                  INPUT-characterisation, not the internal branch
//                  actually taken; replay accuracy is conservative
//                  per §iter-59-fold-REQ-5 §4.4-bis row 2)
//   2 = wide    (else if den.is_fmpq() at entry — the early-out at
//                  rat.cpp:1097 that bypasses gcd entirely)
struct ReduceValue {
    Poly num_post;
    Poly den_post;
    int  kind;
};

// ---------------------------------------------------------------------------
// Env-gate parsing — parsed once at first call to master_enabled().
// ---------------------------------------------------------------------------
namespace {

struct EnvGate {
    bool master           = false;
    bool off_rat_add      = false;
    bool off_reduce       = false;
    bool off_lf           = false;
    bool off_pf           = false;
    bool off_transform    = false;
    bool collision_log    = true;   // default ON when master is ON
    // iter-76 Option α (default cap=0 = LRU disabled; opt-in semantics):
    // The iter-76 cap-calibration sweep at tst2 (notes/.../iter76_cap_calibration_sweep_outputs)
    // characterised the LRU machinery overhead at OMP=13 across cap ∈ {0, 250,
    // 500, 1000, 2000, 5000}. Finding: per-insert LRU machinery (cap-check +
    // s.map.find + map.size lookup, even when eviction does NOT fire) imposes
    // STRUCTURAL wall overhead that exceeds HARD-4 (≥ −5 % wall reduction)
    // at ALL non-zero cap values:
    //   cap=  0: wall  +8.05 % (PASS) / rss  +95.06 % (FAIL HARD-3)
    //   cap=250: wall −15.94 % (FAIL) / rss  +13.84 % (PASS)
    //   cap=500: wall −13.93 % (FAIL) / rss  +18.88 % (PASS)
    //   cap=1000:wall −11.79 % (FAIL) / rss  +24.68 % (PASS; iter-75 default)
    //   cap=2000:wall  −9.45 % (FAIL) / rss  +33.30 % (FAIL)
    //   cap=5000:wall  −6.83 % (FAIL) / rss  +56.18 % (FAIL)
    // No cap value clears both HARDs simultaneously; the Pareto frontier is
    // empty. iter-76 ships cap=0 (LRU disabled) as the default → restores
    // iter-74 PASS_PARETO at default (wall −7.92 % at tst2). Users with
    // memory-constrained workloads opt in via env var:
    //   HF_OPERATOR_MEMO_LRU_CAP_PER_OP=N (N > 0).
    // The iter-75 LRU machinery (operator_memo.hpp::insert() body + the new
    // test_lru_cap_eviction subtest) stays in source for future per-cache
    // cap differentiation (iter-77+ Option β) or intrusive-list O(1) eviction
    // (iter-77+ Option ε); only the DEFAULT changes from 1000 → 0.
    // See iter76_cap_calibration_verdict.md §1 for the Pareto frontier table.
    std::size_t lru_cap   = 0;  // iter-76 default (LRU disabled; opt-in via env var)

    // §E iter-7: RSS-pressure-driven LRU eviction.
    bool        evict_on_rss            = false;  // master switch (default OFF)
    std::size_t evict_rss_threshold_b   = 0;      // bytes; 0 = "use 80% of phys"
    std::size_t evict_lru_batch         = 64;     // entries per cache per trigger
    bool        evict_strategy_fifo     = false;  // false = LRU (default)
    bool        evict_trace             = false;  // stderr trace
};

static bool env_truthy(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return false;
    return v[0] == '1';
}

static std::size_t env_size(const char* name, std::size_t fallback) {
    const char* v = std::getenv(name);
    if (!v || v[0] == '\0') return fallback;
    char* end = nullptr;
    long long parsed = std::strtoll(v, &end, 10);
    // iter-76 cap=0 sentinel: parsed == 0 is now a legitimate explicit value
    // (e.g., HF_OPERATOR_MEMO_LRU_CAP_PER_OP=0 = "LRU disabled" via the
    // operator_memo.hpp::insert() enable_lru gate). Only parse failure
    // (end == v) and negatives fall back. Pre-iter-76 used `<= 0` which
    // sabotaged the cap=0 sentinel and forced fallback.
    if (end == v || parsed < 0) return fallback;
    return static_cast<std::size_t>(parsed);
}

// §E iter-7 REQ-5: physical-memory probe.
//   Primary path: sysctlbyname("hw.memsize", ...) on Apple Silicon
//   (verified by `nm` post-build per design.md §3 and handoff Phase 7-β:
//   baseline build has `_sysctl U` only; iter-7 adds `_sysctlbyname U`).
//   Fallback: sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE) on Linux
//   or any platform where sysctlbyname fails to resolve. The fallback
//   is also the canonical Linux idiom for "amount of physical RAM in
//   bytes".
static std::size_t probe_physical_memory_bytes() {
#if defined(__APPLE__)
    {
        std::uint64_t mem = 0;
        std::size_t   sz  = sizeof(mem);
        if (sysctlbyname("hw.memsize", &mem, &sz, nullptr, 0) == 0
            && sz == sizeof(mem) && mem > 0) {
            return static_cast<std::size_t>(mem);
        }
    }
#endif
    // sysconf fallback.
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0) {
        return static_cast<std::size_t>(pages)
             * static_cast<std::size_t>(psize);
    }
    // Last-resort fallback: 8 GiB. Conservative for laptops; pessimistic
    // for workstations — but only reached when both probes fail.
    return static_cast<std::size_t>(8) << 30;
}

// §E iter-7: composability mutual-exclusion check (iter-6 design.md §6.3,
// iter-4 lessons_learned[11] pinned). HF_MI_COLLECT_OPTION_M_C=1 selects
// §6.M Option M.c (`mi_collect`-family pool-stickiness lever); §E is
// HF-internal-cache LRU eviction. Running both simultaneously confounds
// memory attribution. Fail-fast at first env_gate access.
static void check_composability_or_abort() {
    const char* mc = HF_FLAG_MI_COLLECT_OPTION_M_C;
    const char* eo = std::getenv(HF_FLAG_NAME_OP_MEMO_EVICT_ON_RSS);
    const bool mc_on = mc && mc[0] == '1';
    const bool eo_on = eo && eo[0] == '1';
    if (mc_on && eo_on) {
        std::fprintf(stderr,
            "[HF FATAL] HF_MI_COLLECT_OPTION_M_C=1 and "
            "HF_OP_MEMO_EVICT_ON_RSS=1 are mutually exclusive "
            "(operator_memo.cpp §E iter-7 composability gate; "
            "design.md §6.3, iter-4 lessons_learned[11]).\n");
        std::abort();
    }
}

EnvGate parse_env_gate() {
    EnvGate g;
    check_composability_or_abort();  // §E iter-7 fail-fast.
    g.master        = env_truthy(HF_FLAG_NAME_OPERATOR_MEMO);
    g.off_rat_add   = env_truthy(HF_FLAG_NAME_OPERATOR_MEMO_OFF_RAT_ADD);
    // iter-73 Option ρ-(b) per iter-72 §7-REVISED Option ν sweep D-i evidence:
    // REDUCE cache accounts for 91.6 % of parity_1 RSS overhead (Run-ν-5 wall
    // 24.07 s FASTER than master-OFF 29.78 s by 5.7 s). REDUCE cache is now
    // DEFAULT DISABLED. The dual-knob semantics preserve iter-72 sweep
    // reproducibility (REQ-3) and provide both opt-in and explicit force-off:
    //   - HF_OPERATOR_MEMO_ENABLE_REDUCE=1: opt-in to REDUCE cache.
    //   - HF_OPERATOR_MEMO_OFF_REDUCE=1   : explicit force-off (overrides
    //                                       ENABLE_REDUCE; preserves the
    //                                       iter-72 Run-ν-5 invocation idiom).
    //   - both absent                     : REDUCE cache disabled (default).
    // Per iter-73-α REQ-1 reviewer fold (agentId a0b6fd4ae8015acfb).
    if (const char* off_v = std::getenv(HF_FLAG_NAME_OPERATOR_MEMO_OFF_REDUCE);
        off_v && off_v[0] == '1') {
        g.off_reduce = true;  // explicit force-off (iter-72 sweep idiom).
    } else if (const char* en_v = std::getenv(HF_FLAG_NAME_OPERATOR_MEMO_ENABLE_REDUCE);
               en_v && en_v[0] == '1') {
        g.off_reduce = false;  // opt-in re-enable (benchmarking + sweep).
    } else {
        g.off_reduce = true;  // default: REDUCE cache disabled at iter-73+.
    }
    g.off_lf        = env_truthy(HF_FLAG_NAME_OPERATOR_MEMO_OFF_LF);
    g.off_pf        = env_truthy(HF_FLAG_NAME_OPERATOR_MEMO_OFF_PF);
    g.off_transform = env_truthy(HF_FLAG_NAME_OPERATOR_MEMO_OFF_TRANSFORM);
    // collision_log: default ON under master; user may explicitly OFF
    // via HF_OPERATOR_MEMO_COLLISION_LOG=0. The "0 means off, 1 means
    // on, missing means default-ON-under-master" rule:
    const char* clog = std::getenv(HF_FLAG_NAME_OPERATOR_MEMO_COLLISION_LOG);
    if (clog && clog[0] == '0') g.collision_log = false;
    // iter-76 Option α default: 0 (LRU disabled; opt-in semantics).
    // See the EnvGate struct member-initialiser comment above for the
    // full Pareto frontier evidence motivating the iter-75 → iter-76 default
    // change. The env_size() helper accepts parsed == 0 as legitimate per
    // iter-76 enable_lru gate refactor (`parsed < 0` falls back, `parsed == 0`
    // returns 0). Pre-iter-76 used `parsed <= 0` which sabotaged the cap=0
    // sentinel; iter-76 fixed that.
    g.lru_cap = env_size(HF_FLAG_NAME_OPERATOR_MEMO_LRU_CAP_PER_OP, 0);

    // §E iter-7 env-gate fields.
    g.evict_on_rss = env_truthy(HF_FLAG_NAME_OP_MEMO_EVICT_ON_RSS);
    {
        // Threshold: explicit env value (in MiB) wins; fallback = 80% of
        // probed physical memory.
        const char* v = std::getenv(HF_FLAG_NAME_OP_MEMO_EVICT_RSS_THRESHOLD_MB);
        if (v && v[0] != '\0') {
            char* end = nullptr;
            long long parsed = std::strtoll(v, &end, 10);
            if (end != v && parsed > 0) {
                g.evict_rss_threshold_b =
                    static_cast<std::size_t>(parsed) << 20;  // MiB → bytes
            }
        }
        if (g.evict_rss_threshold_b == 0) {
            const std::size_t phys = probe_physical_memory_bytes();
            // 80% of physical memory; integer arithmetic to avoid float.
            g.evict_rss_threshold_b = phys / 10 * 8;
        }
    }
    g.evict_lru_batch = env_size(HF_FLAG_NAME_OP_MEMO_EVICT_LRU_BATCH, 64);
    {
        const char* s = std::getenv(HF_FLAG_NAME_OP_MEMO_EVICT_STRATEGY);
        // "fifo" → FIFO; anything else (including "lru" or unset) → LRU.
        g.evict_strategy_fifo =
            (s != nullptr && std::strcmp(s, "fifo") == 0);
    }
    g.evict_trace = env_truthy(HF_FLAG_NAME_OP_MEMO_EVICT_TRACE);

    return g;
}

const EnvGate& env_gate() {
    static EnvGate g = parse_env_gate();
    return g;
}

// Re-parse for tests; replaces the static EnvGate snapshot under a mutex.
// REQ-10 (iter-7 reviewer agentId a3c81a04eceb31293): `g_env_reloaded` is
// `std::atomic<bool>` with acquire/release ordering so the concurrent
// fast-path read at `effective_env_gate()` does not race with the
// release-store at `reload_env_for_testing()`. Production never writes
// (reload is test-only) so the race was benign-in-practice, but the
// previous plain-bool form is UB under the standard memory model.
std::mutex            g_env_reload_mutex;
EnvGate               g_env_reload_value;
std::atomic<bool>     g_env_reloaded{false};

const EnvGate& effective_env_gate() {
    // Fast-path: no reload pending, return the cached static.
    // Acquire-load pairs with the release-store in reload_env_for_testing().
    if (!g_env_reloaded.load(std::memory_order_acquire)) return env_gate();
    std::lock_guard<std::mutex> lk(g_env_reload_mutex);
    return g_env_reload_value;
}

} // namespace

// ---------------------------------------------------------------------------
// SCALAR_REP=1 forced-disable predicate (FOLD-ER3 + §iter-59-fold-REQ-3).
// ---------------------------------------------------------------------------
namespace {

// Avoid pulling in runtime/scalar_rep.hpp's full surface — read the env
// var via the runtime macro home (iter-91 §T7 21st chunk
// Track-cache-scalar-rep macro-layer LAND) and apply our own
// stricter predicate. The HF runtime layer parses the same env var
// once at startup; our local re-parse is at first call to
// scalar_rep_active() and cached in a static. The local reader's
// predicate is `v[0]=='1'` (strict-1 prefix), distinct from the
// runtime accessor's `v[0]!='\0' && v[0]!='0'` (permissive
// non-empty-non-zero). See the env_flags.hpp comment block above
// the HF_FLAG_*SCALAR_REP* macros for the iter-91 adversarial-
// reviewer advisory on this predicate divergence (advisory only,
// out of scope for the iter-91 LAND).
bool scalar_rep_active() {
    static bool active = []{
        const char* v = HF_FLAG_USE_SCALAR_REP;
        return v && v[0] == '1';
    }();
    return active;
}

} // namespace

namespace operator_memo {

bool master_enabled() {
    return effective_env_gate().master;
}

bool rat_add_enabled() {
    const auto& g = effective_env_gate();
    // REQ-7 option b: Rat::add REMAINS ENABLED under SCALAR_REP=1.
    return g.master && !g.off_rat_add;
}

bool reduce_enabled() {
    const auto& g = effective_env_gate();
    // REQ-7 option b: reduce_inplace REMAINS ENABLED under SCALAR_REP=1.
    return g.master && !g.off_reduce;
}

bool lf_enabled() {
    const auto& g = effective_env_gate();
    // REQ-3 option b: lf is DISABLED under SCALAR_REP=1.
    if (scalar_rep_active()) return false;
    return g.master && !g.off_lf;
}

bool pf_enabled() {
    const auto& g = effective_env_gate();
    // REQ-3 option b: pf is DISABLED under SCALAR_REP=1.
    if (scalar_rep_active()) return false;
    return g.master && !g.off_pf;
}

bool transform_shuffle_enabled() {
    const auto& g = effective_env_gate();
    // Primary FOLD-ER3 mandate: transform_shuffle is DISABLED under SCALAR_REP=1.
    if (scalar_rep_active()) return false;
    return g.master && !g.off_transform;
}

std::size_t lru_cap_per_op() {
    return effective_env_gate().lru_cap;
}

bool collision_log_enabled() {
    const auto& g = effective_env_gate();
    return g.master && g.collision_log;
}

void reload_env_for_testing() {
    std::lock_guard<std::mutex> lk(g_env_reload_mutex);
    g_env_reload_value = parse_env_gate();
    // REQ-10: release-store pairs with the acquire-load fast-path read
    // in effective_env_gate().
    g_env_reloaded.store(true, std::memory_order_release);
    // Reset the scalar_rep_active() static the only way we can: the
    // function-level static initialiser is one-shot, so a test that
    // needs to toggle HF_USE_SCALAR_REP between iterations must
    // launch a subprocess. The reload here covers the HF_OPERATOR_MEMO
    // family; SCALAR_REP toggling for tests goes through subprocess
    // launch (test/test_operator_memo.cpp matches this pattern in
    // iter-61+ Step E.2-impl-2 body-fill).
}

// ---------------------------------------------------------------------------
// Global per-op cache singletons.
//
// Defined as function-local statics so initialisation order across TUs is
// well-defined (first-call initialisation per the Magic-Statics rule).
// Each per-op cache is referenced from the corresponding wrap site at
// iter-61+ via the `g_<op>_cache()` accessor.
// ---------------------------------------------------------------------------

} // namespace operator_memo

// Function-level statics: per-op cache instances. The accessor pattern
// keeps the linkage clean (no global ctors) and works under the static
// hyperflint library link model.

OperatorMemo<canonical_signature::RatAddKey, Rat>& g_rat_add_cache() {
    static OperatorMemo<canonical_signature::RatAddKey, Rat> c;
    return c;
}

OperatorMemo<canonical_signature::ReduceKey, ReduceValue>& g_reduce_cache() {
    static OperatorMemo<canonical_signature::ReduceKey, ReduceValue> c;
    return c;
}

OperatorMemo<canonical_signature::LfKey, LinearFactorization>& g_lf_cache_outer() {
    static OperatorMemo<canonical_signature::LfKey, LinearFactorization> c;
    return c;
}

OperatorMemo<canonical_signature::PfKey, PartialFractionization>& g_pf_cache_outer() {
    static OperatorMemo<canonical_signature::PfKey, PartialFractionization> c;
    return c;
}

// iter-62-β.2: transform_shuffle outer cache singleton.
// Declared in transform.cpp directly (TransformResultSym must be complete at
// the wrap callsite); operator_memo.hpp does not forward-declare this
// accessor because the TransformResultSym alias resolves to a std::vector<>
// of an incomplete-in-header type, which is awkward to forward-declare.
OperatorMemo<canonical_signature::TransformShuffleKey, TransformResultSym>&
g_transform_shuffle_cache() {
    static OperatorMemo<canonical_signature::TransformShuffleKey,
                        TransformResultSym> c;
    return c;
}

// ---------------------------------------------------------------------------
// reduce helpers (iter-62-β.1): hide ReduceValue inside this TU so the
// wrap site at rat.cpp::reduce_inplace need not see ReduceValue's
// definition. Declared in operator_memo.hpp's `operator_memo` namespace.
// ---------------------------------------------------------------------------
namespace operator_memo {

bool reduce_try_lookup_and_apply(
    const canonical_signature::ReduceKey& key,
    std::uint64_t key_hash,
    Poly& num_out,
    Poly& den_out,
    int& kind_out)
{
    // iter-70 REC-2: try_lookup returns std::shared_ptr<const ReduceValue>
    // (ref-counted handle, COW). The referent is const, so we cannot
    // move-from its fields — we copy into the by-reference slots instead.
    // The caller-by-value cost is identical to the iter-63 REQ-1 form
    // (one deep-copy of each Poly at assignment to num_out / den_out);
    // REC-2's net win is that the cache-side deep-copy under shared_lock
    // is eliminated (the lock-held window shrinks to O(1) ref-count++).
    auto cached_sp = g_reduce_cache().try_lookup(key, key_hash);
    if (!cached_sp) return false;
    num_out  = cached_sp->num_post;
    den_out  = cached_sp->den_post;
    kind_out = cached_sp->kind;
    return true;
}

void reduce_insert_with_kind(
    canonical_signature::ReduceKey key,
    std::uint64_t key_hash,
    const Poly& num_post,
    const Poly& den_post,
    int kind)
{
    g_reduce_cache().insert(
        std::move(key),
        key_hash,
        ReduceValue{Poly(num_post), Poly(den_post), kind});
}

// ---------------------------------------------------------------------------
// §E iter-7: RSS-pressure-driven LRU eviction at integration-step boundary.
// See notes/.../lever_e_op_memo_eviction_rss_pressure/design.md §6 +
// determinism_audit.md.
// ---------------------------------------------------------------------------
bool evict_on_rss_enabled() {
    return effective_env_gate().evict_on_rss;
}

std::size_t evict_rss_threshold_bytes() {
    return effective_env_gate().evict_rss_threshold_b;
}

std::size_t evict_lru_batch_size() {
    return effective_env_gate().evict_lru_batch;
}

bool evict_strategy_is_fifo() {
    return effective_env_gate().evict_strategy_fifo;
}

bool evict_trace_enabled() {
    return effective_env_gate().evict_trace;
}

std::size_t evict_lru_batch_all_caches(std::size_t n_per_cache) {
    if (n_per_cache == 0) return 0;
    std::size_t total = 0;
    // §E iter-9 REQ-9: per-cache dtor wall instrumentation. Under
    // HF_OP_MEMO_EVICT_TRACE=1 (`evict_trace_enabled()` true), emit one
    // `[hf-evict-percache] cache=X evicted=Y wall_s=Z` stderr line per
    // cache so the runner_iter9 measurement can attribute aggregate
    // dtor wall to its 5 contributors.
    //
    // Default-OFF posture: when the trace flag is false the per-cache
    // chrono::steady_clock now() calls are skipped via the if-branch
    // below, preserving the iter-7 baseline byte-id smoke (REQ-12) +
    // wall (no measurable overhead vs the iter-7 path).
    const bool trace = evict_trace_enabled();

    if (!trace) {
        total += g_rat_add_cache().evict_lru_batch(n_per_cache);
        total += g_reduce_cache().evict_lru_batch(n_per_cache);
        total += g_lf_cache_outer().evict_lru_batch(n_per_cache);
        total += g_pf_cache_outer().evict_lru_batch(n_per_cache);
        total += g_transform_shuffle_cache().evict_lru_batch(n_per_cache);
        return total;
    }

    // Trace path: tick-tock around each call.
    using clock = std::chrono::steady_clock;
    auto emit = [](const char* cache_name, std::size_t evicted,
                   double wall_s) {
        std::fprintf(stderr,
            "[hf-evict-percache] cache=%s evicted=%zu wall_s=%.6f\n",
            cache_name, evicted, wall_s);
    };

    {
        const auto t0 = clock::now();
        const std::size_t e = g_rat_add_cache().evict_lru_batch(n_per_cache);
        const auto t1 = clock::now();
        const double dt = std::chrono::duration<double>(t1 - t0).count();
        emit("rat_add", e, dt);
        total += e;
    }
    {
        const auto t0 = clock::now();
        const std::size_t e = g_reduce_cache().evict_lru_batch(n_per_cache);
        const auto t1 = clock::now();
        const double dt = std::chrono::duration<double>(t1 - t0).count();
        emit("reduce", e, dt);
        total += e;
    }
    {
        const auto t0 = clock::now();
        const std::size_t e = g_lf_cache_outer().evict_lru_batch(n_per_cache);
        const auto t1 = clock::now();
        const double dt = std::chrono::duration<double>(t1 - t0).count();
        emit("lf", e, dt);
        total += e;
    }
    {
        const auto t0 = clock::now();
        const std::size_t e = g_pf_cache_outer().evict_lru_batch(n_per_cache);
        const auto t1 = clock::now();
        const double dt = std::chrono::duration<double>(t1 - t0).count();
        emit("pf", e, dt);
        total += e;
    }
    {
        const auto t0 = clock::now();
        const std::size_t e = g_transform_shuffle_cache().evict_lru_batch(n_per_cache);
        const auto t1 = clock::now();
        const double dt = std::chrono::duration<double>(t1 - t0).count();
        emit("transform_shuffle", e, dt);
        total += e;
    }
    return total;
}

// Read current peak RSS in bytes from getrusage. macOS reports
// ru_maxrss in bytes; Linux reports KiB. Normalises to bytes.
// Returns 0 on error (in which case the hook becomes a no-op for this
// call — fail-open rather than triggering spurious evictions on
// malformed kernel state).
static std::size_t read_peak_rss_bytes() {
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<std::size_t>(ru.ru_maxrss);
#else
    return static_cast<std::size_t>(ru.ru_maxrss) * 1024u;
#endif
}

void evict_post_step_hook() {
    // Fast-path no-op when master switch is OFF. One env_gate load;
    // the cost is one branch on a cached bool, negligible vs the
    // surrounding per-step machinery (~ms).
    const auto& g = effective_env_gate();
    if (!g.evict_on_rss) return;

    const std::size_t rss_bytes = read_peak_rss_bytes();
    if (rss_bytes == 0) return;  // getrusage failed; fail-open.
    if (rss_bytes < g.evict_rss_threshold_b) return;

    const std::size_t n_per_cache = g.evict_lru_batch;
    const std::size_t evicted    = evict_lru_batch_all_caches(n_per_cache);

    if (g.evict_trace) {
        std::fprintf(stderr,
            "[hf-evict] rss=%zu threshold=%zu evicted=%zu "
            "(strategy=%s batch=%zu)\n",
            rss_bytes, g.evict_rss_threshold_b, evicted,
            g.evict_strategy_fifo ? "fifo" : "lru",
            n_per_cache);
    }
}

} // namespace operator_memo

// ---------------------------------------------------------------------------
// clear_between_fixtures — clears all five §E caches.
//
// Per §7.3: outer §E caches first, then any inner caches. Inner caches
// are owned by the algebra/integrator layers; the wiring at iter-N+ Step
// E.2-impl-2 invokes the existing `bump_pf_cache_generation()` +
// `clear_linear_factors_cache()` from this body. iter-60 ships the outer
// clear only; the inner-clear bridge lands with the per-op wraps at iter-61.
// ---------------------------------------------------------------------------
namespace operator_memo {

void clear_between_fixtures() {
    g_rat_add_cache().clear_all_shards();
    g_reduce_cache().clear_all_shards();
    g_lf_cache_outer().clear_all_shards();
    g_pf_cache_outer().clear_all_shards();
    // iter-62-β.2: transform_shuffle cache cleared alongside the other
    // four caches now that ValueT (TransformResultSym) is wired.
    g_transform_shuffle_cache().clear_all_shards();
}

} // namespace operator_memo

// ---------------------------------------------------------------------------
// counter_replay shims (§iter-59-fold-REQ-5 §4.4-bis table).
//
// Iter-61 wires the rat_add shim to the production per-thread call-count
// storage at `rat.cpp:129`. The other 4 shims remain stubs pending their
// per-op wraps landing (reduce + transform_shuffle deferred to iter-62;
// lf + pf shims have no call-count to replay because their per-thread
// observers are inner-cache-state probes, not call counters — see
// §iter-59-fold-REQ-5 §4.4-bis table rows 3 and 4).
//
// The per-thread counter arrays live in the existing instrumentation
// TUs (rat.cpp). The accessor `rat_add_calls_storage()` lives inside
// rat.cpp's anonymous namespace (internal linkage), so we route through
// the public shim `rat_add_record_call_for_thread(tid)` declared in
// `rat.hpp` and defined alongside the storage in `rat.cpp`.
// ---------------------------------------------------------------------------

namespace counter_replay {

void rat_add_on_hit() {
    // iter-61-β.6a: replay the per-thread call-count counter that
    // `Rat::add_impl` would have incremented on a cache MISS at
    // rat.cpp:~2253-2255. Wall counters (rat_add_legacy_wall_storage,
    // rat_add_via_qu_wall_storage) are deliberately NOT replayed; a
    // HIT really did take ~0 wall (the cached value is returned by
    // copy-ctor) and the wall counter should reflect that. Per
    // §iter-59-fold-REQ-5 §4.4-bis table row 1.
    //
    // The tid lookup mirrors the rat.cpp:2251 pattern (resolves
    // omp thread id under OMP, GCD slot index under HF_USE_GCD=1).
    // Bounds-check against the storage size happens inside the
    // public shim `rat_add_record_call_for_thread`.
    const int tid = ::hyperflint::runtime::hf_get_thread_num();
    rat_add_record_call_for_thread(tid);
}

void reduce_on_hit(int kind) {
    // iter-62-β.3: replay the per-thread per-classifier call-count
    // counter that `reduce_inplace_impl` would have incremented on a
    // cache MISS. `kind` is the classifier stored in the cache value
    // (ReduceValue.kind; set by the wrap from PRE-impl input):
    //   0 = zero    (rat.cpp:1078 reduce_zero_calls_storage[tid] += 1)
    //   1 = narrow  (rat.cpp:1486 reduce_narrow_calls_storage[tid] += 1 or
    //                the wide fall-through; iter-62 records the INPUT
    //                characterisation, which is one of {1, 2}, and
    //                cannot distinguish narrow-vs-wide fall-through
    //                internally — see §iter-59-fold-REQ-5 §4.4-bis row 2
    //                for the conservative-accounting rationale)
    //   2 = wide    (rat.cpp:1107 reduce_wide_calls_storage[tid] += 1
    //                early-out for den.is_fmpq() inputs)
    //
    // Wall counters (reduce_narrow_storage / reduce_wide_storage) are
    // deliberately NOT replayed; a HIT really did take ~0 wall (the
    // cached deep-copy is microsecond-scale).
    const int tid = ::hyperflint::runtime::hf_get_thread_num();
    ::hyperflint::reduce_record_call_for_thread(tid, kind);
}

void lf_on_hit() {
    // iter-60: stub. iter-61: op_call probe re-fires at integrator step;
    // the LF-cache-state probes are observers of inner state and do not
    // need outer-HIT replay per §iter-59-fold-REQ-5.
}

void pf_on_hit() {
    // iter-60: stub. iter-61: op_call probe re-fires; inner pf_cache
    // counters are not replayed because the inner cache is default-OFF
    // at HEAD (§iter-59-fold-REQ-4). If a future iteration re-enables
    // HF_ENABLE_KNOWN_BROKEN_PF_CACHE=1 the inner counters must be
    // replayed; forward-scaffolded but inactive at HEAD.
}

void transform_shuffle_on_hit() {
    // iter-60: stub. iter-61: op_call probe re-fires at
    // transform.cpp:1074-1081; no other per-thread storage is materially
    // affected (transform_shuffle is a pure functional transformation).
}

} // namespace counter_replay

// ---------------------------------------------------------------------------
// Explicit template instantiations.
// ---------------------------------------------------------------------------
template class OperatorMemo<canonical_signature::RatAddKey, Rat>;
template class OperatorMemo<canonical_signature::ReduceKey, ReduceValue>;
template class OperatorMemo<canonical_signature::LfKey, LinearFactorization>;
template class OperatorMemo<canonical_signature::PfKey, PartialFractionization>;
// iter-62-β.2: 5th explicit instantiation (TransformResultSym lives in
// integrator/transform.hpp; full type is in scope because we include it
// at the top of this TU).
template class OperatorMemo<canonical_signature::TransformShuffleKey,
                            TransformResultSym>;

} // namespace hyperflint
