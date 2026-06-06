// Rat: rational function = num / den in lowest terms.
//
// The invariant (num, den coprime; den normalized) is maintained on
// every constructor / op. Maintaining canonical form on construction
// is how HyperFLINT realizes the ~100× Together/Cancel speedup the
// benchmark captured: Together is literally a no-op because the
// representation is already together, and every arithmetic op
// produces a canonical result.
//
// This header only depends on Poly / PolyCtx; no direct FLINT calls.

#pragma once

#include "hyperflint/core/poly.hpp"

#include <optional>
#include <string>
#include <utility>

namespace hyperflint {

class Rat {
public:
    // Construct from a polynomial; denominator = 1. Non-explicit so
    // Poly decays to Rat at call boundaries. Uses Poly::one_of to
    // skip FLINT's string-parse of "1" — the old parse accounted for
    // a nontrivial share of tst0 time once everything else was fast
    // since *every* Rat(Poly) call paid it.
    Rat(Poly p) : num_(std::move(p)), den_(Poly::one_of(num_.ctx())) {
        // HF FF Phase 5 §A.1 iter-50: value_create emit at Rat(Poly p) ctor.
        // OFF-path fast-guard via `hf_probe_active` branch.
        hf_probe_emit_rat_create(reinterpret_cast<uintptr_t>(this),
                                 num_.raw(), den_.raw(),
                                 num_.ctx().raw());
    }

    // Construct from num/den, reducing to lowest terms.
    Rat(Poly num, Poly den);

    // HF FF Phase 5 §A.1 iter-50: explicit destructor (value_destroy emit).
    // Previously implicit; now declared so every Rat lifetime is paired with
    // a destroy event by instance_id. The body is no-op-equivalent to the
    // implicit version: the Poly members destruct in reverse declaration
    // order (cached_str_, then den_, then num_) after this body returns.
    ~Rat() {
        hf_probe_emit_rat_destroy(reinterpret_cast<uintptr_t>(this));
    }
    // Copy/move are explicitly defaulted in behaviour but the bodies emit a
    // create event so the create↔destroy bookkeeping stays balanced.
    Rat(const Rat& other)
        : num_(other.num_), den_(other.den_), cached_str_(other.cached_str_) {
        hf_probe_emit_rat_create(reinterpret_cast<uintptr_t>(this),
                                 num_.raw(), den_.raw(),
                                 num_.ctx().raw());
    }
    Rat& operator=(const Rat& other) {
        if (this != &other) {
            num_        = other.num_;
            den_        = other.den_;
            cached_str_ = other.cached_str_;
        }
        return *this;
    }
    Rat(Rat&& other) noexcept
        : num_(std::move(other.num_)),
          den_(std::move(other.den_)),
          cached_str_(std::move(other.cached_str_)) {
        hf_probe_emit_rat_create(reinterpret_cast<uintptr_t>(this),
                                 num_.raw(), den_.raw(),
                                 num_.ctx().raw());
    }
    Rat& operator=(Rat&& other) noexcept {
        if (this != &other) {
            num_        = std::move(other.num_);
            den_        = std::move(other.den_);
            cached_str_ = std::move(other.cached_str_);
        }
        return *this;
    }

    // Construct from num/den that the caller asserts are already in
    // canonical form (gcd(num,den)=1, integer content normalised, but
    // possibly not sign-canonicalised). Skips reduce_inplace's expensive
    // narrow round-trip; only does the cheap sign-canon (flip both
    // signs if den's leading coeff is negative) and constant-den
    // absorption. Used by add_via_q_underscore where _fmpz_mpoly_q_add
    // already produces canonical output. See
    // notes/hf_flint_pool_experiment/transplant_levers.md follow-up.
    static Rat from_canonical(Poly num, Poly den);

private:
    // Tag-dispatched private constructor: builds a Rat by directly
    // moving num/den into the fields without running reduce_inplace.
    // Used internally by from_canonical to avoid the awkward
    // "construct-then-overwrite" pattern.
    struct RawTag {};
    Rat(RawTag, Poly num, Poly den)
        : num_(std::move(num)), den_(std::move(den)) {
        // HF FF Phase 5 §A.1 iter-50: value_create emit for RawTag ctor.
        hf_probe_emit_rat_create(reinterpret_cast<uintptr_t>(this),
                                 num_.raw(), den_.raw(),
                                 num_.ctx().raw());
    }
public:

    // Integer-constant fast paths — same spirit as Poly::from_int.
    // Avoid the Poly-string-parse that Rat(Poly(ctx,"0"))-style call
    // sites used to pay.
    static Rat zero_of(const PolyCtx& ctx) { return Rat(Poly::zero_of(ctx)); }
    static Rat one_of (const PolyCtx& ctx) { return Rat(Poly::one_of (ctx)); }
    static Rat from_int(const PolyCtx& ctx, long value) {
        return Rat(Poly::from_int(ctx, value));
    }

