// Value-gate for partial_fractions_factored_den (A2, HF perf campaign).
//
// The overload computes the partial-fraction decomposition of
//   num / (den_base)^mult   in `var`
// WITHOUT ever forming the expanded den_base^mult (the parse-time pow_fps
// wall, campaign findings F3). Correctness is asserted two ways for every
// fixture:
//
//   (1) RECONSTRUCTION: got == polynomial_part + sum_k coefs[k-1]/(var-a)^k
//       must equal the original num/den_base^mult (path-independent value
//       identity; a wrong factorial / mis-evaluated residue / dropped pole
//       all break it).
//   (2) EQUIVALENCE to the STANDARD path: the same structure and value as
//       partial_fractions(Rat(num, den_base^mult), var, ...). In the coprime
//       single-linear-pole regime A2 reproduces the standard reduced result
//       exactly; the safe-degrade fixtures route through that very path so the
//       equality is definitional there.
//
// Win fixtures (W*) exercise the deferred fast path; degrade fixtures (D*)
// exercise each safe-degrade guard (non-linear-in-var den_base, deg_var(num)
// >= mult, and gcd(num, den_base) != 1).

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

// VALUE-equality of two rationals, independent of representation (Rat is in
// lowest polynomial terms but not content-canonical). Cross-multiply and test
// the DIFFERENCE is the zero polynomial: a.num*b.den - b.num*a.den == 0. This
// is robust to overall sign/content differences (a negative-leading-coefficient
// den_base, which the real qbox faces have, can flip the sign of both sides
// of an `equal()` comparison; diff-is-zero is immune). [physics review
// 2026-06-15.]
bool rat_equal(const Rat& a, const Rat& b) {
    return a.num().mul(b.den()).sub(b.num().mul(a.den())).is_zero();
}

Rat var_minus(const Poly& var_poly, const Rat& a) { return Rat(var_poly) - a; }

Rat rat_pow(const Rat& base, long e) {
    Rat acc = base;
    for (long k = 1; k < e; ++k) acc = acc * base;
    return acc;
}

// Reconstruct f from a PF result and compare to the original rational.
bool reconstruct_equals(const PartialFractionization& res, const Rat& f,
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

// Structural + value equivalence of two PF results (order-sensitive: both come
// from the same factor order, so poles line up index-for-index).
bool pf_equivalent(const PartialFractionization& a,
                   const PartialFractionization& b) {
    if (!rat_equal(a.polynomial_part, b.polynomial_part)) return false;
    if (a.poles.size() != b.poles.size()) return false;
    for (size_t i = 0; i < a.poles.size(); ++i) {
        const auto& pa = a.poles[i];
        const auto& pb = b.poles[i];
        if (pa.multiplicity != pb.multiplicity) return false;
        if (!rat_equal(pa.pole, pb.pole)) return false;
        if (pa.coefs.size() != pb.coefs.size()) return false;
        for (size_t k = 0; k < pa.coefs.size(); ++k)
            if (!rat_equal(pa.coefs[k], pb.coefs[k])) return false;
    }
    return true;
}

// Run both paths on (num, den_base, mult) and assert reconstruction + the
// equivalence to the standard partial_fractions(Rat(num, den_base^mult)).
void run_fixture(const std::string& tag, const Poly& num,
                 const Poly& den_base, long mult, size_t var_idx,
                 long expect_poles, long expect_total_mult) {
    const PolyCtx& ctx = num.ctx();
    auto zw = std::make_shared<ZWTable>(ctx);

    Rat f(Poly(num), den_base.pow(static_cast<unsigned long>(mult)));

    PartialFractionization got =
        partial_fractions_factored_den(num, den_base, mult, var_idx, zw);
    PartialFractionization ref =
        partial_fractions(f, var_idx, zw, false);

    check(reconstruct_equals(got, f, var_idx),
          tag + ": got reconstructs to num/den_base^mult");
    check(reconstruct_equals(ref, f, var_idx),
          tag + ": ref reconstructs to num/den_base^mult");
    check(pf_equivalent(got, ref),
          tag + ": deferred == standard (structure + value)");

    if (expect_poles >= 0)
        check(static_cast<long>(got.poles.size()) == expect_poles,
              tag + ": expected pole count");
    if (expect_total_mult >= 0) {
        long tm = 0;
        for (const auto& p : got.poles) tm += p.multiplicity;
        check(tm == expect_total_mult, tag + ": expected total multiplicity");
    }
}

}  // namespace

