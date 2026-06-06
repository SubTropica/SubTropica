// Phase-A round-trip test for the rat_split adapter
// (notes/hf_mzv_rewrite_design_2026-05-05/design.md §3.5, §5.1).
//
// Asserts that for each fixture Rat r over a wide ctx F = N ⊕ W:
//   recombine_rat_split(split_rat_by_w_monomial(r, N, tab, F→N), F, tab, N→F)
// produces a Rat bit-equal to r (string-canonical).
//
// Fixtures cover the design §5.1 "synthetic adversarial" corpus:
//   - linear-in-N (Feynman-only, no W);
//   - linear-in-W (Mandelstam-only);
//   - Wm/Wp bilinear (both Wm[1] and Wp[1] live in W);
//   - cross-term (a · x · s12  +  b · y · m^2);
//   - sign-fold (Rat with negative-leading denominator);
//   - W-only-in-denominator (denominator is `s12 - 1`, num touches no W);
//   - empty cases (0/1 and 1/1).

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/rat_split.hpp"
#include "hyperflint/core/zw_table.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace hyperflint;

namespace {

struct Case {
    std::string name;
    std::vector<std::string> F_vars;
    std::vector<std::string> N_vars;
    std::string num_str;
    std::string den_str;
};

// Build a Rat from a "num", "den" pair of strings over the wide ctx F.
Rat make_rat(const PolyCtx& F,
             const std::string& num,
             const std::string& den) {
    return Rat(Poly(F, num), Poly(F, den));
}

bool one_case(const Case& c) {
    PolyCtx F(c.F_vars);
    PolyCtx N(c.N_vars);

    Rat r = make_rat(F, c.num_str, c.den_str);
    ZWTable tab(F);
    FNIndexMaps maps = build_fn_index_maps(F, N);

    auto parts = split_rat_by_w_monomial(r, N, tab, maps.F_to_N_idx);
    Rat r_recon = recombine_rat_split(parts, F, tab, maps.N_to_F_idx);

    const std::string& src_str   = r.to_string();
    const std::string& recon_str = r_recon.to_string();

    bool ok = (src_str == recon_str);
    std::cout << "[" << (ok ? "OK " : "FAIL")
              << "] " << c.name
              << " parts=" << parts.size()
              << " zw_size=" << tab.size()
              << " zw_intern_calls=" << tab.intern_calls()
              << "\n";
    if (!ok) {
        std::cout << "    src   = " << src_str   << "\n";
        std::cout << "    recon = " << recon_str << "\n";
    }
    return ok;
}

}  // namespace

int main() {
    const std::vector<Case> cases = {
        {
            "linear_in_N (no W)",
            {"x", "y", "s12"},
            {"x", "y"},
            "x + y",
            "1",
        },
        {
            "linear_in_W (Mandelstam-only)",
            {"x", "s12"},
            {"x"},
            "s12",
            "1",
        },
        {
            "Wm_Wp_bilinear",
            {"x", "y", "s12", "Wm1", "Wp1"},
            {"x", "y"},
            "Wm1*Wp1*x + s12*y",
            "1",
        },
        {
            "Cross_term",
            {"x", "y", "s12", "m"},
            {"x", "y"},
            "3*x*s12 + 5*y*m^2",
            "1",
        },
        {
            "Den_is_W_only",
            // Denominator is purely in W (no N dependence). Numerator
            // is N-only. A naive adapter would lose the W-side den;
            // adversarial-reviewer 2026-05-06 flagged this as the
            // edge case to verify.
            {"x", "y", "s12"},
            {"x", "y"},
            "x*y",
            "s12 - 1",
        },
        {
            "Sign_fold",
            // Constructed to exercise the sign-canonicalization path.
            // Rat(num,den) ctor will canonicalize this internally,
            // but we still re-check the round-trip after construction.
            {"x", "s12"},
            {"x"},
            "s12*x",
            "x - s12",
        },
        {
            "Empty_zero",
            {"x", "y"},
            {"x", "y"},
            "0",
            "1",
        },
        {
            "Empty_one",
            {"x", "y"},
            {"x", "y"},
            "1",
            "1",
        },
        {
            "Den_mixed_N_W_cross",
            {"x", "s12"},
            {"x"},
            "x",
            "x + s12",
        },
        {
            "Higher_degree_W",
            {"x", "y", "s12", "m"},
            {"x", "y"},
            "s12^2*m*x + s12*m^2*y",
            "1",
        },
        {
            "Multi_term_num_shared_den",
            {"t1", "t2", "s12"},
            {"t1", "t2"},
            "t1^2 + 2*t1*t2 + t2^2 + s12*t1",
            "t1 + t2 + 1",
        },
        // Reviewer round 3 additions:
        {
            // Source numerator with explicit pre-cancellation. After
            // Rat(num,den) ctor's reduce_inplace, the source
            // canonicalizes BEFORE the splitter sees it. This guards
            // against a regression where canonicalize loses a term
            // (e.g., 1*x*s12 - 1*x*s12 + 1 -> 1).
            "Pre_cancellation_in_num",
            {"x", "s12"},
            {"x"},
            "1 - x*s12 + x*s12",  // canonicalizes to "1"
            "x - 1",
        },
        {
            // Denominator that factors over W only into a non-trivial
            // product. Phase A interns the denominator whole; the
            // round-trip must preserve the W-factored shape.
            "Den_factors_over_W_only",
            {"x", "s12", "s23", "Wm1"},
            {"x"},
            "x",
            "(s12 - 1)*(s23 + Wm1)",  // = s12*s23 + s12*Wm1 - s23 - Wm1
        },
        {
            // Reviewer's third missing case: "multiple SymMonomials
            // in a single SymCoef" is at the SymCoef level (test in
            // test_sym_coef_split_roundtrip.cpp). Here at the rat
            // level, the analog is multiple monomials with
            // hetero-W content sharing the same denominator.
            "Hetero_W_shared_den",
            {"x", "s12", "s23"},
            {"x"},
            "s12 + s23 + s12*s23 + 1",
            "x + s12",
        },
        {
            // Reviewer Q4: explicit sign-canon assertion. The
            // round-trip path goes through Rat(num,den) ctor which
            // SHOULD sign-canonicalize (positive leading coef on
            // denominator). If it does not, the splitter's sign-fold
            // path (`den_F.leading_coef_is_negative()` branch) is
            // unreachable on production traffic. We construct a
            // negative-leading-coef denominator string and verify
            // the round-trip still works (and the splitter's sign
            // fold either fires or is benign).
            "Sign_canon_neg_den",
            {"x", "s"},
            {"x"},
            "x",
            "-1*s + x",   // negative leading coef on `s` (after FLINT
                          // canonical ordering); reduce_inplace will
                          // flip both sides.
        },
    };

    int n_pass = 0, n_fail = 0;
    for (const auto& c : cases) {
        if (one_case(c)) ++n_pass; else ++n_fail;
    }

    std::cout << "\n[summary] passed " << n_pass
              << " / " << (n_pass + n_fail) << "\n";
    return n_fail == 0 ? 0 : 1;
}
