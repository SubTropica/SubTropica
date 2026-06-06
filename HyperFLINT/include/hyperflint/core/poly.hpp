// Poly: RAII wrapper around FLINT's fmpq_mpoly_t.
//
// Phase 0 scope: construction from string, factorization, canonical
// string output. Full op set (add/sub/mul/div/gcd/resultant/...) comes
// in Phase 1.

#pragma once

#include <flint/fmpq_mpoly.h>
#include <flint/fmpq_mpoly_factor.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// HF FF Phase 5 §A.1 iter-49: probe instrumentation header. Forward-declares
// `hf_probe_active` + emit helpers; the OFF path early-returns at the first
// instruction of every emit site, so the cost when the probe is disabled is
// one TLS-read + branch. Per FOLD-PR-REQ-3 (BINDING; iter-48-γ reviewer
// abd5207a0ac38940b), the iter-49 source change must preserve the §0.3
// baseline within ±5 % RSS + ±10 % wall on the 4-fixture OFF-path budget
// gate; the OFF-path overhead from this include + the emit-site branches is
// what that gate measures.
#include "hyperflint/instrumentation/dag_hashcons_probe.hpp"

namespace hyperflint {

// Variable table + fmpq_mpoly_ctx, shared across all Poly instances
// that live in the same polynomial ring.
class PolyCtx {
public:
    explicit PolyCtx(std::vector<std::string> vars)
        : vars_(std::move(vars)) {
        std::vector<const char*> cvars;
        cvars.reserve(vars_.size());
        for (const auto& v : vars_) cvars.push_back(v.c_str());
        fmpq_mpoly_ctx_init(ctx_, static_cast<slong>(vars_.size()), ORD_LEX);
        cvars_ = std::move(cvars);
        // Eagerly populate the name->index lookup so that it is
        // concurrent-read-safe (the previous lazy-populate under
        // `mutable` carried a data race for the OpenMP plan).
        index_map_.reserve(vars_.size());
        for (size_t i = 0; i < vars_.size(); ++i)
            index_map_.emplace(vars_[i], i);
    }

    ~PolyCtx() { fmpq_mpoly_ctx_clear(ctx_); }
    PolyCtx(const PolyCtx&) = delete;
    PolyCtx& operator=(const PolyCtx&) = delete;

    const std::vector<std::string>& vars() const { return vars_; }
    // FLINT's fmpq_mpoly_get_str_pretty wants `const char**` (non-const
    // outer pointer); we hold `const char*` items in a vector and hand
    // a const_cast'd pointer. Safe because FLINT only reads.
    const char** cvars() const {
        return const_cast<const char**>(cvars_.data());
    }
    fmpq_mpoly_ctx_struct* raw() const { return ctx_; }

    // Return the index of `name` in the variable list, or SIZE_MAX
    // if not present. Amortized O(1) via a hashmap populated eagerly
    // at ctx construction — the read is concurrent-safe with no
    // mutable-state race.
    size_t index_of(const std::string& name) const {
        auto it = index_map_.find(name);
        return it == index_map_.end() ? SIZE_MAX : it->second;
    }

private:
    // `mutable` here is a C/C++-friction concession, not a logical
    // mutability hint: FLINT's fmpq_mpoly_ctx_t is an array typedef
    // (struct[1]) which decays to a non-const pointer when passed
    // to FLINT entry points, defeating const propagation through
    // `PolyCtx::raw() const`. The struct is treated as logically
    // immutable post-construction; concurrent reads are safe (the
    // index_map_ population in the ctor is the *only* mutation,
    // and it completes before any reader observes the object).
    // See notes/hf_omp_ctx_audit/findings.md (R5 audit, 2026-05-01).
    mutable fmpq_mpoly_ctx_t ctx_;
    std::vector<std::string> vars_;
    std::vector<const char*> cvars_;
    std::unordered_map<std::string, size_t> index_map_;
};

// Polynomial / rational-function over Q in the variables of PolyCtx.
// fmpq_mpoly's canonical form guarantees that num/den are in lowest
// terms and monomials are sorted -- so "Together" and "Cancel" are
// invariants of construction, not separate operations.
class Poly {
public:
    explicit Poly(const PolyCtx& ctx) : ctx_(&ctx) {
        fmpq_mpoly_init(raw_, ctx_->raw());
        // HF FF Phase 5 §A.1 iter-49: value_create emit at leaf ctor end.
        // Emits on empty Poly state; outer delegating ctors (Poly(ctx,expr),
        // copy ctor, etc.) emit again post-mutation, so the aggregator takes
        // the LAST value_create per instance_id (the final-state hash).
        // OFF-path: hf_probe_emit_poly_create's first instruction is the
        // `if (!hf_probe_active) return;` branch — at 1 TLS-read + 1 branch
        // it should stay below the ±5 % RSS / ±10 % wall budget gate. The
        // address is used as instance_id (stable for object lifetime).
        hf_probe_emit_poly_create(reinterpret_cast<uintptr_t>(this),
                                  raw_, ctx_->raw());
    }

