// Rat implementation -- rational function in lowest terms.
//
// Canonical form: gcd(num, den) = 1, and den's sign is "positive" in
// the sense that the leading coefficient of den under FLINT's monomial
// order is positive. Every op restores this invariant.

#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/env_flags_rat.hpp"  // iter-74 §T7 seventh chunk: HF_FLAG_DISABLE_FROM_CANONICAL
#include "hyperflint/core/env_flags_reduce.hpp"  // iter-76 §T7 eighth chunk: HF_FLAG_REDUCE_*, HF_FLAG_REPSWAP_NVARS_MIN
#include "hyperflint/core/operator_memo.hpp"
#include "hyperflint/core/canonical_signature.hpp"
#include "hyperflint/runtime/hf_thread_num.hpp"
#include "hyperflint/algebra/poly_struct_hash.hpp"
#include "hyperflint/diagnostics/env_flags.hpp"
#include "hyperflint/runtime/env_flags.hpp"  // HF_FLAG_WIDE_CTX_AUDIT, HF_FLAG_NO_NARROW_REDUCE (iter-69)
#include "hyperflint/diagnostics/operand_sparsity.hpp"
#include "hyperflint/diagnostics/structural_sharing_probe.hpp"
#include "hyperflint/instrumentation/phase_timer.hpp"  // Track 4.2 iter-26: PoC scope-aware timers (gcd_cofactors_narrow/wide + reduce_narrow cross-cutting)
#include "hyperflint/integrator/ctx_probe.hpp"
#include "hyperflint/runtime/trace_gate.hpp"

#include <cctype>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <flint/fmpq.h>
#include <flint/fmpq_mpoly.h>
#include <flint/fmpz_mpoly.h>
#include <flint/fmpz_mpoly_q.h>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef HF_HAVE_OPENMP
#include <omp.h>
#endif

namespace hyperflint {

namespace {

// 2026-04-27 (3l3pt profile-deepening): per-thread counters and
// timers for the two paths inside `reduce_inplace`. These fire on
// every Rat-producing op (Rat::add, sub, mul, div, pow, derivative,
// and the (Poly,Poly) ctor) — the reviewer's correction to the
// original "rat_add_*" naming, which was misleading.
//
// Hypothesis under test on 3l3pt: the wide-ctx FLINT GCD path
// dominates step-3 wall (where the FLINT mpoly context accumulates
// ~30+ vars including growing Wm/Wp algebraic letters), and the
// narrow-ctx hoist gates out for these inputs because
// `worth_narrowing = used_count*4 < nvars_wide` fails when
// used_count grows alongside nvars_wide.
//
// FALSIFIED 2026-04-27: wide-ctx path is essentially free
// (≤ 0.4 s in every step on 3l3pt). Narrow-ctx hoist is the
// dominant reducer cost. The 125 s "dark mass" between
// `bump_addto_s` and `reduce_narrow_s` in step 3 is the next
// hypothesis — wide-ctx Poly mults inside `Rat::add` operator+
// before reduce_inplace fires, in bump's else-branch only.
//
// Same per-thread vector pattern as `bump_addto_storage` in
// primitive.cpp.  init/reset/sum exposed through rat.hpp for the
// step harvester in integration_step.cpp.  Each storage vector is
// sized at OMP-team init and bounds-checked at use, so the
// standalone CLI path (no team) reads/writes slot 0 safely.
std::vector<double>& reduce_narrow_storage()
    { static std::vector<double> v; return v; }
std::vector<double>& reduce_wide_storage()
    { static std::vector<double> v; return v; }
std::vector<long>&   reduce_narrow_calls_storage()
    { static std::vector<long>   v; return v; }
std::vector<long>&   reduce_wide_calls_storage()
    { static std::vector<long>   v; return v; }
std::vector<long>&   reduce_zero_calls_storage()
    { static std::vector<long>   v; return v; }

// 2026-05-02 (HF FLINT-pool experiment, re-scoped to GCD): wrap just
// the fmpq_mpoly_gcd_cofactors call wall (excluding transplant /
// canonicalization), on both the narrow and wide branches of
// reduce_inplace. Lets us measure FLINT's actual share of
// reduce_inplace, which `reduce_narrow_s` (whole round-trip) over-
// states by 2-3x. See notes/hf_flint_pool_experiment/baseline.md.
std::vector<double>& gcd_cofactors_storage()
    { static std::vector<double> v; return v; }
std::vector<long>&   gcd_cofactors_calls_storage()
    { static std::vector<long>   v; return v; }

// 2026-05-02 (Phase-0-GCD follow-up): three sub-timers decomposing
// the non-GCD share of reduce_inplace's narrow path. Together with
// gcd_cofactors_s on the same call, these let us identify which
// HF-internal primitive dominates the 95% remainder. See
// notes/hf_flint_pool_experiment/gcd_baseline.md (the Sub-timer
// breakdown section).
//   rn_used_vars_s : two fmpq_mpoly_used_vars scans + OR-merge (paid
//                    on every reduce_inplace call where the size
//                    gate passes, regardless of whether the narrow
//                    path is actually taken).
//   rn_setup_s     : narrow PolyCtx construction + two transplants
//                    in (num/den into narrow ctx). Narrow path only.
//   rn_post_s      : sign-canonicalization + two transplants out
//                    (num/den back to wide ctx). Narrow path only.
std::vector<double>& rn_used_vars_storage()
    { static std::vector<double> v; return v; }
std::vector<double>& rn_setup_storage()
    { static std::vector<double> v; return v; }
std::vector<double>& rn_post_storage()
    { static std::vector<double> v; return v; }

// Lever-1 extended (2026-04-27): per-Poly-op timers inside
// `Rat::add` operator+, scoped to disambiguate the 125 s step-3
// "dark mass" between `bump_addto_s` and `reduce_narrow_s`. The
// `add` operator does:
//     Poly tmp1 = num * b.den;          // wide-ctx Poly mul #1
//     Poly tmp2 = b.num * den;          // wide-ctx Poly mul #2
//     Poly new_num = tmp1 + tmp2;       // wide-ctx Poly add
//     Poly new_den = den * b.den;       // wide-ctx Poly mul #3
//     Rat r{new_num, new_den};          // ctor calls reduce_inplace
// We time the three mults + the add separately from reduce_inplace.
// `rat_add_calls_storage` counts Rat::add invocations specifically
// (distinct from `reduce_*_calls`, which count all reducer calls
// from the entire pipeline).
std::vector<double>& rat_add_polymul_storage()
    { static std::vector<double> v; return v; }
std::vector<double>& rat_add_polyadd_storage()
    { static std::vector<double> v; return v; }
std::vector<long>&   rat_add_calls_storage()
    { static std::vector<long>   v; return v; }

// 2026-05-03 (chain 20, Phase 2-A2 PF3 prep): per-thread total wall
// per backend at the Rat::add / operator+= dispatch fork.  Anchors the
// "Tier A1 = 1.75 ms/call" guess from R29 (which has no chain
// measurement; see notes/hf_memory_plan/phase2_repswap/pf1_verdict.md
// CF3).  Always-on; matches the existing rat_add_polymul_storage
// pattern (~2 extra chrono calls per Rat::add invocation, negligible).
//
// `legacy_wall` accumulates wall in the cross-mult+gcd_cofactors
// path (`add_legacy` + the inline fork in `operator+=`).
// `via_qu_wall` accumulates wall in the chain-11 Tier A1 path
// (`add_via_q_underscore`).
std::vector<double>& rat_add_legacy_wall_storage()
    { static std::vector<double> v; return v; }
std::vector<double>& rat_add_via_qu_wall_storage()
    { static std::vector<double> v; return v; }
std::vector<long>&   rat_add_legacy_calls_storage()
    { static std::vector<long>   v; return v; }
std::vector<long>&   rat_add_via_qu_calls_storage()
    { static std::vector<long>   v; return v; }

// 2026-05-01 (Tier 3 Phase-0 lazy-pair diagnostic): per-thread
// nterm-blowup accumulators around `reduce_inplace`. Each call's
// pre-reduce (num.length + den.length) and post-reduce sum are
// captured by an RAII sentinel; per-thread totals + maxes are summed
// after the OMP barrier.  Env-gated by HF_REDUCE_NTERM_LOG=1
// (default off, zero overhead).
//
// The diagnostic answers Tier 3's go/no-go question: "if Rat::add
// skipped reduce_inplace (lazy-pair semantics), would num/den blow
// up under chained ops or stay bounded?"  Reading: avg_shrink =
// post_total/pre_total.  If close to 1, GCD is doing little work →
// deferring is cheap.  If << 1, GCD shrinks polys substantially →
// deferring causes chain-effect blow-up.
std::vector<long>&   reduce_nterm_calls_storage()
    { static std::vector<long>   v; return v; }
std::vector<long>&   reduce_nterm_pre_total_storage()
    { static std::vector<long>   v; return v; }
std::vector<long>&   reduce_nterm_post_total_storage()
    { static std::vector<long>   v; return v; }
std::vector<long>&   reduce_nterm_pre_max_storage()
    { static std::vector<long>   v; return v; }
std::vector<long>&   reduce_nterm_post_max_storage()
    { static std::vector<long>   v; return v; }

// 2026-05-01 (Tier 3 refined lever, reviewer round acd9edeb): counter
// of wide-ctx GCD calls that fall through specifically because of
// the raised size_gate (i.e., len_total < size_gate_min would have
// taken narrow at the legacy threshold of 4 but doesn't now).
// Diagnostic only — fired regardless of HF_REDUCE_SIZE_GATE_MIN
// value (it's the count of "small-poly fallthrough" events that
// the lever creates relative to legacy).
std::vector<long>&   reduce_wide_smallfall_calls_storage()
    { static std::vector<long>   v; return v; }

bool reduce_nterm_log_enabled() {
    static const bool e = []{
        const char* s = HF_FLAG_REDUCE_NTERM_LOG;
        return s && s[0] == '1';
    }();
    return e;
}

// RAII sentinel: capture pre-reduce nterms in ctor, capture
// post-reduce nterms in dtor, update per-thread accumulators.
// Use ONLY when HF_REDUCE_NTERM_LOG=1 (default off → constructed
// inactive, dtor is a no-op fast-path).
struct ReduceNtermLogSentinel {
    Poly& num;
    Poly& den;
    int tid;
    bool active;
    long pre_sum;
    ReduceNtermLogSentinel(Poly& n, Poly& d, int t)
        : num(n), den(d), tid(t),
          active(reduce_nterm_log_enabled()), pre_sum(0) {
        if (!active) return;
        const long pre_num = static_cast<long>(
            fmpq_mpoly_length(num.raw(), num.ctx().raw()));
        const long pre_den = static_cast<long>(
            fmpq_mpoly_length(den.raw(), den.ctx().raw()));
        pre_sum = pre_num + pre_den;
    }
    ~ReduceNtermLogSentinel() {
        if (!active) return;
        const long post_num = static_cast<long>(
            fmpq_mpoly_length(num.raw(), num.ctx().raw()));
        const long post_den = static_cast<long>(
            fmpq_mpoly_length(den.raw(), den.ctx().raw()));
        const long post_sum = post_num + post_den;

        auto& cv = reduce_nterm_calls_storage();
        auto& pre_v = reduce_nterm_pre_total_storage();
        auto& post_v = reduce_nterm_post_total_storage();
        auto& pre_max_v = reduce_nterm_pre_max_storage();
        auto& post_max_v = reduce_nterm_post_max_storage();
        if (static_cast<size_t>(tid) >= cv.size()) return;  // unsized
        cv[tid]      += 1;
        pre_v[tid]   += pre_sum;
        post_v[tid]  += post_sum;
        if (pre_sum  > pre_max_v[tid])  pre_max_v[tid]  = pre_sum;
        if (post_sum > post_max_v[tid]) post_max_v[tid] = post_sum;
    }
};

// Avenue G v4 pre-flight (2026-05-01): support-set fingerprint probe.
//
// HF_REDUCE_SUPPORT_PROBE=1   enables the probe (default off).
//
// Avenue G v1 (struct-hash key, verify-on-hit) regressed parity-1 by
// +24-30 s — the per-call O(nterms) Poly::equal verify dominated the
// saved gcd_cofactors call on small polys (writeup at
// notes/3l3pt_avenue_G_falsifier/avenue_G_writeup_2026-05-01.md).
// Research suggests a coarser key — bitmask of which wide-ctx vars
// appear in (num, den) — would have a cheaper hash and possibly hit
// > 30 % at lower per-call cost.  This probe answers two questions
// simultaneously, in a single instrumented run:
//
//   1. Support-set hit rate (how often the same support recurs).
//   2. Bucket quality: per support_key, how many distinct full-content
//      keys land in it.  Bucket size > 1 means verify-on-hit would
//      churn on false positives — exactly the cost that killed v1.
//      Bucket size ~ 1 means support-set is approximately as
//      discriminative as the full hash and could replace it.
//
// Cost when enabled: ~5 µs/call (poly_struct_hash_raw on num + den) +
// support-mask construction (~30 ns/call) + global-mutex lookup.
// Probe is correctness-neutral: no production output changes.
struct ReduceSupportProbe {
    bool enabled = false;
    long total_calls = 0;

    // For each support_key, the set of distinct full_keys we've seen.
    // Bucket size > 1 means verify-on-hit churn risk.  Both
    // unique_support_keys and unique_full_keys are computed from
    // `buckets` alone at dump time, so per-call updates only do one
    // map+set insert (no separate seen_full structure).
    struct SupportKey {
        uint64_t lo, hi;
        bool operator==(const SupportKey& o) const {
            return lo == o.lo && hi == o.hi;
        }
    };
    struct SupportKeyHash {
        size_t operator()(const SupportKey& k) const {
            return static_cast<size_t>(
                k.lo ^ (k.hi * 0x9e3779b97f4a7c15ULL));
        }
    };
    struct FullKey {
        uint64_t a, b, c, d;
        bool operator==(const FullKey& o) const {
            return a == o.a && b == o.b && c == o.c && d == o.d;
        }
    };
    struct FullKeyHash {
        size_t operator()(const FullKey& k) const {
            return static_cast<size_t>(
                k.a ^ (k.b * 0x9e3779b97f4a7c15ULL)
                ^ k.c ^ (k.d * 0xbf58476d1ce4e5b9ULL));
        }
    };
    std::unordered_map<SupportKey,
                       std::unordered_set<FullKey, FullKeyHash>,
                       SupportKeyHash> buckets;
    std::mutex mu;

    ReduceSupportProbe() {
        const char* e = HF_FLAG_REDUCE_SUPPORT_PROBE;
        if (e && e[0] == '1') {
            enabled = true;
            std::fprintf(stderr,
                "[reduce_support_probe] enabled\n");
        }
    }
    bool is_enabled() const { return enabled; }
    ~ReduceSupportProbe() {
        if (!enabled) return;
        // Derive all summary stats from `buckets` alone.
        const long unique_support_keys =
            static_cast<long>(buckets.size());
        long unique_full_keys = 0;
        long max_bucket = 0;
        long total_collide_buckets = 0;
        long total_distinct_in_collide = 0;
        std::unordered_map<size_t, long> hist;
        for (const auto& kv : buckets) {
            const size_t sz = kv.second.size();
            unique_full_keys += static_cast<long>(sz);
            ++hist[sz];
            if (static_cast<long>(sz) > max_bucket)
                max_bucket = static_cast<long>(sz);
            if (sz >= 2) {
                ++total_collide_buckets;
                total_distinct_in_collide += static_cast<long>(sz);
            }
        }
        const long support_repeats = total_calls - unique_support_keys;
        const long full_repeats    = total_calls - unique_full_keys;
        const double sup_hit = total_calls > 0
            ? 100.0 * static_cast<double>(support_repeats)
              / static_cast<double>(total_calls) : 0.0;
        const double full_hit = total_calls > 0
            ? 100.0 * static_cast<double>(full_repeats)
              / static_cast<double>(total_calls) : 0.0;
        const double mean_collide = total_collide_buckets > 0
            ? static_cast<double>(total_distinct_in_collide)
              / static_cast<double>(total_collide_buckets) : 0.0;
        std::fprintf(stderr,
            "[reduce_support_probe] total=%ld unique_support=%ld "
            "unique_full=%ld support_repeats=%ld full_repeats=%ld "
            "sup_hit=%.2f%% full_hit=%.2f%% "
            "max_bucket=%ld collide_buckets=%ld mean_collide_bucket=%.2f\n",
            total_calls, unique_support_keys, unique_full_keys,
            support_repeats, full_repeats, sup_hit, full_hit,
            max_bucket, total_collide_buckets, mean_collide);
        // Histogram dump for offline analysis.
        std::fprintf(stderr,
            "[reduce_support_probe] bucket_size_histogram:");
        // Sorted-by-key dump (only sizes 1..16 + an "rest" bin to keep
        // stderr small).
        long rest = 0;
        for (size_t sz = 1; sz <= 16; ++sz) {
            auto it = hist.find(sz);
            const long c = (it != hist.end()) ? it->second : 0;
            std::fprintf(stderr, " %zu:%ld", sz, c);
        }
        for (const auto& kv : hist) {
            if (kv.first > 16) rest += kv.second;
        }
        std::fprintf(stderr, " >16:%ld\n", rest);
    }

    // Compute a support fingerprint for `used` (bool-vector of length
    // nvars).  For nvars ≤ 64 this is just the bitmask packed into one
    // uint64; for nvars > 64 we FNV-1a-mix successive 64-bit words.
    static uint64_t support_fingerprint(const std::vector<int>& used) {
        const size_t n = used.size();
        if (n <= 64) {
            uint64_t bits = 0;
            for (size_t j = 0; j < n; ++j)
                if (used[j]) bits |= (uint64_t{1} << j);
            return bits;
        }
        // Wide path: fold 64-bit chunks via FNV-1a.
        constexpr uint64_t kFNV1aOffset = 0xcbf29ce484222325ULL;
        constexpr uint64_t kFNV1aPrime  = 0x100000001b3ULL;
        uint64_t h = kFNV1aOffset;
        size_t j = 0;
        while (j < n) {
            uint64_t bits = 0;
            const size_t end = std::min(j + 64, n);
            for (size_t k = j; k < end; ++k)
                if (used[k]) bits |= (uint64_t{1} << (k - j));
            h ^= bits;
            h *= kFNV1aPrime;
            j = end;
        }
        return h;
    }

