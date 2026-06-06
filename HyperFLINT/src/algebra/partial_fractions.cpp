// PartialFractions implementation.
//
// Steps (mirroring HyperIntica.wl:3210):
//   1. Factor the denominator in var.
//   2. For each linear factor (lc*var + c0)^m, record the pole
//      a = -c0/lc with multiplicity m; non-linear factors raise.
//   3. Extract the polynomial part: schoolbook long division in var
//      with Rat coefficients for the other variables.
//   4. For each pole a with multiplicity m, compute
//        expr = (var - a)^m * f
//      then the coefficients c_j for 1 <= j <= m via
//        c_j = [ d^(m-j)/dvar^(m-j) expr ]_{var=a} / (m-j)!
//      The j=1 coefficient is c_1 = coef of 1/(var-a); j=m gives c_m =
//      coef of 1/(var-a)^m.
//
// This mirrors the fast-path in HyperIntica that replaces O(m^2) Series
// calls with O(m) derivatives.

#include "hyperflint/algebra/partial_fractions.hpp"

#include "hyperflint/core/addpf_probe.hpp"
#include "hyperflint/algebra/env_flags.hpp"           // iter-71 §T7 sixth chunk: HF_FLAG_PF_* macro layer
#include "hyperflint/core/operator_memo.hpp"
#include "hyperflint/core/canonical_signature.hpp"
#include "hyperflint/diagnostics/structural_sharing_probe.hpp"
#include "hyperflint/runtime/hf_thread_num.hpp"
#include "hyperflint/algebra/linear_factors.hpp"
#include "hyperflint/algebra/poly_struct_hash.hpp"   // Lever A: cache key
#include "hyperflint/core/zw_table.hpp"              // iter-52 C0c.1: ZWTable for linear_factors transient
#include "hyperflint/core/factored_rat.hpp"          // B1.3: single-pole factored residue fast path
#include "hyperflint/instrumentation/dag_hashcons_probe.hpp"  // §A.1 iter-50: op_call emit at function entry

#include <flint/fmpq.h>
#include <flint/fmpq_mpoly_factor.h>

#ifdef HF_HAVE_OPENMP
#include <omp.h>
#endif

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>      // std::fprintf (iter-17 pf_storage probe)
#include <cstdlib>     // std::getenv (HF_ENABLE_KNOWN_BROKEN_PF_CACHE)
#include <functional>  // std::hash (REQ-16.3 fold; iter-17)
#include <iostream>    // std::cerr (retired-env-var warning)
#include <sstream>
#include <stdexcept>
#include <string>      // std::string (REQ-16.3 fold; iter-17)
#include <unordered_map>
#include <utility>
#include <vector>

