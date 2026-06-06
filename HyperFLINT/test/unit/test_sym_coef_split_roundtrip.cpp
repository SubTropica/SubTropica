// HF MZV-rewrite Phase A commit (5): SymCoefSplit basic-op +
// round-trip tests against SymCoef. Bit-identity is enforced via
// canonical to_string equality.
//
// Cases mirror the design v2 §5.1 Phase-A corpus expanded to carry
// transcendental factors (Pi, I, Log, delta).

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/symcoef.hpp"
#include "hyperflint/core/sym_coef_split.hpp"
#include "hyperflint/core/zw_table.hpp"

#include <iostream>
#include <map>
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

// Build a SymCoef given a list of (Rat, transcendentals) tuples.
SymCoef make_symcoef(const PolyCtx& F,
                     const std::vector<SymMonomial>& mons) {
    return SymCoef::from_monomials(F, std::vector<SymMonomial>(mons));
}

bool roundtrip_one(const std::string& name,
                   const PolyCtx& F,
                   const PolyCtx& N,
                   const std::vector<SymMonomial>& mons) {
    SymCoef src = make_symcoef(F, mons);
    auto tab = std::make_shared<ZWTable>(F);
    SymCoefSplit split = SymCoefSplit::from_rat(src, N, tab);
    SymCoef recon = split.as_rat();

    bool ok = (src.to_string() == recon.to_string());
    std::cout << "[" << (ok ? "OK " : "FAIL") << "] roundtrip: " << name
              << " src_terms=" << src.terms().size()
              << " split_terms=" << split.terms().size()
              << " recon_terms=" << recon.terms().size() << "\n";
    if (!ok) {
        std::cout << "    src   = " << src.to_string()   << "\n";
        std::cout << "    recon = " << recon.to_string() << "\n";
        g_pass = false;
    }
    return ok;
}

void test_roundtrip_pure_rat() {
    PolyCtx F({"x", "y", "s"});
    PolyCtx N({"x", "y"});

    SymMonomial m1(Rat::parse(F, "(x + y) / (x - 1)"));
    SymMonomial m2(Rat::parse(F, "s*x / (s + 1)"));
    roundtrip_one("pure_rat_mixed_NW", F, N, {m1, m2});
}

void test_roundtrip_with_pi() {
    PolyCtx F({"x", "s"});
    PolyCtx N({"x"});

    SymMonomial m1(Rat::parse(F, "x"));
    m1.pi_power = 2;
    SymMonomial m2(Rat::parse(F, "s + x"));
    m2.pi_power = 1;
    m2.i_power = 1;
    roundtrip_one("with_pi_and_i", F, N, {m1, m2});
}

void test_roundtrip_with_log_and_delta() {
    PolyCtx F({"x", "y", "s"});
    PolyCtx N({"x", "y"});

    SymMonomial m1(Rat::parse(F, "x*y / (x + 1)"));
    m1.log_powers[2] = 1;     // Log[2]^1
    SymMonomial m2(Rat::parse(F, "s"));
    m2.delta_powers["x"] = 1;
    SymMonomial m3(Rat::parse(F, "x + s"));
    m3.log_powers[3] = 2;
    m3.delta_powers["y"] = 1;
    roundtrip_one("with_log_and_delta", F, N, {m1, m2, m3});
}

void test_roundtrip_collapse_like_terms() {
    PolyCtx F({"x", "s"});
    PolyCtx N({"x"});

    // Two SymMonomials with identical transcendental key and the
    // same (after split) den_zw + num_zw should collapse to one
    // canonical leaf in SymCoefSplit, then re-emit as a single
    // collapsed Rat in SymCoef::canonicalize.
    SymMonomial m1(Rat::parse(F, "x"));
    m1.pi_power = 1;
    SymMonomial m2(Rat::parse(F, "2*x"));  // same key as m1, num_N adds
    m2.pi_power = 1;
    roundtrip_one("collapse_like_terms", F, N, {m1, m2});
}