    void probe(const Poly& num, const Poly& den,
               const std::vector<int>& used_n,
               const std::vector<int>& used_d) {
        if (!enabled) return;
        SupportKey sk{
            support_fingerprint(used_n),
            support_fingerprint(used_d)
        };
        uint64_t hn1 = 0, hn2 = 0, hd1 = 0, hd2 = 0;
        std::tie(hn1, hn2) = poly_struct_hash_seed();
        poly_struct_hash_raw(hn1, hn2, num);
        std::tie(hd1, hd2) = poly_struct_hash_seed();
        poly_struct_hash_raw(hd1, hd2, den);
        FullKey fk{hn1, hn2, hd1, hd2};
        std::lock_guard<std::mutex> lk(mu);
        ++total_calls;
        buckets[sk].insert(fk);
    }
};
ReduceSupportProbe& reduce_support_probe()
    { static ReduceSupportProbe p; return p; }

// FOLD-4 (per-step aggregation): a global step-id counter that ticks
// at every integration_step boundary via reset_reduce_per_thread().
// Read by the probe to tag each call with the step in which it fired.
// Initial value -1 means "before the first step"; first reset bumps
// to 0. Atomic to be safe against the OMP team teardown path even
// though all reset calls are serial.
std::atomic<int> g_transplant_probe_step_id{-1};

// Phase 3 §A.3 (2026-05-10): canonical FNV-1a 64-bit hash over the
// sorted variable-name list of a wide PolyCtx. Used by the
// HF_WIDE_CTX_AUDIT path to fingerprint the wide ctx in a way that
// is robust to ASLR / heap reuse across distinct logical ctx instances
// (two contexts with identical var lists hash to the same value even
// if their pointers differ). Computed only when the audit is enabled.
static uint64_t hash_var_list_canonical(
        const std::vector<std::string>& vars) {
    std::vector<std::string> sorted = vars;
    std::sort(sorted.begin(), sorted.end());
    constexpr uint64_t kFNV1aOffset = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFNV1aPrime  = 0x100000001b3ULL;
    uint64_t h = kFNV1aOffset;
    for (const auto& v : sorted) {
        for (unsigned char c : v) { h ^= c; h *= kFNV1aPrime; }
        h ^= 0xff; h *= kFNV1aPrime;  // separator
    }
    return h;
}

// Phase 2 §B.2 (2026-05-10 iter-09): Lever Q.1 transplant-recurrence
// probe. Default-OFF. Instruments the worth_narrowing branch of
// reduce_inplace to measure the recurrence rate of the
// (narrow_ctx, wide-num, wide-den) triple over reduce_narrow_calls.
// Iter-09 BINDING reviewer agentId a5c2cd83e2cfc3b6f reframed this
// from a Phase-2 SHIP-eligible lever to a Phase-3-feeder pre-flight:
// FOLD-1 found the original 7.72% ceiling optimistic by ~2x because
// rn_post (transplant-out) cannot be skipped under any wide-ctx-output
// cache scheme without an additional invariant (scheme (c)). Honest
// scheme-(b) ceiling is 3.89%, structurally net-negative after
// per-hit overhead. Probe runs to discover structural recurrence
// rate as Phase-3 design input; iter-10 production-form NOT
// authorized at any rate.
//
// HF_TRANSPLANT_RECURRENCE_PROBE=1  enables the probe (default off).
//
// FOLD-2: key composition mirrors pf_key in
// algebra/partial_fractions.cpp:155-168 (single sentinel between
// num and den; Rat-invariant breaks swap-aliasing upstream).
// FOLD-4: per-step aggregation (step_id from
// HF_STEP_TRACE current_integration_step accessor; falls back to
// step_id=-1 if unavailable, which still gives a global aggregate).
// FOLD-R1: poly-size histogram for STRESS D (overhead vs. signal)
// validation.
struct TransplantRecurrenceProbe {
    bool enabled = false;
    // Phase 3 §A.3 (FOLD-1/3/4 of §A.2 reviewer a0ab7e25712176cb3):
    // independent gate for the wide-ctx-stability audit. Histograms
    // below are populated only when this is true; per-call cost when
    // false is one branch (and one ref-passing of `wide_ctx`).
    bool wide_ctx_audit_enabled = false;
    long total_calls = 0;
    struct TransplantKey {
        uint64_t narrow_id_lo, narrow_id_hi;
        uint64_t poly_h1, poly_h2;
        bool operator==(const TransplantKey& o) const {
            return narrow_id_lo == o.narrow_id_lo
                && narrow_id_hi == o.narrow_id_hi
                && poly_h1 == o.poly_h1
                && poly_h2 == o.poly_h2;
        }
    };
    struct TransplantKeyHash {
        size_t operator()(const TransplantKey& k) const {
            return static_cast<size_t>(
                k.narrow_id_lo
                ^ (k.narrow_id_hi * 0x9e3779b97f4a7c15ULL)
                ^ k.poly_h1
                ^ (k.poly_h2 * 0xbf58476d1ce4e5b9ULL));
        }
    };
    std::unordered_map<TransplantKey, long, TransplantKeyHash>
        bucket_counts;
    // FOLD-4: per-step aggregation. step_id = -1 means "unknown".
    std::unordered_map<int, long> per_step_calls;
    std::unordered_map<int,
        std::unordered_map<TransplantKey, long, TransplantKeyHash>>
        per_step_buckets;
    // FOLD-R1: poly-size histogram. Bin = (num.length+den.length)/8.
    std::unordered_map<long, long> size_histogram;
    // Phase 3 §A.3 wide-ctx audit histograms (per memo §3).
    //   per_step_wide_ctx_ptr: step_id -> wide PolyCtx pointer -> count.
    //     Pointer is `reinterpret_cast<uintptr_t>(&wide_ctx)` at probe
    //     entry, before any narrow ctx construction.
    //   per_step_var_list_hash: step_id -> FNV-1a digest of the
    //     canonical (sorted) wide-ctx var-name list -> count. Robust
    //     to ASLR / heap reuse: identical var lists collide deliberately.
    std::unordered_map<int, std::unordered_map<uintptr_t, long>>
        per_step_wide_ctx_ptr;
    std::unordered_map<int, std::unordered_map<uint64_t, long>>
        per_step_var_list_hash;
    std::mutex mu;

    TransplantRecurrenceProbe() {
        // iter-88 §T7 eighteenth chunk (Track-recurrence-probe LAND):
        // call-site reads HF_FLAG_TRANSPLANT_RECURRENCE_PROBE from
        // hyperflint/core/env_flags_rat.hpp (twelfth precedent of
        // "extend existing same-domain same-family env_flags.hpp"
        // pattern; third extension of this header after iter-84 +
        // iter-87). The macro expands to the same env-var reference
        // previously inlined here verbatim; semantics preserved.
        // Default-direction matches docs/env_flags.md §2 row 268
        // "unset⇒OFF" verbatim: enabled stays false unless e[0]=='1'.
        const char* e = HF_FLAG_TRANSPLANT_RECURRENCE_PROBE;
        if (e && e[0] == '1') {
            enabled = true;
            std::fprintf(stderr,
                "[transplant_recurrence_probe] enabled\n");
        }
        const char* a = HF_FLAG_WIDE_CTX_AUDIT;
        if (a && a[0] == '1') {
            wide_ctx_audit_enabled = true;
            std::fprintf(stderr,
                "[transplant_recurrence_probe] wide_ctx_audit enabled\n");
        }
    }
    bool is_enabled() const { return enabled; }

    void probe(uint64_t narrow_id_lo, uint64_t narrow_id_hi,
               const Poly& num, const Poly& den,
               int step_id,
               const PolyCtx& wide_ctx) {
        if (!enabled) return;
        // Hash construction mirrors pf_key in partial_fractions.cpp:155-168.
        // Single sentinel between num and den hashes; Rat-invariant
        // (den monic-positive) breaks (num,den) swap-aliasing upstream.
        auto seed = poly_struct_hash_seed();
        uint64_t h1 = seed.first;
        uint64_t h2 = seed.second;
        poly_struct_hash_mix(h1, h2, narrow_id_lo);
        poly_struct_hash_mix(h1, h2, narrow_id_hi);
        poly_struct_hash_raw(h1, h2, num);
        // Sentinel separator between num and den hash streams.
        poly_struct_hash_mix(h1, h2, 0xfffffffffffffffeULL);
        poly_struct_hash_raw(h1, h2, den);
        TransplantKey k{narrow_id_lo, narrow_id_hi, h1, h2};
        // Poly-size for FOLD-R1 histogram. Cost: O(1) per FLINT.
        const long size_terms =
            static_cast<long>(fmpq_mpoly_length(num.raw(), num.ctx().raw()))
          + static_cast<long>(fmpq_mpoly_length(den.raw(), den.ctx().raw()));
        const long size_bin = size_terms / 8;
        // Phase 3 §A.3: wide-ctx audit fields. Computed only when
        // HF_WIDE_CTX_AUDIT=1; otherwise zero overhead (one branch).
        const bool wca = wide_ctx_audit_enabled;
        const uintptr_t wide_ptr = wca
            ? reinterpret_cast<uintptr_t>(&wide_ctx) : 0;
        const uint64_t var_list_h = wca
            ? hash_var_list_canonical(wide_ctx.vars()) : 0;
        std::lock_guard<std::mutex> lk(mu);
        ++total_calls;
        ++bucket_counts[k];
        ++per_step_calls[step_id];
        ++per_step_buckets[step_id][k];
        ++size_histogram[size_bin];
        if (wca) {
            ++per_step_wide_ctx_ptr[step_id][wide_ptr];
            ++per_step_var_list_hash[step_id][var_list_h];
        }
    }