    // Parse from a string "num" or "num/den". For a single poly the
    // denominator is 1. For a nested expression like "(a+b)/(c+d)"
    // the parser expects a literal slash at the top level --
    // sub-expressions with their own divisions need parentheses.
    static Rat parse(const PolyCtx& ctx, const std::string& expr);

    // Sentinel-tolerant variant of `parse` (R24 rev 2 / chain-16
    // scaffolding).  Returns std::nullopt when the underlying
    // `parse` would throw `std::runtime_error` (typically "Poly:
    // parse error: ..." raised by the Poly ctor on an unknown
    // variable name in `expr`).
    //
    // This is the load-bearing primitive for the narrow-ctx
    // sentinel-tolerance design: callsites under the OMP region
    // that may parse runtime-constructed mzv-symbol names use
    // this in place of the throwing `parse`, so a missing-var
    // case does not escape the parallel region as
    // `std::terminate`.  The corresponding flag in
    // `runtime/narrow_ctx_flag.hpp` is set by the caller on
    // `nullopt`; the host post-region check translates the flag
    // into a `NarrowCtxTooNarrow` host-side throw.
    //
    // Relies on the chain-14 fix at `poly.hpp:88-103` (the Poly
    // ctor's destructor disarm): without that fix, the parse
    // failure SIGTRAPs instead of throwing and `try/catch` cannot
    // intercept.  Verified by the Phase 2 fork-per-case smoke
    // test in `bench/bench_parse_unknown_var.cpp`.
    static std::optional<Rat> parse_or_none(const PolyCtx& ctx,
                                              const std::string& expr);

    const Poly& num() const { return num_; }
    const Poly& den() const { return den_; }
    const PolyCtx& ctx() const { return num_.ctx(); }

    bool is_zero() const { return num_.is_zero(); }
    bool equal(const Rat& other) const;

    // Canonical single-line string "num/den" (or "num" when den == 1).
    // Result is memoized on first call: Rat is effectively immutable
    // after construction (every op returns a fresh Rat), so the
    // cached string stays valid for the Rat's lifetime. Profiled
    // win comes from transform / regulator / linear-factors cache
    // keys that stringify the same Rat repeatedly.
    const std::string& to_string() const;

    // ---- Arithmetic ----
    Rat add(const Rat& b) const;
    // HF FF Phase 5 §E Step E.2-impl-2 (iter-61-β.1): `add_impl` is the
    // pre-iter-61 body of `add`, renamed and made public so the §E
    // operator-memo wrap at `add` can delegate on MISS. Production
    // callers MUST call `add` (the cached entry-point); `add_impl` is
    // exposed for the cache wrap and for test_rat_add_equivalence which
    // bypasses the cache to compare semantic equivalence. Per
    // implementation_memo §3.1 + §iter-59-fold-REQ-7 (REMAINS ENABLED
    // under SCALAR_REP=1 — no ZWTable embedding hazard).
    Rat add_impl(const Rat& b) const;
    // Legacy cross-mult+gcd_cofactors path. Retained for the
    // HF_USE_LEGACY_RAT_ADD env-gate kill-switch and for the
    // semantic-equivalence unit test
    // (test/test_rat_add_equivalence.cpp). Do NOT call from
    // production hot paths — `add` dispatches between the two.
    Rat add_legacy(const Rat& b) const;
    // 2026-05-07 (iter-30, HF MZV-rewrite C-prep.4 content audit):
    // explicit rep-swap (add_via_q_underscore) entry point.  Symmetric
    // to `add_legacy`; lets test_rat_content_invariance verify that
    // both production paths produce byte-identical Rat::to_string on
    // algebraically-equal inputs without depending on the static-cached
    // env-gate lambdas at rat.cpp:1230-1235 / 1284-1292.  Do NOT call
    // from production hot paths — `add` dispatches between the two.
    Rat add_repswap(const Rat& b) const;
    Rat sub(const Rat& b) const;
    Rat mul(const Rat& b) const;
    Rat div(const Rat& b) const;   // throws if b is zero
    Rat neg() const;
    Rat pow(long n) const;          // negative n: reciprocal power

    Rat operator+(const Rat& b) const { return add(b); }
    Rat operator-(const Rat& b) const { return sub(b); }
    Rat operator*(const Rat& b) const { return mul(b); }
    Rat operator/(const Rat& b) const { return div(b); }
    Rat operator-() const             { return neg(); }

    // 2026-04-27: in-place add for the bump-aggregation hot path.
    // `rows[i].coef += c` avoids constructing a temporary Rat (and
    // its 4 Poly tempor moves) that the equivalent `coef = coef + c`
    // pattern incurs.  Combined with the Poly move-assign added in
    // poly.hpp, this strips the 67 s C++ residual on 3l3pt step-3.
    Rat& operator+=(const Rat& b);

