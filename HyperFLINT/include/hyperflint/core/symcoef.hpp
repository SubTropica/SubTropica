// Phase 6d-v-i: SymCoef — symbolic-coefficient sidecar for BreakUpContour.
//
// After the positive-letter recursion, RegTerm coefficients are no
// longer pure Rats: they carry literal Pi, I, Log[n], and delta[var]
// factors that HyperIntica produces from `Pi*I*imPart - Log[smallest]`
// and its polynomial descendants. Adding those symbols as PolyCtx
// variables would force every Rat in the pipeline to grow its arity;
// SymCoef is the sidecar alternative.
//
// Representation:
//   SymCoef = sum_i  prefactor_i · Pi^{p_i} · I^{e_i}
//                     · prod_n Log[n]^{ℓ_{i,n}}
//                     · prod_v delta[v]^{d_{i,v}}
// where prefactor_i is a Rat (i.e. an element of Q(x_1,...,x_k))
// — the same ambient Rat that holds MZV basis variables after
// apply_mzv_reductions.
//
// Canonical form (applied after every op):
//   * i_power reduced to {0, 1} modulo I^2 = -1, sign folded into prefactor.
//   * delta[v] is a sign indicator (±1, per HyperIntica.wl:59), so
//     delta_powers[v] is reduced modulo 2 — even powers drop out, odd
//     powers collapse to 1. Every canonical SymMonomial satisfies
//     delta_powers[v] ∈ {0, 1} for every v (0 entries erased).
//   * Like monomials (same p, e, log, delta exponents) collected.
//   * Monomials with zero prefactor dropped.
//
// `reduce_to_rat(s, table)` attempts to absorb Pi^(2k) -> (6 mzv_2)^k
// and unwrap the sidecar into a plain Rat. It throws if any Log[n],
// delta[v], odd Pi power, or residual I power remains.

#pragma once

#include "hyperflint/core/rat.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hyperflint {

// Forward-declare the MZV reduction table; reduce_to_rat needs it but
// pulling the full header here would force every translation unit that
// uses SymCoef to depend on the reduction machinery.
struct MzvReductionTable;

struct SymMonomial {
    Rat                        prefactor;
    int                        pi_power = 0;
    int                        i_power  = 0;   // canonical: 0 or 1
    std::map<long, int>        log_powers;     // Log[n]^k indexed by n > 0
    std::map<std::string, int> delta_powers;   // delta[var]^k indexed by var
    // Period-tuples Phase 1 (spec 2026-06-04): opaque period generators
    // (PeriodTable ids) with integer exponents -- structural home for
    // transcendental constants, parallel to log_powers. Free commutative
    // monoid: products = exponent addition; NO reduction here (lazy at
    // the boundary, Phase 3).
    std::map<std::uint32_t, int> period_powers;

    explicit SymMonomial(Rat pref) : prefactor(std::move(pref)) {}

    // Stable string key of the symbolic powers (excludes prefactor) —
    // suitable for the final std::sort comparator in canonicalize()
    // (preserves polesInf.terms ordering for downstream bit-identity).
    std::string power_key() const;

    // 2026-04-30 (Tier 1.4b): 128-bit FNV-1a structural hash over
    // (pi_power, i_power, sorted log_powers entries, sorted
    // delta_powers entries). Used as the equality key in canonicalize's
    // like-term coalesce map — replaces the per-monomial
    // `power_key()` ostringstream + unordered_map<string,…> lookup
    // with a u128 hash + integer compare. Same collision-risk model as
    // poly_struct_hash (~1.5e-27 at 1e5 keys).
    std::pair<uint64_t, uint64_t> power_hash() const;

    // True iff every symbolic power is zero — the monomial is a plain Rat.
    bool is_pure_rat() const;

    // Human-readable single-monomial string.
    std::string to_string() const;
};

class SymCoef {
public:
    // Zero (empty monomial list) in ambient PolyCtx `ctx`.
    explicit SymCoef(const PolyCtx& ctx) : ctx_(&ctx) {
        // HF FF Phase 5 §A.1 iter-50: value_create emit at SymCoef ctor.
        // MVP scope (per design.md §3 and handoff iter-50-β option B):
        // empty-init hash = kFnv1a64OffsetBasis, n_terms=0, payload_bytes_est=0.
        // Post-mutation events (from_rat / add / mul / canonicalize) are NOT
        // emitted at iter-50; aggregate.py treats the ctor-only events as a
        // lower bound on SymCoef-layer dedup and notes the uncertainty in
        // verdict.md per design.md §6.5.
        hf_probe_emit_symcoef_create(reinterpret_cast<uintptr_t>(this),
                                     kFnv1a64OffsetBasis, 0, 0);
    }

