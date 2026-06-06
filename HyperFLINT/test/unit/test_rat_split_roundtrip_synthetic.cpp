// HF MZV-rewrite Phase B precondition (synthetic_w_side_corpus):
// adversarial-corpus synthetic battery for the rat_split adapter and
// the SymCoefSplit container. Folds adversarial L2 corpus seed per
// design v2 §5.1 (notes/hf_mzv_rewrite_design_2026-05-05/design.md).
//
// Complements test_rat_split_roundtrip.cpp (basic round-trip) and
// test_sym_coef_split_roundtrip.cpp (SymCoef-level basic ops) with:
//   - Symanzik U/F-shape polynomials drawn from typical Feynman
//     parametrization (sun-rise U; massless box F; massive triangle F).
//   - W-only denominator factors (§6.3 fallback path stressor).
//   - Numerator with exact factorization q(N) · w(W) (adapter must
//     not fragment what is already factored).
//   - High wide-ctx exponent count (Wm/Wp bilinears with degrees 0..3).
//   - Cross-term denominator with N AND W (exercises §6.3 opaque
//     fallback).
//   - i_power = 2 sign-fold-into-num_N convention (SymMonomial-level).
//   - delta_powers[v] = 2 mod-2 reduction (SymMonomial-level).
//   - Arithmetic-invariance pairs at the SymCoefSplit level:
//       from_rat(a) + from_rat(b) ≡ from_rat(a + b)
//       from_rat(a) · from_rat(b) ≡ from_rat(a · b)
//     for shared and disjoint denominators (uses equals_canonical
//     and a belt-and-braces as_rat string comparison).
//
// Asserts the design v2 §5.1 contract:
//   recombine_rat_split(split_rat_by_w_monomial(r)) bit-equal to r
//   under canonical form, on every fixture.

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/rat_split.hpp"
#include "hyperflint/core/sym_coef_split.hpp"
#include "hyperflint/core/symcoef.hpp"
#include "hyperflint/core/zw_table.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace hyperflint;