int main() {
    // ---- W1: win regime. Single rational pole P/Q with Q non-constant,
    // mult 5 (qbox-like q=5), deg_var(num)=2 < 5, coprime. Fast path. -------
    {
        PolyCtx ctx({"x", "y", "z"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly z = Poly::gen(ctx, 2);
        Poly one = Poly::one_of(ctx);
        // den_base = z*x - y  (linear in x, content 1, pole y/z).
        Poly den_base = z.mul(x).sub(y);
        // num = x^2 + y z + z^2 (deg_var 2 < 5), coprime to den_base.
        Poly num = x.mul(x).add(y.mul(z)).add(z.mul(z));
        run_fixture("W1", num, den_base, 5, 0, /*poles*/ 1, /*mult*/ 5);
    }

    // ---- W2: Q == 1 (polynomial pole), mult 3, deg_var(num)=2 < 3. --------
    {
        PolyCtx ctx({"x", "y"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly two = Poly::from_int(ctx, 2);
        Poly den_base = x.sub(y);                 // x - y, pole y, Q=1
        Poly num = x.mul(x).add(two.mul(y));      // x^2 + 2y (coprime, deg 2<3)
        run_fixture("W2", num, den_base, 3, 0, 1, 3);
    }

    // ---- W3: den_base carries a var-free CONTENT (z) not shared with num,
    // so the standard Rat ctor leaves multiplicity = mult; content_base = z.
    {
        PolyCtx ctx({"x", "y", "z"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly z = Poly::gen(ctx, 2);
        Poly den_base = z.mul(x.sub(y));          // z*(x - y) = z x - z y
        Poly num = x.add(y.mul(y));               // x + y^2 (deg 1 < 4), coprime
        run_fixture("W3", num, den_base, 4, 0, 1, 4);
    }

    // ---- W4: mult == 1 (degenerate; reduces to ordinary PF of num/den_base).
    {
        PolyCtx ctx({"x", "y"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly den_base = x.sub(y);
        Poly num = Poly::from_int(ctx, 1);        // 1/(x-y)
        run_fixture("W4", num, den_base, 1, 0, 1, 1);
    }

    // ---- W5: NEGATIVE leading coefficient in var (the real qbox shape: its
    // denominator base starts with a unary minus), single rational pole, mult 3.
    // Guards the sign/content path of the residue algebra and the content-robust
    // value-equality. [physics review 2026-06-15.]
    {
        PolyCtx ctx({"x", "y", "z"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly z = Poly::gen(ctx, 2);
        // den_base = -z*x - y  (negative leading coeff in x), pole -y/z.
        Poly den_base = z.mul(x).add(y).neg();
        Poly num = x.add(y.mul(z)).add(z.mul(z));   // coprime, deg 1 < 3
        run_fixture("W5", num, den_base, 3, 0, 1, 3);
    }

    // ---- D1: deg_var(num) >= mult -> genuine polynomial part -> safe-degrade.
    {
        PolyCtx ctx({"x", "y"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly two = Poly::from_int(ctx, 2);
        Poly den_base = x.sub(y);                 // (x - y)^2
        // num = x^3 + 2x + y, deg_var 3 >= mult 2 -> degrade; ref has a
        // non-trivial polynomial part. Equivalence still asserted.
        Poly num = x.pow(3).add(two.mul(x)).add(y);
        run_fixture("D1", num, den_base, 2, 0, /*poles*/ 1, /*mult*/ 2);
    }

    // ---- D2: gcd(num, den_base) != 1 (num shares the pole factor) ->
    // safe-degrade; the standard reduction lowers the multiplicity. -------
    {
        PolyCtx ctx({"x", "y"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly den_base = x.sub(y);                 // den = (x-y)^3
        // num = (x - y) * (x + y) shares (x - y): reduced f = (x+y)/(x-y)^2.
        Poly num = x.sub(y).mul(x.add(y));
        // ref multiplicity reduces to 2; just assert equivalence (no fixed
        // pole/mult expectation -> pass -1).
        run_fixture("D2", num, den_base, 3, 0, -1, -1);
    }

    // ---- D3: den_base NOT linear in var (degree 2) -> safe-degrade. ------
    {
        PolyCtx ctx({"x", "y"});
        Poly x = Poly::gen(ctx, 0);
        Poly y = Poly::gen(ctx, 1);
        Poly one = Poly::one_of(ctx);
        // den_base = (x - y)(x + 1) = x^2 + (1-y)x - y, degree 2 in x.
        Poly den_base = x.sub(y).mul(x.add(one));
        Poly num = x.add(y.mul(y));               // coprime, deg 1
        // den_base^2 has two distinct poles each mult 2 -> ref poles = 2.
        run_fixture("D3", num, den_base, 2, 0, /*poles*/ 2, /*mult*/ 4);
    }

    if (g_failures == 0) {
        std::cout << "test_partial_fractions_factored_den: ALL PASS\n";
        return 0;
    }
    std::cerr << "test_partial_fractions_factored_den: " << g_failures
              << " FAIL\n";
    return 1;
}