    ~TransplantRecurrenceProbe() {
        if (!enabled) return;
        // Global aggregate.
        const long unique_keys = static_cast<long>(bucket_counts.size());
        const long recurrences = total_calls - unique_keys;
        const double rec_rate_pct = total_calls > 0
            ? 100.0 * static_cast<double>(recurrences)
              / static_cast<double>(total_calls) : 0.0;
        // Bucket-size histogram (how often a key was seen N times).
        std::unordered_map<long, long> bucket_size_hist;
        long max_bucket = 0;
        for (const auto& kv : bucket_counts) {
            ++bucket_size_hist[kv.second];
            if (kv.second > max_bucket) max_bucket = kv.second;
        }
        // Top-5 most-recurring keys.
        std::vector<std::pair<long, TransplantKey>> by_count;
        by_count.reserve(bucket_counts.size());
        for (const auto& kv : bucket_counts) {
            by_count.emplace_back(kv.second, kv.first);
        }
        std::sort(by_count.begin(), by_count.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
        const size_t topN = std::min<size_t>(5, by_count.size());

        std::fprintf(stderr,
            "[transplant_recurrence_probe] total=%ld unique=%ld "
            "recurrences=%ld rec_rate=%.2f%% max_bucket=%ld\n",
            total_calls, unique_keys, recurrences, rec_rate_pct,
            max_bucket);

        // Write JSON file under cwd.
        FILE* fp = std::fopen("transplant_recurrence_probe.json", "w");
        if (fp) {
            std::fprintf(fp, "{\n");
            // Phase 3 §A.3 (FOLD-R3 of §A.2 reviewer a0ab7e25712176cb3):
            // schema_version 2 adds per_step_wide_ctx_ptr and
            // per_step_var_list_hash sections (gated on
            // HF_WIDE_CTX_AUDIT=1; empty objects when audit was off).
            std::fprintf(fp, "  \"schema_version\": 2,\n");
            std::fprintf(fp, "  \"wide_ctx_audit_enabled\": %s,\n",
                wide_ctx_audit_enabled ? "true" : "false");
            std::fprintf(fp, "  \"total_calls\": %ld,\n", total_calls);
            std::fprintf(fp, "  \"unique_keys\": %ld,\n", unique_keys);
            std::fprintf(fp, "  \"recurrences\": %ld,\n", recurrences);
            std::fprintf(fp, "  \"recurrence_rate_pct\": %.4f,\n",
                rec_rate_pct);
            std::fprintf(fp, "  \"max_bucket_size\": %ld,\n", max_bucket);
            // Bucket-size histogram.
            std::fprintf(fp, "  \"bucket_size_histogram\": {\n");
            std::vector<long> sizes;
            sizes.reserve(bucket_size_hist.size());
            for (const auto& kv : bucket_size_hist) sizes.push_back(kv.first);
            std::sort(sizes.begin(), sizes.end());
            for (size_t i = 0; i < sizes.size(); ++i) {
                std::fprintf(fp,
                    "    \"%ld\": %ld%s\n", sizes[i],
                    bucket_size_hist[sizes[i]],
                    (i + 1 < sizes.size()) ? "," : "");
            }
            std::fprintf(fp, "  },\n");
            // Top-5.
            std::fprintf(fp, "  \"top_keys\": [\n");
            for (size_t i = 0; i < topN; ++i) {
                std::fprintf(fp,
                    "    {\"count\": %ld, \"narrow_id_lo\": \"0x%llx\", "
                    "\"narrow_id_hi\": \"0x%llx\", \"h1\": \"0x%llx\", "
                    "\"h2\": \"0x%llx\"}%s\n",
                    by_count[i].first,
                    (unsigned long long)by_count[i].second.narrow_id_lo,
                    (unsigned long long)by_count[i].second.narrow_id_hi,
                    (unsigned long long)by_count[i].second.poly_h1,
                    (unsigned long long)by_count[i].second.poly_h2,
                    (i + 1 < topN) ? "," : "");
            }
            std::fprintf(fp, "  ],\n");
            // Per-step aggregate.
            std::fprintf(fp, "  \"per_step\": {\n");
            std::vector<int> steps;
            steps.reserve(per_step_calls.size());
            for (const auto& kv : per_step_calls) steps.push_back(kv.first);
            std::sort(steps.begin(), steps.end());
            for (size_t i = 0; i < steps.size(); ++i) {
                const int sid = steps[i];
                const long sc = per_step_calls[sid];
                const long su = static_cast<long>(per_step_buckets[sid].size());
                const double srr = sc > 0
                    ? 100.0 * static_cast<double>(sc - su)
                      / static_cast<double>(sc) : 0.0;
                std::fprintf(fp,
                    "    \"%d\": {\"calls\": %ld, \"unique\": %ld, "
                    "\"recurrence_rate_pct\": %.4f}%s\n",
                    sid, sc, su, srr,
                    (i + 1 < steps.size()) ? "," : "");
            }
            std::fprintf(fp, "  },\n");
            // Poly-size histogram.
            std::fprintf(fp, "  \"size_histogram\": {\n");
            std::vector<long> bins;
            bins.reserve(size_histogram.size());
            for (const auto& kv : size_histogram) bins.push_back(kv.first);
            std::sort(bins.begin(), bins.end());
            for (size_t i = 0; i < bins.size(); ++i) {
                std::fprintf(fp,
                    "    \"%ld\": %ld%s\n", bins[i] * 8,
                    size_histogram[bins[i]],
                    (i + 1 < bins.size()) ? "," : "");
            }
            std::fprintf(fp, "  },\n");
            // Phase 3 §A.3: per-step wide-ctx pointer histogram (FULL,
            // sorted desc by count per FOLD-R3). Empty inner objects
            // when wide_ctx_audit_enabled was false. Step_ids sorted
            // ascending; ptr/hash entries within a step sorted desc by
            // count so verdict.md can inspect the long tail directly.
            std::fprintf(fp, "  \"per_step_wide_ctx_ptr\": {\n");
            {
                std::vector<int> wptr_steps;
                wptr_steps.reserve(per_step_wide_ctx_ptr.size());
                for (const auto& kv : per_step_wide_ctx_ptr)
                    wptr_steps.push_back(kv.first);
                std::sort(wptr_steps.begin(), wptr_steps.end());
                for (size_t i = 0; i < wptr_steps.size(); ++i) {
                    const int sid = wptr_steps[i];
                    const auto& m = per_step_wide_ctx_ptr[sid];
                    std::vector<std::pair<long, uintptr_t>> entries;
                    entries.reserve(m.size());
                    for (const auto& kv : m)
                        entries.emplace_back(kv.second, kv.first);
                    std::sort(entries.begin(), entries.end(),
                        [](const auto& a, const auto& b) {
                            return a.first > b.first;
                        });
                    std::fprintf(fp, "    \"%d\": {\n", sid);
                    for (size_t j = 0; j < entries.size(); ++j) {
                        std::fprintf(fp,
                            "      \"0x%llx\": %ld%s\n",
                            (unsigned long long)entries[j].second,
                            entries[j].first,
                            (j + 1 < entries.size()) ? "," : "");
                    }
                    std::fprintf(fp, "    }%s\n",
                        (i + 1 < wptr_steps.size()) ? "," : "");
                }
            }
            std::fprintf(fp, "  },\n");
            std::fprintf(fp, "  \"per_step_var_list_hash\": {\n");
            {
                std::vector<int> vh_steps;
                vh_steps.reserve(per_step_var_list_hash.size());
                for (const auto& kv : per_step_var_list_hash)
                    vh_steps.push_back(kv.first);
                std::sort(vh_steps.begin(), vh_steps.end());
                for (size_t i = 0; i < vh_steps.size(); ++i) {
                    const int sid = vh_steps[i];
                    const auto& m = per_step_var_list_hash[sid];
                    std::vector<std::pair<long, uint64_t>> entries;
                    entries.reserve(m.size());
                    for (const auto& kv : m)
                        entries.emplace_back(kv.second, kv.first);
                    std::sort(entries.begin(), entries.end(),
                        [](const auto& a, const auto& b) {
                            return a.first > b.first;
                        });
                    std::fprintf(fp, "    \"%d\": {\n", sid);
                    for (size_t j = 0; j < entries.size(); ++j) {
                        std::fprintf(fp,
                            "      \"0x%llx\": %ld%s\n",
                            (unsigned long long)entries[j].second,
                            entries[j].first,
                            (j + 1 < entries.size()) ? "," : "");
                    }
                    std::fprintf(fp, "    }%s\n",
                        (i + 1 < vh_steps.size()) ? "," : "");
                }
            }
            std::fprintf(fp, "  }\n");
            std::fprintf(fp, "}\n");
            std::fclose(fp);
            std::fprintf(stderr,
                "[transplant_recurrence_probe] wrote "
                "transplant_recurrence_probe.json\n");
        } else {
            std::fprintf(stderr,
                "[transplant_recurrence_probe] failed to open "
                "transplant_recurrence_probe.json for writing\n");
        }
    }
};
TransplantRecurrenceProbe& transplant_recurrence_probe()
    { static TransplantRecurrenceProbe p; return p; }

// =====================================================================
// Phase 3 §C Lever V — narrow-ctx-fixed result cache (scheme-(c)).
// Memo: notes/hf_finite_field_program/phase3_combined/lever_v_scheme_c_cache/design.md
// Authority: Phase 3 §A wide-ctx-stability audit verdict (PASS, commit
// c6c295907) authorises (narrow_ctx_fp, num_hash, den_hash) keying
// without step_id and without per-call wide_ctx_ptr equality check;
// Phase 2 Lever Q.1 probe (commit 26f0fd973) measured 91.89% global
// recurrence on tst2.
//
// Per FOLD-1 + FOLD-5 (iter-11 binding reviewer agentId
// a6c839c1fe0b0b99c), the default cache is single-global +
// std::shared_mutex (~1.3 GB peak; +7.4% contention) rather than
// per-thread (~11 GB peak; ~0% sync overhead). Per FOLD-4, cached
// Polys are bound to the wide_ctx active at insert time; the
// clear-on-step-transition discipline (in reset_reduce_per_thread,
// BEFORE the next OMP region) prevents any cross-step heap-aliasing
// hazard.
//
// Default-OFF (HF_LEVER_V_ENABLE unset/=0): no allocation, no lookup,
// no insert; clear is a no-op on an empty global map. Byte-id with
// hyperflint_v2 is preserved by the §C.2.4 + §C.2.5 sha-id 4/4 PASS
// gates.
// =====================================================================

struct LeverVKey {
    uint64_t narrow_id_lo;     // bitmask of `used` (or FNV-1a fold)
    uint64_t narrow_id_hi;     // FNV-1a fold OR nvars_wide tag
    uint64_t num_hash;         // FNV-1a-folded fmpq_mpoly content hash of `num`
    uint64_t den_hash;         // FNV-1a-folded fmpq_mpoly content hash of `den`
    bool operator==(const LeverVKey& o) const noexcept {
        return narrow_id_lo == o.narrow_id_lo
            && narrow_id_hi == o.narrow_id_hi
            && num_hash    == o.num_hash
            && den_hash    == o.den_hash;
    }
};

struct LeverVKeyHasher {
    size_t operator()(const LeverVKey& k) const noexcept {
        // The four 64-bit fields are already well-distributed FNV-1a
        // outputs (narrow_id from Q.1 bitmask/FNV-1a fold;
        // num/den_hash from poly_struct_hash_raw 128-bit fold). XOR
        // with rotation gives a cheap bucket index without forfeiting
        // distinguishability.
        uint64_t h = k.narrow_id_lo;
        h ^= (k.narrow_id_hi << 17) | (k.narrow_id_hi >> 47);
        h ^= (k.num_hash     << 31) | (k.num_hash     >> 33);
        h ^= (k.den_hash     << 13) | (k.den_hash     >> 51);
        return static_cast<size_t>(h);
    }
};

struct LeverVCachedPair {
    Poly num_out;
    Poly den_out;
};

// Single-global cache + shared_mutex (FOLD-1 + FOLD-5 default; §6.1).
// Process-wide; one heap allocation per inserted key (Poly's internal
// fmpq_mpoly_t plus an unordered_map node). Lifetime per-step:
// `lever_v_cache_clear()` is called at the top of every
// `reset_reduce_per_thread()` BEFORE the next step's OMP region per
// §A.6 verdict §5 / FOLD-4 lifecycle chain.
std::unordered_map<LeverVKey, LeverVCachedPair, LeverVKeyHasher>&
lever_v_cache_storage() {
    static std::unordered_map<LeverVKey, LeverVCachedPair,
                              LeverVKeyHasher> m;
    return m;
}
std::shared_mutex& lever_v_cache_mutex() {
    static std::shared_mutex m;
    return m;
}

// thread_local cached env reads (mirrors hf_gcd_chunk_size from §B
// + transplant_recurrence_probe from Q.1). Per design memo §2.1,
// HF_LEVER_V_ENABLE values:
//   0 -> disabled (default).
//   1 -> single-global + shared_mutex (iter-12 default-ON variant;
//        FOLD-1 + FOLD-5 corrected from per-thread default).
//   2..3 -> reserved for iter-13 §C.3 sweep variants (per-thread,
//        sharded). iter-12 source treats them as disabled (no
//        mis-routing into a half-implemented variant).
inline int hf_lever_v_enable() {
    static thread_local int cached = -1;
    if (cached >= 0) return cached;
    // iter-89: env var read routed through HF_FLAG_LEVER_V_ENABLE in
    // hyperflint/core/env_flags_rat.hpp so the registry parser-B grep
    // discovers the literal name in the include/-tree macro layer
    // rather than here. Macro expands to the const char* result of the
    // underlying getenv lookup; semantics (null/empty -> 0, else strtol
    // clamped to [0, 3]) preserved verbatim from the pre-macro form.
    const char* e = HF_FLAG_LEVER_V_ENABLE;
    if (!e || !e[0]) {
        cached = 0;
    } else {
        char* end = nullptr;
        long v = std::strtol(e, &end, 10);
        if (end == e || v < 0 || v > 3) v = 0;
        cached = static_cast<int>(v);
    }
    return cached;
}
inline bool hf_lever_v_active() {
    // Iter-12 ships only HF_LEVER_V_ENABLE=1 (single-global). Other
    // values (=2/=3, reserved) are NOT active in iter-12 source.
    return hf_lever_v_enable() == 1;
}
inline bool hf_lever_v_probe_enabled() {
    static thread_local int cached = -1;
    if (cached >= 0) return cached != 0;
    // iter-89: env var read routed through HF_FLAG_LEVER_V_PROBE in
    // hyperflint/core/env_flags_rat.hpp; same registry-parser rationale
    // as hf_lever_v_enable() above. Semantics preserved verbatim
    // (cached = 1 iff e is non-null and its first character is the
    // ASCII digit one, else 0).
    const char* e = HF_FLAG_LEVER_V_PROBE;
    cached = (e && e[0] == '1') ? 1 : 0;
    return cached != 0;
}

// Lookup under shared lock. Returns std::nullopt on miss; on hit,
// the cached Polys are deep-copied out of the cache (Poly's copy ctor
// wraps fmpq_mpoly_set against the same wide_ctx; per §A.6 the wide
// ctx is invariant within a step so the cached Polys' ctx pointer
// matches the active call's ctx pointer).
inline std::optional<LeverVCachedPair>
lever_v_cache_lookup(const LeverVKey& k) {
    auto& m = lever_v_cache_storage();
    auto& mu = lever_v_cache_mutex();
    std::shared_lock<std::shared_mutex> lock(mu);
    auto it = m.find(k);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

// Insert under exclusive lock. try_emplace skips overwriting if
// another thread inserted the same key concurrently between this
// thread's miss-path lookup and this insert (lost-race semantics:
// keep whichever entry landed first; both compute the same value
// per §A.6's deterministic gcd_cofactors contract).
inline void lever_v_cache_insert(const LeverVKey& k,
                                  const Poly& num_out,
                                  const Poly& den_out) {
    auto& m = lever_v_cache_storage();
    auto& mu = lever_v_cache_mutex();
    std::unique_lock<std::shared_mutex> lock(mu);
    m.try_emplace(k, LeverVCachedPair{num_out, den_out});
}

// Single-threaded clear at top of reset_reduce_per_thread, BEFORE
// the next step's OMP region (§A.6 verdict §5 / FOLD-2 timing
// correction from §A.2 reviewer). The shared_mutex unique_lock is
// uncontested (no concurrent readers/writers); the clear destroys
// every cached Poly, releasing wide-ctx-bound fmpq_mpoly_t storage
// before any next-step wide_ctx pointer change could occur.
inline void lever_v_cache_clear() {
    auto& m = lever_v_cache_storage();
    auto& mu = lever_v_cache_mutex();
    std::unique_lock<std::shared_mutex> lock(mu);
    m.clear();
}

// Branch I (2026-05-01 evening): canonical Rat-operand QUAD dumper.
//
// Dumps (a.num, a.den, b.num, b.den) at the entry of Rat::add and
// Rat::operator+= so the fmpz_mpoly_q POC bench can run on
// canonical operands taken directly from the production hot path,
// answering reviewer F1+F2 (small-poly, post-reduce-input bias).
//
// HF_DUMP_RAT_QUADS=1            enables the dumper.
// HF_DUMP_RAT_QUADS_PATH=<file>  output path (default rat_quads_dump.txt).
// HF_DUMP_RAT_QUADS_MAX=<N>      stop after N quads (default 50).
// HF_DUMP_RAT_QUADS_STRIDE=<S>   only dump every S-th call (default 1).
// HF_DUMP_RAT_QUADS_MIN_NVARS=<M> require nvars >= M (default 0).
//
// Format:
//   QUAD <i>
//   VARS: v1,v2,...
//   A_NUM: ...
//   A_DEN: ...
//   B_NUM: ...
//   B_DEN: ...
//   ENDQUAD
struct RatAddQuadDumper {
    bool enabled = false;
    long max_count = 50;
    long stride = 1;
    long min_nvars = 0;
    long call_counter = 0;
    long dumped_count = 0;
    FILE* fp = nullptr;
    std::mutex mu;

    RatAddQuadDumper() {
        const char* g = HF_FLAG_DUMP_RAT_QUADS;
        if (!g || g[0] != '1') return;
        const char* p = HF_FLAG_DUMP_RAT_QUADS_PATH;
        const char* path = (p && p[0]) ? p : "rat_quads_dump.txt";
        if (const char* m = HF_FLAG_DUMP_RAT_QUADS_MAX)
            try { max_count = std::max(1L, std::stol(m)); } catch (...) {}
        if (const char* s = HF_FLAG_DUMP_RAT_QUADS_STRIDE)
            try { stride = std::max(1L, std::stol(s)); } catch (...) {}
        if (const char* mv = HF_FLAG_DUMP_RAT_QUADS_MIN_NVARS)
            try { min_nvars = std::max(0L, std::stol(mv)); } catch (...) {}
        fp = std::fopen(path, "w");
        if (!fp) {
            std::fprintf(stderr, "[rat_quads_dump] failed to open %s\n", path);
            return;
        }
        enabled = true;
        std::fprintf(stderr,
            "[rat_quads_dump] enabled: path=%s max=%ld stride=%ld "
            "min_nvars=%ld\n", path, max_count, stride, min_nvars);
    }
    ~RatAddQuadDumper() {
        if (fp) {
            std::fprintf(stderr,
                "[rat_quads_dump] dumped=%ld total_calls_seen=%ld\n",
                dumped_count, call_counter);
            std::fclose(fp);
        }
    }
    void maybe_dump(const Poly& a_num, const Poly& a_den,
                    const Poly& b_num, const Poly& b_den) {
        if (!enabled) return;
        std::lock_guard<std::mutex> lk(mu);
        const long c = ++call_counter;
        if (dumped_count >= max_count) return;
        if ((c % stride) != 0) return;
        const PolyCtx& ctx = a_num.ctx();
        const auto& vars = ctx.vars();
        if (static_cast<long>(vars.size()) < min_nvars) return;
        // Sanity: all four polys should share the same ctx in practice.
        // Skip the dump if not, rather than risk a crash.
        if (&a_den.ctx() != &ctx || &b_num.ctx() != &ctx ||
            &b_den.ctx() != &ctx) return;
        std::vector<const char*> varptrs;
        varptrs.reserve(vars.size());
        for (const auto& v : vars) varptrs.push_back(v.c_str());
        char* an_s = fmpq_mpoly_get_str_pretty(a_num.raw(), varptrs.data(), ctx.raw());
        char* ad_s = fmpq_mpoly_get_str_pretty(a_den.raw(), varptrs.data(), ctx.raw());
        char* bn_s = fmpq_mpoly_get_str_pretty(b_num.raw(), varptrs.data(), ctx.raw());
        char* bd_s = fmpq_mpoly_get_str_pretty(b_den.raw(), varptrs.data(), ctx.raw());
        std::fprintf(fp, "QUAD %ld\n", dumped_count);
        std::fputs("VARS: ", fp);
        for (size_t i = 0; i < vars.size(); ++i) {
            if (i) std::fputc(',', fp);
            std::fputs(vars[i].c_str(), fp);
        }
        std::fputc('\n', fp);
        std::fprintf(fp, "A_NUM: %s\n", an_s ? an_s : "0");
        std::fprintf(fp, "A_DEN: %s\n", ad_s ? ad_s : "1");
        std::fprintf(fp, "B_NUM: %s\n", bn_s ? bn_s : "0");
        std::fprintf(fp, "B_DEN: %s\n", bd_s ? bd_s : "1");
        std::fputs("ENDQUAD\n", fp);
        std::fflush(fp);
        if (an_s) flint_free(an_s);
        if (ad_s) flint_free(ad_s);
        if (bn_s) flint_free(bn_s);
        if (bd_s) flint_free(bd_s);
        ++dumped_count;
    }
};
RatAddQuadDumper& rat_add_quad_dumper()
    { static RatAddQuadDumper d; return d; }

// Reduce (num, den) to lowest terms, in place.
//
// Narrow-ctx hoist: the previous implementation called num.gcd(den),
// num.divexact(g), den.divexact(g) separately. Each of those — at
// least for gcd — runs a narrow-ctx transplant internally, so we
// paid three transplant cycles when one would do. This version does
// the used-vars scan once, transplants num+den into a narrow ctx
// once, runs gcd + the two divexacts *inside* the narrow ctx, and
// transplants only the two final results back. Correctness rests
// on the invariant that `a divides b` holds in ctx C iff it holds
// in any narrower ctx that contains every variable used by both
// polys (proven in the round-8 review).
//
// The reviewer's benchmarks showed that narrowing the mul/divexact
// ops *in isolation* regresses — narrow-ctx pays off only when the
// transplant cost is amortized across at least one gcd call. This
// function is the only place that amortization holds.
//
// HF FF Phase 5 §E Step E.2-impl-2 (iter-62-β.1): renamed from
// `reduce_inplace` to `reduce_inplace_impl` and made static (file-local).
// The new public `reduce_inplace` (defined just past this body) is the
// §E operator-memo wrap; on cache MISS it delegates here. The internal
// callers at rat.cpp:2115 + 2486 (Rat constructors) go through the
// public wrap — fixture-boundary cache HITs short-circuit the body
// entirely.
static void reduce_inplace_impl(Poly& num, Poly& den) {
    if (den.is_zero()) {
        throw std::runtime_error("Rat: zero denominator");
    }
    // Track 4.2 iter-26 PoC cross-cutting phase-timer.  parents=0 →
    // predicate-true unconditional; RAII tick lives until function exit,
    // accumulating wall for the WHOLE reduce_inplace_impl body including
    // early-outs (Avenue I constant-den path + num-zero path). The legacy
    // `reduce_narrow_storage` accumulator at line 1444-1452 (narrow path
    // only) is preserved unchanged — Track 4.2 v1 is ADDITIVE.  iter-25
    // §F audit row 13 + handoff §"reduce_narrow" cross-cutting design
    // ratifies function-entry placement over the iter-25 draft's
    // line-1345 (narrow-only) placement.
    HF_PHASE_TIMER_TICK(reduce_narrow);
    // HF FF Phase 5 §A.1 iter-50: op_call emit at reduce_inplace entry
    // (§3.1 op #2). Arity=2 records (num, den) input tuple; hash combines
    // both polys in declaration order. OFF-path fast-guard via
    // `hf_probe_active` branch.
    if (hf_probe_active) {
        uint64_t ih = kFnv1a64OffsetBasis;
        ih = hf_probe_fnv1a64_mix_u64(ih,
                hf_probe_canonical_hash_poly(num.raw(), num.ctx().raw()));
        ih = hf_probe_fnv1a64_mix_u64(ih,
                hf_probe_canonical_hash_poly(den.raw(), den.ctx().raw()));
        hf_probe_emit_op_call("reduce_inplace", ih, 2);
    }
    // Resolve thread id once; per-thread storage slots are bounds-checked
    // since init_reduce_per_thread may not have been called on the
    // standalone CLI path.
    // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num() in OMP mode;
    // under HF_USE_GCD=1 returns the GCD slot index.
    const int _ra_tid = ::hyperflint::runtime::hf_get_thread_num();
    // Tier 3 Phase-0 nterm-blowup diagnostic.  Sentinel captures
    // pre-reduce (num+den).length() in ctor, post-reduce in dtor,
    // updating per-thread accumulators.  Inactive (zero overhead)
    // unless HF_REDUCE_NTERM_LOG=1.
    ReduceNtermLogSentinel _rnt_sentinel(num, den, _ra_tid);

    // Round-19 wide-ctx probe (HF_PROBE_CTX_USAGE).  RAII sentinel
    // records vars-with-nonzero-exp from the post-reduce num and den
    // into thread-local seen[var]. Zero overhead unless env set.
    struct CtxProbeSentinel {
        Poly* num; Poly* den;
        ~CtxProbeSentinel() {
            ctx_probe_record(num->raw(), num->ctx());
            ctx_probe_record(den->raw(), den->ctx());
        }
    } _ctx_probe{&num, &den};
    if (num.is_zero()) {
        // 0/den -> 0/1.  Counts as a "zero" call for accounting
        // completeness; no GCD work performed.
        auto& _zv = reduce_zero_calls_storage();
        if (static_cast<size_t>(_ra_tid) < _zv.size()) _zv[_ra_tid] += 1;
        den = Poly::one_of(num.ctx());
        return;
    }

    // Avenue I (2026-04-30): short-circuit when den is a pure fmpq
    // constant. fmpq_mpoly_gcd over ℚ returns 1 when one argument is a
    // unit, so the full gcd_cofactors call collapses to "absorb the
    // constant into num and set den := 1." Hoisting the check upfront
    // saves one fmpq_mpoly_gcd_cofactors invocation, the used-vars
    // scan, and (in the narrow path) the entire transplant round-trip
    // — all wasted work on rats with constant denominator.
    //
    // Correctness: num.scalar_div_fmpq_const(den) divides every
    // coefficient of num by the rational constant `c` in den; setting
    // den = 1 yields the canonical form num/1. Sign-canonicalization
    // (the post-GCD branch) is automatic: scalar_div_fmpq_const with
    // c < 0 flips signs in num, and den = 1 has positive leading coef.
    if (den.is_fmpq()) {
        const auto _ra_w0 = std::chrono::steady_clock::now();
        num.scalar_div_fmpq_const(den);
        den = Poly::one_of(num.ctx());
        const auto _ra_w1 = std::chrono::steady_clock::now();
        auto& _wv = reduce_wide_storage();
        if (static_cast<size_t>(_ra_tid) < _wv.size())
            _wv[_ra_tid] += std::chrono::duration<double>(
                _ra_w1 - _ra_w0).count();
        auto& _cv = reduce_wide_calls_storage();
        if (static_cast<size_t>(_ra_tid) < _cv.size())
            _cv[_ra_tid] += 1;
        return;
    }

    const PolyCtx& ctx = num.ctx();
    const size_t nvars_wide = ctx.vars().size();

    // Size gate: skip the narrow-ctx hoist for small polys. The
    // transplant round-trip dominates when polys are trivially small.
    // Threshold chosen from the adversarial review's FLINT benchmarks:
    // sum-of-lengths < 4 is a net loss; >= 4 the gcd Johnson/
    // subresultant work starts to dominate. Same constant as
    // Poly::gcd::kNarrowMinLen.
    //
    // 2026-05-02 correction: an earlier version of this comment cited
    // a "~33% transplant / ~16% real poly math" tst1/tst2 profile.
    // That was wrong by an order of magnitude. Direct measurement
    // (gcd_cofactors_s timer wrapping just fmpq_mpoly_gcd_cofactors)
    // shows the FLINT-GCD share is ~5% of reduce_narrow_s wall on
    // tst1/tst2 today, not 16%. The remaining ~95% is everything
    // *around* the GCD call: two fmpq_mpoly_used_vars scans, narrow
    // PolyCtx construction, two transplants in (num/den → narrow),
    // sign-canonicalization, and two transplants back. See
    // notes/hf_flint_pool_experiment/gcd_baseline.md.
    const slong len_total =
        fmpq_mpoly_length(num.raw(), ctx.raw())
        + fmpq_mpoly_length(den.raw(), ctx.raw());
    // 2026-05-01 (Tier 3 refined lever): env-gated raise of the size-gate
    // threshold. Default 4 = current behavior. Higher values push more
    // tiny-poly calls onto the wide-ctx GCD path, skipping the narrow-ctx
    // transplant overhead. Phase-0 nterm-blowup data showed 97% of
    // reduce_inplace calls (steps 5-7 of parity-1) operate on polys with
    // avg num+den length 2-18 and shrink ratio 0.85-0.99 — GCD removes
    // ~0-1 monomials per call but pays the full transplant + narrow-ctx
    // setup cost. Raising the threshold may amortize that overhead by
    // routing through wide-ctx instead.
    static const slong size_gate_min_static = []{
        const char* e = HF_FLAG_REDUCE_SIZE_GATE_MIN;
        if (!e || !*e) return slong{4};
        long v = std::strtol(e, nullptr, 10);
        return v > 0 ? static_cast<slong>(v) : slong{4};
    }();
    // 2026-05-01 (adaptive size-gate): when HF_REDUCE_SIZE_GATE_DIVISOR=N
    // is set (N > 0), the threshold becomes per-call:
    //   gate_min = max(4, nvars_wide / DIVISOR)
    // This auto-tunes between fixtures: parity-1 (nvars_wide ≈ 30 in
    // late steps) gets gate_min ≈ 10 at DIVISOR=3, capturing the win;
    // tst2 (nvars_wide ≈ 5-12) gets gate_min ≈ 4 (legacy), no
    // regression. DIVISOR overrides _MIN if both are set.
    static const slong size_gate_divisor = []{
        const char* e = HF_FLAG_REDUCE_SIZE_GATE_DIVISOR;
        if (!e || !*e) return slong{0};
        long v = std::strtol(e, nullptr, 10);
        return v > 0 ? static_cast<slong>(v) : slong{0};
    }();
    slong size_gate_min;
    if (size_gate_divisor > 0) {
        slong adaptive = static_cast<slong>(nvars_wide) / size_gate_divisor;
        size_gate_min = std::max(slong{4}, adaptive);
    } else {
        size_gate_min = size_gate_min_static;
    }
    const bool size_gate_passes = len_total >= size_gate_min;
    // Diagnostic: count calls that the raised threshold pushed into
    // the wide-ctx fallthrough (would have taken narrow at legacy
    // threshold 4 but don't at the new threshold).
    if (size_gate_min > 4 && len_total >= 4 && !size_gate_passes) {
        auto& sfv = reduce_wide_smallfall_calls_storage();
        if (static_cast<size_t>(_ra_tid) < sfv.size())
            sfv[_ra_tid] += 1;
    }

    // Scan used-var union of num and den via FLINT's native primitive.
    // Despite the internal "_or_" in mpoly_used_vars_or_sp, the
    // public fmpq_mpoly_used_vars OVERWRITES the buffer (verified
    // empirically — do not rely on accumulation). Use two separate
    // buffers and OR-merge.
    size_t used_count = 0;
    std::vector<int> used;
    // Avenue G v4 pre-flight (HF_REDUCE_SUPPORT_PROBE): hoist num+den
    // bool vectors so the worth_narrowing block can probe them later.
    // Default-empty when probe is disabled — zero allocation cost.
    std::vector<int> used_n_for_probe;
    std::vector<int> used_d_for_probe;
    if (size_gate_passes) {
        const auto _uv_t0 = std::chrono::steady_clock::now();
        used.assign(nvars_wide, 0);
        std::vector<int> used_n(nvars_wide, 0);
        fmpq_mpoly_used_vars(used_n.data(),
            const_cast<fmpq_mpoly_struct*>(num.raw()), ctx.raw());
        fmpq_mpoly_used_vars(used.data(),
            const_cast<fmpq_mpoly_struct*>(den.raw()), ctx.raw());
        // Snapshot before OR-merge clobbers `used`.
        if (reduce_support_probe().is_enabled()) {
            used_n_for_probe = used_n;
            used_d_for_probe = used;
        }
        for (size_t j = 0; j < nvars_wide; ++j)
            if (used_n[j]) used[j] = 1;
        for (size_t j = 0; j < nvars_wide; ++j) if (used[j]) ++used_count;
        {
            auto& _uv = rn_used_vars_storage();
            if (static_cast<size_t>(_ra_tid) < _uv.size())
                _uv[_ra_tid] += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - _uv_t0).count();
        }
    }
    // 2026-04-30 (Tier 1.8 disable-narrow experiment): force-disable
    // when HF_NO_NARROW_REDUCE=1 to characterise reduce_narrow_s
    // contribution. Heap-allocated static cache avoids per-call getenv.
    static const bool no_narrow_reduce = []{
        const char* e = HF_FLAG_NO_NARROW_REDUCE;
        return e && e[0] == '1';
    }();
    const bool worth_narrowing =
        !no_narrow_reduce && size_gate_passes && used_count * 4 < nvars_wide;

    if (worth_narrowing) {
        // Track 4.2 iter-26 PoC scope guard.  Sets the narrow-branch bit
        // in g_active_scope_mask for the rest of this block; combined
        // with the omp_parallel_for_integration_step scope (set by
        // integration_step.cpp's per-iteration HF_SCOPE_ENTER), this
        // satisfies the parents-mask predicate for the
        // `gcd_cofactors_narrow` timer at rat.cpp:~1399 below.  RAII
        // dtor pops the bit at the closing brace of this if-block.
        HF_SCOPE_ENTER(rat_reduce_inplace_narrow_branch);
        // Avenue G v4 pre-flight: this is the same call subset as the
        // v1 probe (worth_narrowing == true).  Probe takes the
        // pre-OR-merge snapshots of num/den support vectors plus the
        // wide-ctx polys for full-content hashing.  No-op when env
        // unset.
        reduce_support_probe().probe(num, den,
            used_n_for_probe, used_d_for_probe);
        // Phase 2 §B.2 iter-09 Lever Q.1 transplant-recurrence probe.
        // Default-OFF; correctness-neutral. Computes the (narrow_ctx,
        // wide-num, wide-den) triple key over reduce_narrow_calls and
        // emits global + per-step recurrence-rate aggregates at process
        // exit. Iter-08 verdict.md §6 Q.1 + iter-09 binding-reviewer
        // FOLD-4 require per-step aggregation; FOLD-1 reframes the
        // probe outcome as Phase-3-feeder data (no SHIP cell at iter-09).
        //
        // Phase 3 §C.2 (Lever V) reuses the same FOLD-2 narrow_id
        // encoding for the cache key; the encoding is hoisted out of
        // the Q.1-only branch so Lever V can run independently of
        // HF_TRANSPLANT_RECURRENCE_PROBE. When BOTH knobs are off
        // (default-OFF), `need_narrow_id` is false and the encoding
        // is skipped entirely (zero-overhead default-OFF).
        const bool _lever_v_active_in_branch = hf_lever_v_active();
        const bool _q1_probe_active =
            transplant_recurrence_probe().is_enabled();
        const bool need_narrow_id =
            _q1_probe_active || _lever_v_active_in_branch;
        uint64_t narrow_id_lo = 0;
        uint64_t narrow_id_hi = static_cast<uint64_t>(nvars_wide);
        if (need_narrow_id) {
            // FOLD-2: narrow_id encoding. Bitmask path for nvars_wide
            // <= 64 (covers tst2 entirely; nvars_wide <= 12 per
            // HF_STEP_TRACE iter-08 step-3 data). FNV-1a-folded path
            // for wider contexts (defensive coverage; parity-1 step-7+
            // ~30 vars).
            if (nvars_wide <= 64) {
                for (size_t j = 0; j < nvars_wide; ++j) {
                    if (used[j]) {
                        narrow_id_lo |= (uint64_t{1} << j);
                    }
                }
            } else {
                // Fold 64-bit chunks via FNV-1a into a 128-bit pair.
                constexpr uint64_t kFNV1aOffset1 = 0xcbf29ce484222325ULL;
                constexpr uint64_t kFNV1aOffset2 = 0x84222325cbf29ce4ULL;
                constexpr uint64_t kFNV1aPrime  = 0x100000001b3ULL;
                uint64_t h1 = kFNV1aOffset1;
                uint64_t h2 = kFNV1aOffset2;
                size_t j = 0;
                while (j < nvars_wide) {
                    uint64_t bits = 0;
                    const size_t end = std::min(j + 64, nvars_wide);
                    for (size_t k = j; k < end; ++k) {
                        if (used[k]) bits |= (uint64_t{1} << (k - j));
                    }
                    h1 ^= bits;
                    h1 *= kFNV1aPrime;
                    h2 ^= (bits ^ static_cast<uint64_t>(j));
                    h2 *= kFNV1aPrime;
                    j = end;
                }
                narrow_id_lo = h1;
                narrow_id_hi = h2;
            }
        }
        if (_q1_probe_active) {
            const int step_id =
                g_transplant_probe_step_id.load(std::memory_order_relaxed);
            transplant_recurrence_probe().probe(
                narrow_id_lo, narrow_id_hi, num, den, step_id, ctx);
        }

        // Phase 3 §C.2 Lever V cache lookup (default-OFF; gated by
        // HF_LEVER_V_ENABLE=1). Computes 128-bit content hashes for
        // num + den via poly_struct_hash_raw, folds each pair to
        // 64-bit (XOR), composes the LeverVKey, and looks up the
        // single-global cache under shared lock. On hit, replaces
        // (num, den) with the cached (num_out, den_out) pair and
        // returns BEFORE the narrow PolyCtx construction +
        // transplant-in + gcd_cofactors_narrow + transplant-back
        // chain below — the entire `reduce_narrow` mass that §1.3
        // FOLD-2 corrected ceiling targets (~6-8% of v2 wall).
        // On miss, captures the key for the post-transplant insert
        // at the bottom of the worth_narrowing block.
        LeverVKey lever_v_key{0, 0, 0, 0};
        if (_lever_v_active_in_branch) {
            const auto _seed = poly_struct_hash_seed();
            uint64_t nh1 = _seed.first;
            uint64_t nh2 = _seed.second;
            poly_struct_hash_raw(nh1, nh2, num);
            const uint64_t num_hash = nh1 ^ nh2;
            uint64_t dh1 = _seed.first;
            uint64_t dh2 = _seed.second;
            poly_struct_hash_raw(dh1, dh2, den);
            const uint64_t den_hash = dh1 ^ dh2;
            lever_v_key = LeverVKey{narrow_id_lo, narrow_id_hi,
                                    num_hash, den_hash};
            if (auto cached = lever_v_cache_lookup(lever_v_key)) {
                // HIT: replace num + den with cached output. The
                // cached Polys' wide_ctx pointer matches the active
                // call's wide_ctx pointer per §A.6 verdict (100.0000%
                // dominant_ptr_share_pct across all active step_ids).
                // Poly::operator= deep-copies via fmpq_mpoly_set
                // against the matching ctx; no transplant or sign
                // canonicalisation needed (the cached value already
                // passed through both at insert time).
                num = cached->num_out;
                den = cached->den_out;
                return;
            }
        }
        const auto _ra_t0 = std::chrono::steady_clock::now();
        const auto _setup_t0 = _ra_t0;
        std::vector<std::string> narrow_var_names;
        std::vector<size_t> wide_to_narrow(nvars_wide, SIZE_MAX);
        std::vector<size_t> narrow_to_wide;
        narrow_var_names.reserve(used_count);
        narrow_to_wide.reserve(used_count);
        for (size_t j = 0; j < nvars_wide; ++j) {
            if (!used[j]) continue;
            wide_to_narrow[j] = narrow_to_wide.size();
            narrow_to_wide.push_back(j);
            narrow_var_names.push_back(ctx.vars()[j]);
        }
        // Lifetime note: `narrow` outlives every narrow Poly below
        // (LIFO scope destruction).
        const PolyCtx narrow(std::move(narrow_var_names));

        // skip_precheck=true: we built `wide_to_narrow` above from the
        // same `fmpq_mpoly_used_vars` scan that the precheck would
        // re-do, so the precheck is redundant for this caller. See
        // Poly::transplant's signature for the safety contract.
        Poly num_n = num.transplant(narrow, wide_to_narrow,
                                     /*skip_precheck=*/true);
        Poly den_n = den.transplant(narrow, wide_to_narrow,
                                     /*skip_precheck=*/true);
        {
            auto& _sv = rn_setup_storage();
            if (static_cast<size_t>(_ra_tid) < _sv.size())
                _sv[_ra_tid] += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - _setup_t0).count();
        }

        // PINNED 2026-05-18 (v2 iter-23) — narrow-path gcd_cofactors hot call;
        //   fixture/gate : Smirnov tst3 6-rep paired baseline (v2 iter-4),
        //                  cross-checked by iter-7 scope-counter audit
        //   measurement  : narrow-path gcd_cofactors wall share ≤ 0.47% of
        //                  total wall on tst3 (constant-den early-out at
        //                  L1404-1407 absorbs ≈97.9% of wide-path calls;
        //                  see HyperFLINT development notes: track3 rescoped
        //                  falsifier + rat-invariant audit (iter-7 RETIRE verdict)
        //   falsifier    : gcd_cofactors_s/wall > 5% on any fixture with
        //                  HF_PHASE_TIMER coverage over both narrow AND
        //                  wide call surfaces → re-open the lazy-GCD lever
        //                  (Track 3). Inflated narrow+wide aggregate
        //                  counters (the 6.77% v1 figure) do NOT trigger;
        //                  use scope-disambiguated measurement only.
        //
        // fmpq_mpoly_gcd_cofactors returns (g, num/g, den/g) in one
        // pass — cheaper than gcd + 2×divexact because FLINT's
        // exponent-packing / bit-width setup happens once.
        Poly g_n(narrow);
        Poly rn_n(narrow);
        Poly rd_n(narrow);
        // Track 4.2 iter-26 PoC: scope-gated narrow-path gcd_cofactors
        // timer.  Predicate gates on
        //   (omp_parallel_for_integration_step && rat_reduce_inplace_narrow_branch).
        // RAII tick's scope is the enclosing if-block; the timer
        // accumulates wall from here through the post-gcd transplant
        // back at the bottom of the narrow branch.  Slight over-scope
        // vs the existing `gcd_cofactors_storage()` accumulator (which
        // wraps only the FLINT call) — acceptable for the PoC since
        // the FLINT call dominates wall; precise scoping is iter-27+
        // bulk-replacement work.
        HF_PHASE_TIMER_TICK(gcd_cofactors_narrow);
        const auto _gc_t0 = std::chrono::steady_clock::now();
        const int _gc_rc = fmpq_mpoly_gcd_cofactors(
            g_n.raw(), rn_n.raw(), rd_n.raw(),
            num_n.raw(), den_n.raw(), narrow.raw());
        {
            const double _gc_dt = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - _gc_t0).count();
            auto& _gv = gcd_cofactors_storage();
            if (static_cast<size_t>(_ra_tid) < _gv.size()) _gv[_ra_tid] += _gc_dt;
            auto& _cv = gcd_cofactors_calls_storage();
            if (static_cast<size_t>(_ra_tid) < _cv.size()) _cv[_ra_tid] += 1;
        }
        if (_gc_rc == 0) {
            throw std::runtime_error(
                "reduce_inplace: fmpq_mpoly_gcd_cofactors failed (narrow)");
        }