void test_arithmetic_neg_add() {
    PolyCtx F({"x", "s"});
    PolyCtx N({"x"});

    SymMonomial m1(Rat::parse(F, "x*s"));
    m1.pi_power = 1;
    SymCoef src = make_symcoef(F, {m1});
    auto tab = std::make_shared<ZWTable>(F);
    SymCoefSplit s = SymCoefSplit::from_rat(src, N, tab);
    SymCoefSplit nl = s.neg();
    SymCoef recon_neg = nl.as_rat();
    SymCoef expected_neg = src.neg();
    check(expected_neg.to_string() == recon_neg.to_string(),
          "neg: SymCoefSplit::neg matches SymCoef::neg via round-trip");

    // s + (-s) = 0
    SymCoefSplit zero = s.add(nl);
    SymCoef recon_zero = zero.as_rat();
    check(recon_zero.is_zero(),
          "add: s + neg(s) = 0 (canonical zero)");
}

void test_roundtrip_hetero_trans_shared_den() {
    // Reviewer round 3 missing case: multiple SymMonomials in a
    // single SymCoef whose splits share `den_zw` and have DIFFERENT
    // `num_zw` AND DIFFERENT transcendentals. Stresses the
    // SplitKey collapse logic in canonicalize.
    PolyCtx F({"x", "s12", "s23"});
    PolyCtx N({"x"});

    // Three SymMonomials, all with the same Rat denominator, but
    // different W-monomials in the numerator and different
    // transcendental flags.
    SymMonomial m1(Rat::parse(F, "s12 / (x - 1)"));
    m1.pi_power = 1;
    SymMonomial m2(Rat::parse(F, "s23 / (x - 1)"));
    m2.pi_power = 2;
    SymMonomial m3(Rat::parse(F, "s12*s23 / (x - 1)"));
    m3.log_powers[2] = 1;
    roundtrip_one("hetero_trans_shared_den", F, N, {m1, m2, m3});
}

void test_arithmetic_mul_rat() {
    PolyCtx F({"x", "y", "s"});
    PolyCtx N({"x", "y"});

    SymMonomial m1(Rat::parse(F, "x"));
    SymCoef src = make_symcoef(F, {m1});
    auto tab = std::make_shared<ZWTable>(F);
    SymCoefSplit sp = SymCoefSplit::from_rat(src, N, tab);

    Rat r = Rat::parse(F, "(s + y) / (x + 1)");
    SymCoefSplit prod = sp.mul_rat(r);
    SymCoef recon = prod.as_rat();
    SymCoef expected = src.mul_rat(r);
    check(expected.to_string() == recon.to_string(),
          "mul_rat: SymCoefSplit::mul_rat matches SymCoef::mul_rat");
}

// B1.a (HF MZV-rewrite Phase B commit (1) sub-commit a): mul(SymCoefSplit)
// + canonical-form-equality predicate. Five cases per the scoping memo
// notes/hf_mzv_rewrite_design_2026-05-05/b1_scoping_memo.md:
//   (i)   two W-side-trivial inputs (pure narrow-ctx Rats),
//   (ii)  one trivial × one non-trivial (W-side present on one side),
//   (iii) two non-trivial (W-side present on both sides),
//   (iv)  zero × non-trivial,
//   (v)   from_rat(a) * from_rat(b) matches from_rat(a*b) on canonical
//         form (the round-trip-equivalence sanity check).
//
// Each case compares SymCoefSplit::mul → as_rat against the source
// SymCoef::mul of the lifted operands; the canonical-form equality
// predicate equals_canonical is also exercised.