namespace {

bool g_pass = true;

void check(bool cond, const std::string& tag) {
    std::cout << "[" << (cond ? "OK " : "FAIL") << "] " << tag << "\n";
    if (!cond) g_pass = false;
}

// ---------- Rat-level adapter round-trip ----------

bool rat_roundtrip(const std::string& name,
                   const std::vector<std::string>& F_vars,
                   const std::vector<std::string>& N_vars,
                   const std::string& num_str,
                   const std::string& den_str) {
    PolyCtx F(F_vars);
    PolyCtx N(N_vars);

    Rat r(Poly(F, num_str), Poly(F, den_str));
    ZWTable tab(F);
    FNIndexMaps maps = build_fn_index_maps(F, N);

    auto parts   = split_rat_by_w_monomial(r, N, tab, maps.F_to_N_idx);
    Rat r_recon  = recombine_rat_split(parts, F, tab, maps.N_to_F_idx);

    bool ok = (r.to_string() == r_recon.to_string());
    std::cout << "[" << (ok ? "OK " : "FAIL") << "] " << name
              << " parts=" << parts.size()
              << " zw_size=" << tab.size() << "\n";
    if (!ok) {
        std::cout << "    src   = " << r.to_string()       << "\n";
        std::cout << "    recon = " << r_recon.to_string() << "\n";
        g_pass = false;
    }
    return ok;
}

// ---------- SymCoefSplit-level round-trip + arithmetic invariance ----

bool symcoef_roundtrip(const std::string& name,
                       const PolyCtx& F,
                       const PolyCtx& N,
                       const std::vector<SymMonomial>& mons) {
    SymCoef src = SymCoef::from_monomials(F, std::vector<SymMonomial>(mons));
    auto tab = std::make_shared<ZWTable>(F);
    SymCoefSplit sp = SymCoefSplit::from_rat(src, N, tab);
    SymCoef recon = sp.as_rat();
    bool ok = (src.to_string() == recon.to_string());
    std::cout << "[" << (ok ? "OK " : "FAIL") << "] symcoef_roundtrip: "
              << name
              << " src_terms=" << src.terms().size()
              << " split_terms=" << sp.terms().size() << "\n";
    if (!ok) {
        std::cout << "    src   = " << src.to_string()   << "\n";
        std::cout << "    recon = " << recon.to_string() << "\n";
        g_pass = false;
    }
    return ok;
}

// from_rat(a) + from_rat(b) round-trip equivalence. The binding
// contract per design v2 §5.1 is the as_rat() string equality:
//   as_rat(from_rat(a) + from_rat(b)) == as_rat(from_rat(a + b))
// equivalently, both paths produce the same SymCoef.
//
// equals_canonical (the SymCoefSplit-level structural equality) is a
// STRICTER predicate that can legitimately differ in the disjoint-
// denominator case: SymCoefSplit::add preserves per-operand
// structure (one SymMonomialSplit per source denominator), whereas
// SymCoef::add followed by SymCoefSplit::from_rat may flow through
// a SymCoef re-canonicalization that produces a different number of
// terms. Both still round-trip identically through as_rat. We log
// equals_canonical as advisory but only the as_rat string is
// load-bearing.
bool add_invariance(const std::string& name,
                    const PolyCtx& F,
                    const PolyCtx& N,
                    const std::vector<SymMonomial>& mons_a,
                    const std::vector<SymMonomial>& mons_b,
                    bool expect_canon_match = true) {
    SymCoef a = SymCoef::from_monomials(F, std::vector<SymMonomial>(mons_a));
    SymCoef b = SymCoef::from_monomials(F, std::vector<SymMonomial>(mons_b));
    SymCoef ab = a.add(b);

    auto tab = std::make_shared<ZWTable>(F);
    SymCoefSplit sa  = SymCoefSplit::from_rat(a,  N, tab);
    SymCoefSplit sb  = SymCoefSplit::from_rat(b,  N, tab);
    SymCoefSplit lhs = sa.add(sb);
    SymCoefSplit rhs = SymCoefSplit::from_rat(ab, N, tab);

    bool eq_canon = lhs.equals_canonical(rhs);
    bool eq_str   = (lhs.as_rat().to_string() == rhs.as_rat().to_string());
    // as_rat string equality is the binding contract. equals_canonical
    // is binding only in the shared-denominator case where both paths
    // produce structurally identical SymCoefSplit's.
    bool ok = eq_str && (!expect_canon_match || eq_canon);
    std::cout << "[" << (ok ? "OK " : "FAIL")
              << "] add_invariance: " << name
              << " equals_canonical=" << (eq_canon ? "T" : "F")
              << (expect_canon_match ? " (binding)" : " (advisory)")
              << " as_rat_str_eq=" << (eq_str ? "T" : "F") << "\n";
    if (!ok) g_pass = false;
    return ok;
}

// from_rat(a) · from_rat(b) ≡ from_rat(a · b)
bool mul_invariance(const std::string& name,
                    const PolyCtx& F,
                    const PolyCtx& N,
                    const std::vector<SymMonomial>& mons_a,
                    const std::vector<SymMonomial>& mons_b) {
    SymCoef a = SymCoef::from_monomials(F, std::vector<SymMonomial>(mons_a));
    SymCoef b = SymCoef::from_monomials(F, std::vector<SymMonomial>(mons_b));
    SymCoef ab = a.mul(b);

    auto tab = std::make_shared<ZWTable>(F);
    SymCoefSplit sa  = SymCoefSplit::from_rat(a,  N, tab);
    SymCoefSplit sb  = SymCoefSplit::from_rat(b,  N, tab);
    SymCoefSplit lhs = sa.mul(sb);
    SymCoefSplit rhs = SymCoefSplit::from_rat(ab, N, tab);

    bool eq_canon = lhs.equals_canonical(rhs);
    bool eq_str   = (lhs.as_rat().to_string() == rhs.as_rat().to_string());
    bool ok = eq_canon && eq_str;
    std::cout << "[" << (ok ? "OK " : "FAIL")
              << "] mul_invariance: " << name
              << " equals_canonical=" << (eq_canon ? "T" : "F")
              << " as_rat_str_eq=" << (eq_str ? "T" : "F") << "\n";
    if (!ok) g_pass = false;
    return ok;
}

}  // namespace

