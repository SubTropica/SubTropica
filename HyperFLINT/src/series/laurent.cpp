// Phase 5e-i: Laurent series_expansion — mirrors HyperIntica.wl:3405.

#include "hyperflint/series/laurent.hpp"

#include "hyperflint/core/poly.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace hyperflint {

Rat series_expansion(const PolyCtx& ctx,
                     const Rat& f,
                     size_t var_idx,
                     long max_order) {
    if (f.is_zero()) return Rat{Poly::zero_of(ctx)};

    // Pole degree = min-exp(num, var) - min-exp(den, var).
    long nmin = f.num().min_exponent_in_var(var_idx);
    long dmin = f.den().min_exponent_in_var(var_idx);
    long pole_deg = nmin - dmin;

    // If the leading Laurent term already exceeds max_order, truncation
    // leaves nothing.
    if (pole_deg > max_order) return Rat{Poly::zero_of(ctx)};

    // Number of Taylor coefficients c_0..c_M needed.
    long M = max_order - pole_deg;

    // Extract p'_j = [var^(nmin+j)] num    (as Poly in the other vars)
    //         q'_j = [var^(dmin+j)] den.
    // Both wrapped as Rats so we can divide by q'_0 below.
    std::vector<Rat> p_prime;
    std::vector<Rat> q_prime;
    p_prime.reserve(static_cast<size_t>(M + 1));
    q_prime.reserve(static_cast<size_t>(M + 1));
    for (long j = 0; j <= M; ++j) {
        p_prime.emplace_back(f.num().coefficient_of_var(var_idx, nmin + j));
        q_prime.emplace_back(f.den().coefficient_of_var(var_idx, dmin + j));
    }

    // Cauchy-product recurrence: q'_0 is nonzero by construction.
    const Rat& q0 = q_prime[0];
    std::vector<Rat> c;
    c.reserve(static_cast<size_t>(M + 1));
    for (long j = 0; j <= M; ++j) {
        Rat acc = p_prime[static_cast<size_t>(j)];
        for (long i = 1; i <= j; ++i) {
            acc = acc - q_prime[static_cast<size_t>(i)] *
                        c[static_cast<size_t>(j - i)];
        }
        c.push_back(acc / q0);
    }

    // Assemble: result = sum_j c_j * var^(pole_deg + j).
    Rat var_rat{Poly::gen(ctx, var_idx)};

    Rat result{Poly::zero_of(ctx)};
    for (long j = 0; j <= M; ++j) {
        const Rat& cj = c[static_cast<size_t>(j)];
        if (cj.is_zero()) continue;
        long exp = pole_deg + j;
        Rat term = cj;
        if (exp > 0) {
            term = term * var_rat.pow(exp);
        } else if (exp < 0) {
            term = term / var_rat.pow(-exp);
        }
        result = result + term;
    }
    return result;
}

Rat substitute_var_reciprocal(const PolyCtx& ctx, const Rat& f, size_t var_idx) {
    long n_deg = f.num().degree_in_var(var_idx);
    long d_deg = f.den().degree_in_var(var_idx);
    long N = std::max<long>(n_deg, d_deg);
    if (N < 0) N = 0;

    const Poly var_poly = Poly::gen(ctx, var_idx);
    const Poly one      = Poly::one_of(ctx);

    auto reverse = [&](const Poly& p) -> Poly {
        long pmin = p.min_exponent_in_var(var_idx);
        long pmax = p.degree_in_var(var_idx);
        Poly out(ctx);
        if (pmin < 0 || pmax < 0) return out;   // zero poly
        for (long k = pmin; k <= pmax; ++k) {
            Poly ck = p.coefficient_of_var(var_idx, k);
            if (ck.is_zero()) continue;
            long e = N - k;
            if (e < 0) continue;  // shouldn't happen since N >= pmax >= k
            const Poly mono = (e == 0)
                ? one
                : var_poly.pow(static_cast<unsigned long>(e));
            out = out + ck * mono;
        }
        return out;
    };

    return Rat(reverse(f.num()), reverse(f.den()));
}

Rat rat_var0_coefficient(const PolyCtx& ctx, const Rat& r, size_t var_idx) {
    if (r.is_zero()) return Rat{Poly::zero_of(ctx)};

    // Fast path: denominator has a non-zero constant-in-var term. Then
    // r has no pole at var = 0, and the [var^0] coefficient of its
    // Laurent (in fact Taylor) expansion is num(var=0) / den(var=0).
    // This mirrors HyperIntica.wl:4782 `coef = Together[poly] /. var -> 0`
    // on the IntegrationStep `minOrder === 0` (trivial-convergence) branch,
    // whose C++ counterpart (integration_step.cpp call site 2) can feed a
    // Rat whose denominator is a general polynomial in var.
    Poly den_at_0 = r.den().coefficient_of_var(var_idx, 0);
    if (!den_at_0.is_zero()) {
        Poly num_at_0 = r.num().coefficient_of_var(var_idx, 0);
        return Rat(num_at_0, den_at_0);
    }

    // den(var=0) = 0: r has a pole at var = 0. The legacy contract
    // required the denominator to be a pure var-monomial (the shape
    // produced by series_expansion); then [var^0] = num.coeff(var^k) /
    // den.coeff(var^k) with k = dmin = dmax. Call sites 1 and 3 in
    // integration_step multiply a sum_term by series_expansion output
    // and always satisfy this invariant.
    long dmin = r.den().min_exponent_in_var(var_idx);
    long dmax = r.den().degree_in_var(var_idx);
    if (dmin != dmax) {
        throw std::runtime_error(
            "rat_var0_coefficient: denominator vanishes at var=0 but is "
            "not a pure var-monomial (dmin != dmax). Caller must route "
            "through series_expansion for this case.");
    }
    Poly num_coef = r.num().coefficient_of_var(var_idx, dmin);
    Poly den_coef = r.den().coefficient_of_var(var_idx, dmin);
    return Rat(num_coef, den_coef);
}

}  // namespace hyperflint