        const auto _post_t0 = std::chrono::steady_clock::now();
        // If the remaining narrow den is a pure fmpq constant, absorb
        // into num before transplanting back. Keeps the wide-ctx
        // fast path below symmetric.
        if (rd_n.is_fmpq()) {
            rn_n.scalar_div_fmpq_const(rd_n);
            rd_n = Poly::one_of(narrow);
        }
        if (rd_n.leading_coef_is_negative()) {
            rn_n = -rn_n;
            rd_n = -rd_n;
        }
        // skip_precheck=true: narrow_to_wide was built from
        // `used_wide` above, which is the union of num's and den's
        // wide-ctx supports. The post-GCD narrow polys (rn_n, rd_n)
        // can only use vars from that support, so every used var has
        // a mapping. The precheck would be redundant.
        num = rn_n.transplant(ctx, narrow_to_wide,
                               /*skip_precheck=*/true);
        den = rd_n.transplant(ctx, narrow_to_wide,
                               /*skip_precheck=*/true);
        const auto _ra_t1 = std::chrono::steady_clock::now();
        {
            auto& _pv = rn_post_storage();
            if (static_cast<size_t>(_ra_tid) < _pv.size())
                _pv[_ra_tid] += std::chrono::duration<double>(
                    _ra_t1 - _post_t0).count();
        }
        {
            auto& _nv = reduce_narrow_storage();
            if (static_cast<size_t>(_ra_tid) < _nv.size())
                _nv[_ra_tid] += std::chrono::duration<double>(
                    _ra_t1 - _ra_t0).count();
            auto& _cv = reduce_narrow_calls_storage();
            if (static_cast<size_t>(_ra_tid) < _cv.size())
                _cv[_ra_tid] += 1;
        }
        // Phase 3 §C.2 Lever V miss-path insert. (num, den) are now
        // the post-transplant wide-ctx outputs (num_out, den_out)
        // from the narrow-ctx reduce path above. Insert under
        // exclusive lock; try_emplace tolerates the (rare) race where
        // another thread inserted the same key concurrently between
        // our miss-path lookup and this insert. Default-OFF when
        // _lever_v_active_in_branch is false (no allocation, no
        // lock acquisition).
        if (_lever_v_active_in_branch) {
            lever_v_cache_insert(lever_v_key, num, den);
        }
        return;
    }

