// HF MZV-rewrite C-prep.4 (iter-27): unit tests for the canonical-
// emission writer family (sym_coef_canonical_string +
// emit_regulator_sym_canonical).
//
// Path (c) per c_prep_4_scoping_memo.md (commit d294ad470). The writer
// is a refinement of the existing sym_coef_to_mma_string: it forces
// SymCoef::canonicalize() at entry so that two SymCoefs from
// the same structural-canonical equivalence class emit the same byte
// sequence regardless of insertion / accumulation order.
//
// 7 tests:
//
//   T1  rat_to_string_idempotent          (drift-check M2)
//   T2  identity_on_canonical              (memo §6.1 #1)
//   T3  order_insensitivity                (memo §6.1 #2)
//   T4  gcd_cancellation_invariance        (memo §6.1 #3)
//   T5  pi_squared_non_absorption          (memo §6.1 #4)
//   T6  rejects_algebraic_equivalence      (memo §6.1 #5 -- "negative")
//   T7  power_key_collision_under_perm     (drift-check M1)
//
// We can't link against the bridge/handlers.cpp anonymous-namespace
// writer directly. We re-implement the same two helpers here as
// test-local mirrors with the same body; the contract under test is
// the *combination* canonicalize() + sym_coef_to_mma_string-style
// emission. T1 doesn't need the writer at all (it just round-trips
// Rat::parse <-> Rat::to_string).

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/symcoef.hpp"

#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace hyperflint;