    // Parse from algebraic string (e.g. "x^2 + 2*x*y - 1/3").
    //
    // R23 (2026-05-03): on parse failure, disarm the destructor by
    // setting ctx_ = nullptr BEFORE the throw.  The delegating ctor
    // `: Poly(ctx)` already called fmpq_mpoly_init; on certain failed
    // parses (compound expressions with rational coefficients, e.g.
    // "1/2*Log2^2-1/2*mzv_2" against ctx={Log2}), fmpq_mpoly_clear
    // is non-idempotent — calling it once here and then again from
    // ~Poly during stack unwinding SIGTRAPs the process.  ~Poly
    // already gates its clear on `if (ctx_)`, so nulling the pointer
    // is enough to disarm.  Verified by the Phase 2 fork-per-case
    // smoke test in HyperFLINT/bench/bench_parse_unknown_var.cpp.
    Poly(const PolyCtx& ctx, const std::string& expr) : Poly(ctx) {
        if (fmpq_mpoly_set_str_pretty(raw_, expr.c_str(),
                                      ctx_->cvars(), ctx_->raw()) != 0) {
            fmpq_mpoly_clear(raw_, ctx_->raw());
            ctx_ = nullptr;
            throw std::runtime_error("Poly: parse error: " + expr);
        }
    }

    // Integer-constant fast path — avoids fmpq_mpoly_set_str_pretty,
    // which at a 700+ variable PolyCtx costs ms per call. Use for
    // the very common Poly(ctx,"0") / Poly(ctx,"1") / Poly(ctx,"-1")
    // / Poly(ctx, std::to_string(k)) construction patterns in hot
    // loops (integrate_ii, partial_fractions).
    static Poly from_int(const PolyCtx& ctx, long value) {
        Poly r(ctx);
        fmpq_mpoly_set_si(r.raw_, value, ctx.raw());
        return r;
    }
    static Poly zero_of(const PolyCtx& ctx) { return Poly(ctx); }
    static Poly one_of (const PolyCtx& ctx) { return from_int(ctx, 1); }

    // Build the generator at `var_idx` (i.e. the polynomial equal to
    // the variable itself). Avoids fmpq_mpoly_set_str_pretty on a
    // "<varname>" literal, which at 700+ ctx variables costs several
    // ms per call.
    static Poly gen(const PolyCtx& ctx, size_t var_idx);

    ~Poly() {
        // HF FF Phase 5 §A.1 iter-49: value_destroy emit BEFORE clear so the
        // address is still meaningful as the instance_id pairing key. The
        // emit itself is fast-path-guarded by `if (!hf_probe_active) return;`.
        hf_probe_emit_poly_destroy(reinterpret_cast<uintptr_t>(this));
        if (ctx_) fmpq_mpoly_clear(raw_, ctx_->raw());
    }

    // Copy = explicit clone (FLINT types are by-value, but we want to
    // make copies explicit so performance-sensitive code is legible).
    Poly(const Poly& other) : Poly(*other.ctx_) {
        fmpq_mpoly_set(raw_, other.raw_, ctx_->raw());
    }
    Poly& operator=(const Poly& other) {
        if (this != &other) {
            // ctx must match; if not, rebuild
            if (ctx_ != other.ctx_) {
                if (ctx_) fmpq_mpoly_clear(raw_, ctx_->raw());
                ctx_ = other.ctx_;
                fmpq_mpoly_init(raw_, ctx_->raw());
            }
            fmpq_mpoly_set(raw_, other.raw_, ctx_->raw());
        }
        return *this;
    }

    Poly(Poly&& other) noexcept : ctx_(other.ctx_) {
        // Move by swapping into a freshly-init'd raw_: there's no
        // zero-cost "move" for fmpq_mpoly_t, so we init + swap.
        fmpq_mpoly_init(raw_, ctx_->raw());
        fmpq_mpoly_swap(raw_, other.raw_, ctx_->raw());
    }