    // Track 4.2 iter-26 PoC scope guard for the wide-branch path.
    // Reached when worth_narrowing is false (size_gate fail OR
    // used_count*4 >= nvars_wide).  RAII lifetime is the enclosing
    // function block; the wide-branch bit stays set for the rest of
    // reduce_inplace_impl.  Sets the parent-mask bit required by the
    // `gcd_cofactors_wide` timer below.
    HF_SCOPE_ENTER(rat_reduce_inplace_wide_branch);
    // Wide-ctx fast path — one fmpq_mpoly_gcd_cofactors call instead
    // of gcd + 2×divexact, same reason as the narrow-ctx block above.
    const auto _ra_w0 = std::chrono::steady_clock::now();
    Poly g(ctx);
    Poly rn(ctx);
    Poly rd(ctx);
    // Track 4.2 iter-26 PoC: scope-gated wide-path gcd_cofactors timer.
    // Predicate gates on
    //   (omp_parallel_for_integration_step && rat_reduce_inplace_wide_branch).
    // RAII tick's scope is the enclosing function block; the timer
    // accumulates wall from here through end-of-function (covers the
    // FLINT call + post-processing + sign-canonicalization + the
    // reduce_wide_storage accounting at the bottom).  Same additive
    // contract as the narrow tick above.
    HF_PHASE_TIMER_TICK(gcd_cofactors_wide);
    const auto _gc_t0 = std::chrono::steady_clock::now();
    const int _gc_rc = fmpq_mpoly_gcd_cofactors(
        g.raw(), rn.raw(), rd.raw(),
        num.raw(), den.raw(), ctx.raw());
    {
        const double _gc_dt = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - _gc_t0).count();
        auto& _gv = gcd_cofactors_storage();
        if (static_cast<size_t>(_ra_tid) < _gv.size()) _gv[_ra_tid] += _gc_dt;
        auto& _cv = gcd_cofactors_calls_storage();
        if (static_cast<size_t>(_ra_tid) < _cv.size()) _cv[_ra_tid] += 1;
    }
    if (_gc_rc == 0) {
        throw std::runtime_error(
            "reduce_inplace: fmpq_mpoly_gcd_cofactors failed");
    }
    num = std::move(rn);
    den = std::move(rd);
    // fmpq_mpoly_gcd over ℚ returns 1 when both arguments are units,
    // so an integer ratio like 48/64 survives the gcd step unreduced.
    // When the *remaining* denominator is a pure fmpq constant, absorb
    // it into num via scalar multiplication by 1/c, leaving den = 1.
    if (den.is_fmpq()) {
        num.scalar_div_fmpq_const(den);
        den = Poly::one_of(num.ctx());
    }
    // Sign-canonicalization: if the leading coefficient of `den` is
    // negative, flip signs on both.
    if (den.leading_coef_is_negative()) {
        num = -num;
        den = -den;
    }
    const auto _ra_w1 = std::chrono::steady_clock::now();
    {
        auto& _wv = reduce_wide_storage();
        if (static_cast<size_t>(_ra_tid) < _wv.size())
            _wv[_ra_tid] += std::chrono::duration<double>(
                _ra_w1 - _ra_w0).count();
        auto& _cv = reduce_wide_calls_storage();
        if (static_cast<size_t>(_ra_tid) < _cv.size())
            _cv[_ra_tid] += 1;
    }
}

// HF FF Phase 5 §E Step E.2-impl-2 (iter-62-β.1).
//
// Public `reduce_inplace` entry-point with operator-memoization wrap.
// On cache HIT, deep-copies the cached (num_post, den_post) into the
// caller's by-reference slots and replays the per-thread call-count
// counter via `counter_replay::reduce_on_hit(cached_kind)`. On cache
// MISS, delegates to `reduce_inplace_impl` (the pre-iter-62 body) and
// inserts the post-state with the wrap-derived classifier kind.
//
// Classifier `kind` is derived from PRE-impl input characterisation per
// §iter-59-fold-REQ-5 §4.4-bis row 2:
//   0 = num.is_zero()             → reduce_zero_calls path (rat.cpp:1077-1082)
//   2 = (else) den.is_fmpq()      → reduce_wide early-out  (rat.cpp:1097-1110)
//   1 = otherwise                 → narrow path (general case)
// The kind=1 catch-all may internally fall through to wide accounting on
// the narrow-ineligible branch; the wrap captures only INPUT shape so
// HIT-replay is conservative (logs the most likely path; iter-63 §7
// falsification gate surfaces any HIT-rate discrepancy).
//
// Per implementation_memo §3.2 + §iter-59-fold-REQ-7 option b: the cache
// REMAINS ENABLED under HF_USE_SCALAR_REP=1 — Rat-cached values do not
// embed ZWTable references, so the FOLD-ER3 hazard does not apply.
//
// Master-switch fast path: when HF_OPERATOR_MEMO=0 (default), the wrap
// is a single load-from-cached-bool + branch + tail-call; canonical
// output is byte-identical to pre-iter-62 (byte-id smoke gate at
// iter-62-ε / iter-63 §7).
void reduce_inplace(Poly& num, Poly& den) {
    // HF FF Phase 6 REVISED §6.P iter-17 (BINDING pre-build reviewer at
    // iter-16): env-gated structural-sharing probe.  Default-OFF guarded
    // by the master predicate; production binary byte-output under
    // default-OFF is sha-identical to the iter-15 close binary
    // 580556dfa3... (FOLD-D-DISCIPLINE-N invariance).  Probe wraps the
    // PUBLIC entry-point so both cache HIT and MISS paths are observed
    // (design memo §1 + §2 NOTE: HIT-path emits with pre = caller's
    // incoming (num,den) and post = the cached (num',den') deep-copied
    // back into the caller's slots).
    //
    // FOLD-DC5 stratified sampling: should_emit_op() is the per-call
    // gate; sample_rate() reads HF_STRUCTURAL_SHARING_PROBE_SAMPLE_RATE
    // (per-thread counter, no shared lock).  When the gate fires, we
    // snapshot pre via deep-copy (necessary because reduce_inplace
    // mutates `num`/`den` in place); the deep-copy cost is paid only
    // on the sampled fraction of calls.
    //
    // unchanged_bytes: iter-17 ships the -1 "not measured" sentinel for
    // Rat::reduce_inplace because monomial-level intersection via
    // canonical to_string-split (design memo §2.1) would require a third
    // Poly deep-copy of the post-state plus an O(n log n) sort + merge
    // per emission, blowing the +10% wall regression bound (design memo
    // §2.3) at any reasonable stratification rate.  iter-18+ refines per
    // the BINDING pre-build reviewer's REQ-fold.  pre_bytes / post_bytes
    // alone are sufficient to compute the byte-weighted ACTIVATION gate's
    // DENOMINATOR (Σ frequency × total_bytes); the NUMERATOR
    // (shareable_bytes) requires unchanged_bytes and is deferred.
    const bool probe = structural_sharing::probe_reduce_inplace_instrumented()
                       && structural_sharing::should_emit_op("reduce_inplace");
    std::optional<Poly> num_pre_snapshot;
    std::optional<Poly> den_pre_snapshot;
    int64_t pre_bytes = 0;
    if (probe) {
        // Deep-copy snapshots so post-mutation byte-size remains computable.
        // Bounded by stratified-sampling fraction (FOLD-DC5).
        num_pre_snapshot.emplace(num);
        den_pre_snapshot.emplace(den);
        pre_bytes =
            static_cast<int64_t>(num_pre_snapshot->total_bytes()) +
            static_cast<int64_t>(den_pre_snapshot->total_bytes());
    }

    if (!operator_memo::reduce_enabled()) {
        reduce_inplace_impl(num, den);
        if (probe) {
            const int64_t post_bytes =
                static_cast<int64_t>(num.total_bytes()) +
                static_cast<int64_t>(den.total_bytes());
            structural_sharing::emit("reduce_inplace", pre_bytes, post_bytes,
                                     /*unchanged_bytes=*/-1);
        }
        return;
    }
    // Classify INPUT before computing key (the cache is keyed on input
    // payload sigs, so classification and key computation share the
    // characterisation work).
    int kind;
    if (num.is_zero())        kind = 0;
    else if (den.is_fmpq())   kind = 2;
    else                      kind = 1;

    canonical_signature::ReduceKey key =
        canonical_signature::make_reduce_key(num, den);
    const std::uint64_t key_hash =
        canonical_signature::hash_reduce_key(key);

    int cached_kind = 0;
    if (operator_memo::reduce_try_lookup_and_apply(
            key, key_hash, num, den, cached_kind)) {
        counter_replay::reduce_on_hit(cached_kind);
        if (probe) {
            const int64_t post_bytes =
                static_cast<int64_t>(num.total_bytes()) +
                static_cast<int64_t>(den.total_bytes());
            structural_sharing::emit("reduce_inplace", pre_bytes, post_bytes,
                                     /*unchanged_bytes=*/-1);
        }
        return;
    }
    // MISS: run the impl in place, then insert the (post-state, kind).
    reduce_inplace_impl(num, den);
    operator_memo::reduce_insert_with_kind(
        std::move(key), key_hash, num, den, kind);
    if (probe) {
        const int64_t post_bytes =
            static_cast<int64_t>(num.total_bytes()) +
            static_cast<int64_t>(den.total_bytes());
        structural_sharing::emit("reduce_inplace", pre_bytes, post_bytes,
                                 /*unchanged_bytes=*/-1);
    }
}

// =====================================================================
// Branch I rep-swap (2026-05-01): production-grade `_fmpz_mpoly_q_add`
// wrapper for `Rat::add` / `Rat::operator+=`.
//
// Pre-flight 8 (notes/3l3pt_branch_I_fmpz_q_poc/quads_underscore_findings.md)
// established a +72.4 % aggregate speed-up vs the cross-mult+gcd_cofactors
// path on 200 production-canonical operand quads (200/200 correctness),
// using the underscore primitive `_fmpz_mpoly_q_add` (FLINT 3.4.0
// fmpz_mpoly_q.h:138-142). The high-level `fmpz_mpoly_q_add` is
// inadmissible because its eager-canonicalise paths through wide-ctx
// only, which would silently destroy the d=3 size-gate lever in
// `reduce_inplace`. The underscore primitive accepts six fmpz_mpoly_t
// num/den arguments + ctx, letting HF wrap it with a narrow-ctx hoist
// that mirrors the predicate in `reduce_inplace`.
//
// Lift convention (matches bench_fmpz_mpoly_q_quads_v5.cpp's
// `fmpz_mpoly_q_set_from_fmpq_pair`): for a pair (num_q, den_q) where
//   num_q = (Cn) * Nz,    Cn = nc/nd  (rational content)
//   den_q = (Cd) * Dz,    Cd = dc/dd
// the lifted fmpz_mpoly pair (Nz', Dz') is
//   Nz' = (nc * dd) * Nz
//   Dz' = (nd * dc) * Dz
// so that Nz'/Dz' equals num_q/den_q exactly (no leftover Q-scalar). The
// trailing global rational then evaporates and the underscore primitive
// can be called with the lifted pair directly.
//
// Lower convention: build a fresh fmpq_mpoly with content = 1 and zpoly
// = the result fmpz_mpoly. This trades some content-distribution cost
// against the eager canonicalise that fmpq_mpoly performs on its own.
// The Rat ctor's reduce_inplace then runs on the lowered pair; on
// already-canonical operands it short-circuits cheaply (gcd_cofactors
// returns g = 1 and leaves operands intact, hitting the wide-ctx
// fast-path's tiny <1 us cost when present, or Avenue I's constant-den
// short-circuit if the lower yields den_q = const).
//
// Narrow-ctx hoist: replicates the `worth_narrowing` predicate from
// `reduce_inplace` (size_gate ∧ used_count*4 < nvars_wide). When
// hoisting, transplant both Rat operands into a narrow PolyCtx,
// then take the embedded fmpz_mpoly_ctx (`narrow.raw()->zctx`) as the
// underscore primitive's context. After the call, transplant the
// fmpz_mpoly result back to wide ctx.
// =====================================================================

// Lift a (fmpq_mpoly num, fmpq_mpoly den) pair to (fmpz_mpoly num,
// fmpz_mpoly den). On entry, `out_num_z` and `out_den_z` must be
// already-`fmpz_mpoly_init`'d in `ctx_z`; on exit they hold the lifted
// polynomials. `ctx_z` MUST be the embedded zctx of a fmpq_mpoly_ctx
// (i.e., `ctx_q->raw()->zctx`) so the variable layout matches.
//
// Convention: out_num_z = (num.content_num * den.content_den) * num.zpoly,
//             out_den_z = (num.content_den * den.content_num) * den.zpoly.
// Then out_num_z / out_den_z exactly equals (num/den) in value.
inline void lift_pair_to_fmpz(const Poly& num_q, const Poly& den_q,
                              const fmpz_mpoly_ctx_t ctx_z,
                              fmpz_mpoly_t out_num_z,
                              fmpz_mpoly_t out_den_z) {
    fmpq_mpoly_struct* num_raw =
        const_cast<fmpq_mpoly_struct*>(num_q.raw());
    fmpq_mpoly_struct* den_raw =
        const_cast<fmpq_mpoly_struct*>(den_q.raw());
    fmpz_t s_num, s_den;
    fmpz_init(s_num);
    fmpz_init(s_den);
    // s_num = (num.content.num) * (den.content.den)
    fmpz_mul(s_num, fmpq_numref(num_raw->content),
                    fmpq_denref(den_raw->content));
    // s_den = (num.content.den) * (den.content.num)
    fmpz_mul(s_den, fmpq_denref(num_raw->content),
                    fmpq_numref(den_raw->content));
    fmpz_mpoly_set(out_num_z, num_raw->zpoly, ctx_z);
    fmpz_mpoly_scalar_mul_fmpz(out_num_z, out_num_z, s_num, ctx_z);
    fmpz_mpoly_set(out_den_z, den_raw->zpoly, ctx_z);
    fmpz_mpoly_scalar_mul_fmpz(out_den_z, out_den_z, s_den, ctx_z);
    fmpz_clear(s_num);
    fmpz_clear(s_den);
}

// Inverse of lift_pair_to_fmpz: build a (fmpq_mpoly num, fmpq_mpoly
// den) pair from a (fmpz_mpoly num, fmpz_mpoly den) pair. The fmpq_mpoly
// representation here is "content = 1, zpoly = res_z" — the eager
// canonicalisation that fmpq_mpoly_set normally enforces on construction
// is bypassed by direct field write. After lower, the caller is
// expected to pass through `reduce_inplace` (or the Rat(Poly,Poly)
// ctor) which restores HF's canonical-form invariant (gcd(num,den)=1,
// den's leading coeff positive, integer coeffs of num/den each have gcd
// 1). The `_fmpz_mpoly_q_add` underscore primitive normally returns
// canonical form, so reduce_inplace's gcd_cofactors call collapses to
// O(nterms) gcd-with-itself work; cheap relative to the saved
// cross-mult.
inline void lower_pair_to_fmpq(const fmpz_mpoly_t in_num_z,
                                const fmpz_mpoly_t in_den_z,
                                const PolyCtx& ctx_q,
                                Poly& out_num_q,
                                Poly& out_den_q) {
    // out_num_q.zpoly := in_num_z, out_num_q.content := 1.
    fmpq_mpoly_struct* nout = out_num_q.raw();
    fmpq_one(nout->content);
    fmpz_mpoly_set(nout->zpoly,
                   const_cast<fmpz_mpoly_struct*>(in_num_z),
                   ctx_q.raw()->zctx);
    fmpq_mpoly_struct* dout = out_den_q.raw();
    fmpq_one(dout->content);
    fmpz_mpoly_set(dout->zpoly,
                   const_cast<fmpz_mpoly_struct*>(in_den_z),
                   ctx_q.raw()->zctx);
}

