// Period-tuples Phase 1 unit tests (spec 2026-06-04 §4 + reviewer
// zero-test stress pair). Direct main, exit 0 = pass.

#include "hyperflint/core/period_table.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/symcoef.hpp"
#include "hyperflint/reduce/mzv_reduce.hpp"
#include "hyperflint/reduce/period_scratch.hpp"
#include "hyperflint/symbols/word.hpp"

#include <iostream>
#include <string>

using namespace hyperflint;

static int g_fail = 0;
static void check(bool c, const std::string& m) {
    if (!c) { std::cerr << "FAIL: " << m << "\n"; ++g_fail; }
}

int main() {
    // 1. Registry round-trip + id stability.
    auto& pt = PeriodTable::instance();
    const auto z2 = pt.id_for("mzv_2");
    const auto z3 = pt.id_for("mzv_3");
    check(z2 != z3, "distinct keys get distinct ids");
    check(pt.id_for("mzv_2") == z2, "id stable on re-intern");
    check(pt.key_for(z3) == "mzv_3", "reverse lookup");

    PolyCtx ctx({"s12", "s23"});
    const Rat s12 = Rat::parse(ctx, "s12");

    // 2. power_key separates period content; equal content collides.
    SymMonomial a{s12};  a.period_powers[z2] = 1; a.period_powers[z3] = 1;
    SymMonomial b{s12};  b.period_powers[z2] = 1; b.period_powers[z3] = 1;
    SymMonomial c{s12};  c.period_powers[z2] = 2;
    check(a.power_key() == b.power_key(), "same periods -> same key");
    check(a.power_key() != c.power_key(), "different periods -> different key");
    check(a.power_hash() == b.power_hash(), "same periods -> same hash");
    check(a.power_hash() != c.power_hash(), "different periods -> different hash");
    SymMonomial p{s12};  // no periods
    check(p.power_key() != c.power_key(), "period-free vs period key differ");

    // 3. Zero-test stress pair (physics review §4): +s12*P2*P3 and
    //    -s12*P2*P3 cancel; one-generator-apart pair does not.
    SymMonomial neg{-s12}; neg.period_powers[z2] = 1; neg.period_powers[z3] = 1;
    SymCoef sum = SymCoef::from_monomials(ctx, {a, neg});
    check(sum.is_zero(), "equal-and-opposite same period monomial cancels");
    SymMonomial other{-s12}; other.period_powers[z2] = 1;
    other.period_powers[pt.id_for("mzv_5")] = 1;
    SymCoef sum2 = SymCoef::from_monomials(ctx, {b, other});
    check(!sum2.is_zero(), "one-generator-apart pair does NOT cancel");
    check(sum2.terms().size() == 2, "both distinct monomials retained");

    // 4. merge_sorted_canonical collects like period monomials.
    SymCoef m1 = SymCoef::from_monomials(ctx, {a});
    SymCoef m2 = SymCoef::from_monomials(ctx, {b});
    SymCoef merged = SymCoef::merge_sorted_canonical(m1, m2);
    check(merged.terms().size() == 1, "like period monomials merge to one");
    check(merged.terms()[0].period_powers.at(z2) == 1, "exponents preserved");

    // 5. Exponent-zero normalization: P^0 entries are erased.
    SymMonomial zexp{s12}; zexp.period_powers[z2] = 0;
    SymCoef zc = SymCoef::from_monomials(ctx, {zexp});
    check(zc.terms().size() == 1 &&
          zc.terms()[0].period_powers.empty(),
          "zero exponent erased by canonicalize");

    // 6. Scratch-ring mint round-trip wiring (Phase 2 bridge): the
    //    weight-1 boundary word {-2} mints to a SymCoef over the SLIM
    //    ctx whose period content lives in period_powers (numeric
    //    prefactors; at least one period id; no kinematic-ctx leak).
    {
        MzvReductionTable table;  // empty table: weight-1 needs no rules
        Word w; w.letters.push_back(Rat::parse(ctx, "-2"));
        SymCoef minted = mint_period_sym(ctx, w, table, /*zero_one=*/false);
        check(!minted.is_zero(), "mint {-2} nonzero");
        bool any_period = false, prefs_numeric = true;
        for (const auto& m : minted.terms()) {
            if (!m.period_powers.empty()) any_period = true;
            if (!m.prefactor.den().used_var_indices().empty() ||
                !m.prefactor.num().used_var_indices().empty())
                prefs_numeric = false;
        }
        check(any_period, "mint carries period_powers content");
        check(prefs_numeric, "mint prefactors numeric over slim ctx");
    }

    // 7. Review folds (B2): period content must not masquerade as a
    //    pure Rat, and reduce_to_rat must refuse residual periods.
    {
        SymMonomial pm{s12};
        pm.period_powers[z2] = 1;
        check(!pm.is_pure_rat(), "period monomial is NOT pure rat");
        SymCoef pc = SymCoef::from_monomials(ctx, {pm});
        MzvReductionTable t2;
        bool threw = false;
        try { (void)reduce_to_rat(pc, t2); } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "reduce_to_rat throws on residual period");
    }

    if (g_fail == 0) { std::cout << "period_tuples: all OK\n"; return 0; }
    return 1;
}