    // 2026-04-27 (3l3pt step-3 67 s C++ residual fix): explicit
    // move-assignment.  Without this, the user-declared dtor + copy
    // ctor + copy assign suppress the implicit move-assign, so a
    // bare `p = std::move(q)` falls back to copy-assign, which calls
    // `fmpq_mpoly_set` (full deep copy) instead of `fmpq_mpoly_swap`
    // (cheap pointer swap).  Empirically: the 67 s "dark mass" in
    // 3l3pt step-3's `bump_addto_s` lives in the 1700 deep copies
    // performed by `rows[i].coef = rows[i].coef + c`.  Defining
    // this assigns the rvalue's pointers in via swap, leaving the
    // moved-from poly in a valid (zero) state for its dtor.
    Poly& operator=(Poly&& other) noexcept {
        if (this == &other) return *this;
        if (ctx_ != other.ctx_) {
            if (ctx_) fmpq_mpoly_clear(raw_, ctx_->raw());
            ctx_ = other.ctx_;
            fmpq_mpoly_init(raw_, ctx_->raw());
        }
        fmpq_mpoly_swap(raw_, other.raw_, ctx_->raw());
        return *this;
    }

    // Convert to canonical string using the ctx's variable names.
    std::string to_string() const {
        char* s = fmpq_mpoly_get_str_pretty(raw_, ctx_->cvars(), ctx_->raw());
        std::string out(s);
        flint_free(s);
        return out;
    }

    // Raw FLINT handle for inner-loop code. fmpq_mpoly_t is an array
    // typedef, so `raw_` already decays to `fmpq_mpoly_struct*`.
    fmpq_mpoly_struct* raw() { return raw_; }
    const fmpq_mpoly_struct* raw() const { return raw_; }
    const PolyCtx& ctx() const { return *ctx_; }

    // -------- Basic props --------
    bool is_zero() const;
    bool is_one() const;
    bool equal(const Poly& other) const;

    // True iff the leading-monomial coefficient is negative. Used by
    // Rat's sign canonicalizer as a fast path around den.to_string().
    // Zero polynomials are reported as non-negative.
    bool leading_coef_is_negative() const;

    // List of variable indices (in *this polynomial's own ctx) that
    // appear with a positive exponent somewhere in the polynomial.
    // O(nterms · nvars). Used to pick a narrow ctx when factoring.
    std::vector<size_t> used_var_indices() const;

    // Transplant this polynomial into `dst_ctx`. `src_to_dst_idx[i]`
    // gives the index in dst_ctx corresponding to src's variable `i`;
    // SIZE_MAX means "not mapped" (error if that variable appears in
    // this poly with positive exponent). Monomial coefficients are
    // copied as-is; the variable mapping just reshuffles exponent
    // slots. Constant polynomials are trivially transplanted.
    //
    // `skip_precheck`: when true, omits the
    // `fmpq_mpoly_used_vars`-based precheck that guards against an
    // unmapped src variable being used. The caller is then responsible
    // for ensuring the mapping is complete; if it isn't, the transplant
    // silently drops affected exponents. Used by `Rat::reduce_inplace`,
    // which builds the mapping from its own `fmpq_mpoly_used_vars` scan
    // and so has already validated it. Default false (precheck on)
    // preserves the safe behaviour for unaudited callers. Measurement:
    // skipping the precheck on `reduce_inplace`'s 4 transplants per
    // call cuts tst2 wall by ~12 % on Smirnov fixtures (the wide-ctx
    // precheck scan was a hidden ~330 s aggregate cost on tst2,
    // dropping to ~140 s with the flag enabled). See
    // `notes/hf_flint_pool_experiment/transplant_levers.md`.
    Poly transplant(const PolyCtx& dst_ctx,
                    const std::vector<size_t>& src_to_dst_idx,
                    bool skip_precheck = false) const;

    // -------- Phase 1a arithmetic --------
    // Binary ops return a fresh Poly owning its own fmpq_mpoly_t.
    // Operands must share the same PolyCtx (enforced by assertion).
    Poly add(const Poly& b) const;
    Poly sub(const Poly& b) const;
    Poly mul(const Poly& b) const;
    Poly neg() const;
    Poly pow(unsigned long n) const;

    // Operator sugar for the common cases.
    Poly operator+(const Poly& b) const { return add(b); }
    Poly operator-(const Poly& b) const { return sub(b); }
    Poly operator*(const Poly& b) const { return mul(b); }
    Poly operator-() const { return neg(); }

    // -------- Phase 1b calculus --------
    // Derivative with respect to ctx's variable at index `var_idx`.
    Poly derivative(size_t var_idx) const;