namespace hyperflint {

// Phase-d15 follow-up: file-scope per-thread accumulator for the
// linear_factors call (FLINT fmpq_mpoly_factor) inside partial_fractions.
// Same pattern as primitive.cpp's pf_per_thread_storage. Initialized,
// reset, and summed by the integration_step driver around the OMP region.
namespace {
std::vector<double>& lf_per_thread_storage() {
    static std::vector<double> v;
    return v;
}
}  // namespace (anon, file-scope)

void init_linear_factors_per_thread(int n_threads) {
    auto& v = lf_per_thread_storage();
    v.assign(static_cast<size_t>(n_threads > 0 ? n_threads : 1), 0.0);
}
void reset_linear_factors_per_thread() {
    auto& v = lf_per_thread_storage();
    for (auto& x : v) x = 0.0;
}
double sum_linear_factors_per_thread() {
    double s = 0.0;
    for (double x : lf_per_thread_storage()) s += x;
    return s;
}

// 2026-04-27 (Lever A — partial_fractions option remember): per-thread
// memoization of partial_fractions(f, var_idx, intro_al). Mirrors HI's
// `option remember` on `partialFractions` (HyperInt.mpl). Each entry
// of the OMP-parallel loop in integration_step calls this function on
// the same {f.num(), f.den(), var_idx, intro_al} key thousands of
// times across distinct primitives in the loop body; tst2 step 3 has
// pf_calls_in_loop = 684,207 and pf_unique_dens = 210,065 (69% DEN-
// redundancy; (num, den) joint redundancy in practice ~50-65%).
//
// Cache value is a deep copy of the PartialFractionization. Per-call
// PF cost is ~290 µs avg; copy cost on PartialFractionization is
// O(poles + Rat sizes), typically tens of µs. Net: 100-200 µs per hit
// on hot keys.
//
// Concurrency: thread_local; each OMP worker has its own map. No
// locks; no cross-thread sharing. Bounded by a generation counter
// bumped at integration_step entry; workers lazily clear their own
// thread_local caches on the first access in the new step.
//
// Kill switch: HF_DISABLE_PF_CACHE=1 forces the legacy uncached path
// for two release cycles' A/B observability.
namespace {

using PfCacheKey = std::pair<uint64_t, uint64_t>;

// 2026-04-27 (verify-on-hit fix): store the input (num, den) Polys
// alongside every cached value.  On every cache hit, FLINT-equality-
// compare stored vs current input; mismatch means hash collision
// (root cause: 128-bit FNV-1a fails to avalanche content-only-
// different fmpq_mpoly inputs that share the FLINT-canonical zpoly
// term stream — see VERIFY_PROBES.md).  Cost: ~+30-50 % per cache
// entry (one extra (num, den) Poly pair) plus one Poly::equal call
// per hit.  Guards correctness against any hash collision, not just
// the specific 5/3-proportional case observed on tst2 OMP=4.
//
// `unique_ptr<Poly>` (rather than holding by value) is dictated by
// `Poly` requiring a ctx for default construction; the indirection
// is also incidentally useful for collision-recompute paths that
// need to leave the slot intact.
struct PfCacheEntry {
    PartialFractionization value;
    std::unique_ptr<Poly> stored_num;
    std::unique_ptr<Poly> stored_den;
};

thread_local std::unordered_map<PfCacheKey, PfCacheEntry,
                                 PairU64Hash> g_pf_cache;
thread_local long g_pf_cache_hits        = 0;
thread_local long g_pf_cache_misses      = 0;
thread_local long g_pf_cache_collisions  = 0;

// 2026-04-27 (memory backstop): hard cap on cache entries per
// thread.  Once the cap is reached, fresh values keep being
// computed but are not inserted; the existing cache continues
// serving its current hot set.  Tunable via HF_PF_CACHE_MAX_ENTRIES;
// default 200000, which at ~12 kB per entry caps memory at ~2.4 GB
// per thread on tst2-class workloads.  No correctness risk: the
// equality-on-hit check guards against any insertion-order
// surprise.
size_t pf_cache_max_entries() {
    static const size_t v = []{
        if (const char* e = HF_FLAG_PF_CACHE_MAX_ENTRIES) {
            char* end = nullptr;
            unsigned long long n = std::strtoull(e, &end, 10);
            if (end != e && n > 0) return static_cast<size_t>(n);
        }
        return static_cast<size_t>(200000);
    }();
    return v;
}

std::atomic<long> g_pf_global_gen{0};
thread_local long g_pf_local_gen = 0;

inline void pf_invalidate_if_stale() {
    const long gen = g_pf_global_gen.load(std::memory_order_relaxed);
    if (g_pf_local_gen != gen) {
        g_pf_cache.clear();
        g_pf_cache_hits = 0;
        g_pf_cache_misses = 0;
        g_pf_cache_collisions = 0;
        g_pf_local_gen = gen;
    }
}

inline PfCacheKey pf_key(const Rat& f, size_t var_idx,
                          bool introduce_algebraic_letters) {
    auto seed = poly_struct_hash_seed();
    uint64_t h1 = seed.first;
    uint64_t h2 = seed.second;
    poly_struct_hash_mix(h1, h2, static_cast<uint64_t>(var_idx));
    poly_struct_hash_mix(h1, h2,
        introduce_algebraic_letters ? 0x1ULL : 0x0ULL);
    poly_struct_hash_raw(h1, h2, f.num());
    // Sentinel separator between num and den so concatenated streams
    // can't collide across (num, den) pairings.
    poly_struct_hash_mix(h1, h2, 0xfffffffffffffffeULL);
    poly_struct_hash_raw(h1, h2, f.den());
    return {h1, h2};
}

// PINNED 2026-05-18 (v2 iter-23) — pf_cache default-OFF + double-gate
//   fixture/gate : Smirnov tst2 cross-OMP correctness (bisect 2026-04-27,
//                  cordon SESSION_SUMMARY.md)
//   measurement  : with pf_cache ON @ OMP=4, output gains a `Pi*I*delta[t3]`
//                  residue plus 6/8 differing mzv-basis coefficients vs
//                  OMP={8,13} @ 100-digit precision — NOT algebraically
//                  equivalent (BISECT_RESULT.md)
//   falsifier    : bit-identical FLINT-equality (or 100-digit-precision
//                  numeric equality) across OMP ∈ {4, 8, 13} on tst2 AND
//                  tst3 with cache ON unlocks default-ON; until then the
//                  double-gate (HF_ENABLE_KNOWN_BROKEN_PF_CACHE=1 AND
//                  HF_I_KNOW_THIS_IS_BROKEN=1) is the only re-enable path.
//
// 2026-04-27: Lever A's per-thread `partial_fractions` cache was
// bisect-localized to be the source of a cross-OMP correctness fault on
// Smirnov tst2.  With the cache enabled at OMP=4, the result has an
// extra `Pi*I*delta[t3]` residue plus 6/8 differing mzv-basis
// coefficients vs OMP={8,13} — the outputs are NOT algebraically
// equivalent (verified at 100-digit precision).  Disabling the cache
// (or explicitly setting `OMP_NUM_THREADS=13`) restores cross-OMP
// determinism.  Setting both env vars below produces incorrect, not
// just slow, numerical results in this regime.  See
// `notes/benchmark_smirnov/sqf_round/parity_session/cross_omp_bisect/
// BISECT_RESULT.md` for the full bisect data and
// `SESSION_SUMMARY.md` for the cordon that proved the fix.
//
// Until the exact mechanism is pinned (likely race-determined letter-
// table allocation order leaking into cached values) and a
// deterministic fix lands, the cache is DISABLED BY DEFAULT.  It can
// be re-enabled for forensic A/B by setting BOTH
// `HF_ENABLE_KNOWN_BROKEN_PF_CACHE=1` AND `HF_I_KNOW_THIS_IS_BROKEN=1`
// in the environment.  The double-gate makes accidental re-enable on
// a production run impossible.
//
// Note: the verify-on-hit infrastructure introduced in commit
// `66d8ec036` (FLINT-equality compare stored vs current input on
// every cache hit, recompute on mismatch) remains in place below
// and is the right per-call safety mechanism.  It addresses the
// known FNV-1a hash-collision class on content-proportional inputs
// but does not (yet) prove the cross-OMP fault is solely a hash-
// collision artifact, so the cache stays opt-in until that is
// pinned down.
//
// Cost of the disable on Smirnov tst2 at OMP=13: 17 - 47 % wall.  The
// original Lever-A commit (`b11fcef77`) reported a clean trio at
// 88.49 ± 0.86 s with the cache and 106.20 ± 0.88 s without
// (= +17 %), measured on a quiet machine.  The post-fix cordon on
// `build-instr/` measured +47 % (88 -> 130 s) under contention from
// an unrelated background process eating one core; expect ~+20 %
// on a quiet release build.  Cost on tst0/tst1 is in the noise.
// Cost on the 3L3pt fixture is presumed small since `closure_body_s`
// is ~12 us per step and `partial_fractions_s` lives in the OUTER
// integration_step loop where parallelism is unaffected.
bool pf_cache_enabled() {
    static const bool enabled = []{
        // Warn once per process if a caller is still using the retired
        // `HF_DISABLE_PF_CACHE` env var.  Either value is now a no-op
        // (the cache is OFF by default); callers that previously set
        // `HF_DISABLE_PF_CACHE=0` to *force-enable* the cache will
        // silently get the disabled path without this warning.
        if (const char* e = HF_FLAG_DISABLE_PF_CACHE) {
            std::cerr << "hyperflint: warning: HF_DISABLE_PF_CACHE='" << e
                      << "' is retired; the partial_fractions cache "
                         "is now off by default (cross-OMP correctness "
                         "fix).  Set HF_ENABLE_KNOWN_BROKEN_PF_CACHE=1 "
                         "AND HF_I_KNOW_THIS_IS_BROKEN=1 to opt in to "
                         "the broken path for forensic A/B.\n";
        }
        const char* e1 = HF_FLAG_ENABLE_KNOWN_BROKEN_PF_CACHE;
        const char* e2 = HF_FLAG_I_KNOW_THIS_IS_BROKEN;
        return (e1 && e1[0] == '1') && (e2 && e2[0] == '1');
    }();
    return enabled;
}

// Backwards-compatible API: callers that previously asked
// `pf_cache_disabled()` get the negation.
bool pf_cache_disabled() { return !pf_cache_enabled(); }

}  // namespace

void bump_pf_cache_generation() {
    g_pf_global_gen.fetch_add(1, std::memory_order_relaxed);
}

long read_pf_cache_hits()       { return g_pf_cache_hits; }
long read_pf_cache_misses()     { return g_pf_cache_misses; }
long read_pf_cache_collisions() { return g_pf_cache_collisions; }

// iter-17 (Track 0.4): pfrac-row storage probe.
// Spec: pfrac-row storage spec v4 (internal development notes, iter-14/16).
// The gate is a
// payload-only (`Poly::total_bytes_buckets()`) per-bucket-per-container
// snapshot.  Default-OFF; env-gated via `HF_PF_STORAGE_STATS=1`.
std::atomic<bool> g_pf_storage_stats_enabled{false};
std::atomic<bool> g_pf_storage_debug_asserts_enabled{false};

namespace {

bool pf_storage_stats_resolve_enabled() {
    static const bool v = []{
        const char* e = HF_FLAG_PF_STORAGE_STATS;
        return (e && e[0] && e[0] != '0');
    }();
    return v;
}

bool pf_storage_debug_asserts_resolve_enabled() {
    static const bool v = []{
        const char* e = HF_FLAG_PF_STORAGE_DEBUG_ASSERTS;
        return (e && e[0] && e[0] != '0');
    }();
    return v;
}

int pf_storage_throttle_period() {
    static const int n = []{
        const char* e = HF_FLAG_PF_STORAGE_STATS_THROTTLE;
        if (!e || !e[0]) return 1;
        int v = std::atoi(e);
        return v >= 1 ? v : 1;
    }();
    return n;
}

inline void pf_storage_stats_lazy_init() {
    // Compatibility shim: previously lazy-init under an atomic.
    // Now a no-op because PfStorageStaticInit below resolves env at
    // static-init time, before any partial_fractions call.  Retained
    // so call sites that pre-date the static-init move continue to
    // compile.
}

struct PfStorageStaticInit {
    PfStorageStaticInit() {
        const char* e1 = HF_FLAG_PF_STORAGE_STATS;
        g_pf_storage_stats_enabled.store(
            (e1 && e1[0] && e1[0] != '0'),
            std::memory_order_relaxed);
        const char* e2 = HF_FLAG_PF_STORAGE_DEBUG_ASSERTS;
        g_pf_storage_debug_asserts_enabled.store(
            (e2 && e2[0] && e2[0] != '0'),
            std::memory_order_relaxed);
    }
};
PfStorageStaticInit pf_storage_static_init_instance;

// Sum Poly::PolyByteBuckets in place.
inline void accumulate_buckets(Poly::PolyByteBuckets& dst,
                                const Poly::PolyByteBuckets& src) {
    dst.coeff_intrinsic += src.coeff_intrinsic;
    dst.exp_live        += src.exp_live;
    dst.exp_slack       += src.exp_slack;
    dst.handle_live     += src.handle_live;
    dst.handle_slack    += src.handle_slack;
    dst.content_fmpq    += src.content_fmpq;
}

// Walk the contents of one PartialFractionization (polynomial_part +
// all pole.pole + all pole.coefs) and accumulate the six-bucket totals
// of the underlying Rat's num + den Polys.
inline void walk_partial_fractionization_into(
        const PartialFractionization& pf,
        Poly::PolyByteBuckets& dst) {
    accumulate_buckets(dst, pf.polynomial_part.num().total_bytes_buckets());
    accumulate_buckets(dst, pf.polynomial_part.den().total_bytes_buckets());
    for (const auto& pole : pf.poles) {
        accumulate_buckets(dst, pole.pole.num().total_bytes_buckets());
        accumulate_buckets(dst, pole.pole.den().total_bytes_buckets());
        for (const auto& c : pole.coefs) {
            accumulate_buckets(dst, c.num().total_bytes_buckets());
            accumulate_buckets(dst, c.den().total_bytes_buckets());
        }
    }
}

// Emit six lines (one per bucket) for a given (step, var, phase, thread,
// container) tuple, plus optionally the n_cache_entries scalar.
inline void emit_six_bucket_lines(long step, const char* var_name,
                                    const char* phase, int thread,
                                    const char* container,
                                    const Poly::PolyByteBuckets& b) {
    const char* names[6] = {
        "coeff_intrinsic", "exp_live", "exp_slack",
        "handle_live", "handle_slack", "content_fmpq"
    };
    size_t vals[6] = {
        b.coeff_intrinsic, b.exp_live, b.exp_slack,
        b.handle_live, b.handle_slack, b.content_fmpq
    };
    for (int i = 0; i < 6; ++i) {
        std::fprintf(stderr,
            "[pf_storage] step=%ld var=%s phase=%s thread=%d "
            "container=%s bucket=%s value=%zu\n",
            step, var_name, phase, thread, container, names[i], vals[i]);
    }
}

}  // namespace (anon)

void emit_pf_storage_stats(long step, const char* var_name,
                            const char* phase,
                            const PartialFractionization* live_or_null) {
    pf_storage_stats_lazy_init();
    if (!g_pf_storage_stats_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const int thread = runtime::hf_get_thread_num();

    // Container 1: thread_local g_pf_cache (current thread's slice).
    Poly::PolyByteBuckets cache_b{};
    const size_t n_cache_entries = g_pf_cache.size();
    for (const auto& kv : g_pf_cache) {
        const PfCacheEntry& entry = kv.second;
        // Stored input pair (num, den) -- ~30-50% of entry footprint
        // per the cache comment; attribute under `cache`.
        if (entry.stored_num) {
            accumulate_buckets(cache_b, entry.stored_num->total_bytes_buckets());
        }
        if (entry.stored_den) {
            accumulate_buckets(cache_b, entry.stored_den->total_bytes_buckets());
        }
        walk_partial_fractionization_into(entry.value, cache_b);
    }
    emit_six_bucket_lines(step, var_name, phase, thread, "cache", cache_b);
    std::fprintf(stderr,
        "[pf_storage] step=%ld var=%s phase=%s thread=%d "
        "n_cache_entries=%zu\n",
        step, var_name, phase, thread, n_cache_entries);

    // Container 2: live PartialFractionization just computed (after_pf
    // phase). Skipped on pre_step / post_step calls (no in-flight value).
    if (live_or_null != nullptr) {
        Poly::PolyByteBuckets live_b{};
        walk_partial_fractionization_into(*live_or_null, live_b);
        emit_six_bucket_lines(step, var_name, phase, thread,
                                "live", live_b);
    }

    std::fflush(stderr);
}

namespace {

// Univariate-in-var long division of p by q (both polynomials in var,
// with polynomial coefficients in the other vars). Returns (quot, rem)
// such that p = quot * q + rem and deg_var(rem) < deg_var(q).
//
// The quotient and remainder coefficients (in var) are in general
// rational functions in the other variables; we represent both quot
// and rem as Rat.
struct PolyDivResult {
    Rat quotient;
    Rat remainder;
};

PolyDivResult univariate_div(const Poly& p, const Poly& q, size_t var_idx) {
    const PolyCtx& ctx = p.ctx();
    long dq = q.degree_in_var(var_idx);
    if (dq < 0) {
        throw std::runtime_error("univariate_div: divisor is zero");
    }
    if (dq == 0) {
        // q is a nonzero poly in the other vars: p / q as Rat.
        return {Rat(Poly(p), Poly(q)), Rat{Poly::zero_of(ctx)}};
    }
    long dp = p.degree_in_var(var_idx);

    // r starts as p (as Rat over same ctx so we can accumulate
    // rational-valued coefficients in var).
    Rat rem{Poly(p)};

    // Build x^k as a Rat via fmpq_mpoly_gen + pow. Avoids the wide-ctx
    // string parse that ran once per quotient term.
    const Poly var_poly = Poly::gen(ctx, var_idx);
    auto var_pow = [&](long k) {
        if (k == 1) return Rat(var_poly);
        return Rat(var_poly.pow(static_cast<unsigned long>(k)));
    };

    // Leading coef of q in var, as Rat (is actually a Poly because q's
    // leading coef is a polynomial in the other vars).
    Rat lead_q{q.coefficient_of_var(var_idx, dq)};

    Rat quot{Poly::zero_of(ctx)};

    // We iterate while rem has any component with degree >= dq in var.
    // Strict definition of "degree-in-var of a Rat": degree of its
    // numerator in var (since the denominator is free of var after
    // Together... wait, not in general).  For our caller (partial_fractions),
    // p and q are polynomials (numerator and denominator of the input Rat
    // separately).  So rem starts as a Rat whose den=1 and num=p; as we
    // subtract (term * q), the denominator can grow as a function of the
    // other vars but stays monomial in var. In practice the coefficients
    // are Rat in the other vars and the structure in var stays polynomial.
    while (true) {
        // deg_var of rem's numerator ignoring denominator
        long dr = rem.num().degree_in_var(var_idx);
        if (dr < dq) break;

        // leading coef of rem (in var) as Rat in other vars
        Poly lead_rem_num = rem.num().coefficient_of_var(var_idx, dr);
        Rat lead_rem{lead_rem_num, rem.den()};

        // term_coef = lead_rem / lead_q (as Rat in other vars)
        Rat term_coef = lead_rem / lead_q;

        // term = term_coef * var^(dr - dq)
        Rat term = term_coef * var_pow(dr - dq);

        // Accumulate
        quot = quot + term;
        // rem := rem - term * q   (q is polynomial in all vars)
        rem  = rem - term * Rat(Poly(q));
    }

    return {std::move(quot), std::move(rem)};
}

}  // namespace

// Forward-declare the uncached implementation so the public
// partial_fractions can call it on cache miss and on collision.
namespace {
PartialFractionization partial_fractions_impl(
    const Rat& f, size_t var_idx,
    std::shared_ptr<ZWTable> zw_tab,
    bool introduce_algebraic_letters);
}  // namespace

// HF FF Phase 5 §E Step E.2-impl-2 (iter-61-β.4).
//
// Renamed from `partial_fractions` per implementation_memo §3.4 +
// §iter-59-fold-REQ-2. The pre-iter-61 body (which includes the inner
// `pf_cache` lookup; default-OFF at HEAD per §iter-59-fold-REQ-4) is
// unchanged below. The new public `partial_fractions` (further down
// in this TU) wraps this with the §E outer cache.
//
// Note: `partial_fractions_impl` (anon-namespace at line ~421) is the
// pre-existing inner-cache helper and is NOT renamed by iter-61. The
// rename is at the public boundary only.
static PartialFractionization partial_fractions_with_inner_cache(
    const Rat& f, size_t var_idx,
    std::shared_ptr<ZWTable> zw_tab,
    bool introduce_algebraic_letters) {
    const PolyCtx& ctx = f.ctx();
    // HF FF Phase 5 §A.1 iter-50: op_call emit at partial_fractions entry
    // (§3.1 op #4). Arity=1; input hash mixes the Rat's (num, den)
    // canonical-bits hashes with var_idx and the introduce_algebraic_letters
    // flag. OFF-path fast-guard via `hf_probe_active` branch.
    if (hf_probe_active) {
        uint64_t ih = kFnv1a64OffsetBasis;
        ih = hf_probe_fnv1a64_mix_u64(ih,
                hf_probe_canonical_hash_poly(f.num().raw(), ctx.raw()));
        ih = hf_probe_fnv1a64_mix_u64(ih,
                hf_probe_canonical_hash_poly(f.den().raw(), ctx.raw()));
        ih = hf_probe_fnv1a64_mix_u64(ih, (uint64_t)var_idx);
        ih = hf_probe_fnv1a64_mix_u64(ih,
                (uint64_t)(introduce_algebraic_letters ? 1u : 0u));
        hf_probe_emit_op_call("partial_fractions", ih, 1);
    }
    PartialFractionization out{Rat{Poly::zero_of(ctx)}, {}};

    // Zero input: return zero polynomial part, no poles. Skip the
    // cache for trivial cases — not worth the hash cost.
    if (f.is_zero()) return out;

    // Lever A: per-thread `partial_fractions` cache.  See the
    // long comment block on `pf_cache_enabled()` for the bisect
    // history and the input-verify fix that closes the cross-OMP
    // correctness fault.
    const bool use_cache = !pf_cache_disabled() &&
                            f.den().degree_in_var(var_idx) > 0;
    PfCacheKey key{};
    if (use_cache) {
        pf_invalidate_if_stale();
        key = pf_key(f, var_idx, introduce_algebraic_letters);
        auto it = g_pf_cache.find(key);
        if (it != g_pf_cache.end()) {
            // Always-on input-verify: if the stored input matches
            // the current input, the cached value is sound and we
            // return it.  Mismatch (hash collision) → fall through
            // to fresh recompute without overwriting the cache; the
            // existing slot keeps serving the input that owns it.
            const PfCacheEntry& entry = it->second;
            assert(entry.stored_num && entry.stored_den);
            if (f.num().equal(*entry.stored_num) &&
                f.den().equal(*entry.stored_den)) {
                ++g_pf_cache_hits;
                return entry.value;   // deep copy on return-by-value
            }
            // Hash collision.  Tracked separately from fresh misses
            // so observability tools can distinguish the two.
            ++g_pf_cache_collisions;
            return partial_fractions_impl(f, var_idx, zw_tab,
                                           introduce_algebraic_letters);
        }
        ++g_pf_cache_misses;
    }

    // Cache miss (or cache disabled): compute via uncached impl,
    // store a deep copy of (input, value) if caching is on, return.
    PartialFractionization result =
        partial_fractions_impl(f, var_idx, zw_tab, introduce_algebraic_letters);

    if (use_cache && g_pf_cache.size() < pf_cache_max_entries()) {
        // Hold a deep copy of the input alongside the cached value;
        // a later hit can compare for collision and recompute on
        // mismatch.  Memory cost: ~+30-50 % per cache entry.  When
        // the per-thread cache reaches the size cap, fresh values
        // are still computed and returned but no longer inserted —
        // the cache stops growing while continuing to serve its
        // current hot set.  No correctness risk; just bounded perf.
        PfCacheEntry entry{result,
                           std::make_unique<Poly>(f.num()),
                           std::make_unique<Poly>(f.den())};
        g_pf_cache.emplace(key, std::move(entry));
    }
    return result;
}

// HF FF Phase 5 §E Step E.2-impl-2 (iter-61-β.4).
//
// Public `partial_fractions` entry-point with operator-memoization wrap.
// On cache HIT, returns a deep-copy of the stored `PartialFractionization`;
// on cache MISS, delegates to `partial_fractions_with_inner_cache` (the
// pre-iter-61 body, which itself contains the inner `pf_cache` opt-in
// kill-switch at line ~366; default-OFF per §iter-59-fold-REQ-4).
//
// Per implementation_memo §3.4 + §iter-59-fold-REQ-2 corrected snippet,
// §iter-59-fold-REQ-3 (option b): the outer §E cache is DISABLED under
// HF_USE_SCALAR_REP=1 via `operator_memo::pf_enabled()` returning false
// regardless of HF_OPERATOR_MEMO_OFF_PF.
//
// Key includes `zw_tab.get()` (raw pointer identity) per
// §iter-59-fold-REQ-3 defense-in-depth.
PartialFractionization partial_fractions(
    const Rat& f, size_t var_idx,
    std::shared_ptr<ZWTable> zw_tab,
    bool introduce_algebraic_letters) {
    // HF FF Phase 6 REVISED §6.P iter-17 (BINDING pre-build reviewer at
    // iter-16): env-gated structural-sharing probe.  Default-OFF guarded
    // by master predicate.  Observes the PUBLIC entry-point (both
    // §E-cache HIT and MISS paths).  pre_bytes = bytes(f); post_bytes
    // = bytes(polynomial_part) + Σ bytes of pole-side residue coefs.
    //
    // unchanged_bytes (REQ-16.3 fold; iter-16 BINDING reviewer
    // ae98902a768f7f242): iter-17 implements fraction-level structural
    // intersection per design memo §2.1.  We compute a 64-bit signature
    // hash for each "fraction" (one per pole-side entry) from its
    // canonical-string representation: the pole-Rat string concatenated
    // with each residue-coefficient Rat string.  We also probe the
    // polynomial part as a single "fraction" with its own canonical
    // string.  The `introduce_algebraic_letters` bit is mixed into the
    // signature so the seen-set does not conflate Wm/Wp-introduced
    // factorisations with the default path (R17.8 scope-by-key-attribute
    // carry).  Each signature is submitted to
    // `structural_sharing::observe_pf_fraction_bytes(sig, bytes)`, which
    // returns `bytes` if previously observed (shareable) or 0 (first
    // observation).  At sample_rate >= 10 on PF (design memo §2.2
    // FOLD-DC5 stratification), the mutex acquisition rate is bounded
    // and the +10 % wall budget holds.
    const bool probe =
        structural_sharing::probe_partial_fractions_instrumented()
        && structural_sharing::should_emit_op("partial_fractions");
    int64_t pre_bytes = 0;
    if (probe) {
        pre_bytes = static_cast<int64_t>(f.total_bytes());
    }

    // R17.8 scope-by-key-attribute: PF cache key includes
    // `introduce_algebraic_letters`; mix into the high bit of the
    // signature so the seen-set partitions by that attribute.
    const std::uint64_t scope_bits =
        (static_cast<std::uint64_t>(introduce_algebraic_letters ? 1u : 0u) << 63);

    auto emit_probe = [&](const PartialFractionization& pf) {
        if (!probe) return;
        int64_t post_bytes = static_cast<int64_t>(
            pf.polynomial_part.total_bytes());
        int64_t unchanged_bytes = 0;
        std::hash<std::string> str_hasher;
        // Treat the polynomial part as its own "fraction" for the
        // intersection: emit its signature first so cross-call sharing
        // of the polynomial-part rat is observable.
        {
            const int64_t b = static_cast<int64_t>(
                pf.polynomial_part.total_bytes());
            const std::uint64_t sig =
                static_cast<std::uint64_t>(
                    str_hasher(pf.polynomial_part.to_string()))
                ^ scope_bits;
            unchanged_bytes +=
                structural_sharing::observe_pf_fraction_bytes(sig, b);
        }
        for (const auto& pole : pf.poles) {
            // Build the canonical fraction string by concatenating the
            // pole's canonical string with each residue coefficient's
            // canonical string.  The result is a stable representation
            // of the (pole, c_1, ..., c_m) tuple that fmpq_mpoly_factor
            // would emit identically across calls.
            std::string canon = pole.pole.to_string();
            int64_t b = static_cast<int64_t>(pole.pole.total_bytes());
            post_bytes += b;
            for (const auto& c : pole.coefs) {
                canon.push_back('|');
                canon += c.to_string();
                b += static_cast<int64_t>(c.total_bytes());
                post_bytes += static_cast<int64_t>(c.total_bytes());
            }
            const std::uint64_t sig =
                static_cast<std::uint64_t>(str_hasher(canon)) ^ scope_bits;
            unchanged_bytes +=
                structural_sharing::observe_pf_fraction_bytes(sig, b);
        }
        structural_sharing::emit("partial_fractions", pre_bytes, post_bytes,
                                 unchanged_bytes);
    };

    // iter-17 (Track 0.4): after_pf probe at the public-entry
    // boundary.  Captures the pfrac-row storage of the *returned*
    // PartialFractionization (post-cache-hit OR post-recompute), so the
    // measurement reflects what downstream sees.  Per-thread throttle gates
    // emit cadence; lazy env-resolve on first call.  Default-OFF runtime.
    auto emit_storage_probe = [&](const PartialFractionization& pf) {
        pf_storage_stats_lazy_init();
        if (!g_pf_storage_stats_enabled.load(std::memory_order_relaxed)) return;
        static thread_local int t_counter = 0;
        if (t_counter > 0) { --t_counter; return; }
        t_counter = pf_storage_throttle_period() - 1;
        const char* vn = (var_idx < f.ctx().vars().size())
                         ? f.ctx().vars()[var_idx].c_str() : "?";
        emit_pf_storage_stats(-1L, vn, "after_pf", &pf);
    };

    // HF_ADDPF_PROBE (additive-parfrac Phase 0): unconditional call
    // counter + nonzero-poly-part fraction (D6 eager poly-part fusion
    // re-examination data). Counts every public call regardless of
    // memo hits so the ON-arm estimate uses real call volume.
    auto record_addpf = [&](const PartialFractionization& pf) {
        if (addpf_probe::enabled())
            addpf_probe::record_pf_call(!pf.polynomial_part.is_zero());
    };

    if (operator_memo::pf_enabled()) {
        canonical_signature::PfKey key = canonical_signature::make_pf_key(
            f, var_idx, zw_tab.get(), introduce_algebraic_letters);
        const std::uint64_t key_hash =
            canonical_signature::hash_pf_key(key);
        // iter-70 REC-2: try_lookup returns
        // std::shared_ptr<const PartialFractionization> (ref-counted COW).
        // On HIT, `return *cached_sp` invokes PartialFractionization's
        // copy ctor at the caller-by-value boundary; cache-side lock-held
        // window shrinks to O(1) ref-count++.
        auto cached_sp = g_pf_cache_outer().try_lookup(key, key_hash);
        if (cached_sp) {
            counter_replay::pf_on_hit();
            // REQ-17.1 fold option (a) — iter-17 BINDING reviewer
            // a25d12e86d3f96bf8. Deep-copy `*cached_sp` to a local
            // before invoking emit_probe. emit_probe walks
            // pf.polynomial_part.to_string() + pole.pole.to_string()
            // + c.to_string() for each residue coefficient, all of
            // which mutate `mutable cached_str_` on Rat / Poly. Two
            // concurrent OMP workers HITting the same cache slot
            // would race on the shared cached_sp->{polynomial_part,
            // poles[*].pole, poles[*].coefs[*]} cached_str_ strings.
            // Default-OFF (HF_STRUCTURAL_SHARING_PROBE unset) keeps
            // the previous ref-count++ fast path; deep-copy only
            // fires under the probe gate.
            PartialFractionization result = *cached_sp;
            emit_probe(result);
            emit_storage_probe(result);
            record_addpf(result);
            return result;
        }
        PartialFractionization result = partial_fractions_with_inner_cache(
            f, var_idx, zw_tab, introduce_algebraic_letters);
        g_pf_cache_outer().insert(std::move(key), key_hash,
                                  PartialFractionization(result));
        emit_probe(result);
        emit_storage_probe(result);
        record_addpf(result);
        return result;
    }
    PartialFractionization result = partial_fractions_with_inner_cache(
        f, var_idx, zw_tab, introduce_algebraic_letters);
    emit_probe(result);
    emit_storage_probe(result);
    record_addpf(result);
    return result;
}

namespace {
// 2026-04-27 (verify-mode refactor): pulled the body of the original
// `partial_fractions` (everything after the cache lookup) into this
// helper.  The public `partial_fractions` calls it on cache miss; in
// verify mode, also on cache hit for fresh-vs-cached comparison.
PartialFractionization partial_fractions_impl(
    const Rat& f, size_t var_idx,
    std::shared_ptr<ZWTable> zw_tab,
    bool introduce_algebraic_letters) {
    const PolyCtx& ctx = f.ctx();
    PartialFractionization out{Rat{Poly::zero_of(ctx)}, {}};

    // Zero input is handled by the public wrapper; impl is only ever
    // called on non-zero input, but keep the guard for safety.
    if (f.is_zero()) return out;

    // Denominator independent of var: f is polynomial-in-var, all of it
    // is the polynomial part.
    if (f.den().degree_in_var(var_idx) <= 0) {
        out.polynomial_part = f;
        return out;
    }

    // Step 1: polynomial-part via univariate-in-var division.
    // iter-24-3 §B.1 Patches A+B: dv.quotient and dv.remainder are each
    // consumed exactly once after univariate_div returns; move both into
    // out.polynomial_part / rem to avoid the implicit Rat (= 2 Poly) copy.
    PolyDivResult dv = univariate_div(f.num(), f.den(), var_idx);
    out.polynomial_part = std::move(dv.quotient);

    // Remainder r / den is the proper rational part whose PF we compute.
    // univariate_div produces a Rat rem whose denominator can accumulate
    // leading-in-var coefficients of f.den() (as a polynomial in the
    // other vars) over the course of the division; the previous code
    // constructed `proper` from rem.num() alone and silently dropped
    // rem.den(), scaling every pole residue by its reciprocal. Compute
    // `proper = rem / f.den()` so rem.den() is retained.
    Rat rem = std::move(dv.remainder);
    Rat proper = rem / Rat{Poly(f.den())};

    // Step 2: factor the denominator in var. Phase 7-vi-a propagates the
    // algebraic-letter flag so deg-2 irreducibles become Wm/Wp pairs.
    // Phase-d15 timer: capture the FLINT factor cost to localize the
    // partial_fractions hot spot.
    // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num() in OMP mode;
    // under HF_USE_GCD=1 returns the GCD slot index.
    const int _lf_tid = ::hyperflint::runtime::hf_get_thread_num();
    auto& _lf_v = lf_per_thread_storage();
    const auto _lf_t0 = std::chrono::steady_clock::now();
    // Iter-52 C0c.1 Increment β: zw_tab is the threaded-through param
    // (replaces the iter-52a Increment α caller-side transient). When
    // the caller is the production integration chain
    // (hyper_int.cpp -> integration_step -> integrate_ii ->
    // partial_fractions), this is the persistent driver-entry ZWTable
    // allocated under runtime::scalar_rep_enabled() at hyper_int.cpp:~463.
    // When the caller is a bridge/CLI handler or test, this is a
    // caller-side transient (one allocation per public-API entry, not
    // one per partial_fractions call).
    LinearFactorization lf = linear_factors(
        f.den(), var_idx, zw_tab, introduce_algebraic_letters);
    if (static_cast<size_t>(_lf_tid) < _lf_v.size()) {
        _lf_v[static_cast<size_t>(_lf_tid)] +=
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - _lf_t0).count();
    }
    if (!lf.nonlinear.empty()) {
        throw std::runtime_error(
            "partial_fractions: nonlinear factor in denominator "
            "(Phase 7 handles degree-2 via Wm/Wp; degree >= 3 unsupported)");
    }

    // Phase 7-vi-b: when the algebraic-letter path fires, reassemble the
    // denominator from the explicit (lc_var · ∏ (var − pole_i)^m_i)
    // factored form and use that for `proper`. The native (var²−K) form
    // keeps Wm/Wp only implicitly via Vieta — the derivative-at-pole
    // formula then evaluates (var−Wm)·1/(var²−K) at var=Wm and gets 0/0
    // since FLINT's Rat canonicalization treats the monolithic quadratic
    // and its Wm/Wp factorization as structurally distinct. Mirrors
    // HyperIntica.wl:3252 (`simp007 = Together[simp007 · (fac/factored)^m]`).
    //
    // `lc_var` is the leading-in-var coefficient of f.den() — NOT
    // `lf.constant`. FLINT's fmpq_mpoly_factor sometimes absorbs a sign
    // flip into `lf.constant` (pairing with a negated base factor like
    // (-x - 1)), which leaves lf.constant differing from f.den()'s
    // leading-in-var coefficient. Using the latter keeps factored_den
    // and f.den() in agreement on overall scale, so proper = rem / f.den()
    // matches proper = rem / factored_den structurally (modulo Vieta
    // for the deg-2 piece).
    if (introduce_algebraic_letters) {
        long deg_in_var = f.den().degree_in_var(var_idx);
        Poly lc_var = f.den().coefficient_of_var(var_idx, deg_in_var);
        Rat factored_den{lc_var};
        Rat var_rat{Poly::gen(ctx, var_idx)};
        for (auto& linf : lf.linear) {
            Rat vma = var_rat - linf.pole;
            factored_den = factored_den * vma.pow(linf.multiplicity);
        }
        // Same rem.den() invariant as the native branch above: keep it
        // rather than dropping it via `Rat{rem.num(), ...}`.
        proper = rem / factored_den;
    }

    // Step 3: per-pole residue extraction.

    // --- B1.3b FAST PATH (any pole count, no algebraic letters) -----------
    // When the denominator factors into linear factors only -- no
    // nonlinear/irreducible buckets, no algebraic-letter mode -- the
    // per-pole residue algebra can be carried in FactoredRat (numerator
    // over a product of (base)^exp factors) so that no multivariate GCD is
    // taken until the final materialize_to_rat() per coefficient. The
    // legacy Rat residue loop below spends ~93 % of a 277 s req integration
    // inside fmpq_mpoly_gcd_cofactors; the factored path replaces that with
    // exponent bookkeeping plus per-factor evaluation at each pole.
    //
    // B1.3 covered exactly one distinct pole; B1.3b generalizes to ANY pole
    // count so the multi-pole MZV fixtures (tst*) also avoid the per-op
    // GCD.  This block fills out.poles for every linear pole and returns;
    // the result is value-equal (same ascending Laurent coefficients) to
    // the Rat loop below, which is left byte-identical and still used for
    // the algebraic-letter / nonlinear cases.
    // Defensive fallback (adversarial review, 2026-05-30): the fast path
    // assumes each primitive linear factor equals exactly Q_k*var - P_k, so
    // Poly::divexact never sees a true remainder. Should a future
    // linear_factors return a factor whose primitive form differs from
    // Q_k*var - P_k by a non-unit, divexact THROWS instead of degrading. We
    // wrap the whole fast-path body in try/catch over a LOCAL fast_poles
    // vector: on ANY std::exception (e.g. from divexact, or the vanishing-
    // denominator-factor guard) we abandon the partial fast-path result
    // WITHOUT having mutated out.poles, set fast_ok=false, and fall through
    // to the always-correct legacy Rat residue loop below (which fills
    // out.poles itself, using the `proper = rem / f.den()` computed above).
    // No log/print here: this is the hot path and the legacy loop is a
    // correct silent degrade, not an error.
    const bool use_factored =
        !introduce_algebraic_letters && lf.nonlinear.empty();
    if (use_factored) {
      bool fast_ok = true;
      std::vector<PartialFractionPole> fast_poles;
      try {
        Poly var_poly = Poly::gen(ctx, var_idx);
        Poly one = Poly::one_of(ctx);

        // Primitive linear factors lin_k = Q_k*var - P_k for each pole
        // a_k = P_k/Q_k.  Peel each (via EXACT division) off f.den() so the
        // remaining factor `den_remaining` holds whatever content / leading
        // coefficient is left.  INVARIANT after the loop:
        //   f.den() == den_remaining * prod_k lin_k^{m_k}  (exactly).
        struct Prim { Poly lin; Poly Q; Poly P; long m; };
        std::vector<Prim> prims;
        prims.reserve(lf.linear.size());
        Poly den_remaining = f.den();
        for (const auto& linf : lf.linear) {
            Poly Pk = linf.pole.num();
            Poly Qk = linf.pole.den();
            Poly lin = Qk.mul(var_poly).sub(Pk);            // Q_k*var - P_k
            // EXACT division: f.den() factors as prod lin_k^{m_k} times a
            // var-free remainder, so divexact never has a true remainder.
            den_remaining = den_remaining.divexact(
                lin.pow(static_cast<unsigned long>(linf.multiplicity)));
            prims.push_back({std::move(lin), std::move(Qk), std::move(Pk),
                             linf.multiplicity});
        }

        // Evaluate a Poly h(var) at var = P/Q with per-factor
        // Q-homogenization:
        //   d = max(0, deg_var(h));  homog = sum_{k=0..d} [h]_k * P^k * Q^{d-k};
        //   h(P/Q) = homog / Q^d, returned factored (Rat(homog, Q^d)).
        // Q^0 = one handles Q == 1 and d == 0 trivially.  Parameterized by
        // the CURRENT pole's (P, Q): B1.3b evaluates every pole, so the
        // homogenization base must be that pole's own denominator.
        auto eval_poly = [&](const Poly& h, const Poly& P,
                             const Poly& Q) -> FactoredRat {
            long dd = h.degree_in_var(var_idx);
            long d = (dd > 0) ? dd : 0;
            Poly homog = Poly::zero_of(ctx);
            for (long k = 0; k <= d; ++k) {
                Poly ck = h.coefficient_of_var(var_idx, k);
                if (ck.is_zero()) continue;
                Poly term = ck.mul(P.pow(static_cast<unsigned long>(k)))
                              .mul(Q.pow(static_cast<unsigned long>(d - k)));
                homog = homog.add(term);
            }
            Poly Qd = Q.pow(static_cast<unsigned long>(d));  // Q^0 = 1 if d==0
            return FactoredRat::from_rat(Rat(homog, Qd));
        };

        // Evaluate a FactoredRat D at var = P/Q, factor by factor:
        //   D(P/Q) = eval_poly(num) / prod_i eval_poly(g_i)^e_i.
        // The pole a_k's own primitive factor lin_k never appears in expr_k's
        // denominator (it was cancelled at construction below), so the only
        // residual denominator factors are the OTHER poles' lin_j (j!=k),
        // which are nonzero at a_k, plus Q_k^{m_k} and den_remaining.  A
        // vanishing denominator factor at the pole would therefore signal a
        // coincident pole (impossible for distinct linear factors); guard
        // defensively rather than divide by zero.
        auto eval_factored = [&](const FactoredRat& D, const Poly& P,
                                 const Poly& Q) -> FactoredRat {
            FactoredRat r = eval_poly(D.numerator(), P, Q);
            for (const auto& g : D.den_factors()) {
                FactoredRat gv = eval_poly(g.base, P, Q);
                if (gv.numerator().is_zero()) {
                    throw std::runtime_error(
                        "partial_fractions_impl factored fast path: "
                        "denominator factor vanishes at pole");
                }
                r = r.div(gv.pow(g.exp));
            }
            return r;
        };

        fast_poles.reserve(prims.size());
        for (size_t kpole = 0; kpole < prims.size(); ++kpole) {
            const Prim& pk = prims[kpole];
            const Rat& a = lf.linear[kpole].pole;
            long m = pk.m;
            const Poly& P = pk.P;
            const Poly& Q = pk.Q;

            // expr_k = (var - a_k)^{m_k} * proper, with proper = rem / f.den().
            //   var - a_k = (Q_k*var - P_k)/Q_k = lin_k/Q_k,
            //     so (var - a_k)^{m_k} = lin_k^{m_k} / Q_k^{m_k}.
            //   f.den() = den_remaining * prod_j lin_j^{m_j}.
            //   => expr_k = (lin_k^{m_k} / Q_k^{m_k}) * rem
            //                 / (den_remaining * prod_j lin_j^{m_j})
            //             = rem / (Q_k^{m_k} * den_remaining
            //                       * prod_{j!=k} lin_j^{m_j}).
            // We build the CANCELLED form directly so that no copy of lin_k
            // survives in the denominator: lin_k(P_k/Q_k) = 0, so a surviving
            // lin_k denominator factor would vanish when we evaluate at the
            // pole.  Holding it factored keeps the representation GCD-free
            // until materialize_to_rat().  Constant Q_k (the polynomial-pole
            // case a.den()==1) or constant den_remaining folds away through
            // from_rat, so this one expression covers both branches the
            // legacy loop handles separately.
            FactoredRat expr_fac =
                FactoredRat::from_poly(rem.num())
                    .mul(FactoredRat::from_rat(Rat(one, rem.den())))
                    .mul(FactoredRat::from_rat(Rat(one, Q)).pow(m))
                    .mul(FactoredRat::from_rat(Rat(one, den_remaining)));
            for (size_t j = 0; j < prims.size(); ++j) {
                if (j == kpole) continue;  // lin_k cancelled by (var-a_k)^m
                expr_fac = expr_fac.mul(
                    FactoredRat::from_rat(Rat(one, prims[j].lin))
                        .pow(prims[j].m));
            }

            // Derivative chain in FactoredRat: derivs[t] = d^t/dvar^t expr_k.
            // HF_FR_MAT_PEEL: peel removable base powers off each chain
            // numerator as it is built. FactoredRat::derivative never
            // cancels, so without the peel the chain numerators swell with
            // whole powers of the den bases and the downstream eval_poly
            // homogenization pays huge fmpz_mpoly_mul calls (ord_-4_face_90:
            // ~10 s residual after the materialize-side peel alone).
            // Value-preserving: each peel step is an exact division applied
            // to numerator and factored denominator alike.
            std::vector<FactoredRat> derivs;
            derivs.reserve(static_cast<size_t>(m));
            derivs.push_back(expr_fac);
            if (FactoredRat::peel_enabled()) derivs.back().peel_known_factors();
            for (long t = 1; t < m; ++t) {
                derivs.push_back(derivs.back().derivative(var_idx));
                if (FactoredRat::peel_enabled())
                    derivs.back().peel_known_factors();
            }

            PartialFractionPole pole{a, m, {}};
            pole.coefs.reserve(static_cast<size_t>(m));
            for (long j = 1; j <= m; ++j) {
                long deriv_order = m - j;
                FactoredRat val_fac =
                    eval_factored(derivs[static_cast<size_t>(deriv_order)],
                                  P, Q);
                Rat val = val_fac.materialize_to_rat();
                if (deriv_order > 0) {
                    // Divide by (m-j)! = deriv_order!.  NOTE: divide the
                    // MATERIALIZED Rat by the scalar, NOT the FactoredRat: a
                    // constant FactoredRat divisor is silently dropped by
                    // FactoredRat::div (push_factor discards constant/one
                    // bases), which would leave the factorial out.  The
                    // scalar Rat divide here is identical to the legacy loop
                    // below and reduces correctly.
                    long fact = 1;
                    for (long k = 2; k <= deriv_order; ++k) fact *= k;
                    val = val / Rat::from_int(ctx, fact);
                }
                pole.coefs.push_back(std::move(val));
            }
            fast_poles.push_back(std::move(pole));
          }
      } catch (const std::exception&) {
        // divexact-exactness assumption (primitive factor == Q_k*var-P_k)
        // violated, or a defensive guard tripped: silently degrade to the
        // legacy Rat residue loop. fast_poles is discarded; out.poles is
        // untouched (we never wrote to it), so the legacy loop sees a clean
        // slate. out.polynomial_part (set from quot above) and `proper`
        // remain valid for the fallback.
        fast_ok = false;
      }
      if (fast_ok) {
        out.poles = std::move(fast_poles);
        return out;
      }
      // fall through to legacy loop below
    }
    // --- END B1.3b FAST PATH ----------------------------------------------

    for (auto& linf : lf.linear) {
        PartialFractionPole pole{linf.pole, linf.multiplicity, {}};

        // expr = (var - a)^m * (proper)
        Rat var_rat{Poly::gen(ctx, var_idx)};
        Rat var_minus_a = var_rat - linf.pole;
        Rat expr = var_minus_a.pow(linf.multiplicity) * proper;

        // Derivatives 0..m-1.
        // iter-24-3 §B.1 Patch C: expr is consumed exactly once by the
        // initial push_back; the subsequent loop reads from derivs.back(),
        // not from expr. Move avoids one Rat (= 2 Poly) copy per pole.
        std::vector<Rat> derivs;
        derivs.reserve(linf.multiplicity);
        derivs.push_back(std::move(expr));
        for (long k = 1; k < linf.multiplicity; ++k) {
            derivs.push_back(derivs.back().derivative(var_idx));
        }

        // Substitute var -> a in each derivative; divide by (m-j)!.
        // Result ordering: coefs[0] = c_1, coefs[1] = c_2, ..., coefs[m-1] = c_m
        // per HyperIntica's AppendTo loop.
        for (long j = 1; j <= linf.multiplicity; ++j) {
            long deriv_order = linf.multiplicity - j;
            // Substitute var -> pole in the deriv. Since pole is a Rat,
            // we implement via: set up a Poly-eval via substitute_one_rat
            // only when pole is purely numeric; otherwise we do symbolic
            // substitution via (num_of_deriv, den_of_deriv) evaluated at
            // var = pole_num / pole_den, giving a Rat result.
            //
            // Strategy: derivs[deriv_order] = N(var) / D(var).  After
            // substitution var -> pole = P/Q, we get
            //   N(P/Q) / D(P/Q)
            // We homogenize: multiply numerator and denominator by
            // Q^deg_in_var(N) to clear P/Q inside N (and similarly for
            // D).  Then substitute var -> P in the homogenized numerator
            // over den=Q^deg.  The cleanest implementation: use Rat's
            // substitute via evaluate-all, but Rat only has
            // substitute_one_rat for a *rational constant*, not a
            // symbolic Rat.  For now, cover the common subcase where
            // `pole.den().is_one()` is true (pole is a polynomial in
            // the other vars) -- substituting var -> pole.num() is a
            // plain multivariate compose, which FLINT supports via
            // fmpq_mpoly_compose_fmpq_mpoly.
            //
            // Rat substitution with non-trivial denominator is a
            // Phase 5 lift; for Phase 2c we throw a clear error.
            // iter-24-3 §B.1 Patch D: deriv is read-only inside both
            // branches (only deriv.num() / deriv.den() are accessed).
            // Each deriv_order index is read exactly once across the
            // multiplicity loop. const& eliminates the Rat copy entirely.
            const Rat& deriv = derivs[deriv_order];

            // Evaluate deriv at var = pole.  For pole with den = 1, do
            // the compose-numerator-only path.  Otherwise bail for now.
            //
            // 2026-04-30 (axis-E dead-to_string cleanup): the original
            // computed `polestr_num = linf.pole.num().to_string()` and
            // never read it (dead code); `polestr_den != "1"` is a
            // string-compare driven by an O(coef-density) Poly::to_string.
            // Same axis-C/D pattern: replaced with structural
            // Poly::is_one(). On parity-1 ord_1_face_1 step 7 this fired
            // hot inside the residue-extraction inner loop.
            const bool pole_den_is_one = linf.pole.den().is_one();
            if (!pole_den_is_one) {
                // General case: evaluate N(var)/D(var) at var = P/Q.
                // Substitute numerator: N(P/Q) = (homog_num(P,Q)) / Q^{deg_N}
                // Same for denominator.
                // iter-24-3 §B.1 Patch E: P and Q are read-only inside
                // the eval_rat_at_rat lambda (used as multiplicands in
                // Pp = Pp * P; and Q.pow(...)). const& avoids 2 Poly copies.
                const Poly& P = linf.pole.num();
                const Poly& Q = linf.pole.den();

                auto eval_rat_at_rat = [&](const Poly& N) {
                    // Compute homogenized N evaluated at var=P, scaled by Q^(deg)
                    long d = std::max<long>(0L, N.degree_in_var(var_idx));
                    Poly acc = Poly::zero_of(ctx);
                    std::vector<Poly> coefs;
                    coefs.reserve(static_cast<size_t>(d + 1));
                    for (long k = 0; k <= d; ++k) {
                        coefs.push_back(N.coefficient_of_var(var_idx, k));
                    }
                    // Horner-like: accumulate sum over k with P^k and Q^(d-k).
                    Poly Pp = Poly::one_of(ctx);
                    for (long k = 0; k <= d; ++k) {
                        // Q^(d-k) = Qp / Q^k ; compute Q^(d-k) freshly:
                        Poly Qdk = (k == d) ? Poly::one_of(ctx)
                                            : Q.pow(static_cast<unsigned long>(d - k));
                        Poly term = coefs[static_cast<size_t>(k)] * Pp * Qdk;
                        acc = acc + term;
                        Pp = Pp * P;
                    }
                    return Rat(std::move(acc), Q.pow(static_cast<unsigned long>(d)));
                };

                Rat N_at = eval_rat_at_rat(deriv.num());
                Rat D_at = eval_rat_at_rat(deriv.den());
                Rat val  = N_at / D_at;

                // Divide by (m-j)! to get c_j.
                if (deriv_order > 0) {
                    // Compute deriv_order! as a long (safe up to 20!).
                    // Falls off to fmpz for large orders; in practice
                    // deriv_order <= pole multiplicity which stays small
                    // for every tst0-through-tst4 input.
                    long fact = 1;
                    for (long k = 2; k <= deriv_order; ++k) fact *= k;
                    val = val / Rat::from_int(ctx, fact);
                }
                pole.coefs.push_back(std::move(val));
                continue;
            }

            // Pole is in "num only", denominator is 1: do clean compose.
            Poly Ncomposed = deriv.num().substitute_one_rat(var_idx, "0");
            // Wait, substitute_one_rat takes a *string rational* value,
            // not a Poly. We need fmpq_mpoly_compose, which I haven't
            // wrapped yet. Fallback: use the eval_rat_at_rat branch
            // above by treating pole.den = 1.
            // Simpler: go through the Q=1 branch of the code above.
            // iter-24-3 §B.1 Patch F: P is read-only inside the
            // eval_rat_at_poly lambda (Pp = Pp * P; only). const& avoids
            // one Poly copy per non-trivial-pole iteration.
            const Poly& P = linf.pole.num();
            auto eval_rat_at_poly = [&](const Poly& N) {
                // Evaluate N(var) at var = P (Poly), returning Poly.
                long d = std::max<long>(0L, N.degree_in_var(var_idx));
                Poly acc = Poly::zero_of(ctx);
                Poly Pp  = Poly::one_of(ctx);
                for (long k = 0; k <= d; ++k) {
                    Poly ck = N.coefficient_of_var(var_idx, k);
                    acc = acc + ck * Pp;
                    Pp = Pp * P;
                }
                return acc;
            };
            Poly Nv = eval_rat_at_poly(deriv.num());
            Poly Dv = eval_rat_at_poly(deriv.den());
            Rat val{std::move(Nv), std::move(Dv)};
            if (deriv_order > 0) {
                long fact = 1;
                for (long k = 2; k <= deriv_order; ++k) fact *= k;
                val = val / Rat::from_int(ctx, fact);
            }
            pole.coefs.push_back(std::move(val));
        }

        out.poles.push_back(std::move(pole));
    }

    // Cache store happens in the public wrapper, not here; impl is
    // pure with respect to the cache.
    return out;
}

}  // namespace (anon, partial_fractions_impl + verify helpers)

}  // namespace hyperflint