    // HF FF Phase 5 §A.1 iter-50: explicit destructor (value_destroy emit).
    // Body is no-op-equivalent to the implicit version.
    ~SymCoef() {
        hf_probe_emit_symcoef_destroy(reinterpret_cast<uintptr_t>(this));
    }
    SymCoef(const SymCoef& other)
        : ctx_(other.ctx_), terms_(other.terms_) {
        hf_probe_emit_symcoef_create(reinterpret_cast<uintptr_t>(this),
                                     kFnv1a64OffsetBasis,
                                     (uint64_t)terms_.size(), 0);
    }
    SymCoef& operator=(const SymCoef& other) {
        if (this != &other) {
            ctx_   = other.ctx_;
            terms_ = other.terms_;
        }
        return *this;
    }
    SymCoef(SymCoef&& other) noexcept
        : ctx_(other.ctx_), terms_(std::move(other.terms_)) {
        hf_probe_emit_symcoef_create(reinterpret_cast<uintptr_t>(this),
                                     kFnv1a64OffsetBasis,
                                     (uint64_t)terms_.size(), 0);
    }
    SymCoef& operator=(SymCoef&& other) noexcept {
        if (this != &other) {
            ctx_   = other.ctx_;
            terms_ = std::move(other.terms_);
        }
        return *this;
    }

    // Promote a Rat. Single monomial, prefactor = r, all powers = 0.
    static SymCoef from_rat(const Rat& r);

    // Named transcendentals. The prefactor is the constant 1 over `ctx`.
    static SymCoef pi_factor     (const PolyCtx& ctx);
    static SymCoef im_factor     (const PolyCtx& ctx);           // I
    static SymCoef log_factor    (const PolyCtx& ctx, long n);   // Log[n], n > 0
    static SymCoef delta_factor  (const PolyCtx& ctx,
                                  const std::string& var_name);

    const PolyCtx& ctx()   const { return *ctx_; }
    const std::vector<SymMonomial>& terms() const { return terms_; }

    // 2026-05-05 (path-A diagnostic). Sums total_bytes across all
    // monomial prefactors. Skips the SymMonomial map overheads (small,
    // bounded by per-monomial transcendental factor count).
    size_t total_bytes() const {
        size_t b = 0;
        for (const auto& m : terms_) b += m.prefactor.total_bytes();
        return b;
    }

    bool is_zero() const { return terms_.empty(); }
    // Reducible to a plain Rat: single monomial with all symbolic
    // powers zero (or zero SymCoef).
    bool is_rat()  const;
    // Unwrap to the underlying Rat. Requires is_rat() == true.
    Rat  as_rat()  const;

    // --- Arithmetic (all auto-canonicalize) ---
    SymCoef add    (const SymCoef& o) const;
    SymCoef sub    (const SymCoef& o) const;
    SymCoef neg    ()                 const;
    SymCoef mul    (const SymCoef& o) const;
    SymCoef mul_rat(const Rat& r)     const;
    SymCoef div_rat(const Rat& r)     const;     // throws on r == 0

    SymCoef operator+(const SymCoef& o) const { return add(o); }
    SymCoef operator-(const SymCoef& o) const { return sub(o); }
    SymCoef operator*(const SymCoef& o) const { return mul(o); }
    SymCoef operator-()                 const { return neg(); }

    // 2026-04-30 (axis-E in-place Rat-style += for SymCoef): avoids
    // the LHS deep-copy of `terms_` that operator+ pays. Hot site is
    // bucket_bump's collision branch (integration_step.cpp:610) which
    // hammered `terms[i].coef = terms[i].coef + coef` (264 CPU-s on
    // step 6 of parity-1 ord_1_face_1).
    SymCoef& operator+=(const SymCoef& o);

    // 2026-04-30 (Tier 1.6a): linear merge of two ALREADY-canonical
    // SymCoefs. Both operands MUST satisfy the canonicalize() output
    // invariant — sorted by `power_key()` strict-less ordering, like
    // terms collapsed (each `power_key` appears at most once),
    // `delta_powers[v] ∈ {0,1}`, `i_power ∈ {0,1}`, no zero
    // prefactors. The result satisfies the same invariant.
    //
    // Two-iterator gather-cluster-then-emit: pop heads, when keys
    // equal, sum prefactors; when sum is zero, drop. O(N+M) time, no
    // hash table, no string rebuild beyond the per-step comparator
    // peek. Replaces the `append + canonicalize from scratch` cost
    // when both operands are pre-canonical (e.g. inside operator+=
    // at the bucket-collision call site, where LHS is a running
    // canonical accumulator and RHS is a freshly-built SymCoef).
    //
    // Falls back to slow `add()` if either operand fails a quick
    // sortedness self-check; the fall-back is intended only as a
    // safety net for future regressions in the canonical-output
    // invariant — the live hot path should never hit it.
    static SymCoef merge_sorted_canonical(const SymCoef& a,
                                          const SymCoef& b);