// Replicates the `worth_narrowing` predicate from `reduce_inplace`
// (line 535-619 above) but applied to the four-operand union of
// (a.num, a.den, b.num, b.den). Returns the narrow PolyCtx + var
// mapping when narrowing is profitable; when not, returns `nullopt`.
//
// The size-gate uses (a_num.length + a_den.length + b_num.length +
// b_den.length) ≥ size_gate_min — same constant as reduce_inplace.
// HF_REDUCE_SIZE_GATE_MIN / HF_REDUCE_SIZE_GATE_DIVISOR / HF_NO_NARROW_REDUCE
// are honoured (re-using the static caches initialised above; they're
// process-wide).
struct AddNarrowDecision {
    bool narrow;
    std::vector<size_t> wide_to_narrow;  // size = nvars_wide
    std::vector<size_t> narrow_to_wide;  // size = used_count
    std::vector<std::string> narrow_var_names;
};

inline bool add_size_gate_passes(slong len_total,
                                 size_t nvars_wide,
                                 slong size_gate_min_static,
                                 slong size_gate_divisor) {
    slong gate_min = size_gate_min_static;
    if (size_gate_divisor > 0) {
        const slong adaptive =
            static_cast<slong>(nvars_wide) / size_gate_divisor;
        gate_min = std::max(slong{4}, adaptive);
    }
    return len_total >= gate_min;
}

inline AddNarrowDecision decide_narrow_ctx_for_add(
    const Poly& a_num, const Poly& a_den,
    const Poly& b_num, const Poly& b_den) {
    AddNarrowDecision out;
    out.narrow = false;
    const PolyCtx& ctx = a_num.ctx();
    const size_t nvars_wide = ctx.vars().size();
    // Same env-cached constants as reduce_inplace (lines 547-565). Static
    // caches are process-wide so re-evaluating them here costs one
    // already-resolved branch per call; they are NOT re-read from env.
    static const slong size_gate_min_static = []{
        const char* e = HF_FLAG_REDUCE_SIZE_GATE_MIN;
        if (!e || !*e) return slong{4};
        long v = std::strtol(e, nullptr, 10);
        return v > 0 ? static_cast<slong>(v) : slong{4};
    }();
    static const slong size_gate_divisor = []{
        const char* e = HF_FLAG_REDUCE_SIZE_GATE_DIVISOR;
        if (!e || !*e) return slong{0};
        long v = std::strtol(e, nullptr, 10);
        return v > 0 ? static_cast<slong>(v) : slong{0};
    }();
    static const bool no_narrow_reduce = []{
        const char* e = HF_FLAG_NO_NARROW_REDUCE;
        return e && e[0] == '1';
    }();
    if (no_narrow_reduce) return out;
    const slong len_total =
        fmpq_mpoly_length(a_num.raw(), ctx.raw()) +
        fmpq_mpoly_length(a_den.raw(), ctx.raw()) +
        fmpq_mpoly_length(b_num.raw(), ctx.raw()) +
        fmpq_mpoly_length(b_den.raw(), ctx.raw());
    if (!add_size_gate_passes(len_total, nvars_wide,
                              size_gate_min_static, size_gate_divisor)) {
        return out;
    }
    // Used-vars union over all four operands.
    std::vector<int> used(nvars_wide, 0);
    std::vector<int> tmp(nvars_wide, 0);
    auto or_in = [&](const Poly& p) {
        std::fill(tmp.begin(), tmp.end(), 0);
        fmpq_mpoly_used_vars(tmp.data(),
            const_cast<fmpq_mpoly_struct*>(p.raw()), ctx.raw());
        for (size_t j = 0; j < nvars_wide; ++j)
            if (tmp[j]) used[j] = 1;
    };
    or_in(a_num); or_in(a_den); or_in(b_num); or_in(b_den);
    size_t used_count = 0;
    for (size_t j = 0; j < nvars_wide; ++j) if (used[j]) ++used_count;
    if (used_count * 4 >= nvars_wide) return out;  // not worth it
    out.narrow = true;
    out.wide_to_narrow.assign(nvars_wide, SIZE_MAX);
    out.narrow_to_wide.reserve(used_count);
    out.narrow_var_names.reserve(used_count);
    for (size_t j = 0; j < nvars_wide; ++j) {
        if (!used[j]) continue;
        out.wide_to_narrow[j] = out.narrow_to_wide.size();
        out.narrow_to_wide.push_back(j);
        out.narrow_var_names.push_back(ctx.vars()[j]);
    }
    return out;
}

// Core new-add path. Caller passes the four operand Polys (a.num, a.den,
// b.num, b.den) and receives a freshly-constructed Rat in canonical
// form. Edge cases (zero numerator on either operand) are handled at
// the top — _fmpz_mpoly_q_add accepts them but the short-circuit is
// strictly cheaper than running a full transduce + canonicalise round-trip.
// 2026-05-02 (extension to mul/sub/div): generic dispatcher over the four
// _fmpz_mpoly_q_* underscore primitives. All four share an identical
// signature — see fmpz_mpoly_q.h:139-167. The narrow-decision /
// lift / lower / transplant boilerplate is identical across the ops;
// only the underscore primitive itself varies. Routes the result through
// Rat::from_canonical (the underscore primitives all produce canonical
// fmpz_mpoly_q output, so reduce_inplace is redundant).
using QUnderscoreFn = void(*)(fmpz_mpoly_t, fmpz_mpoly_t,
                              const fmpz_mpoly_t, const fmpz_mpoly_t,
                              const fmpz_mpoly_t, const fmpz_mpoly_t,
                              const fmpz_mpoly_ctx_t);

inline Rat op_via_q_underscore_impl(const Poly& a_num, const Poly& a_den,
                                     const Poly& b_num, const Poly& b_den,
                                     QUnderscoreFn op) {
    const PolyCtx& ctx_q = a_num.ctx();
    const AddNarrowDecision dec =
        decide_narrow_ctx_for_add(a_num, a_den, b_num, b_den);
    if (dec.narrow) {
        // Narrow-ctx hoist path. Transplant fmpq_mpoly operands into a
        // narrow PolyCtx; take the embedded fmpz_mpoly_ctx for the
        // underscore primitive; transplant the fmpz_mpoly result back
        // through a wrapping fmpq_mpoly Poly.
        const PolyCtx narrow(std::vector<std::string>(dec.narrow_var_names));
        Poly a_num_n = a_num.transplant(narrow, dec.wide_to_narrow,
                                          /*skip_precheck=*/true);
        Poly a_den_n = a_den.transplant(narrow, dec.wide_to_narrow,
                                          /*skip_precheck=*/true);
        Poly b_num_n = b_num.transplant(narrow, dec.wide_to_narrow,
                                          /*skip_precheck=*/true);
        Poly b_den_n = b_den.transplant(narrow, dec.wide_to_narrow,
                                          /*skip_precheck=*/true);
        const fmpz_mpoly_ctx_struct* ctx_z_n = narrow.raw()->zctx;

        fmpz_mpoly_t a_num_z, a_den_z, b_num_z, b_den_z;
        fmpz_mpoly_init(a_num_z, ctx_z_n);
        fmpz_mpoly_init(a_den_z, ctx_z_n);
        fmpz_mpoly_init(b_num_z, ctx_z_n);
        fmpz_mpoly_init(b_den_z, ctx_z_n);
        lift_pair_to_fmpz(a_num_n, a_den_n, ctx_z_n, a_num_z, a_den_z);
        lift_pair_to_fmpz(b_num_n, b_den_n, ctx_z_n, b_num_z, b_den_z);

        fmpz_mpoly_t res_num_z, res_den_z;
        fmpz_mpoly_init(res_num_z, ctx_z_n);
        fmpz_mpoly_init(res_den_z, ctx_z_n);
        op(res_num_z, res_den_z,
           a_num_z, a_den_z, b_num_z, b_den_z, ctx_z_n);

        // Lower back to fmpq_mpoly representation in narrow ctx, then
        // transplant to wide ctx. Constructing through Rat(...) lets
        // reduce_inplace finalise canonical form.
        Poly out_num_n(narrow);
        Poly out_den_n(narrow);
        lower_pair_to_fmpq(res_num_z, res_den_z, narrow,
                           out_num_n, out_den_n);

        fmpz_mpoly_clear(res_num_z, ctx_z_n);
        fmpz_mpoly_clear(res_den_z, ctx_z_n);
        fmpz_mpoly_clear(a_num_z,   ctx_z_n);
        fmpz_mpoly_clear(a_den_z,   ctx_z_n);
        fmpz_mpoly_clear(b_num_z,   ctx_z_n);
        fmpz_mpoly_clear(b_den_z,   ctx_z_n);

        // Lever: skip the redundant reduce_inplace inside Rat(...) ctor.
        // _fmpz_mpoly_q_add returned canonical form (gcd(num,den)=1);
        // Rat::from_canonical does only the cheap sign-canon + constant-
        // absorption. Saves one full narrow round-trip per Rat::add call.
        Poly out_num_w = out_num_n.transplant(ctx_q, dec.narrow_to_wide,
                                                /*skip_precheck=*/true);
        Poly out_den_w = out_den_n.transplant(ctx_q, dec.narrow_to_wide,
                                                /*skip_precheck=*/true);
        return Rat::from_canonical(std::move(out_num_w),
                                    std::move(out_den_w));
    }

    // Wide-ctx fast path: no narrowing. Use the embedded zctx directly.
    const fmpz_mpoly_ctx_struct* ctx_z = ctx_q.raw()->zctx;
    fmpz_mpoly_t a_num_z, a_den_z, b_num_z, b_den_z;
    fmpz_mpoly_init(a_num_z, ctx_z);
    fmpz_mpoly_init(a_den_z, ctx_z);
    fmpz_mpoly_init(b_num_z, ctx_z);
    fmpz_mpoly_init(b_den_z, ctx_z);
    lift_pair_to_fmpz(a_num, a_den, ctx_z, a_num_z, a_den_z);
    lift_pair_to_fmpz(b_num, b_den, ctx_z, b_num_z, b_den_z);

    fmpz_mpoly_t res_num_z, res_den_z;
    fmpz_mpoly_init(res_num_z, ctx_z);
    fmpz_mpoly_init(res_den_z, ctx_z);
    op(res_num_z, res_den_z,
       a_num_z, a_den_z, b_num_z, b_den_z, ctx_z);

    Poly out_num(ctx_q);
    Poly out_den(ctx_q);
    lower_pair_to_fmpq(res_num_z, res_den_z, ctx_q, out_num, out_den);

    fmpz_mpoly_clear(res_num_z, ctx_z);
    fmpz_mpoly_clear(res_den_z, ctx_z);
    fmpz_mpoly_clear(a_num_z,   ctx_z);
    fmpz_mpoly_clear(a_den_z,   ctx_z);
    fmpz_mpoly_clear(b_num_z,   ctx_z);
    fmpz_mpoly_clear(b_den_z,   ctx_z);

    // Wide-path also produces canonical output — bypass redundant
    // reduce_inplace via from_canonical (same lever as the narrow
    // path's return statement above).
    return Rat::from_canonical(std::move(out_num), std::move(out_den));
}

// 2026-05-02 (extension to mul/sub/div): four thin wrappers, each
// dispatching to the generic op_via_q_underscore_impl with its own
// underscore primitive + edge-case handling.

Rat add_via_q_underscore(const Poly& a_num, const Poly& a_den,
                         const Poly& b_num, const Poly& b_den) {
    // Edge-case 1: zero numerator on a → result is just b.
    if (a_num.is_zero()) {
        return Rat(Poly(b_num), Poly(b_den));
    }
    // Edge-case 2: zero numerator on b → result is just a.
    if (b_num.is_zero()) {
        return Rat(Poly(a_num), Poly(a_den));
    }
    return op_via_q_underscore_impl(a_num, a_den, b_num, b_den,
                                     _fmpz_mpoly_q_add);
}

Rat sub_via_q_underscore(const Poly& a_num, const Poly& a_den,
                         const Poly& b_num, const Poly& b_den) {
    // a - 0 = a.
    if (b_num.is_zero()) {
        return Rat(Poly(a_num), Poly(a_den));
    }
    // 0 - b = -b. Negate via Poly's operator-, keep den as-is, then
    // canonicalise via Rat::from_canonical (gcd unchanged by neg).
    if (a_num.is_zero()) {
        return Rat::from_canonical(-Poly(b_num), Poly(b_den));
    }
    return op_via_q_underscore_impl(a_num, a_den, b_num, b_den,
                                     _fmpz_mpoly_q_sub);
}

Rat mul_via_q_underscore(const Poly& a_num, const Poly& a_den,
                         const Poly& b_num, const Poly& b_den) {
    // 0 * anything = 0 / 1.
    if (a_num.is_zero() || b_num.is_zero()) {
        return Rat::zero_of(a_num.ctx());
    }
    return op_via_q_underscore_impl(a_num, a_den, b_num, b_den,
                                     _fmpz_mpoly_q_mul);
}

Rat div_via_q_underscore(const Poly& a_num, const Poly& a_den,
                         const Poly& b_num, const Poly& b_den) {
    if (b_num.is_zero()) {
        throw std::runtime_error("Rat::div: division by zero");
    }
    if (a_num.is_zero()) {
        return Rat::zero_of(a_num.ctx());
    }
    return op_via_q_underscore_impl(a_num, a_den, b_num, b_den,
                                     _fmpz_mpoly_q_div);
}

// Env-gate for the legacy (cross-mult+gcd_cofactors) `Rat::add` path.
// Set `HF_USE_LEGACY_RAT_ADD=1` to revert to the pre-rep-swap path.
// Default OFF (new path wins). Resolved once at first call.
inline bool use_legacy_rat_add() {
    static const bool e = []{
        const char* s = HF_FLAG_USE_LEGACY_RAT_ADD;
        return s && s[0] && s[0] != '0';
    }();
    return e;
}

// 2026-05-02 (mul/sub/div q_underscore extension): the rep-swap +
// from_canonical pattern that wins -10 % on tst2 for Rat::add was
// measured to REGRESS by +14 % on tst2 when applied uniformly to
// Rat::mul/sub/div. Hypothesis: for cross-multiply-shaped ops like
// add (a*d + c*b), FLINT's q_add detects substantial cancellation
// in the cross-products; for mul/sub/div the savings don't amortise
// the lift/lower/transplant overhead. Default to legacy for these
// three; opt-in via env var if a different workload makes them win.
//
//   HF_USE_QUNDERSCORE_RAT_MUL=1  : route mul through mul_via_q_underscore
//   HF_USE_QUNDERSCORE_RAT_SUB=1  : route sub through sub_via_q_underscore
//   HF_USE_QUNDERSCORE_RAT_DIV=1  : route div through div_via_q_underscore
inline bool use_qunderscore_rat_mul() {
    static const bool e = []{
        const char* s = HF_FLAG_USE_QUNDERSCORE_RAT_MUL;
        return s && s[0] && s[0] != '0';
    }();
    return e;
}
inline bool use_qunderscore_rat_sub() {
    static const bool e = []{
        const char* s = HF_FLAG_USE_QUNDERSCORE_RAT_SUB;
        return s && s[0] && s[0] != '0';
    }();
    return e;
}
inline bool use_qunderscore_rat_div() {
    static const bool e = []{
        const char* s = HF_FLAG_USE_QUNDERSCORE_RAT_DIV;
        return s && s[0] && s[0] != '0';
    }();
    return e;
}

// Wide-ctx threshold gate (Tier A1, 2026-05-02). The rep-swap path
// pays a fixed lift/lower transduction cost per call (fmpq_mpoly →
// fmpz_mpoly_q → back) plus, in the narrow-ctx case, two transplants.
// On heavy-ctx workloads (parity-1 718 vars: -64%; Smirnov tst2: -31%)
// the saved fmpq_mpoly_gcd_cofactors cost dominates and the rep-swap
// wins. On light-ctx workloads (STBenchmark Long: avg ~3-5 wide-ctx
// vars, +3% net) the transduction overhead exceeds the saving.
// Route adds with nvars < threshold to `add_legacy`. Default 50 keeps
// parity-1 / tst2 on the fast path while letting Long-suite light
// cases stay on legacy. Tunable via `HF_REPSWAP_NVARS_MIN`; set to 0
// to force rep-swap unconditionally, or to a very large value to
// force legacy unconditionally. Resolved once at first call.
inline size_t repswap_nvars_min() {
    static const size_t v = []{
        const char* s = HF_FLAG_REPSWAP_NVARS_MIN;
        if (!s || !s[0]) return static_cast<size_t>(50);
        try { return static_cast<size_t>(std::stoul(s)); }
        catch (...) { return static_cast<size_t>(50); }
    }();
    return v;
}

}  // namespace

