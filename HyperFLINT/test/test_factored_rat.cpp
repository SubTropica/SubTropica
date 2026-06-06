// Unit tests for FactoredRat: equivalence to Rat via materialize_to_rat().
// (PolyCtx is declared inside poly.hpp; there is no poly_ctx.hpp.)
#include "hyperflint/core/factored_rat.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/poly.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace hyperflint;

int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ \
                  << "  " << #cond << "\n"; \
        ++g_failures; \
    } \
} while (0)

// Value-equality of two rational functions, INDEPENDENT of representation.
// Rat is kept in lowest terms (polynomial gcd divided out) but is NOT
// content-canonical: it does not normalize the overall rational scaling, so
// u = (N/D) and (2N)/(2D) are the SAME value yet Rat::equal(u, v) is false
// (Rat::equal compares representation). For a VALUE test we must cross-
// multiply: N_u * D_v == N_v * D_u  (exact for nonzero denominators).
// This mirrors test_rat_add_equivalence.cpp, which compares via
// fmpz_mpoly_q_canonicalise for exactly this reason.
bool value_equal(const Rat& u, const Rat& v) {
    return u.num().mul(v.den()).equal(v.num().mul(u.den()));
}
}  // namespace

int main() {
    using namespace hyperflint;
    PolyCtx ctx({"x", "y", "z"});
    // A pure polynomial: x + y. from_poly -> materialize must equal Rat(x+y).
    Poly p = Poly::gen(ctx, 0).add(Poly::gen(ctx, 1));
    FactoredRat fr = FactoredRat::from_poly(p);
    Rat got = fr.materialize_to_rat();
    CHECK(got.equal(Rat(p)));
    // from_rat -> materialize must equal the original Rat.
    // r = (x*y + z) / (x + 2*y)
    Poly rn = Poly::gen(ctx, 0).mul(Poly::gen(ctx, 1)).add(Poly::gen(ctx, 2));
    Poly rd = Poly::gen(ctx, 0).add(Poly::gen(ctx, 1).mul(Poly::from_int(ctx, 2)));
    Rat r(rn, rd);
    CHECK(FactoredRat::from_rat(r).materialize_to_rat().equal(r));

    // Structural: (1/A) * (1/A) keeps ONE factor with exp 2 (not an expanded A^2).
    Poly Af = Poly::gen(ctx, 0).add(Poly::from_int(ctx, 5));   // x+5
    FactoredRat invA = FactoredRat::from_rat(Rat(Poly::one_of(ctx), Af));
    FactoredRat invA2 = invA.mul(invA);
    CHECK(invA2.den_factors().size() == 1);
    CHECK(invA2.den_factors()[0].exp == 2);
    // and it still materializes correctly to 1/A^2.
    CHECK(invA2.materialize_to_rat().equal(Rat(Poly::one_of(ctx), Af.pow(2))));

    // Structural: pole power (var - a)^4 stays one factor with exp 4.
    Poly vma = Poly::gen(ctx, 0).sub(Poly::gen(ctx, 2));        // x - z
    FactoredRat invPole = FactoredRat::from_rat(Rat(Poly::one_of(ctx), vma));
    CHECK(invPole.pow(4).den_factors().size() == 1);
    CHECK(invPole.pow(4).den_factors()[0].exp == 4);

    // add equivalence, shared denominator: x/(y+1) + z/(y+1).
    Rat s1n(Poly::gen(ctx, 0), Poly::gen(ctx, 1).add(Poly::from_int(ctx, 1)));
    Rat s2n(Poly::gen(ctx, 2), Poly::gen(ctx, 1).add(Poly::from_int(ctx, 1)));
    CHECK(FactoredRat::from_rat(s1n).add(FactoredRat::from_rat(s2n))
              .materialize_to_rat().equal(s1n.add(s2n)));

    // add equivalence, DIFFERENT denominators: x/(y+1) + z/(x+3).
    Rat d1(Poly::gen(ctx, 0), Poly::gen(ctx, 1).add(Poly::from_int(ctx, 1)));
    Rat d2(Poly::gen(ctx, 2), Poly::gen(ctx, 0).add(Poly::from_int(ctx, 3)));
    CHECK(FactoredRat::from_rat(d1).add(FactoredRat::from_rat(d2))
              .materialize_to_rat().equal(d1.add(d2)));

    // add equivalence, ASSOCIATE denominators (A vs -A): x/(y+1) + z/(-(y+1)).
    Poly A = Poly::gen(ctx, 1).add(Poly::from_int(ctx, 1));
    Rat e1(Poly::gen(ctx, 0), A);
    Rat e2(Poly::gen(ctx, 2), A.neg());
    CHECK(FactoredRat::from_rat(e1).add(FactoredRat::from_rat(e2))
              .materialize_to_rat().equal(e1.add(e2)));

    // sub equivalence.
    CHECK(FactoredRat::from_rat(d1).sub(FactoredRat::from_rat(d2))
              .materialize_to_rat().equal(d1.sub(d2)));

    // a = x/(y+1), b = z/(x+3).
    Rat a(Poly::gen(ctx, 0), Poly::gen(ctx, 1).add(Poly::from_int(ctx, 1)));
    Rat b(Poly::gen(ctx, 2), Poly::gen(ctx, 0).add(Poly::from_int(ctx, 3)));
    // div equivalence: (a)/(b) via FactoredRat == via Rat.
    CHECK(FactoredRat::from_rat(a).div(FactoredRat::from_rat(b))
              .materialize_to_rat().equal(a.div(b)));
    // reciprocal of (x)/(y+1) is (y+1)/(x).
    CHECK(FactoredRat::from_rat(a).reciprocal().materialize_to_rat()
              .equal(Rat(Poly::gen(ctx, 1).add(Poly::from_int(ctx, 1)), Poly::gen(ctx, 0))));

    // div / reciprocal must throw on a zero divisor (mirrors Rat::div).
    {
        FactoredRat zero = FactoredRat::from_poly(Poly::from_int(ctx, 0));
        FactoredRat one  = FactoredRat::from_rat(Rat::one_of(ctx));
        bool threw = false;
        try { (void)one.div(zero).materialize_to_rat(); }
        catch (const std::exception&) { threw = true; }
        CHECK(threw);
        bool threw2 = false;
        try { (void)zero.reciprocal().materialize_to_rat(); }
        catch (const std::exception&) { threw2 = true; }
        CHECK(threw2);
    }

    // Coverage: non-coprime add — 1/(x+1) + 1/((x+1)(x+2)).
    {
        Poly xp1 = Poly::gen(ctx, 0).add(Poly::from_int(ctx, 1));
        Poly xp2 = Poly::gen(ctx, 0).add(Poly::from_int(ctx, 2));
        Rat r1(Poly::one_of(ctx), xp1);
        Rat r2(Poly::one_of(ctx), xp1.mul(xp2));
        CHECK(FactoredRat::from_rat(r1).add(FactoredRat::from_rat(r2))
                  .materialize_to_rat().equal(r1.add(r2)));
    }
    // Coverage: repeated-factor accumulation — (1/(x+1))^2 + 1/(x+1).
    {
        Poly xp1 = Poly::gen(ctx, 0).add(Poly::from_int(ctx, 1));
        FactoredRat g = FactoredRat::from_rat(Rat(Poly::one_of(ctx), xp1)).pow(2);
        FactoredRat h = FactoredRat::from_rat(Rat(Poly::one_of(ctx), xp1));
        Rat oracle = Rat(Poly::one_of(ctx), xp1.pow(2)).add(Rat(Poly::one_of(ctx), xp1));
        CHECK(g.add(h).materialize_to_rat().equal(oracle));
    }
    // Coverage: three-way associate sum — 1/A + 1/(-A) + 1/(2A), A=x+1.
    {
        Poly A = Poly::gen(ctx, 0).add(Poly::from_int(ctx, 1));
        Rat t1(Poly::one_of(ctx), A);
        Rat t2(Poly::one_of(ctx), A.neg());
        Rat t3(Poly::one_of(ctx), A.mul(Poly::from_int(ctx, 2)));
        FactoredRat sum = FactoredRat::from_rat(t1)
                              .add(FactoredRat::from_rat(t2))
                              .add(FactoredRat::from_rat(t3));
        CHECK(sum.materialize_to_rat().equal(t1.add(t2).add(t3)));
    }
    // Coverage: reciprocal of composite numerator — ((x*y+z)/(x+1))^{-1}, and f/f==1.
    {
        Poly cn = Poly::gen(ctx, 0).mul(Poly::gen(ctx, 1)).add(Poly::gen(ctx, 2));
        Poly cd = Poly::gen(ctx, 0).add(Poly::from_int(ctx, 1));
        Rat f(cn, cd);
        CHECK(FactoredRat::from_rat(f).reciprocal().materialize_to_rat()
                  .equal(Rat(cd, cn)));
        CHECK(FactoredRat::from_rat(f).div(FactoredRat::from_rat(f))
                  .materialize_to_rat().equal(Rat::one_of(ctx)));
    }
    // Coverage: chained op producing a TRUE zero numerator — a*b - b*a == 0.
    {
        FactoredRat fa = FactoredRat::from_rat(a);   // a,b defined earlier in main
        FactoredRat fb = FactoredRat::from_rat(b);
        FactoredRat z = fa.mul(fb).sub(fb.mul(fa));
        CHECK(z.is_zero());
        CHECK(z.materialize_to_rat().equal(Rat::zero_of(ctx)));
    }

    // Randomized differential test: random rationals, random op chain, vs Rat oracle.
    auto randpoly = [&](unsigned s) -> Poly {
        Poly q = Poly::from_int(ctx, static_cast<long>(1 + (s % 3)));
        if (s & 1) q = q.add(Poly::gen(ctx, 0));
        if (s & 2) q = q.add(Poly::gen(ctx, 1).mul(Poly::from_int(ctx, 2)));
        if (s & 4) q = q.add(Poly::gen(ctx, 2));
        if (q.is_zero()) q = Poly::one_of(ctx);
        return q;
    };
    // Compare by VALUE (value_equal cross-multiplies), NOT Rat::equal: a
    // FactoredRat materialized through a chain can be content-scaled vs the Rat
    // oracle (e.g. (2N)/(2D) vs N/D) — same value, different representation.
    // This is the content-redistribution property; for B1 integration the gate
    // must therefore be value-equality, not byte-identity.
    for (unsigned s = 1; s < 64; ++s) {
        Rat ra(randpoly(s), randpoly(s + 7));
        Rat rb(randpoly(s + 13), randpoly(s + 23));
        FactoredRat fa = FactoredRat::from_rat(ra);
        FactoredRat fb = FactoredRat::from_rat(rb);
        if (!value_equal(fa.mul(fb).materialize_to_rat(), ra.mul(rb))) {
            std::cerr << "MULFAIL seed=" << s << "\n"; ++g_failures; }
        if (!value_equal(fa.add(fb).materialize_to_rat(), ra.add(rb))) {
            std::cerr << "ADDFAIL seed=" << s << "\n"; ++g_failures; }
        if (!value_equal(fa.sub(fb).materialize_to_rat(), ra.sub(rb))) {
            std::cerr << "SUBFAIL seed=" << s << "\n"; ++g_failures; }
        if (!rb.is_zero() &&
            !value_equal(fa.div(fb).materialize_to_rat(), ra.div(rb))) {
            std::cerr << "DIVFAIL seed=" << s << "\n"; ++g_failures; }
        if (!value_equal(fa.pow(3).materialize_to_rat(), ra.pow(3))) {
            std::cerr << "POWFAIL seed=" << s << "\n"; ++g_failures; }
        // chained: (a*b + a)/b  (factor accumulation + cancellation at materialize)
        if (!rb.is_zero()) {
            FactoredRat chain = fa.mul(fb).add(fa).div(fb);
            Rat oracle = ra.mul(rb).add(ra).div(rb);
            if (!value_equal(chain.materialize_to_rat(), oracle)) {
                std::cerr << "CHAINFAIL seed=" << s << "\n"; ++g_failures; }
        }
    }

    // ---- derivative(): differential test vs the Rat oracle ----
    // For each case, d/dx via FactoredRat (keeping the denominator factored)
    // must value-equal d/dx of the materialized Rat. Compared via value_equal
    // (cross-multiply): the factored quotient rule produces a denominator that
    // is content/representation-distinct from the Rat oracle's reduced form,
    // so byte-identity does not hold, but the VALUE must.
    {
        Poly X = Poly::gen(ctx, 0);   // x
        Poly Y = Poly::gen(ctx, 1);   // y
        Poly Z = Poly::gen(ctx, 2);   // z
        Poly one = Poly::one_of(ctx);

        // (a) var-INDEPENDENT denominator (fast path):
        //     (x^2 + x*z + 1) / (y+1)^3, d/dx.  The denominator (y+1)^3 has
        //     no x dependence, so derivative = N'/D with the SAME factors.
        //     Build the denominator in FACTORED form via pow(3) so the single
        //     factor (y+1) genuinely carries exponent 3 (the reducing Rat ctor
        //     in from_rat would otherwise expand (y+1)^3 into one exp-1 poly).
        //     This exercises the fast-path guarantee: NO exponent bump, NO
        //     spurious factor, den_factors_ returned unchanged.
        {
            Poly numA = X.pow(2).add(X.mul(Z)).add(one);
            Poly yp1 = Y.add(Poly::from_int(ctx, 1));
            FactoredRat fr =
                FactoredRat::from_rat(Rat(one, yp1)).pow(3)
                    .mul(FactoredRat::from_poly(numA));
            // sanity: the input is (numA) / (y+1)^3 held factored, exp 3.
            CHECK(fr.den_factors().size() == 1);
            CHECK(fr.den_factors()[0].exp == 3);
            FactoredRat d = fr.derivative(0);
            CHECK(value_equal(d.materialize_to_rat(),
                              fr.materialize_to_rat().derivative(0)));
            // fast path must NOT bump exponents or add spurious factors:
            // the only factor stays (y+1) with exp 3, unchanged.
            CHECK(d.den_factors().size() == 1);
            CHECK(d.den_factors()[0].exp == 3);
        }

        // (b) var-DEPENDENT denominator (general path).
        {
            // z / (x+1), d/dx.
            FactoredRat fr =
                FactoredRat::from_rat(Rat(Z, X.add(Poly::from_int(ctx, 1))));
            FactoredRat d = fr.derivative(0);
            CHECK(value_equal(d.materialize_to_rat(),
                              fr.materialize_to_rat().derivative(0)));
        }
        {
            // (x+z) / ((x+1)^2 * (x*y+1)), d/dx. Two x-dependent factors,
            // one with multiplicity 2.
            Poly num = X.add(Z);
            Poly f1 = X.add(Poly::from_int(ctx, 1));   // x+1
            Poly f2 = X.mul(Y).add(Poly::from_int(ctx, 1));  // x*y+1
            FactoredRat fr =
                FactoredRat::from_rat(Rat(one, f1)).pow(2)
                    .mul(FactoredRat::from_rat(Rat(one, f2)))
                    .mul(FactoredRat::from_poly(num));
            FactoredRat d = fr.derivative(0);
            CHECK(value_equal(d.materialize_to_rat(),
                              fr.materialize_to_rat().derivative(0)));
        }

        // (c) empty denominator (pure polynomial): x^2*y + z, d/dx.
        {
            Poly p = X.pow(2).mul(Y).add(Z);
            FactoredRat fr = FactoredRat::from_poly(p);
            FactoredRat d = fr.derivative(0);
            CHECK(d.den_factors().empty());
            CHECK(value_equal(d.materialize_to_rat(),
                              fr.materialize_to_rat().derivative(0)));
        }

        // (d) multiplicity case mirroring residue extraction: (1/(x-y))^3, d/dx.
        {
            Poly xmy = X.sub(Y);
            FactoredRat fr = FactoredRat::from_rat(Rat(one, xmy)).pow(3);
            FactoredRat d = fr.derivative(0);
            CHECK(value_equal(d.materialize_to_rat(),
                              fr.materialize_to_rat().derivative(0)));
        }

        // also exercise differentiation w.r.t. y on case (a)'s value
        // (now the denominator IS y-dependent: general path on k=1).
        {
            Poly num = X.pow(2).add(X.mul(Z)).add(one);
            Poly den = Y.add(Poly::from_int(ctx, 1)).pow(3);
            FactoredRat fr = FactoredRat::from_rat(Rat(num, den));
            FactoredRat d = fr.derivative(1);
            CHECK(value_equal(d.materialize_to_rat(),
                              fr.materialize_to_rat().derivative(1)));
        }
    }

    // ---- peel_known_factors(): value preservation + structure ----
    // (advisory A2, review of 5f62abe84). All value checks via value_equal:
    // peel changes the representation (and hence content scaling), never the
    // value. min_terms=1 forces the peel on small test numerators; the gate
    // itself is tested separately in (iv).
    {
        Poly X = Poly::gen(ctx, 0);
        Poly Y = Poly::gen(ctx, 1);
        Poly one = Poly::one_of(ctx);
        Poly base = X.add(Y).add(one);                       // x+y+1
        Poly yp3 = Y.add(Poly::from_int(ctx, 3));            // y+3

        // (i) PARTIAL peel: numerator base^2*(x+2) over {base^5, (y+3)^1}.
        //     base exp must drop 5 -> 3 (exactly the two removable powers),
        //     (y+3) must survive untouched, value unchanged.
        {
            Poly num = base.pow(2).mul(X.add(Poly::from_int(ctx, 2)));
            FactoredRat fr = FactoredRat::from_rat(Rat(one, base)).pow(5)
                                 .mul(FactoredRat::from_rat(Rat(one, yp3)))
                                 .mul(FactoredRat::from_poly(num));
            Rat before = fr.materialize_to_rat();
            FactoredRat fp = fr;
            fp.peel_known_factors(1);
            CHECK(value_equal(fp.materialize_to_rat(), before));
            bool base_ok = false, yp3_ok = false;
            for (const auto& f : fp.den_factors()) {
                if (f.base.equal(base)) base_ok = (f.exp == 3);
                if (f.base.equal(yp3))  yp3_ok  = (f.exp == 1);
            }
            CHECK(base_ok);
            CHECK(yp3_ok);
            // numerator no longer divisible by base.
            CHECK(!base.divides(fp.numerator()));
        }

        // (ii) FULL strip to empty denominator: base^4 over {base^3} peels
        //      to numerator base^1 with NO factors left (exhausted factors
        //      are erased so expand_denominator()/add() never see them).
        {
            FactoredRat fr = FactoredRat::from_rat(Rat(one, base)).pow(3)
                                 .mul(FactoredRat::from_poly(base.pow(4)));
            Rat before = fr.materialize_to_rat();
            FactoredRat fp = fr;
            fp.peel_known_factors(1);
            CHECK(fp.den_factors().empty());
            CHECK(fp.numerator().equal(base));
            CHECK(value_equal(fp.materialize_to_rat(), before));
        }

        // (iii) Derivative-chain peel-vs-no-peel value equality (the B1.3b
        //       pattern): expr = (x+2) / denBase^4 with denBase x-dependent,
        //       chain derivs[t] = d^t/dx^t expr, peeling one arm after every
        //       step. Mirrors partial_fractions.cpp's chain-peel call sites.
        {
            Poly denBase = X.mul(Y).add(X).add(one);         // x*y+x+1
            FactoredRat expr =
                FactoredRat::from_rat(Rat(one, denBase)).pow(4)
                    .mul(FactoredRat::from_poly(X.add(Poly::from_int(ctx, 2))));
            FactoredRat plain = expr;
            FactoredRat peeled = expr;
            peeled.peel_known_factors(1);
            for (int t = 0; t < 4; ++t) {
                CHECK(value_equal(peeled.materialize_to_rat(),
                                  plain.materialize_to_rat()));
                plain = plain.derivative(0);
                peeled = peeled.derivative(0);
                peeled.peel_known_factors(1);
            }
            CHECK(value_equal(peeled.materialize_to_rat(),
                              plain.materialize_to_rat()));
        }

        // (iv) min_terms gate: a numerator below the threshold must be left
        //      ENTIRELY untouched (no divides() attempt can alter structure;
        //      exponents and numerator identical), even though base | num.
        {
            FactoredRat fr = FactoredRat::from_rat(Rat(one, base)).pow(2)
                                 .mul(FactoredRat::from_poly(base));  // 3-term num
            FactoredRat fp = fr;
            fp.peel_known_factors();   // default kPeelMinTerms = 64 > 3 terms
            CHECK(fp.numerator().equal(fr.numerator()));
            CHECK(fp.den_factors().size() == fr.den_factors().size());
            CHECK(fp.den_factors()[0].exp == fr.den_factors()[0].exp);
            // and with the gate lowered it does peel:
            fp.peel_known_factors(1);
            CHECK(fp.den_factors()[0].exp == 1);
            CHECK(value_equal(fp.materialize_to_rat(),
                              fr.materialize_to_rat()));
        }
    }

    if (g_failures == 0) std::cout << "test_factored_rat: all passed\n";
    return g_failures;
}