    // Evaluate all variables at rational points; returns the rational
    // constant as a decimal/fraction string (delegated to fmpq).
    // `values` must match the ctx's variable count, each formatted as
    // a fmpq-parseable string ("3", "1/2", "-5/7", etc.).
    std::string evaluate_all(const std::vector<std::string>& values) const;

    // Substitute a single variable with a rational value, returning
    // a Poly in the same context (the substituted variable's slot
    // becomes effectively unused -- we don't rewrite the ctx).
    Poly substitute_one_rat(size_t var_idx,
                            const std::string& value) const;

    // -------- Phase 1c division + GCD --------
    // Exact division: throws if !divides(b). FLINT's fmpq_mpoly is
    // automatically reduced, so this boils down to checking
    // fmpq_mpoly_divides.
    Poly divexact(const Poly& b) const;

    // Test whether *this divides b, i.e. exists q with b = q * (*this).
    // NOTE argument order: a.divides(b) means a | b — the MIRROR of
    // divexact, where a.divexact(b) computes a / b. (This comment used to
    // state the opposite of the implementation; see poly.cpp:769.)
    bool divides(const Poly& b) const;

    // Euclidean-style quotient + remainder. Returns {q, r} such that
    // *this = q*b + r with lt(r) < lt(b) under FLINT's monomial order.
    std::pair<Poly, Poly> divrem(const Poly& b) const;

    // Multivariate GCD (monic, leading-coefficient normalized).
    Poly gcd(const Poly& b) const;

    // Resultant with respect to variable `var_idx`. Both polynomials
    // must be nontrivial in `var_idx`. The result lives in the same
    // PolyCtx but is free of `var_idx`.
    Poly resultant(const Poly& b, size_t var_idx) const;

    // Discriminant with respect to `var_idx`, matching Mathematica's
    // `Discriminant[p, x]` (non-numeric factor content) up to a leading
    // integer sign `(-1)^{n(n-1)/2}` (Gelfand-Kapranov-Zelevinsky,
    // Discriminants, Resultants and Multidimensional Determinants,
    // Ch. 12, Def. 1.29).  Formula: for degree n >= 1 in var_idx,
    //     disc = Resultant(p, dp/dvar, var) / lc(p, var)
    // where lc(p, var) = coefficient of var_idx^n in p.
    //
    // For deg < 1 we return `1` regardless of what Mma does there (Mma
    // returns `1/const^{2n-2}` or raises on 0 — irrelevant for LR-search,
    // since a polynomial with no root in `var` contributes no letters).
    //
    // Why not `Resultant(p, dp/dvar, var)` directly? When lc(p, var)
    // is polynomial in the OTHER variables (always true for Symanzik
    // F-polynomials), it contributes a non-numeric factor that Mma's
    // `Discriminant` does not produce — injecting spurious polys into
    // LR-search dedup.  Verified on the 1-mass double-box F-polynomial
    // 2026-04-24 during Phase β.1.  Always divide.
    Poly discriminant_in_var(size_t var_idx) const;

    // -------- Phase 2 structural helpers --------
    // Degree in a single variable (the target variable, `var_idx`).
    // Returns -1 if the polynomial is zero (convention: deg(0) = -inf,
    // we return -1 as the C-idiomatic sentinel).
    long degree_in_var(size_t var_idx) const;

    // Lowest exponent of `var_idx` that appears in any monomial of
    // *this.  Useful for Laurent-order analysis.  Returns LONG_MAX if
    // the polynomial is zero.
    long min_exponent_in_var(size_t var_idx) const;

    // Coefficient polynomial of var_idx^exp.  The result lives in the
    // same PolyCtx but is independent of var_idx (its `var_idx`-
    // exponent is 0 in every monomial).
    Poly coefficient_of_var(size_t var_idx, long exp) const;

    // True iff this polynomial has no variable dependence -- i.e., it
    // is a pure rational constant.
    bool is_fmpq() const;

    // Scalar-divide every coefficient by the constant `c`.  Requires
    // `c` to be a nonzero fmpq_mpoly constant (use is_fmpq() first).
    // Mutates *this in place.
    void scalar_div_fmpq_const(const Poly& c);

    // Phase β.2 proportionality dedup: return the unique representative
    // of the equivalence class [p] under (p ~ q iff p = c*q for c ∈ ℚ*).
    // Divides by the fmpq coefficient of the leading monomial so that
    // two polynomials proportional over ℚ map to structurally identical
    // FLINT representations.  Analogous to Phase-α.2's stCanonicalPolyForm.
    // For zero/numeric inputs returns zero/one (sign of the constant).
    Poly canonical_prop_form() const;