// Per-thread accumulators for the two paths inside `reduce_inplace`,
// plus Lever-1-extended per-Poly-op timers for `Rat::add` operator+.
// See rat.cpp's anonymous namespace for design rationale (3l3pt
// profile-deepening, 2026-04-27 reviewer correction).
void init_reduce_per_thread(int n_threads) {
    const size_t n = static_cast<size_t>(n_threads > 0 ? n_threads : 1);
    reduce_narrow_storage().assign(n, 0.0);
    reduce_wide_storage().assign(n, 0.0);
    reduce_narrow_calls_storage().assign(n, 0L);
    reduce_wide_calls_storage().assign(n, 0L);
    reduce_zero_calls_storage().assign(n, 0L);
    gcd_cofactors_storage().assign(n, 0.0);
    gcd_cofactors_calls_storage().assign(n, 0L);
    rn_used_vars_storage().assign(n, 0.0);
    rn_setup_storage().assign(n, 0.0);
    rn_post_storage().assign(n, 0.0);
    rat_add_polymul_storage().assign(n, 0.0);
    rat_add_polyadd_storage().assign(n, 0.0);
    rat_add_calls_storage().assign(n, 0L);
    rat_add_legacy_wall_storage().assign(n, 0.0);
    rat_add_via_qu_wall_storage().assign(n, 0.0);
    rat_add_legacy_calls_storage().assign(n, 0L);
    rat_add_via_qu_calls_storage().assign(n, 0L);
    reduce_nterm_calls_storage().assign(n, 0L);
    reduce_nterm_pre_total_storage().assign(n, 0L);
    reduce_nterm_post_total_storage().assign(n, 0L);
    reduce_nterm_pre_max_storage().assign(n, 0L);
    reduce_nterm_post_max_storage().assign(n, 0L);
    reduce_wide_smallfall_calls_storage().assign(n, 0L);
}
void reset_reduce_per_thread() {
    // FOLD-4 (Phase 2 §B.2 iter-09): bump the step-id counter on every
    // integration_step entry. The probe reads this atomic to tag each
    // call with the step in which it fired. No-op cost when the probe
    // is disabled (atomic increment is ~ns).
    g_transplant_probe_step_id.fetch_add(1, std::memory_order_relaxed);
    // Phase 3 §C.2 Lever V cache clear. Per §A.6 verdict §5, this
    // function is called at the TOP of integration_step BEFORE the
    // OMP parallel region; the clear is therefore single-threaded
    // (uncontested unique_lock) and runs BEFORE any next-step
    // wide_ctx pointer change could occur (FOLD-4 lifecycle chain).
    // Default-OFF: clear runs on an empty global map (~no-op cost).
    // Gating on hf_lever_v_active() preserves zero-overhead default-OFF
    // discipline — the global map is never allocated when disabled.
    if (hf_lever_v_active()) {
        lever_v_cache_clear();
    }
    for (auto& x : reduce_narrow_storage())       x = 0.0;
    for (auto& x : reduce_wide_storage())         x = 0.0;
    for (auto& x : reduce_narrow_calls_storage()) x = 0L;
    for (auto& x : reduce_wide_calls_storage())   x = 0L;
    for (auto& x : reduce_zero_calls_storage())   x = 0L;
    for (auto& x : gcd_cofactors_storage())       x = 0.0;
    for (auto& x : gcd_cofactors_calls_storage()) x = 0L;
    for (auto& x : rn_used_vars_storage())        x = 0.0;
    for (auto& x : rn_setup_storage())            x = 0.0;
    for (auto& x : rn_post_storage())             x = 0.0;
    for (auto& x : rat_add_polymul_storage())     x = 0.0;
    for (auto& x : rat_add_polyadd_storage())     x = 0.0;
    for (auto& x : rat_add_calls_storage())       x = 0L;
    for (auto& x : rat_add_legacy_wall_storage())   x = 0.0;
    for (auto& x : rat_add_via_qu_wall_storage())   x = 0.0;
    for (auto& x : rat_add_legacy_calls_storage())  x = 0L;
    for (auto& x : rat_add_via_qu_calls_storage())  x = 0L;
    for (auto& x : reduce_nterm_calls_storage())     x = 0L;
    for (auto& x : reduce_nterm_pre_total_storage())  x = 0L;
    for (auto& x : reduce_nterm_post_total_storage()) x = 0L;
    for (auto& x : reduce_nterm_pre_max_storage())    x = 0L;
    for (auto& x : reduce_nterm_post_max_storage())   x = 0L;
    for (auto& x : reduce_wide_smallfall_calls_storage()) x = 0L;
}
double sum_reduce_narrow_s_per_thread() {
    double s = 0; for (double x : reduce_narrow_storage()) s += x; return s;
}
double sum_reduce_wide_s_per_thread() {
    double s = 0; for (double x : reduce_wide_storage()) s += x; return s;
}
long sum_reduce_narrow_calls_per_thread() {
    long s = 0; for (long x : reduce_narrow_calls_storage()) s += x; return s;
}
long sum_reduce_wide_calls_per_thread() {
    long s = 0; for (long x : reduce_wide_calls_storage()) s += x; return s;
}
long sum_reduce_zero_calls_per_thread() {
    long s = 0; for (long x : reduce_zero_calls_storage()) s += x; return s;
}
double sum_gcd_cofactors_s_per_thread() {
    double s = 0; for (double x : gcd_cofactors_storage()) s += x; return s;
}
long sum_gcd_cofactors_calls_per_thread() {
    long s = 0; for (long x : gcd_cofactors_calls_storage()) s += x; return s;
}
double sum_rn_used_vars_s_per_thread() {
    double s = 0; for (double x : rn_used_vars_storage()) s += x; return s;
}
double sum_rn_setup_s_per_thread() {
    double s = 0; for (double x : rn_setup_storage()) s += x; return s;
}
double sum_rn_post_s_per_thread() {
    double s = 0; for (double x : rn_post_storage()) s += x; return s;
}
double sum_rat_add_polymul_s_per_thread() {
    double s = 0; for (double x : rat_add_polymul_storage()) s += x; return s;
}
double sum_rat_add_polyadd_s_per_thread() {
    double s = 0; for (double x : rat_add_polyadd_storage()) s += x; return s;
}
long sum_rat_add_calls_per_thread() {
    long s = 0; for (long x : rat_add_calls_storage()) s += x; return s;
}

// HF FF Phase 5 §E Step E.2-impl-2 (iter-61-β.6a).
// See rat.hpp declaration for the contract. Defined at file scope (not
// inside the anonymous namespace) so external TUs can link to it; the
// body still accesses the internal-linkage `rat_add_calls_storage()`
// because this TU sees its anon-namespace declaration above.
void rat_add_record_call_for_thread(int tid) {
    auto& cv = rat_add_calls_storage();
    if (static_cast<std::size_t>(tid) < cv.size()) {
        cv[static_cast<std::size_t>(tid)] += 1L;
    }
}

// HF FF Phase 5 §E Step E.2-impl-2 (iter-62-β.1).
// Per-classifier counter increment for reduce_inplace's outer-cache HIT
// replay. See rat.hpp declaration for the contract.
//
// kind ∈ {0 (zero), 1 (narrow), 2 (wide)}; out-of-range kind is a no-op.
// Each per-thread vector is bounds-checked against `tid` since
// init_reduce_per_thread may not have been called on every entry path.
void reduce_record_call_for_thread(int tid, int kind) {
    const std::size_t utid = static_cast<std::size_t>(tid);
    switch (kind) {
        case 0: {
            auto& zv = reduce_zero_calls_storage();
            if (utid < zv.size()) zv[utid] += 1L;
            break;
        }
        case 1: {
            auto& nv = reduce_narrow_calls_storage();
            if (utid < nv.size()) nv[utid] += 1L;
            break;
        }
        case 2: {
            auto& wv = reduce_wide_calls_storage();
            if (utid < wv.size()) wv[utid] += 1L;
            break;
        }
        default:
            // Defensive: unexpected kind classifier silently ignored.
            break;
    }
}
double sum_rat_add_legacy_wall_s_per_thread() {
    double s = 0; for (double x : rat_add_legacy_wall_storage()) s += x; return s;
}
double sum_rat_add_via_qu_wall_s_per_thread() {
    double s = 0; for (double x : rat_add_via_qu_wall_storage()) s += x; return s;
}
long sum_rat_add_legacy_calls_per_thread() {
    long s = 0; for (long x : rat_add_legacy_calls_storage()) s += x; return s;
}
long sum_rat_add_via_qu_calls_per_thread() {
    long s = 0; for (long x : rat_add_via_qu_calls_storage()) s += x; return s;
}
long sum_reduce_nterm_calls_per_thread() {
    long s = 0; for (long x : reduce_nterm_calls_storage()) s += x; return s;
}
long sum_reduce_nterm_pre_total_per_thread() {
    long s = 0; for (long x : reduce_nterm_pre_total_storage()) s += x; return s;
}
long sum_reduce_nterm_post_total_per_thread() {
    long s = 0; for (long x : reduce_nterm_post_total_storage()) s += x; return s;
}
long max_reduce_nterm_pre_per_thread() {
    long m = 0; for (long x : reduce_nterm_pre_max_storage()) if (x > m) m = x; return m;
}
long max_reduce_nterm_post_per_thread() {
    long m = 0; for (long x : reduce_nterm_post_max_storage()) if (x > m) m = x; return m;
}
long sum_reduce_wide_smallfall_calls_per_thread() {
    long s = 0; for (long x : reduce_wide_smallfall_calls_storage()) s += x; return s;
}

Rat::Rat(Poly num, Poly den)
    : num_(std::move(num)), den_(std::move(den)) {
    reduce_inplace(num_, den_);
    // HF FF Phase 5 §A.1 iter-50: value_create emit AFTER reduce_inplace so
    // the canonical-bits hash reflects the final (reduced) num/den, not the
    // pre-reduction inputs. The dispatch into reduce_inplace also gets an
    // op_call emit at the entry of reduce_inplace itself; the two emits are
    // independent (value-layer vs operator-layer) and the aggregator
    // (§3.3 vs §5.1) keeps them separate.
    hf_probe_emit_rat_create(reinterpret_cast<uintptr_t>(this),
                             num_.raw(), den_.raw(),
                             num_.ctx().raw());
}

// 2026-05-02 (rn_post_s lever): bypass reduce_inplace's narrow round-trip
// when the caller has already produced canonical form. Only performs the
// cheap sign-canon + constant-den absorption that reduce_inplace would
// otherwise do at the end. Used by add_via_q_underscore (_fmpz_mpoly_q_add
// returns canonical output per the underscore primitive's contract).
//
// Safety: the caller MUST have ensured gcd(num, den) = 1 in the integer
// sense (q_add's contract guarantees this). If that invariant is wrong,
// downstream gcd_cofactors will produce incorrect cofactors and the
// integration result will diverge silently.
//
// HF_DISABLE_FROM_CANONICAL=1 reverts to the safe Rat(num, den) ctor for
// debugging.
Rat Rat::from_canonical(Poly num, Poly den) {
    static const bool disabled = []{
        const char* e = HF_FLAG_DISABLE_FROM_CANONICAL;
        return e && e[0] && e[0] != '0';
    }();
    if (disabled) {
        return Rat(std::move(num), std::move(den));
    }
    // Sign-canon + constant-den absorption only. No GCD, no narrow
    // round-trip. Mirrors the tail of reduce_inplace (rat.cpp ~744-758).
    if (num.is_zero()) {
        den = Poly::one_of(num.ctx());
    } else if (den.is_fmpq()) {
        num.scalar_div_fmpq_const(den);
        den = Poly::one_of(num.ctx());
    } else if (den.leading_coef_is_negative()) {
        num = -num;
        den = -den;
    }
    return Rat(RawTag{}, std::move(num), std::move(den));
}

Rat Rat::parse(const PolyCtx& ctx, const std::string& expr) {
    // Look for a TOP-LEVEL `/` (outside parentheses) that is NOT a
    // rational-coefficient slash. FLINT's fmpq_mpoly parser handles
    // rational coefficients natively — "1/2*z^2 - 3*z" is a single
    // polynomial — so the `/` inside `1/2` is not a splitter. Heuristic:
    // skip any `/` whose immediate neighbors on both sides are digits.
    //
    // NOTE: we cannot use "try Poly(expr) first, fall back on
    // exception" because FLINT's parser calls `flint_abort()` (not
    // return nonzero) on inputs like "1/(2*z - 3)", which would kill
    // the process.
    int depth = 0;
    int slash_pos = -1;
    for (int i = 0; i < static_cast<int>(expr.size()); ++i) {
        char c = expr[i];
        if (c == '(') depth++;
        else if (c == ')') depth--;
        else if (c == '/' && depth == 0) {
            bool prev_digit = (i > 0) &&
                std::isdigit(static_cast<unsigned char>(expr[i - 1]));
            bool next_digit = (i + 1 < static_cast<int>(expr.size())) &&
                std::isdigit(static_cast<unsigned char>(expr[i + 1]));
            if (prev_digit && next_digit) continue;   // coefficient slash
            slash_pos = i; break;
        }
    }
    if (slash_pos < 0) {
        return Rat(Poly(ctx, expr));
    }
    std::string num_s = expr.substr(0, slash_pos);
    std::string den_s = expr.substr(slash_pos + 1);
    return Rat(Poly(ctx, num_s), Poly(ctx, den_s));
}

std::optional<Rat> Rat::parse_or_none(const PolyCtx& ctx,
                                       const std::string& expr) {
    try {
        return parse(ctx, expr);
    } catch (const std::runtime_error&) {
        // Poly::Poly(ctx, expr) throws on unknown variable in `expr`
        // after the chain-14 ctor disarm at poly.hpp:88-103.  Any
        // other std::runtime_error from `parse` (e.g. malformed
        // input) is also swallowed; the caller distinguishes
        // missing-var by setting the narrow_ctx flag, and any other
        // upstream malformed input is the caller's bug to detect.
        return std::nullopt;
    }
}

bool Rat::equal(const Rat& other) const {
    // Canonical form => equality by component equality.
    return num_.equal(other.num_) && den_.equal(other.den_);
}

const std::string& Rat::to_string() const {
    if (!cached_str_.empty()) return cached_str_;
    // 2026-04-30 (axis-E): den_.is_one() is a structural FLINT check
    // (fmpq_mpoly_is_one), avoiding a Poly::to_string allocation
    // every miss. Same lever as axis-C / partial_fractions cleanup.
    if (den_.is_one()) {
        cached_str_ = num_.to_string();
        return cached_str_;
    }
    std::string d = den_.to_string();
    // Parenthesize non-monomial num/den so the result is unambiguous.
    // A monomial is a single term (no top-level + or -). Anything with
    // a top-level "*" also gets parens on the denominator side, since
    // "a/b*c" would otherwise parse as (a/b)*c when we mean a/(b*c).
    auto has_top_level = [](const std::string& s, char c) {
        int depth = 0;
        for (size_t i = 1; i < s.size(); ++i) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')') depth--;
            else if (depth == 0 && s[i] == c) return true;
        }
        return false;
    };
    auto wrap_num = [&](const std::string& s) {
        if (has_top_level(s, '+') || has_top_level(s, '-') ||
            has_top_level(s, ' '))
            return "(" + s + ")";
        return s;
    };
    auto wrap_den = [&](const std::string& s) {
        // For the denominator we also wrap on top-level * because
        // "a/b*c" parses as "(a/b)*c" not "a/(b*c)".
        if (has_top_level(s, '+') || has_top_level(s, '-') ||
            has_top_level(s, '*') || has_top_level(s, ' '))
            return "(" + s + ")";
        return s;
    };
    cached_str_ = wrap_num(num_.to_string()) + "/" + wrap_den(d);
    return cached_str_;
}

// -------- Arithmetic --------

// Branch I rep-swap dispatcher. New path is `add_via_q_underscore`
// (defined in the anonymous namespace above). Legacy cross-mult path
// is preserved as `Rat::add_legacy`, both for the
// HF_USE_LEGACY_RAT_ADD env-gate kill-switch and for the
// per-call semantic-equivalence unit test
// (HyperFLINT/test/test_rat_add_equivalence.cpp).
Rat Rat::add_impl(const Rat& b) const {
    // HF FF Phase 5 §A.1 iter-50: op_call emit at Rat::add entry (§3.1 of
    // design.md). Combined input hash mixes 4 polys (this.num, this.den,
    // b.num, b.den) in stable order; arity=2 records the binary nature.
    // OFF-path fast-guard via the `if (!hf_probe_active) return;` head of
    // `hf_probe_canonical_hash_poly`, so the 4 hash calls early-out before
    // walking any payloads when the probe is disabled.
    if (hf_probe_active) {
        uint64_t ih = kFnv1a64OffsetBasis;
        ih = hf_probe_fnv1a64_mix_u64(ih,
                hf_probe_canonical_hash_poly(num_.raw(),  num_.ctx().raw()));
        ih = hf_probe_fnv1a64_mix_u64(ih,
                hf_probe_canonical_hash_poly(den_.raw(),  num_.ctx().raw()));
        ih = hf_probe_fnv1a64_mix_u64(ih,
                hf_probe_canonical_hash_poly(b.num_.raw(), b.num_.ctx().raw()));
        ih = hf_probe_fnv1a64_mix_u64(ih,
                hf_probe_canonical_hash_poly(b.den_.raw(), b.num_.ctx().raw()));
        hf_probe_emit_op_call("Rat::add", ih, 2);
    }
    // Branch I (2026-05-01) operand-quad dump: env-gated; no-op when off.
    rat_add_quad_dumper().maybe_dump(num_, den_, b.num_, b.den_);
    // Phase 2-pre.1 (2026-05-03) operand-sparsity probe: env-gated.
    // When HF_DUMP_OPERAND_SPARSITY=1, every (HF_OPERAND_SPARSITY_RATE)-th
    // call emits a JSONL row to stderr with per-Poly k/N statistics
    // (k = popcount(nonzero exp slots) per term).  Default-off:
    // single static-local-cached load + branch, no atomic increment.
    if (sparsity_probe_enabled() && sparsity_probe_should_emit()) {
        emit_sparsity_row(*this, b);
    }
    // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num() in OMP mode;
    // under HF_USE_GCD=1 returns the GCD slot index.
    const int _ra_tid = ::hyperflint::runtime::hf_get_thread_num();
    {
        auto& _cv = rat_add_calls_storage();
        if (static_cast<size_t>(_ra_tid) < _cv.size()) _cv[_ra_tid] += 1;
    }
    // Chain-20 Tier A1 wall-per-call measurement: time around the
    // dispatch fork, attribute to the path actually taken.  Anchors
    // the R29-guessed 1.75 ms baseline (PF1 verdict CF3).
    const bool _ra_route_legacy =
        use_legacy_rat_add() ||
        num_.ctx().vars().size() < repswap_nvars_min();
    const bool _ra_tg = step_trace_enabled();
    const auto _ra_t0 = _ra_tg ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    if (_ra_route_legacy) {
        Rat r = add_legacy(b);
        if (_ra_tg) {
            const auto _ra_t1 = std::chrono::steady_clock::now();
            auto& _wv = rat_add_legacy_wall_storage();
            auto& _nv = rat_add_legacy_calls_storage();
            if (static_cast<size_t>(_ra_tid) < _wv.size())
                _wv[_ra_tid] += std::chrono::duration<double>(
                    _ra_t1 - _ra_t0).count();
            if (static_cast<size_t>(_ra_tid) < _nv.size()) _nv[_ra_tid] += 1;
        }
        return r;
    }
    Rat r = add_via_q_underscore(num_, den_, b.num_, b.den_);
    if (_ra_tg) {
        const auto _ra_t1 = std::chrono::steady_clock::now();
        auto& _wv = rat_add_via_qu_wall_storage();
        auto& _nv = rat_add_via_qu_calls_storage();
        if (static_cast<size_t>(_ra_tid) < _wv.size())
            _wv[_ra_tid] += std::chrono::duration<double>(
                _ra_t1 - _ra_t0).count();
        if (static_cast<size_t>(_ra_tid) < _nv.size()) _nv[_ra_tid] += 1;
    }
    return r;
}