void test_mul_two_trivial() {
    // (i) Both operands are W-side trivial (only narrow vars in num_F);
    // the SCS mul reduces to pure narrow-ctx Poly multiplication on
    // num_N with num_zw=ZW_ONE on every leaf.
    PolyCtx F({"x", "y", "s"});
    PolyCtx N({"x", "y"});

    SymMonomial ma(Rat::parse(F, "(x + y) / (x - 1)"));
    SymMonomial mb(Rat::parse(F, "x*y / (y + 1)"));
    SymCoef sa_sym = make_symcoef(F, {ma});
    SymCoef sb_sym = make_symcoef(F, {mb});
    auto tab = std::make_shared<ZWTable>(F);
    SymCoefSplit sa = SymCoefSplit::from_rat(sa_sym, N, tab);
    SymCoefSplit sb = SymCoefSplit::from_rat(sb_sym, N, tab);

    SymCoefSplit prod = sa.mul(sb);
    SymCoef recon   = prod.as_rat();
    SymCoef expected = sa_sym.mul(sb_sym);
    check(expected.to_string() == recon.to_string(),
          "mul: two W-trivial operands round-trip vs SymCoef::mul");
}

void test_mul_trivial_x_nontrivial() {
    // (ii) One operand is W-side trivial (pure narrow Rat); the other
    // carries Mandelstam dependence in the numerator.
    PolyCtx F({"x", "s12", "s23"});
    PolyCtx N({"x"});

    SymMonomial ma(Rat::parse(F, "x / (x - 1)"));
    SymMonomial mb(Rat::parse(F, "(s12 + s23) / (x + 1)"));
    SymCoef sa_sym = make_symcoef(F, {ma});
    SymCoef sb_sym = make_symcoef(F, {mb});
    auto tab = std::make_shared<ZWTable>(F);
    SymCoefSplit sa = SymCoefSplit::from_rat(sa_sym, N, tab);
    SymCoefSplit sb = SymCoefSplit::from_rat(sb_sym, N, tab);

    SymCoefSplit prod = sa.mul(sb);
    SymCoef recon   = prod.as_rat();
    SymCoef expected = sa_sym.mul(sb_sym);
    check(expected.to_string() == recon.to_string(),
          "mul: trivial × non-trivial round-trip vs SymCoef::mul");
}

void test_mul_two_nontrivial() {
    // (iii) Both operands carry W-side dependence; the SCS mul exercises
    // the ZWTable::multiply path on both num_zw and den_zw handles
    // simultaneously. Both operands also carry transcendental factors,
    // so the per-pair pi_power / i_power / log_powers / delta_powers
    // merge logic is exercised end-to-end. The (i_power = 1) × (i_power
    // = 1) pairing additionally hits the I^2 = -1 sign-fold branch.
    PolyCtx F({"x", "y", "s12", "s23"});
    PolyCtx N({"x", "y"});

    SymMonomial ma1(Rat::parse(F, "(s12 + x) / (y + 1)"));
    ma1.pi_power = 1;
    ma1.i_power  = 1;
    ma1.log_powers[2] = 1;
    SymMonomial ma2(Rat::parse(F, "(s23 - x) / (y - 1)"));
    ma2.delta_powers["x"] = 1;

    SymMonomial mb1(Rat::parse(F, "(s12 - s23) / (x + 1)"));
    mb1.pi_power = 1;
    mb1.i_power  = 1;
    SymMonomial mb2(Rat::parse(F, "s12*s23 / (x - 1)"));
    mb2.log_powers[3] = 2;
    mb2.delta_powers["x"] = 1;

    SymCoef sa_sym = make_symcoef(F, {ma1, ma2});
    SymCoef sb_sym = make_symcoef(F, {mb1, mb2});
    auto tab = std::make_shared<ZWTable>(F);
    SymCoefSplit sa = SymCoefSplit::from_rat(sa_sym, N, tab);
    SymCoefSplit sb = SymCoefSplit::from_rat(sb_sym, N, tab);

    SymCoefSplit prod = sa.mul(sb);
    SymCoef recon   = prod.as_rat();
    SymCoef expected = sa_sym.mul(sb_sym);
    check(expected.to_string() == recon.to_string(),
          "mul: two non-trivial operands round-trip vs SymCoef::mul "
          "(I^2 sign-fold + delta mod 2 + log sum)");
}