    // 2026-05-05 (path-A diagnostic). Heap byte estimate for *this Poly.
    // Walks fmpq_mpoly internals; never calls fmpq_mpoly_to_string.
    // Accounts for: per-poly content fmpq (num+den fmpz bodies);
    // zpoly exponent array `alloc * words_per_exp_sp(bits, mctx) * 8`;
    // zpoly coeffs[] handle array `alloc * sizeof(fmpz)`; per-coefficient
    // large-fmpz body bytes via `fmpz_size`. Skips one-time PolyCtx overhead.
    // Used by the diagnostic probe walkers; default-OFF env-gated.
    size_t total_bytes() const;

    // Number of monomial terms (fmpq_mpoly_length). Added for the
    // HF_ADDPF_PROBE attribution probe (additive-parfrac Phase 0,
    // 2026-06-03); there was no public term-count accessor before.
    size_t n_terms() const;

    // 2026-05-05 (path-A diagnostic — Probe 4 6-bucket decomposition).
    // Same arithmetic as total_bytes() but split into the six attribution
    // buckets defined in
    // notes/hf_mzv_scalar_rewrite_2026-05-05/diagnostic_design.md §"Probe 4":
    //   coeff_intrinsic, exp_live, exp_slack, handle_live, handle_slack,
    //   content_fmpq.
    // Sum of all six = total_bytes(). Attribution: structural FLINT overhead
    // = exp_live + handle_live; alloc-vs-length slack = exp_slack +
    // handle_slack; intrinsic data = coeff_intrinsic + content_fmpq.
    struct PolyByteBuckets {
        size_t coeff_intrinsic = 0;
        size_t exp_live        = 0;
        size_t exp_slack       = 0;
        size_t handle_live     = 0;
        size_t handle_slack    = 0;
        size_t content_fmpq    = 0;
        size_t total() const {
            return coeff_intrinsic + exp_live + exp_slack
                 + handle_live + handle_slack + content_fmpq;
        }
    };
    PolyByteBuckets total_bytes_buckets() const;

private:
    const PolyCtx* ctx_ = nullptr;
    mutable fmpq_mpoly_t raw_;
};

// Factored form: a constant + list of {poly, exponent}.
struct Factored {
    std::string constant;             // leading rational, as string
    std::vector<std::pair<std::string, long>> factors;  // (base_str, exp)
};

Factored factor(const Poly& p);

// Factor-bases-as-Polys: identical logical factorization as `factor(p)`
// but returns each factor as a Poly in the SAME PolyCtx (skipping the
// string round-trip that forces `Rat::parse` on each use).  Omits the
// leading rational constant and multiplicity (one entry per distinct
// irreducible factor).  Matches Mma's `FactorList[p, Modulus -> 0][[All, 1]]`
// minus the leading numeric factor.  Empty return for numeric p.
std::vector<Poly> factor_bases(const Poly& p);

// 2026-04-27 Avenue A: per-thread narrow-vs-wide path counters and
// timers for the narrow-ctx hoist in Poly::mul.  Mirrors the existing
// pattern for `reduce_inplace`.  Wired into integration_step's step
// harvest so HF_STEP_TRACE emits per-step:
//   mul_narrow_calls / mul_narrow_s
//   mul_wide_calls   / mul_wide_s
//   mul_gated_calls  (size_gate fired — narrow path skipped because
//                     len_total < kNarrowMulMinLen)
void   init_mul_per_thread(int n_threads);
void   reset_mul_per_thread();
double sum_mul_narrow_s_per_thread();
double sum_mul_wide_s_per_thread();
long   sum_mul_narrow_calls_per_thread();
long   sum_mul_wide_calls_per_thread();
long   sum_mul_gated_calls_per_thread();

// Reviewer round 7 distribution counters.
// la·lb log bins:  <1k, <4k, <16k, <64k, <256k, ≥256k  (6 bins)
// U bins:          1, 2, 3, 4-7, 8+                     (5 bins)
std::vector<long>   sum_nbin_lalb_count_per_thread();
std::vector<double> sum_nbin_lalb_us_per_thread();
std::vector<double> sum_nbin_lalb_max_per_thread();
std::vector<long>   sum_nbin_u_count_per_thread();
std::vector<double> sum_nbin_u_us_per_thread();

}  // namespace hyperflint