// HF FF Phase 5 §E Step E.2-impl-2 (iter-61-β.1).
//
// Public `Rat::add` entry-point with operator-memoization wrap. On cache
// HIT, returns a deep-copy of the stored value and replays the call-count
// counter via `counter_replay::rat_add_on_hit()`; wall-time counters are
// not replayed (HIT really did take ~0 wall — that is the intended
// semantics). On cache MISS, delegates to `add_impl` (the pre-iter-61
// body) and inserts the result.
//
// Per implementation_memo §3.1 + §iter-59-fold-REQ-7 (option b): the
// cache REMAINS ENABLED under HF_USE_SCALAR_REP=1; the Rat value type
// (a pair of Polys) does not embed ZWTable references, so the
// FOLD-ER3 SCALAR_REP=1 hazard does not apply here.
//
// Master-switch fast path: when HF_OPERATOR_MEMO=0 (default), the wrap
// is a single load-from-cached-bool + branch; canonical output is
// byte-identical to pre-iter-61 (byte-id smoke gate at iter-61-γ).
Rat Rat::add(const Rat& b) const {
    // HF FF Phase 6 REVISED §6.P iter-17 (BINDING pre-build reviewer at
    // iter-16): env-gated structural-sharing probe.  Default-OFF guarded
    // by the master predicate; FOLD-D-DISCIPLINE-N byte-id invariance
    // holds under default-OFF.  Operates on the PUBLIC entry-point so
    // both memo HIT and MISS paths are observed.  operator+= (in-place)
    // is intentionally NOT instrumented per design memo §2 NOTE and
    // FOLD-DC5: structural-sharing semantics are well-defined only for
    // non-in-place mutations.
    //
    // unchanged_bytes: iter-17 ships the -1 "not measured" sentinel for
    // Rat::add for the same wall-regression reason as reduce_inplace
    // (see that hook's comment); iter-18+ refines per BINDING reviewer's
    // REQ-fold.  pre_bytes is bytes(*this) + bytes(b); post_bytes is
    // bytes(result).
    const bool probe = structural_sharing::probe_add_instrumented()
                       && structural_sharing::should_emit_op("add");
    int64_t pre_bytes = 0;
    if (probe) {
        pre_bytes =
            static_cast<int64_t>(this->total_bytes()) +
            static_cast<int64_t>(b.total_bytes());
    }

    if (operator_memo::rat_add_enabled()) {
        canonical_signature::RatAddKey key =
            canonical_signature::make_rat_add_key(*this, b);
        const std::uint64_t key_hash =
            canonical_signature::hash_rat_add_key(key);
        // iter-70 REC-2: try_lookup returns std::shared_ptr<const Rat>
        // (ref-counted handle, COW). On HIT, dereferencing yields
        // `const Rat&`; the `return *cached_sp` invokes Rat's copy ctor
        // at the caller-by-value return boundary. The cache's lock-held
        // window shrinks from O(deep-copy) to O(1) ref-count++.
        auto cached_sp = g_rat_add_cache().try_lookup(key, key_hash);
        if (cached_sp) {
            counter_replay::rat_add_on_hit();
            if (probe) {
                structural_sharing::emit(
                    "add", pre_bytes,
                    static_cast<int64_t>(cached_sp->total_bytes()),
                    /*unchanged_bytes=*/-1);
            }
            return *cached_sp;
        }
        Rat result = add_impl(b);
        g_rat_add_cache().insert(std::move(key), key_hash, Rat(result));
        if (probe) {
            structural_sharing::emit(
                "add", pre_bytes,
                static_cast<int64_t>(result.total_bytes()),
                /*unchanged_bytes=*/-1);
        }
        return result;
    }
    Rat result = add_impl(b);
    if (probe) {
        structural_sharing::emit(
            "add", pre_bytes,
            static_cast<int64_t>(result.total_bytes()),
            /*unchanged_bytes=*/-1);
    }
    return result;
}

Rat Rat::add_legacy(const Rat& b) const {
    // (p/q) + (r/s) = (p*s + r*q) / (q*s), reduced.
    //
    // Lever-1 extended (2026-04-27): time the three wide-ctx Poly
    // mults and the wide-ctx Poly add separately from the
    // reduce_inplace call (which lives in the (Poly,Poly) ctor and
    // is timed by reduce_narrow_*/reduce_wide_*). The dark mass in
    // bump_addto_s on 3l3pt step-3 (125 s = 65 % of bump_addto)
    // should bottom out here if the wide-ctx mult hypothesis is
    // correct.
    // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num() in OMP mode;
    // under HF_USE_GCD=1 returns the GCD slot index.
    const int _ra_tid = ::hyperflint::runtime::hf_get_thread_num();
    const bool _tg = step_trace_enabled();
    const auto _t0 = _tg ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{};
    Poly tmp1 = num_ * b.den_;
    Poly tmp2 = b.num_ * den_;
    const auto _t1 = _tg ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{};
    Poly new_num = tmp1 + tmp2;
    const auto _t2 = _tg ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{};
    Poly new_den = den_ * b.den_;
    if (_tg) {
        const auto _t3 = std::chrono::steady_clock::now();
        auto& _mv = rat_add_polymul_storage();
        if (static_cast<size_t>(_ra_tid) < _mv.size()) {
            _mv[_ra_tid] += std::chrono::duration<double>(_t1 - _t0).count();
            _mv[_ra_tid] += std::chrono::duration<double>(_t3 - _t2).count();
        }
        auto& _av = rat_add_polyadd_storage();
        if (static_cast<size_t>(_ra_tid) < _av.size())
            _av[_ra_tid] += std::chrono::duration<double>(_t2 - _t1).count();
    }
    return Rat(std::move(new_num), std::move(new_den));
}

// 2026-05-07 (iter-30, HF MZV-rewrite C-prep.4 content audit):
// explicit rep-swap entry point.  Symmetric to `add_legacy`; routes
// through `add_via_q_underscore` (same code that `Rat::add` dispatches
// to when nvars >= HF_REPSWAP_NVARS_MIN and HF_USE_LEGACY_RAT_ADD is
// not set).  Used by test_rat_content_invariance to verify that the
// two production paths produce byte-identical Rat::to_string output
// on algebraically-equal inputs, without depending on the static
// env-gate lambdas at rat.cpp:1230-1235 / 1284-1292 (which are
// resolved once at first call and cannot be reset per-test from
// outside).  Production hot paths must continue to call `add` (or
// `operator+`/`operator+=`); the dispatcher picks the right backend.
Rat Rat::add_repswap(const Rat& b) const {
    return add_via_q_underscore(num_, den_, b.num_, b.den_);
}

// In-place add for the bump-aggregation hot path
// (primitive.cpp::integrate_ii).  Functionally identical to
// `*this = *this + b;` but avoids:
//   * constructing a temporary Rat to hold the operator+ result,
//   * the 4 Poly construction/destruction pairs that flow through
//     std::move chains in `Rat(Poly, Poly)` ctor + return-by-value,
//   * the (now-fixed-via-move-assign) deep copy when the temporary
//     Rat is assigned back into `*this`.
// Same timer hooks as Rat::add so per-Poly-op + per-call counters
// continue to attribute correctly.
Rat& Rat::operator+=(const Rat& b) {
    // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num() in OMP mode;
    // under HF_USE_GCD=1 returns the GCD slot index.
    const int _ra_tid = ::hyperflint::runtime::hf_get_thread_num();
    // Branch I (2026-05-01) operand-quad dump: env-gated; no-op when off.
    rat_add_quad_dumper().maybe_dump(num_, den_, b.num_, b.den_);
    // Phase 2-pre (2026-05-03): operand-sparsity probe. Default-off
    // single-branch when env unset; enabled emits per-monomial k/N
    // popcount stats every Nth call.  Same hook as in Rat::add.
    if (sparsity_probe_enabled() && sparsity_probe_should_emit()) {
        emit_sparsity_row(*this, b);
    }
    {
        auto& _cv = rat_add_calls_storage();
        if (static_cast<size_t>(_ra_tid) < _cv.size()) _cv[_ra_tid] += 1;
    }
    // Tier A1 wide-ctx threshold gate: small ctxs eat the lift/lower
    // overhead without saving enough gcd_cofactors to amortise it.
    const bool route_legacy =
        use_legacy_rat_add() ||
        num_.ctx().vars().size() < repswap_nvars_min();
    // Chain-20 Tier A1 wall-per-call measurement (mirrors Rat::add).
    const bool _tg = step_trace_enabled();
    const auto _ra_t0 = _tg ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
    if (route_legacy) {
        const auto _t0 = _tg ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
        Poly tmp1 = num_ * b.den_;
        Poly tmp2 = b.num_ * den_;
        const auto _t1 = _tg ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
        Poly new_num = tmp1 + tmp2;
        const auto _t2 = _tg ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
        Poly new_den = den_ * b.den_;
        if (_tg) {
            const auto _t3 = std::chrono::steady_clock::now();
            auto& _mv = rat_add_polymul_storage();
            if (static_cast<size_t>(_ra_tid) < _mv.size()) {
                _mv[_ra_tid] += std::chrono::duration<double>(
                    _t1 - _t0).count();
                _mv[_ra_tid] += std::chrono::duration<double>(
                    _t3 - _t2).count();
            }
            auto& _av = rat_add_polyadd_storage();
            if (static_cast<size_t>(_ra_tid) < _av.size())
                _av[_ra_tid] += std::chrono::duration<double>(
                    _t2 - _t1).count();
        }
        num_ = std::move(new_num);
        den_ = std::move(new_den);
        cached_str_.clear();
        reduce_inplace(num_, den_);
        if (_tg) {
            const auto _ra_t1 = std::chrono::steady_clock::now();
            auto& _wv = rat_add_legacy_wall_storage();
            auto& _nv = rat_add_legacy_calls_storage();
            if (static_cast<size_t>(_ra_tid) < _wv.size())
                _wv[_ra_tid] += std::chrono::duration<double>(
                    _ra_t1 - _ra_t0).count();
            if (static_cast<size_t>(_ra_tid) < _nv.size()) _nv[_ra_tid] += 1;
        }
        return *this;
    }
    // Branch I rep-swap path: route through `add_via_q_underscore`
    // (returns a fresh Rat in canonical form), then move-assign back
    // into *this. This is functionally equivalent to `*this = add(b)`
    // and avoids reimplementing the lift/lower/hoist logic in two places.
    *this = add_via_q_underscore(num_, den_, b.num_, b.den_);
    if (_tg) {
        const auto _ra_t1 = std::chrono::steady_clock::now();
        auto& _wv = rat_add_via_qu_wall_storage();
        auto& _nv = rat_add_via_qu_calls_storage();
        if (static_cast<size_t>(_ra_tid) < _wv.size())
            _wv[_ra_tid] += std::chrono::duration<double>(
                _ra_t1 - _ra_t0).count();
        if (static_cast<size_t>(_ra_tid) < _nv.size()) _nv[_ra_tid] += 1;
    }
    return *this;
}

// 2026-05-02 (volume-probe instrumentation): per-thread counters for
// the three remaining legacy Rat ops. Used to decide whether to extend
// the from_canonical pattern (already shipped for Rat::add) to mul/sub/div.
namespace {
std::vector<long>& rat_mul_calls_storage() { static std::vector<long> v; return v; }
std::vector<long>& rat_sub_calls_storage() { static std::vector<long> v; return v; }
std::vector<long>& rat_div_calls_storage() { static std::vector<long> v; return v; }
inline void bump_rat_op(std::vector<long>& v) {
    // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num() in OMP mode;
    // under HF_USE_GCD=1 returns the GCD slot index.
    const int tid = ::hyperflint::runtime::hf_get_thread_num();
    if (static_cast<size_t>(tid) < v.size()) v[tid] += 1;
}
}
void init_rat_op_calls_per_thread(int n) {
    rat_mul_calls_storage().assign(n > 0 ? n : 1, 0L);
    rat_sub_calls_storage().assign(n > 0 ? n : 1, 0L);
    rat_div_calls_storage().assign(n > 0 ? n : 1, 0L);
}
void reset_rat_op_calls_per_thread() {
    for (auto& x : rat_mul_calls_storage()) x = 0L;
    for (auto& x : rat_sub_calls_storage()) x = 0L;
    for (auto& x : rat_div_calls_storage()) x = 0L;
}
long sum_rat_mul_calls_per_thread() {
    long s = 0; for (long x : rat_mul_calls_storage()) s += x; return s;
}
long sum_rat_sub_calls_per_thread() {
    long s = 0; for (long x : rat_sub_calls_storage()) s += x; return s;
}
long sum_rat_div_calls_per_thread() {
    long s = 0; for (long x : rat_div_calls_storage()) s += x; return s;
}

// Legacy cross-multiply + reduce_inplace paths, kept for env-gated
// fallback (HF_USE_LEGACY_RAT_{SUB,MUL,DIV}=1) and for small-ctx routing
// where the rep-swap transduction overhead doesn't amortise.
static Rat rat_sub_legacy(const Poly& a_num, const Poly& a_den,
                          const Poly& b_num, const Poly& b_den) {
    Poly new_num = a_num * b_den - b_num * a_den;
    Poly new_den = a_den * b_den;
    return Rat(std::move(new_num), std::move(new_den));
}
static Rat rat_mul_legacy(const Poly& a_num, const Poly& a_den,
                          const Poly& b_num, const Poly& b_den) {
    Poly new_num = a_num * b_num;
    Poly new_den = a_den * b_den;
    return Rat(std::move(new_num), std::move(new_den));
}
static Rat rat_div_legacy(const Poly& a_num, const Poly& a_den,
                          const Poly& b_num, const Poly& b_den) {
    Poly new_num = a_num * b_den;
    Poly new_den = a_den * b_num;
    return Rat(std::move(new_num), std::move(new_den));
}

Rat Rat::sub(const Rat& b) const {
    bump_rat_op(rat_sub_calls_storage());
    if (use_qunderscore_rat_sub() &&
        num_.ctx().vars().size() >= repswap_nvars_min()) {
        return sub_via_q_underscore(num_, den_, b.num_, b.den_);
    }
    return rat_sub_legacy(num_, den_, b.num_, b.den_);
}

Rat Rat::mul(const Rat& b) const {
    bump_rat_op(rat_mul_calls_storage());
    if (use_qunderscore_rat_mul() &&
        num_.ctx().vars().size() >= repswap_nvars_min()) {
        return mul_via_q_underscore(num_, den_, b.num_, b.den_);
    }
    return rat_mul_legacy(num_, den_, b.num_, b.den_);
}

Rat Rat::div(const Rat& b) const {
    bump_rat_op(rat_div_calls_storage());
    if (b.num_.is_zero()) {
        throw std::runtime_error("Rat::div: division by zero");
    }
    if (use_qunderscore_rat_div() &&
        num_.ctx().vars().size() >= repswap_nvars_min()) {
        return div_via_q_underscore(num_, den_, b.num_, b.den_);
    }
    return rat_div_legacy(num_, den_, b.num_, b.den_);
}

Rat Rat::neg() const {
    Poly n = -num_;
    // Skip reduction -- negation preserves canonical form (gcd is
    // the same up to sign, and we re-normalize den's sign anyway).
    Rat r(std::move(n), Poly(den_));
    return r;
}

Rat Rat::pow(long n) const {
    if (n == 0) {
        return Rat::one_of(num_.ctx());
    }
    if (n > 0) {
        return Rat(num_.pow(static_cast<unsigned long>(n)),
                   den_.pow(static_cast<unsigned long>(n)));
    }
    // n < 0: reciprocal.
    if (num_.is_zero()) {
        throw std::runtime_error("Rat::pow: negative power of zero");
    }
    long m = -n;
    return Rat(den_.pow(static_cast<unsigned long>(m)),
               num_.pow(static_cast<unsigned long>(m)));
}

// -------- Calculus --------

Rat Rat::derivative(size_t var_idx) const {
    // (p/q)' = (p'q - p q') / q^2
    Poly pp = num_.derivative(var_idx);
    Poly qp = den_.derivative(var_idx);
    Poly new_num = pp * den_ - num_ * qp;
    Poly new_den = den_ * den_;
    return Rat(std::move(new_num), std::move(new_den));
}

Rat Rat::substitute_one_rat(size_t var_idx, const std::string& value) const {
    Poly n = num_.substitute_one_rat(var_idx, value);
    Poly d = den_.substitute_one_rat(var_idx, value);
    return Rat(std::move(n), std::move(d));
}

// -------- Phase 2b: Laurent-order helpers --------

long Rat::pole_degree(size_t var_idx) const {
    if (num_.is_zero()) return LONG_MAX;   // "+infinity"
    long nmin = num_.min_exponent_in_var(var_idx);
    long dmin = den_.min_exponent_in_var(var_idx);
    // Both finite: num nonzero ⇒ nmin != LONG_MAX; den nonzero always.
    return nmin - dmin;
}

Rat Rat::rat_residue(size_t var_idx) const {
    if (num_.is_zero()) return Rat::zero_of(ctx());
    long nmin = num_.min_exponent_in_var(var_idx);
    long dmin = den_.min_exponent_in_var(var_idx);
    Poly ncoef = num_.coefficient_of_var(var_idx, nmin);
    Poly dcoef = den_.coefficient_of_var(var_idx, dmin);
    return Rat(std::move(ncoef), std::move(dcoef));
}

std::string Rat::evaluate_all(const std::vector<std::string>& values) const {
    std::string n = num_.evaluate_all(values);
    std::string d = den_.evaluate_all(values);
    if (d == "0") throw std::runtime_error("Rat::evaluate_all: 0/0");
    if (d == "1") return n;
    // Compose as a string fraction.
    // Both are fmpq-format strings ("p", "p/q"); we could reduce here
    // but simple concatenation is OK for now.
    return n + "/" + d;
}

}  // namespace hyperflint
