#include "hyperflint/core/factored_rat.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace hyperflint {

FactoredRat FactoredRat::from_poly(Poly numerator) {
    return FactoredRat(std::move(numerator));
}

void FactoredRat::push_factor(const Poly& base, long exp) {
    if (exp == 0 || base.is_one()) return;
    for (auto& f : den_factors_) {
        if (f.base.equal(base)) { f.exp += exp; return; }
    }
    den_factors_.push_back(Factor{base, exp});  // first-insertion order
}

FactoredRat FactoredRat::from_rat(const Rat& r) {
    FactoredRat fr(r.num());
    if (!r.den().is_one()) fr.push_factor(r.den(), 1);
    return fr;
}

Poly FactoredRat::expand_denominator() const {
    Poly d = Poly::one_of(numerator_.ctx());
    for (const auto& f : den_factors_) {
        d = d.mul(f.base.pow(static_cast<unsigned long>(f.exp)));
    }
    return d;
}

bool FactoredRat::peel_enabled() {
    // DEFAULT-ON since 2026-06-04 (user decision after the evidence bar:
    // tst0-tst4 byte-identical ex-timing with walls in noise; face-family
    // A/B 3.0-14.4x with Mathematica-exact values; adversarial review OK
    // with no binding issues; 896-face re-sweep running as the at-scale
    // corpus). HF_FR_MAT_PEEL=0 opts out.
    static const bool on = []{
        const char* e = std::getenv("HF_FR_MAT_PEEL");
        return !(e && *e == '0' && !e[1]);
    }();
    return on;
}

void FactoredRat::peel_known_factors(long min_terms) {
    if (numerator_.is_zero()) return;
    if (static_cast<long>(numerator_.n_terms()) < min_terms) return;
    // Note on constant bases (advisory A1, review of 5f62abe84): no
    // constant base can currently reach den_factors_ — every factor enters
    // via from_rat (whose Rat ctors absorb constant denominators into the
    // rational content) or push_factor (which skips is_one()). Should a
    // future path admit one anyway, the loop below stays VALUE-EXACT: a
    // constant c divides everything, so its full exponent is stripped and
    // 1/c^e folds into the numerator's rational coefficients (verified by
    // the reviewer's T4 stress case). It is a non-issue for correctness,
    // only a mild inefficiency.
    for (auto& f : den_factors_) {
        while (f.exp > 0 && f.base.divides(numerator_)) {
            numerator_ = numerator_.divexact(f.base);
            --f.exp;
        }
    }
    // Drop exhausted factors so expand_denominator()/add() never touch them.
    den_factors_.erase(
        std::remove_if(den_factors_.begin(), den_factors_.end(),
                       [](const Factor& f) { return f.exp == 0; }),
        den_factors_.end());
}

