// FactoredRat: a rational function held as numerator / prod_i(base_i^exp_i),
// with the denominator kept FACTORED so multiply/power/divide never expand it.
// Materializes to a normal (reduced) Rat exactly once via Rat(num, den).
//
// Value invariant: value == numerator_ / prod_i(base_i^exp_i).
// No invariant that bases are irreducible, sign-canonical, or content-free;
// correctness is restored by the reducing Rat ctor at materialize time.
#pragma once

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"

#include <utility>
#include <vector>

namespace hyperflint {

class FactoredRat {
public:
    struct Factor {
        Poly base;
        long exp;   // strictly positive
    };
    // Factors are de-duplicated by Poly::equal() (there is no
    // Poly::struct_hash); sets are tiny, so a linear scan suffices and
    // den_factors_ stays in first-insertion order.

    // Builders.
    static FactoredRat from_poly(Poly numerator);           // den = {}
    static FactoredRat from_rat(const Rat& r);              // den = {(r.den(),1)}

    // Operations (value-preserving; keep denominator factored).
    FactoredRat mul(const FactoredRat& b) const;
    FactoredRat pow(long n) const;                          // n >= 0 in B0; n<0 via reciprocal
    FactoredRat reciprocal() const;
    FactoredRat div(const FactoredRat& b) const;
    FactoredRat add(const FactoredRat& b) const;
    FactoredRat sub(const FactoredRat& b) const;
    FactoredRat neg() const;
    bool is_zero() const;

    // Derivative w.r.t. variable `var_idx`, keeping the denominator factored.
    // For value = N / prod_i f_i^{e_i}, the quotient rule gives
    //   d/dx (N/D) = [ N' * prod_i f_i
    //                  - N * sum_i ( e_i * f_i' * prod_{j!=i} f_j ) ]
    //                / prod_i f_i^{e_i + 1}
    // so each old factor's exponent is bumped by one. Fast path: if every
    // factor is independent of `var_idx`, the result is N'/D with the SAME
    // factors (no exponent bump). Empty denominator: just N'.
    FactoredRat derivative(size_t var_idx) const;

    // The single materialization boundary.
    Rat materialize_to_rat() const;

    // Peel known denominator-base powers off the numerator by EXACT
    // division: while a base divides the numerator and its exponent is
    // positive, divide it out and decrement the exponent. Value-preserving
    // by construction (each step is an exact division applied to both
    // numerator and the factored denominator). Cheap heap divides versus
    // the monolithic Brown GCD the reducing Rat ctor would otherwise pay;
    // applied between derivative-chain steps (B1.3b) so chain numerators
    // do not swell with removable base powers. No-op below `min_terms`
    // numerator terms (failed divides() attempts are pure overhead there).
    // Shared gate for the peel lever: numerators below this term count skip
    // peeling (failed divides() attempts are pure overhead there). Single
    // source of truth for BOTH the materialize_to_rat gate and the default
    // argument below (advisory A3, review of 5f62abe84).
    static constexpr long kPeelMinTerms = 64;

    void peel_known_factors(long min_terms = kPeelMinTerms);

    // True when HF_FR_MAT_PEEL is set (non-empty, not "0"): callers gate
    // peel_known_factors() invocations on this so the flag stays the single
    // switch for the whole peel lever (materialize + derivative chain).
    static bool peel_enabled();

    const PolyCtx& ctx() const { return numerator_.ctx(); }
    const Poly& numerator() const { return numerator_; }
    const std::vector<Factor>& den_factors() const { return den_factors_; }

private:
    explicit FactoredRat(Poly numerator) : numerator_(std::move(numerator)) {}

    // Multiply one (base, exp) into den_factors_, merging by struct-hash.
    void push_factor(const Poly& base, long exp);
    // Expand the denominator product to a single Poly (used only at materialize
    // and inside reciprocal). Small in practice: factors are letters.
    Poly expand_denominator() const;

    Poly numerator_;
    std::vector<Factor> den_factors_;  // de-duped by Poly::equal
};

}  // namespace hyperflint
