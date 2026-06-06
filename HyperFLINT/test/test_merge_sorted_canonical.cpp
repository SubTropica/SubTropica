// Tier 1.6a self-test: SymCoef::merge_sorted_canonical(a, b) must
// agree with (a + b).canonicalize() for any pair of canonical-input
// SymCoefs. Returns non-zero on any mismatch.

#include "hyperflint/core/symcoef.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace hyperflint;

namespace {
int g_pass = 0;
int g_fail = 0;

void check(const std::string& tag,
           const SymCoef& fast,
           const SymCoef& reference) {
    const std::string sf = fast.to_string();
    const std::string sr = reference.to_string();
    if (sf == sr) {
        std::cout << "[PASS] " << tag << "  result=" << sf << "\n";
        ++g_pass;
    } else {
        std::cout << "[FAIL] " << tag << "\n  fast: " << sf
                                       << "\n  ref : " << sr << "\n";
        ++g_fail;
    }
}

// Reference: full canonicalize-from-scratch of a + b.
SymCoef ref_add(const SymCoef& a, const SymCoef& b) {
    return a.add(b);
}

}  // namespace

int main() {
    PolyCtx ctx({"x", "y", "mzv_2"});

    const Rat one  = Rat::one_of(ctx);
    const Rat two  = Rat::parse(ctx, "2");
    const Rat mone = Rat::parse(ctx, "-1");
    const Rat px   = Rat::parse(ctx, "x");
    const Rat pxy  = Rat::parse(ctx, "x*y");

    const SymCoef pi  = SymCoef::pi_factor(ctx);
    const SymCoef im  = SymCoef::im_factor(ctx);
    const SymCoef l2  = SymCoef::log_factor(ctx, 2);
    const SymCoef l3  = SymCoef::log_factor(ctx, 3);
    const SymCoef dx  = SymCoef::delta_factor(ctx, "x");
    const SymCoef dy  = SymCoef::delta_factor(ctx, "y");

    // Case 1: empty + nontrivial.
    {
        SymCoef a(ctx);
        SymCoef b = pi.mul_rat(two);
        check("empty + Pi*2",
              SymCoef::merge_sorted_canonical(a, b), ref_add(a, b));
        check("Pi*2 + empty",
              SymCoef::merge_sorted_canonical(b, a), ref_add(b, a));
    }

    // Case 2: empty + empty.
    {
        SymCoef a(ctx);
        SymCoef b(ctx);
        check("empty + empty",
              SymCoef::merge_sorted_canonical(a, b), ref_add(a, b));
    }

    // Case 3: disjoint power_keys.
    {
        SymCoef a = pi.mul_rat(px);
        SymCoef b = l2.mul_rat(pxy);
        check("Pi*x + Log[2]*xy",
              SymCoef::merge_sorted_canonical(a, b), ref_add(a, b));
        check("Log[2]*xy + Pi*x (swap)",
              SymCoef::merge_sorted_canonical(b, a), ref_add(b, a));
    }

    // Case 4: matching power_key, prefactors sum to non-zero.
    {
        SymCoef a = pi.mul_rat(px);
        SymCoef b = pi.mul_rat(pxy);
        check("Pi*x + Pi*xy",
              SymCoef::merge_sorted_canonical(a, b), ref_add(a, b));
    }

    // Case 5: matching power_key, prefactors cancel exactly.
    {
        SymCoef a = pi.mul_rat(px);
        SymCoef b = pi.mul_rat(px).neg();
        check("Pi*x + (-Pi*x) [zero]",
              SymCoef::merge_sorted_canonical(a, b), ref_add(a, b));
    }

    // Case 6: multi-term inputs, mixed disjoint + overlap.
    {
        SymCoef a = pi.mul_rat(px) + l2.mul_rat(one) + im.mul_rat(two);
        SymCoef b = pi.mul_rat(pxy) + l3.mul_rat(one) + im.mul_rat(mone);
        check("(Pi*x + Log[2] + I*2) + (Pi*xy + Log[3] - I)",
              SymCoef::merge_sorted_canonical(a, b), ref_add(a, b));
    }

    // Case 7: I^2 ↔ -1 (the canonical-input invariant requires
    // i_power ∈ {0,1}; both inputs already pre-canonical so the merge
    // never sees i_power = 2). Build via canonicalize first.
    {
        SymCoef a = (im.mul(im)).mul_rat(px);  // I^2 * x — canonicalize → -x
        SymCoef b = im.mul_rat(pxy);            // I*xy
        check("(I*I)*x + I*xy",
              SymCoef::merge_sorted_canonical(a, b), ref_add(a, b));
    }

    // Case 8: delta is mod-2 (delta[x]^2 = 1).
    {
        SymCoef a = dx.mul(dx).mul_rat(px);   // delta[x]^2 * x → x
        SymCoef b = dy.mul_rat(pxy);
        check("delta[x]^2 *x + delta[y]*xy",
              SymCoef::merge_sorted_canonical(a, b), ref_add(a, b));
    }

    // Case 9: cross-product structure.
    {
        SymCoef a = (pi.mul(pi)).mul_rat(px);   // Pi^2 * x
        SymCoef b = (pi.mul(l2)).mul_rat(pxy);  // Pi * Log[2] * xy
        SymCoef c = (l2.mul(l2)).mul_rat(one);  // Log[2]^2
        SymCoef ab = SymCoef::merge_sorted_canonical(a, b);
        SymCoef abc = SymCoef::merge_sorted_canonical(ab, c);
        SymCoef ref = ref_add(ref_add(a, b), c);
        check("(Pi^2*x + Pi*L2*xy) + L2^2 (chained merges)",
              abc, ref);
    }

    // Case 10: many-term self-merge — every term has a unique
    // power_key (no sums collapse to zero), to exercise strict
    // ordering. 6 terms split 3+3.
    {
        SymCoef a =   pi.mul_rat(one)
                    + (pi.mul(im)).mul_rat(two)
                    + l2.mul_rat(px);
        SymCoef b =   l3.mul_rat(pxy)
                    + dx.mul_rat(one)
                    + dy.mul_rat(mone);
        check("six disjoint monomials, 3+3",
              SymCoef::merge_sorted_canonical(a, b), ref_add(a, b));
    }

    // Case 11: heavy partial overlap — 4-term + 4-term with 2 keys
    // colliding (one pair sums to non-zero, one cancels).
    {
        SymCoef a =   pi.mul_rat(px)
                    + l2.mul_rat(pxy)
                    + dx.mul_rat(one)
                    + im.mul_rat(two);
        SymCoef b =   pi.mul_rat(px)         // collides with a's term 0 (sums to 2*Pi*x)
                    + l3.mul_rat(one)
                    + dx.mul_rat(mone)        // collides with a's dx term (cancels)
                    + l2.mul_rat(one);
        check("4+4 with mixed overlap incl. cancellation",
              SymCoef::merge_sorted_canonical(a, b), ref_add(a, b));
    }

    // Case 12: associativity — a+(b+c) vs (a+b)+c using merge.
    {
        SymCoef a = pi.mul_rat(px) + l2.mul_rat(one);
        SymCoef b = pi.mul_rat(pxy) + im.mul_rat(two);
        SymCoef c = pi.mul_rat(px).neg() + dx.mul_rat(one);
        SymCoef left  = SymCoef::merge_sorted_canonical(
            SymCoef::merge_sorted_canonical(a, b), c);
        SymCoef right = SymCoef::merge_sorted_canonical(
            a, SymCoef::merge_sorted_canonical(b, c));
        check("associativity (a+b)+c vs a+(b+c)", left, right);
    }

    std::cout << "\n=== merge_sorted_canonical self-test ==="
              << "\n  passed: " << g_pass
              << "\n  failed: " << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}