    // 2026-05-10 (Phase 4 §B.1 iter-24b Patches G+H — rvalue overload):
    // consumes both operands. When `a` or `b` is empty, moves the
    // non-empty side's `terms_` directly into the output (no
    // monomial-by-monomial copies). When both are non-empty, moves each
    // SymMonomial out of A/B into the output during the merge pass
    // (Rat prefactor move = no fmpq_mpoly_set deep-copy). After return,
    // `a` and `b` are in moved-from state (terms_ vectors empty,
    // ctx_ pointer preserved).
    //
    // Same algorithm and invariants as the const overload; same
    // single-equality-pair gather logic at duplicate keys; same
    // fallback (returns slow-add result on sortedness mismatch is
    // NOT performed here — both overloads keep the merge-only path,
    // the const overload's fallback was only ever a forward-looking
    // safety net for future regressions and the rvalue overload
    // preserves that posture).
    static SymCoef merge_sorted_canonical(SymCoef&& a, SymCoef&& b);

    // 2026-04-30 (Tier 1.6b): pairwise tree-reduce of K already-
    // canonical SymCoefs. log2(K) levels of independent pairwise
    // merges; each level halves the number of operands. Asymptotic
    // total work is the same as merge_sorted_canonical applied K-1
    // times by left-fold, but the tree shape keeps intermediate sizes
    // balanced — important when canonicalize/merge cost is
    // super-linear in input size (FLINT mpoly add).
    //
    // The for-loop level pairings here are SERIAL within a slot;
    // call sites that want cross-slot parallelism wrap this in a
    // parallel-for over slots. Caller passes ownership.
    static SymCoef tree_merge(std::vector<SymCoef>&& chunks);

    // Collect like monomials, sort, drop zero prefactors. Idempotent.
    // 2026-05-10 (iter-24b): now `const &`-qualified to coexist with
    // the rvalue overload below. The signature change is silent at
    // every call-site that previously called `x.canonicalize()` —
    // the const-lref overload absorbs all const-ref, mutable-lvalue,
    // and const-lvalue callers; the rvalue overload only binds when
    // the receiver is `std::move(x)` or a prvalue.
    SymCoef canonicalize() const &;

    // 2026-05-10 (Phase 4 §B.1 iter-24b Patches G+H — rvalue overload):
    // consumes *this. Moves `terms_` out at entry, then operates on
    // the local vector with std::move on each SymMonomial during
    // normalization, like-term collection, and sort emit. After
    // return, *this is in moved-from state (terms_ empty, ctx_
    // pointer preserved). Same algorithm + same canonical-output
    // invariant as the const overload.
    SymCoef canonicalize() &&;

    // Deterministic single-line string. Ordering matches canonicalize().
    std::string to_string() const;

    // Build from a raw monomial list and canonicalize. Used by the
    // SymCoef-coef integration_step overload to rebuild individual
    // monomials with a Rat prefactor stripped to 1, so the prefactor
    // can scale the underlying Wordlist while the symbolic factors
    // ride through as an outer scalar on the RegulatorSym output.
    static SymCoef from_monomials(const PolyCtx& ctx,
                                   std::vector<SymMonomial> ms);

    // HF MZV-rewrite Phase A commit (2): round-trip every prefactor
    // Rat through `split_rat_by_w_monomial` / `recombine_rat_split`
    // and assert string-equality. Returns true when every monomial's
    // prefactor round-trips identically; throws / returns false on
    // any divergence (the body in symcoef.cpp keeps the cheap path
    // out of the fast loop — callers gate this behind the
    // `HF_RAT_SPLIT_VERIFY=1` env var, which commit (3) wires).
    //
    // The narrow ctx `N` and the wide ctx `F = ctx()` must satisfy
    // the FNIndexMaps invariant: every variable of N is a name-prefix
    // of F. The verifier owns the ZWTable instance for the duration
    // of the call (no shared state across SymCoefs in this Phase-A
    // path; commit (3) keeps the per-call newing as the documented
    // overhead path).
    bool verify_rat_split_roundtrip(const PolyCtx& N) const;

private:
    const PolyCtx*            ctx_;
    std::vector<SymMonomial>  terms_;
};

// Apply Pi^(2k) -> (6 mzv_2)^k using `table`'s basis names, then
// attempt to unwrap to a plain Rat. Throws std::runtime_error if any
// Log[n] or delta[v] power remains, if i_power is nonzero, or if the
// residual Pi power is odd.
Rat reduce_to_rat(const SymCoef& s, const MzvReductionTable& table);

// Apply Pi^(2k) -> (6 mzv_2)^k; return the simplified SymCoef (may
// still carry Log/delta/odd-Pi/I). A lossless form useful for further
// manipulation before the final reduce_to_rat.
SymCoef simplify_symcoef(const SymCoef& s, const MzvReductionTable& table);

}  // namespace hyperflint
