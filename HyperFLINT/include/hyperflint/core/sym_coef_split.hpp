// SymCoefSplit: SymCoef-shaped container over the (num_N, num_zw,
// den_zw) split representation introduced by the HF MZV-as-scalar
// rewrite. Phase-A commit (5) per design v2 §3.4
// (notes/hf_mzv_rewrite_design_2026-05-05/design.md).
//
// SymCoefSplit is the analogue of SymCoef in the new representation.
// Its terms_ vector holds SymMonomialSplit entries (defined in
// core/rat_split.hpp); each entry's prefactor "(num_N · num_zw) /
// den_zw" is the factored form of the corresponding wide-ctx Rat
// in today's SymCoef.
//
// Phase-A scope: basic ops (add, neg, mul, mul_rat, canonicalize,
// from_rat, as_rat) implemented and round-trip tested against the
// existing SymCoef. Phase-B commits switch hot paths from SymCoef
// to SymCoefSplit; the round-trip serves as the bit-identity gate.

#pragma once

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/rat_split.hpp"
#include "hyperflint/core/symcoef.hpp"
#include "hyperflint/core/zw_table.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace hyperflint {

class SymCoefSplit {
public:
    SymCoefSplit(const PolyCtx& F,
                 const PolyCtx& N,
                 std::shared_ptr<ZWTable> zw_table)
        : wide_ctx_(&F),
          narrow_ctx_(&N),
          zw_table_(std::move(zw_table)) {
        // Owner is not allowed to pass a null table — it would make
        // every call to add / mul a null deref.
        if (!zw_table_) {
            throw std::runtime_error(
                "SymCoefSplit: zw_table must be non-null");
        }
    }

    const PolyCtx& wide_ctx()   const { return *wide_ctx_; }
    const PolyCtx& narrow_ctx() const { return *narrow_ctx_; }
    const ZWTable& zw_table()   const { return *zw_table_; }
    ZWTable&       zw_table()         { return *zw_table_; }

    const std::vector<SymMonomialSplit>& terms() const { return terms_; }

    bool is_zero() const { return terms_.empty(); }

    // Convert from a SymCoef (the wide-ctx representation). Splits
    // every SymMonomial's prefactor Rat per `split_rat_by_w_monomial`
    // and copies the transcendental flags onto each split leaf.
    //
    // The narrow ctx N must satisfy `build_fn_index_maps(F, N)`'s
    // contract: every variable of N is a variable of F. Variables in
    // F not present in N are W-side.
    static SymCoefSplit from_rat(const SymCoef& src,
                                 const PolyCtx& N,
                                 std::shared_ptr<ZWTable> zw_table);

    // Reconstitute back into a SymCoef. Bit-identical (canonical
    // form) to the source SymCoef under the round-trip invariant.
    SymCoef as_rat() const;

    // -------- Arithmetic (all auto-canonicalize) --------
    SymCoefSplit add(const SymCoefSplit& o) const;
    SymCoefSplit neg() const;
    SymCoefSplit sub(const SymCoefSplit& o) const { return add(o.neg()); }
    SymCoefSplit mul_rat(const Rat& r) const;

    // SymCoefSplit × SymCoefSplit (B1.a, design v2 §3.4 + scoping memo
    // notes/hf_mzv_rewrite_design_2026-05-05/b1_scoping_memo.md). The
    // product runs over the cartesian product of `terms_`. Per pair:
    //   num_N'  = a.num_N · b.num_N      (narrow-ctx Poly mul)
    //   num_zw' = zw_table.multiply(a.num_zw, b.num_zw)
    //   den_zw' = zw_table.multiply(a.den_zw, b.den_zw)
    //   pi'     = a.pi + b.pi
    //   i'      = (a.i + b.i) mod 2;  if (a.i + b.i) carries the I^2
    //             bit (==2), the sign is folded into num_N via neg().
    //   log'    = entrywise sum (zero entries dropped).
    //   delta'  = entrywise sum mod 2; zeros dropped.
    // Both operands MUST share the same wide_ctx, narrow_ctx, and
    // ZWTable instance (shared_ptr equality), as for `add`.
    //
    // Input invariant assumed: each operand's terms_ is in canonical
    // form (`canonicalize()` output), so `i_power ∈ {0,1}` and every
    // `delta_powers[v] ∈ {0,1}` on input. Under this invariant the
    // i_power sum is in {0,1,2} and the per-key delta sum is in
    // {0,1,2}, so the inline reduction stays branch-cheap.
    //
    // Output is canonicalize()d.
    SymCoefSplit mul(const SymCoefSplit& o) const;

    // Canonical-form equality predicate (B1.a). Canonicalizes both
    // sides and compares term-by-term. Two terms are equal iff their
    // SplitKey (pi, i, log, delta, num_zw, den_zw) matches AND their
    // num_N polynomials are FLINT-equal (`Poly::equal`).
    //
    // Returns false on any wide_ctx / narrow_ctx / ZWTable mismatch
    // (rather than throwing — the verifier site at the as_rat()
    // boundary in transform.cpp B1.c will treat ctx mismatch as a
    // round-trip failure rather than a crash). Note: ZWHandle equality
    // requires same ZWTable instance, since handles are scoped to
    // a single table.
    bool equals_canonical(const SymCoefSplit& o) const;

    // Phase-A canonicalize: collect like-monomial-splits by
    // (pi_power, i_power, log_powers, delta_powers, num_zw, den_zw)
    // — when equal, sum num_N over the shared narrow ctx. Drop
    // zero-num leaves. Sort by the same key for stable output order.
    SymCoefSplit canonicalize() const;

private:
    // Append `s` to terms_ AS-IS (no canonicalization). Used by the
    // adapter and the arithmetic constructors.
    void push_term(SymMonomialSplit s) {
        terms_.push_back(std::move(s));
    }

    const PolyCtx*                    wide_ctx_;
    const PolyCtx*                    narrow_ctx_;
    std::shared_ptr<ZWTable>          zw_table_;
    std::vector<SymMonomialSplit>     terms_;
};

}  // namespace hyperflint