Rat FactoredRat::materialize_to_rat() const {
    if (den_factors_.empty()) return Rat(numerator_);
    // HF_FR_MAT_PEEL=1 — peel known denominator bases off the numerator by
    // EXACT division before expanding the denominator product. On the
    // 1m-tbox-full `(1+var)*denBase^4` face family the B1.3b derivative
    // chain piles powers of the den bases into the numerator: the three hot
    // materialize calls on ord_-4_face_85 reduce 32k/61k-term operand pairs
    // down to 2-term numerators, i.e. the monolithic Brown GCD spends
    // seconds rediscovering a common factor that is knowable structurally.
    // Each successful divexact strips one base power (cheap heap division
    // by a <=200-term base); the reducing Rat ctor below is UNCHANGED and
    // still canonicalizes whatever the peel misses (integer content,
    // partial-base factors), so the result is value-identical by
    // construction. Gated on >=64 numerator terms: below that the GCD is
    // already ~free and a failed divides() attempt is pure overhead.
    if (peel_enabled() && !numerator_.is_zero() &&
        static_cast<long>(numerator_.n_terms()) >= kPeelMinTerms) {
        auto t0 = std::chrono::steady_clock::now();
        FactoredRat peeled_copy = *this;
        peeled_copy.peel_known_factors();
        double t_peel = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        auto t1 = std::chrono::steady_clock::now();
        Rat r = peeled_copy.den_factors_.empty()
            ? Rat(peeled_copy.numerator_)
            : Rat(peeled_copy.numerator_,
                  peeled_copy.expand_denominator());  // reducing ctor
        if (std::getenv("HF_FR_MAT_STATS")) {
            std::fprintf(stderr,
                "[fr-peel] num_terms=%ld nfac=%zu "
                "peel_wall=%.3fs -> rnum=%ld rden=%ld gcd_wall=%.3fs\n",
                static_cast<long>(numerator_.n_terms()), den_factors_.size(),
                t_peel,
                static_cast<long>(r.num().n_terms()),
                static_cast<long>(r.den().n_terms()),
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t1).count());
        }
        return r;
    }
    // HF_FR_MAT_RAW=1 — DIAGNOSTIC ONLY. Skips the reducing Rat ctor's
    // monolithic fmpq_mpoly_gcd_cofactors(num, expanded_product), which on
    // the 1m-tbox-full `(1+var)*denBase^4` face family is ~99% of the
    // integration wall (ord_-4_face_85: 6.5 s of 6.6 s). Violates
    // from_canonical's coprimality contract on purpose: downstream Rats are
    // then UNREDUCED, so values stay correct but term sizes may swell.
    // Never enable in production; this exists to attribute GCD cost.
    static const bool mat_raw = []{
        const char* e = std::getenv("HF_FR_MAT_RAW");
        return e && *e && *e != '0';
    }();
    if (mat_raw) return Rat::from_canonical(numerator_, expand_denominator());
    // HF_FR_MAT_STATS=1 — DIAGNOSTIC ONLY. Per-call operand/result sizes and
    // wall for the reducing materialize, to attribute the Brown-GCD cost on
    // the 1m-tbox-full face family. Few calls per integration; stderr.
    static const bool mat_stats = []{
        const char* e = std::getenv("HF_FR_MAT_STATS");
        return e && *e && *e != '0';
    }();
    if (mat_stats) {
        Poly den = expand_denominator();
        auto t0 = std::chrono::steady_clock::now();
        Rat r(numerator_, den);
        double dt = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr,
            "[fr-mat] num_terms=%ld den_terms=%ld nfac=%zu -> "
            "rnum_terms=%ld rden_terms=%ld gcd_wall=%.3fs\n",
            static_cast<long>(numerator_.n_terms()),
            static_cast<long>(den.n_terms()), den_factors_.size(),
            static_cast<long>(r.num().n_terms()),
            static_cast<long>(r.den().n_terms()), dt);
        return r;
    }
    return Rat(numerator_, expand_denominator());  // reducing ctor
}

FactoredRat FactoredRat::neg() const {
    FactoredRat r(numerator_.neg());
    r.den_factors_ = den_factors_;
    return r;
}

bool FactoredRat::is_zero() const { return numerator_.is_zero(); }

FactoredRat FactoredRat::add(const FactoredRat& b) const {
    // Common denominator = per-base MAX exponent over the union of factor
    // sets. Lift each numerator by the missing factor powers, then add.
    // Factors are matched by Poly::equal (no hash); sets are tiny.
    auto find_exp = [](const std::vector<Factor>& v, const Poly& base) -> long {
        for (const auto& f : v) if (f.base.equal(base)) return f.exp;
        return 0;
    };
    std::vector<Factor> common = den_factors_;   // first-insertion order
    for (const auto& bf : b.den_factors_) {
        bool found = false;
        for (auto& cf : common) {
            if (cf.base.equal(bf.base)) {
                cf.exp = std::max(cf.exp, bf.exp); found = true; break;
            }
        }
        if (!found) common.push_back(bf);
    }
    // Lift this->numerator_ by prod base^(common_exp - my_exp).
    Poly lifted_a = numerator_;
    for (const auto& cf : common) {
        long d = cf.exp - find_exp(den_factors_, cf.base);
        if (d > 0) lifted_a = lifted_a.mul(cf.base.pow(static_cast<unsigned long>(d)));
    }
    Poly lifted_b = b.numerator_;
    for (const auto& cf : common) {
        long d = cf.exp - find_exp(b.den_factors_, cf.base);
        if (d > 0) lifted_b = lifted_b.mul(cf.base.pow(static_cast<unsigned long>(d)));
    }
    FactoredRat r(lifted_a.add(lifted_b));
    r.den_factors_ = common;
    return r;
}

FactoredRat FactoredRat::sub(const FactoredRat& b) const {
    return add(b.neg());
}


