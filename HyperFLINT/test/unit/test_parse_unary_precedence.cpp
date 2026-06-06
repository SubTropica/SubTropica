// Regression test for the unary-minus / power precedence bug
// (2026-06-04): '^' must bind TIGHTER than unary minus, matching
// Mathematica/Maple: -f^n == -(f^n). The pre-fix grammar parsed the
// sign into the base ((-f)^n), silently flipping the sign of every
// bare-unary-minus EVEN power. Subtraction counterterms emit exactly
// that shape (-(F)^(-2)); the defect surfaced as -4*zeta3/eps on the
// 2-loop triangle-ladder vs the verified library entry + pySecDec.
//
// Strategy: parse each input under the same ctx as a reference input
// that is unambiguous (explicit -1* coefficient), then compare the
// canonical to_string() of both Exprs. Leaf-level cases compare the
// folded rational directly.
#include "hyperflint/convert/parse.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/convert/expr.hpp"

#include <iostream>
#include <string>
#include <vector>

using hyperflint::convert::parse_expression;

namespace {
int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ \
                  << "  " << #cond << "\n"; \
        ++g_failures; \
    } \
} while (0)

// Parse `a` and `b` with the same vars; compare canonical strings.
bool same_parse(const std::string& a, const std::string& b,
                const std::vector<std::string>& vars) {
    auto ra = parse_expression(a, vars);
    auto rb = parse_expression(b, vars);
    return ra.expr.to_canonical_string() == rb.expr.to_canonical_string();
}
}  // namespace

int main() {
    const std::vector<std::string> xy = {"x", "y"};

    // The bug shape: unary minus + even negative exponent.
    CHECK(same_parse("-(1 + x)^(-2)", "-1*(1 + x)^(-2)", xy));
    // Odd exponent (accidentally correct pre-fix; must stay correct).
    CHECK(same_parse("-(1 + x)^(-3)", "-1*(1 + x)^(-3)", xy));
    // Positive even exponent.
    CHECK(same_parse("-(1 + x)^2", "-1*(1 + x)^2", xy));
    // Unary minus on a bare symbol power: -x^2 == -(x^2), NOT (-x)^2.
    CHECK(same_parse("-x^2", "-1*x^2", xy));
    CHECK(!same_parse("-x^2", "(-x)^2", xy));
    // Parenthesized negative base must keep the old meaning.
    CHECK(same_parse("(-x)^2", "x^2", xy));
    // Binary minus in sums (was always correct; guard it).
    CHECK(same_parse("1 - (1 + x)^(-2)", "1 + (-1)*(1 + x)^(-2)", xy));
    // Multivariate counterterm shape from the tladder face.
    CHECK(same_parse("-(x + y + x*y)^(-2)", "-1*(x + y + x*y)^(-2)", xy));
    // Double unary minus.
    CHECK(same_parse("--x^2", "x^2", xy));
    // Unary minus under multiplication binds the power, not the factor.
    CHECK(same_parse("2*-(1 + x)^(-2)", "-2*(1 + x)^(-2)", xy));

    // A2 (review of d74f8c88a): chained '^' is accepted RIGHT-
    // associatively, matching Mathematica: x^2^3 == x^(2^3) == x^8.
    // (The pre-fix grammar rejected chained '^' outright.)
    CHECK(same_parse("x^2^3", "x^8", xy));

    // A1 (review of d74f8c88a): cross-path agreement. The 'f'-field
    // route (Rat::parse -> FLINT's own precedence) and the 'expr'-field
    // route (parse_expression) are independently implemented; pin their
    // mutual agreement on the bug shape so neither can silently diverge.
    {
        hyperflint::PolyCtx F({"x", "y"});
        hyperflint::Rat r = hyperflint::Rat::parse(F, "-(1+x)^2");
        auto pr = parse_expression("-(1 + x)^2", {"x", "y"});
        CHECK(pr.expr.kind() == hyperflint::convert::ExprKind::Leaf);
        CHECK(pr.expr.leaf_rat().to_string() == r.to_string());
    }

    if (g_failures == 0) std::cout << "test_parse_unary_precedence: all passed\n";
    return g_failures;
}
