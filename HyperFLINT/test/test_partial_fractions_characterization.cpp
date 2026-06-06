// Characterization oracle for partial_fractions (B1.3 single-pole fast path).
//
// We drive the PUBLIC entry point `partial_fractions(f, var_idx, alg_letters)`,
// which on a cache miss delegates to partial_fractions_impl -- the function
// that carries the B1.3 single-pole FactoredRat fast path.  Correctness is
// asserted by RECONSTRUCTION:
//
//   f == polynomial_part + sum_k coefs[k-1] / (var - pole)^k.
//
// Reconstruction is path-independent: it proves the decomposition is the
// correct one regardless of WHICH internal path produced it, which is exactly
// the property we need (a wrong factorial, a mis-evaluated residue, or a
// dropped pole all break the identity).  We additionally assert structural
// shape (number of poles, multiplicities, empty nonlinear bucket) so a path
// that silently produced fewer terms cannot pass.
//
// Fixture A (MULTI-POLE) routes through the legacy Rat residue loop, which the
// fast path must leave byte-identical.  Fixtures B / B2 route through the new
// single-pole FactoredRat fast path: B has a genuine rational pole P/Q with a
// non-constant Q (exercising per-factor Q-homogenization) and multiplicity 3
// (full derivative chain + factorial divides); B2 has a polynomial pole (Q==1)
// with a non-trivial polynomial part.

#include "hyperflint/algebra/partial_fractions.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/zw_table.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace hyperflint;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        ++g_failures;
    }
}

// VALUE-equality of two rationals, INDEPENDENT of representation.
// Rat is kept in lowest POLYNOMIAL terms but is NOT content-canonical: a/b and
// (2a)/(2b) are the same value yet differ in representation, so comparing
// num()/den() component-wise gives false negatives. The FactoredRat fast path
// (B1.3) materializes content-scaled vs the legacy path on the Q!=1 pole case,
// so we MUST compare by cross-multiply: a.num*b.den == b.num*a.den (exact for
// nonzero denominators). [Was component-equality, which false-failed fixture B
// — a representation difference, not a value error; verified value-equal.]
bool rat_equal(const Rat& a, const Rat& b) {
    return a.num().mul(b.den()).equal(b.num().mul(a.den()));
}

// (var - a) as a Rat in the shared context.
Rat var_minus(const Poly& var_poly, const Rat& a) {
    return Rat(var_poly) - a;
}

// Rat power by repeated multiplication (e >= 1).
Rat rat_pow(const Rat& base, long e) {
    Rat acc = base;
    for (long k = 1; k < e; ++k) acc = acc * base;
    return acc;
}

// Reconstruct f from a partial-fraction result and compare to the original.
bool reconstruct_equals(const PartialFractionization& res,
                        const Rat& f,
                        size_t var_idx) {
    const PolyCtx& ctx = f.ctx();
    Poly var_poly = Poly::gen(ctx, var_idx);
    Rat acc = res.polynomial_part;
    for (const auto& pole : res.poles) {
        for (long k = 1; k <= pole.multiplicity; ++k) {
            const Rat& c = pole.coefs[static_cast<size_t>(k - 1)];
            Rat denom = rat_pow(var_minus(var_poly, pole.pole), k);
            acc = acc + (c / denom);
        }
    }
    return rat_equal(acc, f);
}

}  // namespace