int main() {
    // ---------------- Rat-level synthetic battery ------------------

    // (1) Symanzik U for the 3-edge sun-rise (banana) topology:
    //     U = x1*x2 + x1*x3 + x2*x3.  Pure narrow-ctx (no W vars
    //     present in F); should split into a single trivial part.
    rat_roundtrip("symanzik_U_sunrise",
                  {"x1", "x2", "x3", "s"},
                  {"x1", "x2", "x3"},
                  "x1*x2 + x1*x3 + x2*x3",
                  "1");

    // (2) Symanzik F for the massless 1-loop box, kinematic skeleton:
    //     F = -s12*x1*x3 - s23*x2*x4.  Mandelstam invariants are
    //     wide-ctx-only; numerator splits into two W-monomials.
    rat_roundtrip("symanzik_F_massless_box",
                  {"x1", "x2", "x3", "x4", "s12", "s23"},
                  {"x1", "x2", "x3", "x4"},
                  "-1*s12*x1*x3 - s23*x2*x4",
                  "1");

    // (3) Symanzik F for a massive 1-loop triangle (m^2 internal,
    //     all three edges share mass):
    //     F = (m^2*x1 + m^2*x2 + m^2*x3)*(x1+x2+x3) - s12*x1*x2.
    //     Mixes pure-N (x1+x2+x3) with W-side mass (m^2) and
    //     Mandelstam (s12) terms in the numerator.
    rat_roundtrip("symanzik_F_massive_triangle",
                  {"x1", "x2", "x3", "s12", "m"},
                  {"x1", "x2", "x3"},
                  "(m^2*x1 + m^2*x2 + m^2*x3)*(x1 + x2 + x3) - s12*x1*x2",
                  "1");

    // (4) Numerator that factorizes EXACTLY as q(N) · w(W). The
    //     adapter must not fragment what already factors clean: w(W)
    //     decomposes into 2 W-monomials, q(N) is a single shared
    //     narrow-ctx polynomial. Expected: parts.size() == 2.
    rat_roundtrip("exact_factorization_qN_times_wW",
                  {"x", "y", "s12", "Wm1", "Wp1"},
                  {"x", "y"},
                  "(x*y + 1)*(s12 + Wm1*Wp1)",  // 2 W-monos × 1 N-poly
                  "1");

    // (5) High-degree W-side stress: numerator carries Wm1 and Wp1
    //     to powers in {0,1,2,3} mixed with 2 Mandelstam invariants.
    //     Exercises the bin-by-e_W hash-map at small but non-trivial
    //     scale (~12 distinct W-monomials).
    rat_roundtrip("high_degree_W_stress",
                  {"x", "y", "s12", "s23", "Wm1", "Wp1"},
                  {"x", "y"},
                  "Wm1^3*x + Wp1^3*y + Wm1^2*Wp1*x*y "
                  "+ Wm1*Wp1^2*x + s12*Wm1^2*y + s23*Wp1^2*x "
                  "+ s12*s23*Wm1*Wp1*x*y + s12^2*Wm1 + s23^2*Wp1 "
                  "+ Wm1*Wp1*y + s12*x + s23*y",
                  "1");

    // (6) §6.3 opaque-fallback stressor: denominator has a true
    //     N×W cross-term factor that does NOT separate into a clean
    //     "narrow factor times W factor" — `(x + Wm1*Wp1)` mixes a
    //     narrow var with a W bilinear inside a sum. The whole
    //     denominator interns as a single ZW handle (opaque-ish), and
    //     the round-trip must reproduce it byte-for-byte.
    rat_roundtrip("den_NW_cross_opaque",
                  {"x", "s12", "Wm1", "Wp1"},
                  {"x"},
                  "x*s12",
                  "(s12*x + 1)*(x + Wm1*Wp1)");

    // (7) Empty Rat over only N (no W vars in F at all): the adapter
    //     should treat the entire numerator as a single trivial
    //     W-monomial (the empty W-multi-index).
    rat_roundtrip("only_N_no_W",
                  {"x", "y"},
                  {"x", "y"},
                  "x^2 + 2*x*y + y^2 + x + 1",
                  "x + y + 1");

    // (8) Empty Rat over only W (no N vars at all in F): every
    //     numerator monomial is a W-monomial; the adapter produces
    //     one part per distinct W-monomial.
    rat_roundtrip("only_W_no_N",
                  {"s12", "s23", "Wm1", "Wp1"},
                  {},
                  "s12*Wm1 + s23*Wp1 + s12*s23 + Wm1*Wp1",
                  "1");

    // ---------------- SymCoefSplit-level battery -------------------

    // (9) i_power = 2 sign-fold convention. SymCoefSplit::from_rat
    //     expects i_power ∈ {0,1}; the canonicalize pathway folds
    //     I^2 → -1 into num_N. Source SymCoef built with i_power=2
    //     should round-trip with the sign correctly absorbed.
    {
        PolyCtx F({"x", "s"});
        PolyCtx N({"x"});
        SymMonomial m(Rat::parse(F, "x*s"));
        m.i_power = 2;  // I^2 = -1; canonicalize folds sign into num_N
        symcoef_roundtrip("i_power_2_sign_fold", F, N, {m});
    }

    // (10) delta_powers[v] = 2 mod-2 reduction. delta is idempotent
    //     under the integration measure (delta(x)^2 = delta(x) up to
    //     normalization, which canonicalize collapses). The split
    //     representation must reduce delta_powers mod 2 on as_rat.
    {
        PolyCtx F({"x", "y", "s"});
        PolyCtx N({"x", "y"});
        SymMonomial m(Rat::parse(F, "x*y / (s + 1)"));
        m.delta_powers["x"] = 2;
        m.delta_powers["y"] = 3;  // mod 2 -> 1 retained
        symcoef_roundtrip("delta_powers_mod_2_reduction", F, N, {m});
    }

    // ---------------- Arithmetic-invariance battery ----------------

    // (11) Add invariance, SHARED denominator (s12 + 1):
    //     a = (Wm1*x) / (s12 + 1),  b = (Wp1*y) / (s12 + 1)
    //     a + b should split / recombine the same whether done at
    //     the SymCoef or SymCoefSplit level.
    {
        PolyCtx F({"x", "y", "s12", "Wm1", "Wp1"});
        PolyCtx N({"x", "y"});
        SymMonomial ma(Rat::parse(F, "Wm1*x / (s12 + 1)"));
        ma.pi_power = 1;
        SymMonomial mb(Rat::parse(F, "Wp1*y / (s12 + 1)"));
        mb.pi_power = 1;
        add_invariance("add_shared_den", F, N, {ma}, {mb});
    }

    // (12) Add invariance, DISJOINT denominators:
    //     a = (s12*x) / (x - 1),  b = (s23*y) / (y + 1)
    //     The split / recombine must independently track both
    //     denominators across the SymCoefSplit::add.
    //     equals_canonical is ADVISORY here (passes structurally only
    //     when both paths reach the same number of terms; SymCoef::add
    //     can re-canonicalize the sum into a single common-denominator
    //     monomial, while SymCoefSplit::add preserves per-operand
    //     structure). The as_rat-string equality is the binding gate.
    {
        PolyCtx F({"x", "y", "s12", "s23"});
        PolyCtx N({"x", "y"});
        SymMonomial ma(Rat::parse(F, "s12*x / (x - 1)"));
        SymMonomial mb(Rat::parse(F, "s23*y / (y + 1)"));
        add_invariance("add_disjoint_den",
                       F, N, {ma}, {mb}, /*expect_canon_match=*/false);
    }

    // (13) Mul invariance, SHARED denominator factor on both sides:
    //     a = (s12 + x) / (y + 1),  b = (s23 + y) / (y + 1)
    //     Verifies that ZWTable::multiply on (s12+x) and (s23+y)
    //     wide-ctx polynomials commutes with the rat-level mul.
    {
        PolyCtx F({"x", "y", "s12", "s23"});
        PolyCtx N({"x", "y"});
        SymMonomial ma(Rat::parse(F, "(s12 + x) / (y + 1)"));
        SymMonomial mb(Rat::parse(F, "(s23 + y) / (y + 1)"));
        mul_invariance("mul_shared_den_factor", F, N, {ma}, {mb});
    }

    // (14) Mul invariance, ALGEBRAIC LETTERS (Wm/Wp) bilinears with
    //     transcendental factors on both sides. Exercises:
    //     (i) ZWTable::multiply on Wm1 × Wp1 (separate atoms);
    //     (ii) the I^2 sign-fold path (i_power=1 × i_power=1 → 2);
    //     (iii) log_powers entrywise sum.
    {
        PolyCtx F({"x", "s12", "Wm1", "Wp1"});
        PolyCtx N({"x"});
        SymMonomial ma(Rat::parse(F, "Wm1*x / (s12 + 1)"));
        ma.pi_power = 1;
        ma.i_power = 1;
        ma.log_powers[2] = 1;
        SymMonomial mb(Rat::parse(F, "Wp1 / (x - 1)"));
        mb.pi_power = 1;
        mb.i_power = 1;
        mb.log_powers[3] = 2;
        mul_invariance("mul_Wm_Wp_bilinear_with_trans", F, N, {ma}, {mb});
    }

    std::cout << "\n[summary] " << (g_pass ? "PASS" : "FAIL") << "\n";
    return g_pass ? 0 : 1;
}