    // ---- Calculus ----
    // Derivative:  d/dx (p/q) = (p'q - pq') / q^2, then reduce.
    Rat derivative(size_t var_idx) const;

    // Single-var rational substitution.
    Rat substitute_one_rat(size_t var_idx, const std::string& value) const;

    // All-var evaluation to a scalar rational, as a "p/q" string.
    std::string evaluate_all(const std::vector<std::string>& values) const;

    // 2026-05-05 (path-A diagnostic). Heap byte estimate for *this Rat:
    //   num.total_bytes() + den.total_bytes() + cached_str_.size().
    // Used by the diagnostic probe walkers; default-OFF env-gated.
    size_t total_bytes() const {
        return num_.total_bytes() + den_.total_bytes() + cached_str_.size();
    }

    // -------- Phase 2b: Laurent-order helpers --------
    // Signed Laurent order of *this at var = 0:
    //   pole_degree = min-degree-in-var(num) - min-degree-in-var(den)
    // Convention: LONG_MAX if *this is identically zero ("plus infinity").
    long pole_degree(size_t var_idx) const;

    // Leading Laurent coefficient at var = 0: the rational coefficient
    // of var^pole_degree in the expansion, as a Rat over the remaining
    // variables.  Returns 0 if *this is zero.
    Rat rat_residue(size_t var_idx) const;

private:
    Poly num_;
    Poly den_;
    // Lazy-filled string cache. Safe because Rat has no mutation
    // methods in its public API — every arithmetic op returns a
    // fresh Rat. The cache is "-1-sentinel" (empty) until first
    // to_string() call.
    mutable std::string cached_str_;
};

// Per-thread counters/timers for the narrow-vs-wide branch inside
// `reduce_inplace` (the GCD-cofactor reducer behind every Rat ctor
// and every Rat arithmetic op).  Wired into integration_step's
// step harvest so HF_STEP_TRACE emits per-step:
//   reduce_narrow_calls / reduce_narrow_s
//   reduce_wide_calls   / reduce_wide_s
//   reduce_zero_calls   (degenerate 0/den short-circuit)
//
// IMPORTANT: these timers fire on every Rat-producing op (Rat::add,
// sub, mul, div, pow, derivative, and the (Poly,Poly) ctor). They
// are NOT specific to Rat::add. The earlier "rat_add_*" naming
// was misleading; renamed to "reduce_*" 2026-04-27 after the
// adversarial reviewer flagged that only ~8 % of step-3 reduce
// calls came from inside `bump`.
//
// Hypothesis under test (initial): wide-ctx GCD path dominates
// step-3 wall on 3l3pt. FALSIFIED 2026-04-27 — wide-ctx is ≤ 0.4 s
// in every step. Narrow-ctx hoist is the dominant reducer cost.
//
// Same per-thread vector pattern as integrator/primitive.cpp's
// bump_addto_storage.  init at OMP team setup, reset at step
// boundary, sum after the parallel region.
void   init_reduce_per_thread(int n_threads);
void   reset_reduce_per_thread();
double sum_reduce_narrow_s_per_thread();
double sum_reduce_wide_s_per_thread();
long   sum_reduce_narrow_calls_per_thread();
long   sum_reduce_wide_calls_per_thread();
long   sum_reduce_zero_calls_per_thread();

// 2026-05-02 (HF FLINT-pool experiment): wraps just the
// fmpq_mpoly_gcd_cofactors call wall (excluding transplant /
// canonicalization) on both narrow and wide branches of
// reduce_inplace. Surfaced in HF_STEP_TRACE JSON as
// gcd_cofactors_s / gcd_cofactors_calls. Lets us measure FLINT's
// actual share without the transplant overhead included in
// reduce_narrow_s.
double sum_gcd_cofactors_s_per_thread();
long   sum_gcd_cofactors_calls_per_thread();

// 2026-05-02 (Phase-0-GCD follow-up): three sub-timers decomposing
// the non-GCD share of reduce_inplace's narrow path. See rat.cpp's
// anonymous namespace for the design rationale and definitions.
double sum_rn_used_vars_s_per_thread();
double sum_rn_setup_s_per_thread();
double sum_rn_post_s_per_thread();

// 2026-05-02 (volume probe): per-thread call counts for Rat::mul/sub/div.
// Used to decide whether the from_canonical lever (already shipped for
// Rat::add via add_via_q_underscore) is worth extending. Surfaced in
// HF_STEP_TRACE JSON as rat_mul_calls / rat_sub_calls / rat_div_calls.
void init_rat_op_calls_per_thread(int n_threads);
void reset_rat_op_calls_per_thread();
long sum_rat_mul_calls_per_thread();
long sum_rat_sub_calls_per_thread();
long sum_rat_div_calls_per_thread();

// Lever-1 extended (2026-04-27): per-Poly-op timers inside
// `Rat::add` operator+, scoped to disambiguate the 125 s step-3
// "dark mass" between `bump_addto_s` and `reduce_narrow_s`.
//   rat_add_polymul_s : wall in the three wide-ctx Poly mults
//                       (num*b.den, b.num*den, den*b.den)
//   rat_add_polyadd_s : wall in the wide-ctx Poly addition
//                       ((num*b.den) + (b.num*den))
//   rat_add_calls     : count of Rat::add invocations specifically
// (The reduce_inplace work that follows the mults/add is timed
// separately by reduce_narrow_s / reduce_wide_s above.)
double sum_rat_add_polymul_s_per_thread();
double sum_rat_add_polyadd_s_per_thread();
long   sum_rat_add_calls_per_thread();

// HF FF Phase 5 §E Step E.2-impl-2 (iter-61-β.6a).
//
// Public shim that increments the per-thread `rat_add_calls_storage`
// counter from outside `rat.cpp`'s anonymous namespace. Used by
// `operator_memo.cpp::counter_replay::rat_add_on_hit()` to replay the
// call-count counter on a §E outer-cache HIT (the cached value short-
// circuits the body that would normally increment at rat.cpp:2253-2255).
// Mirrors the production caller's bounds-check guard.
void rat_add_record_call_for_thread(int tid);

// HF FF Phase 5 §E Step E.2-impl-2 (iter-62-β.1).
//
// Public shim for `reduce_inplace`'s per-thread per-classifier
// call-count counters. `kind` selects which storage to increment:
//   0 = zero    (reduce_zero_calls_storage; rat.cpp:1078)
//   1 = narrow  (reduce_narrow_calls_storage; bounds-checked default)
//   2 = wide    (reduce_wide_calls_storage; rat.cpp:1107, 1486)
//
// `kind` values outside {0,1,2} are silently ignored (out-of-range
// classifier should never occur from the operator-memo wrap; defensive
// bounds-check for forward compatibility).
//
// Used by `operator_memo.cpp::counter_replay::reduce_on_hit(int kind)`.
// Mirrors `rat_add_record_call_for_thread` (iter-61-β.6a) — same pattern.
void reduce_record_call_for_thread(int tid, int kind);

// 2026-05-03 (chain 20, Phase 2-A2 PF3 prep): per-backend wall +
// call counters at the Rat::add / operator+= dispatch fork.  Anchors
// the R29-guessed Tier A1 baseline (PF1 verdict CF3).
//   rat_add_legacy_wall_s  : total wall in the cross-mult+gcd path
//   rat_add_via_qu_wall_s  : total wall in the chain-11 Tier A1
//                            (add_via_q_underscore) path
//   rat_add_legacy_calls   : count of legacy-path Rat::add (or +=)
//   rat_add_via_qu_calls   : count of via_q_underscore-path Rat::add
double sum_rat_add_legacy_wall_s_per_thread();
double sum_rat_add_via_qu_wall_s_per_thread();
long   sum_rat_add_legacy_calls_per_thread();
long   sum_rat_add_via_qu_calls_per_thread();

// 2026-05-01 (Tier 3 Phase-0 lazy-pair diagnostic): per-thread
// nterm-blowup accumulators around `reduce_inplace`. Each call's
// pre-reduce (num.length + den.length) sum and post-reduce sum are
// captured by an RAII sentinel.  Env-gated by HF_REDUCE_NTERM_LOG=1
// (default off, zero overhead — sentinel ctor sets `active=false`
// and dtor short-circuits).
//
// Diagnostic question: if Rat::add deferred reduce_inplace (lazy-pair
// semantics), would num/den blow up under chained ops or stay
// bounded?  Reading: avg_shrink = post_total / pre_total.  Close to 1
// → GCD does little work → deferring is cheap.  Much less than 1 →
// GCD shrinks polys substantially → deferring causes chain-effect
// blow-up.
long sum_reduce_nterm_calls_per_thread();
long sum_reduce_nterm_pre_total_per_thread();
long sum_reduce_nterm_post_total_per_thread();
long max_reduce_nterm_pre_per_thread();
long max_reduce_nterm_post_per_thread();

// 2026-05-01 (Tier 3 refined lever): count of wide-ctx GCD calls
// that fell through specifically because of the raised size-gate
// threshold (HF_REDUCE_SIZE_GATE_MIN > 4).  Diagnostic — verifies
// the lever is actually flipping the predicted call subset.
long sum_reduce_wide_smallfall_calls_per_thread();

}  // namespace hyperflint