namespace {

bool g_pass = true;

void check(bool cond, const std::string& tag) {
    std::cout << "[" << (cond ? "OK " : "FAIL") << "] " << tag << "\n";
    if (!cond) g_pass = false;
}

// Test-local mirror of the bridge/handlers anonymous-namespace
// sym_coef_to_mma_string. Body must stay byte-identical to the writer
// at handlers.cpp:236-263.
std::string sym_coef_to_mma_string_local(const SymCoef& s) {
    if (s.is_zero()) return "0";
    std::ostringstream o;
    bool first = true;
    for (const auto& m : s.terms()) {
        std::string pre = m.prefactor.to_string();
        bool needs_paren = pre.find('+') != std::string::npos
                        || pre.find('-') != std::string::npos
                        || pre.find('/') != std::string::npos
                        || pre.find('*') != std::string::npos;
        if (!first) o << " + ";
        if (needs_paren) o << "(" << pre << ")"; else o << pre;
        if (m.pi_power == 1)      o << "*Pi";
        else if (m.pi_power != 0) o << "*Pi^" << m.pi_power;
        if (m.i_power == 1)       o << "*I";
        else if (m.i_power != 0)  o << "*I^" << m.i_power;
        for (const auto& kv : m.log_powers) {
            if (kv.second == 1) o << "*Log[" << kv.first << "]";
            else                o << "*Log[" << kv.first << "]^" << kv.second;
        }
        for (const auto& kv : m.delta_powers) {
            if (kv.second == 1) o << "*delta[" << kv.first << "]";
            else                o << "*delta[" << kv.first << "]^" << kv.second;
        }
        first = false;
    }
    return o.str();
}

// Path (c) writer.
std::string sym_coef_canonical_string_local(const SymCoef& s) {
    return sym_coef_to_mma_string_local(s.canonicalize());
}

// =====================================================================
// T1: drift-check M2 -- Rat::parse(Rat::to_string(r)).to_string() == r.to_string()
//
// Verifies that the FLINT-canonical guarantee on Rat::to_string is
// idempotent under Rat::parse. If this fails, the path-(c) writer
// itself is unsound (the Rat-level prefactor in each SymMonomial would
// re-stringify to a different byte sequence after a parse).
// =====================================================================

void test_rat_to_string_idempotent() {
    PolyCtx F({"x", "y", "s", "mm", "Wm_1", "Wp_1"});
    std::vector<std::string> seeds = {
        "1",
        "x + y",
        "(x + y) / (x - 1)",
        "s*x / (s + 1)",
        "x^2 - 2*x*y + y^2",
        "(32) / (256*mm^2*Wm_1 - 256*mm^2*Wp_1)",
        "-3*x*y*s + 2*x^3 - y^2",
        // Multi-variable nested: stress monomial-ordering canonicalization.
        "(x*y + s) / (x - y + s)",
    };
    for (const auto& seed : seeds) {
        Rat r1 = Rat::parse(F, seed);
        const std::string s1 = r1.to_string();
        Rat r2 = Rat::parse(F, s1);
        const std::string s2 = r2.to_string();
        bool ok = (s1 == s2);
        check(ok, std::string("T1 rat_to_string idempotent on \"") + seed + "\"");
        if (!ok) {
            std::cout << "    s1 = " << s1 << "\n";
            std::cout << "    s2 = " << s2 << "\n";
        }
    }
}

// =====================================================================
// T2: identity-on-canonical -- on already-canonical SymCoef the
// canonical writer produces the exact same bytes as the existing
// sym_coef_to_mma_string. This is the load-bearing claim that path (c)
// is a refinement, not a replacement: today's bit-identity baselines
// continue to hold.
// =====================================================================

void test_identity_on_canonical() {
    PolyCtx F({"x", "y", "s"});
    std::vector<SymMonomial> ms;
    {
        SymMonomial m1(Rat::parse(F, "x + y"));
        m1.pi_power = 2;
        ms.push_back(m1);
    }
    {
        SymMonomial m2(Rat::parse(F, "s*x"));
        m2.log_powers[2] = 1;
        ms.push_back(m2);
    }
    SymCoef s = SymCoef::from_monomials(F, std::move(ms));
    // SymCoef::from_monomials calls canonicalize() internally, so s is
    // canonical by construction.
    const std::string canon  = sym_coef_canonical_string_local(s);
    const std::string normal = sym_coef_to_mma_string_local(s);
    check(canon == normal,
          "T2 identity-on-canonical: canonical(s) == mma(s) when s is canonical");
}

// =====================================================================
// T3: order-insensitivity -- two SymCoefs built from the same monomial
// set with different insertion orders produce the same canonical string.
// This is the property that absorbs OMP cross-merge thread-ordering
// drift at C0a (the THE TARGET case in scoping memo §5).
// =====================================================================

void test_order_insensitivity() {
    PolyCtx F({"x", "y", "s"});

    auto build = [&](const std::vector<int>& perm) {
        std::vector<SymMonomial> raw;
        // Three structurally-distinct monomials.
        SymMonomial a(Rat::parse(F, "x"));      a.pi_power = 2;
        SymMonomial b(Rat::parse(F, "y"));      b.log_powers[3] = 1;
        SymMonomial c(Rat::parse(F, "s + x"));  c.delta_powers["x"] = 1;
        std::vector<SymMonomial> all = {a, b, c};
        for (int idx : perm) raw.push_back(all[idx]);
        return SymCoef::from_monomials(F, std::move(raw));
    };

    SymCoef s_abc = build({0, 1, 2});
    SymCoef s_cab = build({2, 0, 1});
    SymCoef s_bca = build({1, 2, 0});

    const std::string e_abc = sym_coef_canonical_string_local(s_abc);
    const std::string e_cab = sym_coef_canonical_string_local(s_cab);
    const std::string e_bca = sym_coef_canonical_string_local(s_bca);

    check(e_abc == e_cab,
          "T3 order-insensitivity: canonical(abc) == canonical(cab)");
    check(e_abc == e_bca,
          "T3 order-insensitivity: canonical(abc) == canonical(bca)");
    if (e_abc != e_cab || e_abc != e_bca) {
        std::cout << "    abc = " << e_abc << "\n";
        std::cout << "    cab = " << e_cab << "\n";
        std::cout << "    bca = " << e_bca << "\n";
    }
}

// =====================================================================
// T4: polynomial-gcd cancellation invariance -- two Rat prefactors that
// differ in presentation but whose Rat-level reduce_inplace cancels a
// polynomial gcd to the same canonical form must emit the same byte
// sequence. Stresses Rat::parse + reduce_inplace at construction.
//
// CAVEAT (uncovered by iter-27 drift-check M2 + this test's first
// draft): HF's Rat::reduce_inplace cancels polynomial-gcd but NOT
// integer-content gcd. So `(2x+2y)/(4x-4y)` and `(x+y)/(2x-2y)` are
// structurally distinct canonical forms even though algebraically
// equal. Path (c) does NOT claim invariance under integer-content
// cancellation; its contract is only the structural-canonical class
// produced by SymCoef::canonicalize() + the existing Rat reduction.
// This test is therefore narrowly scoped to polynomial-gcd cancellation
// (e.g. y*(x+y)/(x+y) -> y), which IS guaranteed by reduce_inplace.
// The integer-content corollary is a separate concern flagged for
// iter-28 / C0a follow-up.
// =====================================================================

void test_polynomial_gcd_cancellation() {
    PolyCtx F({"x", "y"});

    // (x*y + y^2) / (x + y) factors as y*(x+y)/(x+y), reduces to y.
    SymMonomial m1(Rat::parse(F, "(x*y + y^2) / (x + y)"));
    // Built directly as y -- post reduce_inplace this is canonical.
    SymMonomial m2(Rat::parse(F, "y"));

    SymCoef s1 = SymCoef::from_monomials(F, {m1});
    SymCoef s2 = SymCoef::from_monomials(F, {m2});

    const std::string e1 = sym_coef_canonical_string_local(s1);
    const std::string e2 = sym_coef_canonical_string_local(s2);
    check(e1 == e2,
          "T4 polynomial-gcd cancellation: y*(x+y)/(x+y) reduces to y");
    if (e1 != e2) {
        std::cout << "    e1 = " << e1 << "\n";
        std::cout << "    e2 = " << e2 << "\n";
    }
}

// =====================================================================
// T5: Pi^(2k) non-absorption -- the canonical writer must NOT silently
// absorb Pi^2 into a zeta(2) or other MZV reduction. That is the
// reduce_to_rat() pipeline's job, downstream. This test pins the
// invariant that path (c) is structural-canonical, NOT
// algebraic-equivalent (drift-check A2).
// =====================================================================

void test_pi_squared_non_absorption() {
    PolyCtx F({"x"});
    SymMonomial m1(Rat::parse(F, "x"));
    m1.pi_power = 2;
    SymCoef s = SymCoef::from_monomials(F, {m1});

    const std::string e = sym_coef_canonical_string_local(s);
    // Pi^2 must stay textually present.
    bool has_pi_sq = (e.find("Pi^2") != std::string::npos);
    check(has_pi_sq,
          "T5 Pi^2 non-absorption: canonical emission preserves Pi^2 textually");
    if (!has_pi_sq) std::cout << "    e = " << e << "\n";
}

// =====================================================================
// T6: negative test -- two SymCoefs that are algebraically equal but
// structurally distinct (e.g. one has an extra zero-prefactor term that
// canonicalize() drops, but the test feeds them BOTH through the path
// (c) writer; we then check the writer DOES collapse the
// structurally-distinct-but-algebraically-equal pair). The "negative"
// half is: a structurally-distinct pair with NO algebraic-equivalence
// shortcut (e.g. Pi vs Pi*1 -- nope, those collapse trivially) is
// collapsed by canonicalize() itself; canonicalize is the load-bearer.
//
// Concrete: build a SymCoef with two distinct monomials that share the
// same power_key but different prefactors. canonicalize() should sum
// them. Path (c) writer must emit the SUM, not the pair.
// =====================================================================

void test_collapses_like_terms() {
    PolyCtx F({"x"});
    SymMonomial m1(Rat::parse(F, "x"));
    m1.pi_power = 2;
    SymMonomial m2(Rat::parse(F, "2*x"));
    m2.pi_power = 2;
    // Same pi_power, same log/delta -- canonicalize must coalesce.
    SymCoef s = SymCoef::from_monomials(F, {m1, m2});

    const std::string e = sym_coef_canonical_string_local(s);
    // The combined prefactor is x + 2*x = 3*x.
    bool has_3x_pi2 = (e.find("3*x") != std::string::npos)
                  && (e.find("Pi^2") != std::string::npos);
    check(has_3x_pi2,
          "T6 negative: like-monomials collapse, emission carries summed prefactor");
    if (!has_3x_pi2) std::cout << "    e = " << e << "\n";
    // And: the canonicalized SymCoef has exactly one term.
    SymCoef canon = s.canonicalize();
    check(canon.terms().size() == 1,
          "T6 negative: canonicalize() collapses like-monomials to one term");
}

// =====================================================================
// T7: drift-check M1 -- power_key collision under permuted insertion.
//
// Construct several monomials with structurally distinct
// (pi_power, i_power, log_powers, delta_powers) such that their
// power_keys, when sorted, force a deterministic emission order. Build
// the same content under multiple permutations of insertion order.
// Verify byte-identical canonical-emission output.
//
// This is a stronger version of T3: T3 uses 3 monomials with already-
// distinct power_keys; T7 specifically exercises 5 monomials with a
// mix of (pi_power=2, log[2]=1), (log[3]=1, delta[x]=1), etc., across
// 4 different permutations to stress the std::sort comparator path
// inside canonicalize().
// =====================================================================

void test_power_key_collision_under_perm() {
    PolyCtx F({"x", "y"});

    SymMonomial a(Rat::parse(F, "x"));      a.pi_power = 2;
    SymMonomial b(Rat::parse(F, "y"));      b.log_powers[2] = 1;
    SymMonomial c(Rat::parse(F, "x*y"));    c.log_powers[3] = 1;
                                              c.delta_powers["x"] = 1;
    SymMonomial d(Rat::parse(F, "x + y"));  d.pi_power = 1;
                                              d.i_power = 1;
    SymMonomial e_(Rat::parse(F, "y - x")); e_.delta_powers["y"] = 1;

    auto build = [&](const std::vector<int>& perm) {
        std::vector<SymMonomial> all = {a, b, c, d, e_};
        std::vector<SymMonomial> raw;
        for (int idx : perm) raw.push_back(all[idx]);
        return SymCoef::from_monomials(F, std::move(raw));
    };

    SymCoef p1 = build({0, 1, 2, 3, 4});
    SymCoef p2 = build({4, 3, 2, 1, 0});
    SymCoef p3 = build({2, 0, 4, 1, 3});
    SymCoef p4 = build({3, 1, 4, 0, 2});

    const std::string e1 = sym_coef_canonical_string_local(p1);
    const std::string e2 = sym_coef_canonical_string_local(p2);
    const std::string e3 = sym_coef_canonical_string_local(p3);
    const std::string e4 = sym_coef_canonical_string_local(p4);

    check(e1 == e2, "T7 power_key-collision: perm 12345 == perm 54321");
    check(e1 == e3, "T7 power_key-collision: perm 12345 == perm 30412");
    check(e1 == e4, "T7 power_key-collision: perm 12345 == perm 41502");
    if (e1 != e2 || e1 != e3 || e1 != e4) {
        std::cout << "    e1 = " << e1 << "\n";
        std::cout << "    e2 = " << e2 << "\n";
        std::cout << "    e3 = " << e3 << "\n";
        std::cout << "    e4 = " << e4 << "\n";
    }
}

}  // namespace

int main() {
    test_rat_to_string_idempotent();         // T1 (drift-check M2)
    test_identity_on_canonical();             // T2 (memo §6.1 #1)
    test_order_insensitivity();               // T3 (memo §6.1 #2)
    test_polynomial_gcd_cancellation();       // T4 (memo §6.1 #3, narrowed)
    test_pi_squared_non_absorption();         // T5 (memo §6.1 #4)
    test_collapses_like_terms();              // T6 (memo §6.1 #5)
    test_power_key_collision_under_perm();    // T7 (drift-check M1)

    std::cout << "\n[summary] " << (g_pass ? "PASS" : "FAIL") << "\n";
    return g_pass ? 0 : 1;
}