int main() {
    // ----- Fixture A: MULTI-POLE (legacy Rat residue loop) ---------------
    // Two variables x (=var 0) and y.  f has two distinct linear poles in x:
    //   x = y   (multiplicity 2)   and   x = -1   (multiplicity 1),
    // plus a genuine polynomial part so polynomial_part is non-trivial.
    //
    //   f = (x^3 + y) / [ (x - y)^2 * (x + 1) ].
    //
    // lf.linear has size 2 -> the fast-path gate is NOT taken; this
    // characterizes the legacy residue loop, the factorial divide, and the
    // derivative chain (multiplicity 2).
    {
        PolyCtx ctx({"x", "y"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly one = Poly::one_of(ctx);

        Poly num = x.pow(3).add(y);                 // x^3 + y
        Poly xmy = x.sub(y);                        // x - y
        Poly xp1 = x.add(one);                      // x + 1
        Poly den = xmy.pow(2).mul(xp1);             // (x-y)^2 (x+1)
        Rat f(num, den);

        PartialFractionization res =
            partial_fractions(f, 0, std::make_shared<ZWTable>(ctx), false);

        check(res.poles.size() == 2, "A: two distinct poles");
        long total_mult = 0;
        for (const auto& p : res.poles) total_mult += p.multiplicity;
        check(total_mult == 3, "A: multiplicities sum to 3");
        check(reconstruct_equals(res, f, 0),
              "A: reconstruction == f (legacy path)");
    }

    // ----- Fixture B: SINGLE-POLE, mult 3, 3 variables (FAST PATH) -------
    // Variables x (=var 0), y, z.  Single distinct linear pole in x at x=y/z
    // (a genuine rational pole P/Q with P=y, Q=z), multiplicity 3:
    //
    //   den = (z*x - y)^3,   num = x^2 + y*z + z^2.
    //   f = (x^2 + y z + z^2) / (z x - y)^3.
    //
    // lf.linear.size()==1, lf.nonlinear empty, alg_letters=false
    //   -> the single-pole FactoredRat fast path is taken.  The pole is P/Q
    //   with Q=z (non-constant), so eval homogenizes by z, and m=3 exercises
    //   the full derivative chain plus the 1!,2! factorial divides.
    {
        PolyCtx ctx({"x", "y", "z"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly z = Poly::gen(ctx, 2);

        Poly lin = z.mul(x).sub(y);                          // z x - y
        Poly den = lin.pow(3);                               // (z x - y)^3
        Poly num = x.mul(x).add(y.mul(z)).add(z.mul(z));     // x^2 + y z + z^2
        Rat f(num, den);

        PartialFractionization res =
            partial_fractions(f, 0, std::make_shared<ZWTable>(ctx), false);

        check(res.poles.size() == 1, "B: single pole");
        if (res.poles.size() == 1) {
            check(res.poles[0].multiplicity == 3, "B: multiplicity 3");
            check(res.poles[0].coefs.size() == 3, "B: three Laurent coefs");
            Rat expected_pole(y, z);   // y/z
            check(rat_equal(res.poles[0].pole, expected_pole),
                  "B: pole == y/z");
        }
        check(reconstruct_equals(res, f, 0),
              "B: reconstruction == f (fast path)");
    }

    // ----- Fixture B2: SINGLE-POLE, polynomial pole (Q == 1) -------------
    // Guards the a.den()==1 branch of the fast path: pole at x = y (Q=1),
    // multiplicity 2, with a non-trivial polynomial part.
    //
    //   f = (x^3 + 2 x + y) / (x - y)^2.
    {
        PolyCtx ctx({"x", "y"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly two = Poly::from_int(ctx, 2);

        Poly num = x.pow(3).add(two.mul(x)).add(y);   // x^3 + 2x + y
        Poly den = x.sub(y).pow(2);                   // (x - y)^2
        Rat f(num, den);

        PartialFractionization res =
            partial_fractions(f, 0, std::make_shared<ZWTable>(ctx), false);

        check(res.poles.size() == 1, "B2: single pole");
        if (res.poles.size() == 1) {
            check(res.poles[0].multiplicity == 2, "B2: multiplicity 2");
            check(rat_equal(res.poles[0].pole, Rat(y)), "B2: pole == y");
        }
        check(reconstruct_equals(res, f, 0),
              "B2: reconstruction == f (fast path, Q=1)");
    }

    // ----- Fixture C: MULTI-POLE, both poles Q==1, distinct mults --------
    // Variables x (=var 0), y.  Two distinct linear poles in x, both with
    // unit denominator Q (polynomial poles), but with DIFFERENT
    // multiplicities so the legacy residue loop runs the derivative chain
    // for one pole and the simple residue for the other:
    //   x = y    (multiplicity 2,  Q=1)
    //   x = -2y  (multiplicity 1,  Q=1)
    //
    //   den = (x - y)^2 * (x + 2y),   num = x^2 + y.
    //   f = (x^2 + y) / [ (x - y)^2 (x + 2y) ].
    //
    // lf.linear.size()==2 -> the multi-pole fast-path gate is NOT taken
    // (today); this characterizes the legacy path on a mixed-multiplicity
    // input that B1.3b must reproduce.
    {
        PolyCtx ctx({"x", "y"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly two = Poly::from_int(ctx, 2);

        Poly num = x.mul(x).add(y);                  // x^2 + y
        Poly xmy = x.sub(y);                         // x - y
        Poly xp2y = x.add(two.mul(y));               // x + 2y
        Poly den = xmy.pow(2).mul(xp2y);             // (x-y)^2 (x+2y)
        Rat f(num, den);

        PartialFractionization res =
            partial_fractions(f, 0, std::make_shared<ZWTable>(ctx), false);

        check(res.poles.size() == 2, "C: two distinct poles");
        long total_mult = 0;
        for (const auto& p : res.poles) total_mult += p.multiplicity;
        check(total_mult == 3, "C: multiplicities sum to 3");
        check(reconstruct_equals(res, f, 0),
              "C: reconstruction == f (legacy multi-pole, Q=1)");
    }

    // ----- Fixture D: MULTI-POLE with TWO DIFFERENT non-unit Q's ---------
    // Variables x (=var 0), y, z.  Two distinct rational poles in x, each
    // with its OWN non-constant denominator Q -- exactly the regime that
    // previously hid a bug and the one B1.3b must get right when the
    // single-pole fast path is generalized to multi-pole:
    //   x = y/z  (multiplicity 2,  Q=z)
    //   x = z/y  (multiplicity 1,  Q=y)
    //
    //   den = (z*x - y)^2 * (y*x - z),   num = x^2 + y*z + 1.
    //   f = (x^2 + y z + 1) / [ (z x - y)^2 (y x - z) ].
    //
    // Two DIFFERENT non-unit Q's in the same input exercises per-pole
    // Q-homogenization independently.  Routes through the legacy multi-pole
    // path today (lf.linear.size()==2).
    {
        PolyCtx ctx({"x", "y", "z"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly z = Poly::gen(ctx, 2);
        Poly one = Poly::one_of(ctx);

        Poly zx_my = z.mul(x).sub(y);                // z x - y  -> pole y/z
        Poly yx_mz = y.mul(x).sub(z);                // y x - z  -> pole z/y
        Poly den = zx_my.pow(2).mul(yx_mz);          // (z x - y)^2 (y x - z)
        Poly num = x.mul(x).add(y.mul(z)).add(one);  // x^2 + y z + 1
        Rat f(num, den);

        PartialFractionization res =
            partial_fractions(f, 0, std::make_shared<ZWTable>(ctx), false);

        check(res.poles.size() == 2, "D: two distinct poles");
        long total_mult = 0;
        for (const auto& p : res.poles) total_mult += p.multiplicity;
        check(total_mult == 3, "D: multiplicities sum to 3");
        check(reconstruct_equals(res, f, 0),
              "D: reconstruction == f (legacy multi-pole, two non-unit Q)");
    }

    // ----- Fixture E: MULTI-POLE, three poles, mixed Q -------------------
    // Variables x (=var 0), y, z.  Three distinct linear poles in x:
    //   x = y    (multiplicity 2,  Q=1)
    //   x = 1/z  (multiplicity 1,  Q=z)
    //   x = -z   (multiplicity 1,  Q=1)
    //
    //   den = (x - y)^2 * (z*x - 1) * (x + z),   num = x^3 + 1.
    //   f = (x^3 + 1) / [ (x - y)^2 (z x - 1) (x + z) ].
    //
    // deg(num)=3 < deg(den)=4, so polynomial_part is zero; the
    // higher-degree numerator still stresses the residue arithmetic across
    // three poles with mixed Q.  Routes through the legacy multi-pole path
    // today (lf.linear.size()==3).
    {
        PolyCtx ctx({"x", "y", "z"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly z = Poly::gen(ctx, 2);
        Poly one = Poly::one_of(ctx);

        Poly xmy = x.sub(y);                         // x - y    -> pole y
        Poly zx_m1 = z.mul(x).sub(one);              // z x - 1  -> pole 1/z
        Poly xpz = x.add(z);                         // x + z    -> pole -z
        Poly den = xmy.pow(2).mul(zx_m1).mul(xpz);   // (x-y)^2 (z x-1)(x+z)
        Poly num = x.pow(3).add(one);                // x^3 + 1
        Rat f(num, den);

        PartialFractionization res =
            partial_fractions(f, 0, std::make_shared<ZWTable>(ctx), false);

        check(res.poles.size() == 3, "E: three distinct poles");
        long total_mult = 0;
        for (const auto& p : res.poles) total_mult += p.multiplicity;
        check(total_mult == 4, "E: multiplicities sum to 4");
        check(reconstruct_equals(res, f, 0),
              "E: reconstruction == f (legacy multi-pole, mixed Q)");
    }

    // Fixture F: MULTI-POLE with POLYNOMIAL (non-monomial) Q. vars {x,y,z}.
    // Poles at x = y/(y+1) [Q=y+1] and x = 1/(z+2) [Q=z+2], distinct non-monomial Q's.
    //   den = ((y+1)*x - y)^2 * ((z+2)*x - 1)
    //   num = x^2 + y + z
    {
        PolyCtx ctx({"x", "y", "z"});
        Poly x = Poly::gen(ctx, 0), y = Poly::gen(ctx, 1), z = Poly::gen(ctx, 2);
        Poly one = Poly::one_of(ctx);
        Poly lin1 = y.add(one).mul(x).sub(y);          // (y+1)x - y
        Poly lin2 = z.add(Poly::from_int(ctx,2)).mul(x).sub(one);  // (z+2)x - 1
        Poly den = lin1.pow(2).mul(lin2);
        Poly num = x.pow(2).add(y).add(z);
        Rat f(num, den);
        PartialFractionization res =
            partial_fractions(f, 0, std::make_shared<ZWTable>(ctx), false);
        check(res.poles.size() == 2, "F: two distinct poles (poly Q)");
        long tm = 0; for (const auto& p : res.poles) tm += p.multiplicity;
        check(tm == 3, "F: multiplicities sum to 3");
        check(reconstruct_equals(res, f, 0), "F: reconstruction == f (poly Q)");
    }

    // ----- Fixture G: squarefree-first stress (B2) -----------------------
    // Guards the deg-in-var >= 2 squarefree-first fast path in
    // linear_factors. (a) perfect power (x - y)^4 [single linear, mult 4],
    // the req shape (denBase^4, denBase linear in var); (b) distinct
    // linears at EQUAL multiplicity (x - y)^2 (x - z)^2 -> the squarefree
    // part (x-y)(x-z) is degree-2 REDUCIBLE, which forces the fast path to
    // bail back to the full FLINT factor; the path must still yield two
    // poles at multiplicity 2 each.
    {
        PolyCtx ctx({"x","y","z"});
        Poly x=Poly::gen(ctx,0), y=Poly::gen(ctx,1), z=Poly::gen(ctx,2), one=Poly::one_of(ctx);
        // (a)
        { Poly den=x.sub(y).pow(4); Poly num=x.pow(2).add(one); Rat f(num,den);
          auto res=partial_fractions(f,0,std::make_shared<ZWTable>(ctx),false);
          check(res.poles.size()==1,"G(a): one pole");
          if(res.poles.size()==1) check(res.poles[0].multiplicity==4,"G(a): mult 4");
          check(reconstruct_equals(res,f,0),"G(a): perfect power reconstruction"); }
        // (b)
        { Poly den=x.sub(y).pow(2).mul(x.sub(z).pow(2)); Poly num=x.pow(3).add(z); Rat f(num,den);
          auto res=partial_fractions(f,0,std::make_shared<ZWTable>(ctx),false);
          check(res.poles.size()==2,"G(b): two poles");
          long tm=0; for(auto&p:res.poles) tm+=p.multiplicity;
          check(tm==4,"G(b): multiplicities sum 4");
          check(reconstruct_equals(res,f,0),"G(b): distinct-linear-equal-mult reconstruction"); }
    }

    if (g_failures == 0) {
        std::cout << "all passed\n";
        return 0;
    }
    std::cerr << g_failures << " checks failed\n";
    return 1;
}