void test_mul_zero_x_nontrivial() {
    // (iv) Empty SymCoefSplit (canonical zero) times a non-trivial
    // operand: result must be empty (canonical zero) on both sides.
    PolyCtx F({"x", "s"});
    PolyCtx N({"x"});

    SymCoef sb_sym = make_symcoef(
        F, {SymMonomial(Rat::parse(F, "(s + x) / (x - 1)"))});
    auto tab = std::make_shared<ZWTable>(F);
    SymCoefSplit zero(F, N, tab);  // empty terms_
    SymCoefSplit sb   = SymCoefSplit::from_rat(sb_sym, N, tab);

    SymCoefSplit lhs_prod = zero.mul(sb);
    SymCoefSplit rhs_prod = sb.mul(zero);
    check(lhs_prod.is_zero(),
          "mul: zero × non-trivial yields canonical zero (lhs)");
    check(rhs_prod.is_zero(),
          "mul: non-trivial × zero yields canonical zero (rhs)");
    check(lhs_prod.as_rat().is_zero(),
          "mul: zero × non-trivial as_rat is zero SymCoef");
}

void test_mul_from_rat_compat() {
    // (v) from_rat(a) * from_rat(b) must canonicalize equal to
    // from_rat(a * b) for any pair of source SymCoefs (a, b). This is
    // the round-trip-equivalence sanity check that hooks B1.c's
    // verifier site at the as_rat() boundary.
    PolyCtx F({"x", "y", "s"});
    PolyCtx N({"x", "y"});

    SymMonomial ma(Rat::parse(F, "(x + s) / (y - 1)"));
    ma.pi_power = 2;
    SymMonomial mb(Rat::parse(F, "(y - s) / (x + 1)"));
    mb.log_powers[5] = 1;

    SymCoef sa_sym = make_symcoef(F, {ma});
    SymCoef sb_sym = make_symcoef(F, {mb});
    auto tab = std::make_shared<ZWTable>(F);
    SymCoefSplit sa = SymCoefSplit::from_rat(sa_sym, N, tab);
    SymCoefSplit sb = SymCoefSplit::from_rat(sb_sym, N, tab);

    SymCoefSplit lhs = sa.mul(sb);
    SymCoef ab_sym = sa_sym.mul(sb_sym);
    SymCoefSplit rhs = SymCoefSplit::from_rat(ab_sym, N, tab);

    check(lhs.equals_canonical(rhs),
          "mul: from_rat(a)*from_rat(b) ≡ from_rat(a*b) "
          "(equals_canonical)");
    // Belt-and-braces: the SymCoef-level reconstitutions must also
    // agree, in case equals_canonical's notion of equality is too
    // narrow.
    check(lhs.as_rat().to_string() == rhs.as_rat().to_string(),
          "mul: from_rat(a)*from_rat(b) round-trip = "
          "from_rat(a*b) round-trip (string equality)");
}

}  // namespace

int main() {
    test_roundtrip_pure_rat();
    test_roundtrip_with_pi();
    test_roundtrip_with_log_and_delta();
    test_roundtrip_collapse_like_terms();
    test_roundtrip_hetero_trans_shared_den();
    test_arithmetic_neg_add();
    test_arithmetic_mul_rat();

    // B1.a (HF MZV-rewrite Phase B commit (1) sub-commit a).
    test_mul_two_trivial();
    test_mul_trivial_x_nontrivial();
    test_mul_two_nontrivial();
    test_mul_zero_x_nontrivial();
    test_mul_from_rat_compat();

    std::cout << "\n[summary] " << (g_pass ? "PASS" : "FAIL") << "\n";
    return g_pass ? 0 : 1;
}