// mul: numerator product, denominator factor-lists concatenated (dedup by Poly::equal).
FactoredRat FactoredRat::mul(const FactoredRat& b) const {
    FactoredRat r(numerator_.mul(b.numerator_));
    r.den_factors_ = den_factors_;
    // copy ours, then merge b's factors (push_factor dedups by Poly::equal).
    for (const auto& f : b.den_factors_) r.push_factor(f.base, f.exp);
    return r;
}

FactoredRat FactoredRat::reciprocal() const {
    // 1 / (num / prod f^e) = (prod f^e) / num.  Reciprocal of zero is
    // undefined; throw rather than pushing a zero denominator factor that
    // would detonate later at materialize. Mirrors Rat::div's zero contract.
    if (numerator_.is_zero()) {
        throw std::runtime_error("FactoredRat::reciprocal: reciprocal of zero");
    }
    FactoredRat r(expand_denominator());
    if (!numerator_.is_one()) r.push_factor(numerator_, 1);
    return r;
}

FactoredRat FactoredRat::div(const FactoredRat& b) const {
    return mul(b.reciprocal());
}

FactoredRat FactoredRat::derivative(size_t var_idx) const {
    // value = N / prod_i f_i^{e_i}.
    // Empty denominator: d/dx (N) = N'.
    if (den_factors_.empty()) {
        return FactoredRat(numerator_.derivative(var_idx));
    }
    // Fast path: if every factor base is independent of var_idx, then
    // d/dx (N/D) = N'/D with the SAME factors (no exponent bump, no
    // spurious factors). This is the common (and required) case.
    bool any_depends = false;
    for (const auto& f : den_factors_) {
        if (f.base.degree_in_var(var_idx) > 0) { any_depends = true; break; }
    }
    if (!any_depends) {
        FactoredRat r(numerator_.derivative(var_idx));
        r.den_factors_ = den_factors_;   // unchanged exponents
        return r;
    }
    // General path. Build the quotient-rule numerator as a single Poly:
    //   P = prod_i f_i   (each base once, exponent 1)
    //   term1 = N' * P
    //   term2 = N * sum_i ( e_i * f_i' * (P / f_i) )
    //   numerator = term1 - term2
    // Result denominator = { (f_i, e_i + 1) } for all i.
    const PolyCtx& c = numerator_.ctx();
    Poly N = numerator_;
    Poly Nprime = numerator_.derivative(var_idx);

    // P = prod_i base_i.
    Poly P = Poly::one_of(c);
    for (const auto& f : den_factors_) P = P.mul(f.base);

    Poly term1 = Nprime.mul(P);

    // sum_i e_i * f_i' * prod_{j!=i} f_j.
    // prod_{j!=i} f_j is built per i by a left/right prefix style product;
    // factor sets are tiny (letters), so the O(k^2) restart is negligible.
    Poly accum = Poly::zero_of(c);
    for (size_t i = 0; i < den_factors_.size(); ++i) {
        Poly prod_rest = Poly::one_of(c);
        for (size_t j = 0; j < den_factors_.size(); ++j) {
            if (j == i) continue;
            prod_rest = prod_rest.mul(den_factors_[j].base);
        }
        Poly fi_prime = den_factors_[i].base.derivative(var_idx);
        Poly ei = Poly::from_int(c, den_factors_[i].exp);
        accum = accum.add(ei.mul(fi_prime).mul(prod_rest));
    }
    Poly term2 = N.mul(accum);

    FactoredRat r(term1.sub(term2));
    for (const auto& f : den_factors_) r.push_factor(f.base, f.exp + 1);
    return r;
}

FactoredRat FactoredRat::pow(long n) const {
    // n == 0 -> 1 (empty denominator, numerator 1).
    // n  > 0 -> numerator^n with each denominator factor exponent scaled by n
    //           (keeps the denominator factored: no expansion).
    // n  < 0 -> reciprocal().pow(-n).
    if (n < 0) return reciprocal().pow(-n);
    if (n == 0) return FactoredRat::from_poly(Poly::one_of(numerator_.ctx()));
    FactoredRat r(numerator_.pow(static_cast<unsigned long>(n)));
    for (const auto& f : den_factors_) r.push_factor(f.base, f.exp * n);
    return r;
}
}  // namespace hyperflint
