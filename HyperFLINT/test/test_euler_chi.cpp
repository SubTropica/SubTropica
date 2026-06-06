// Euler-characteristic counter (DK port) — oracle battery.
//
// Fixtures lifted verbatim from the validated Mathematica reference
// (scripts/doppiofubini/doppio/t20_cleared_dlog.wl, itself pinned to the
// PLD-database chi values):
//
//   (0) staircase combinatorics units (incl. positive-dim + unit ideal);
//   (1) msolve roundtrip on a tiny zero-dim ideal;
//   (2) m=1 Lee-Pomeransky box  G = x1+x2+x3+x4 + s x1x3 + t x2x4:
//       generic total 3; on-locus: s -> 1, s+t -> 2, fake s+7t -> 3;
//   (3) par_generic_zero cubic: generic 13; the NONLINEAR Kallen-type
//       constraint D3 -> 12 (exercises the Diophantine matched-prime
//       branch); fake linear constraint -> 13;
//   (4) exponent-value independence (the cleared-dlog point): two draws
//       on the box and on the charted split pair {U, F}|_{x4=1} agree.
//
// Run directly; exit 0 = pass.  Requires the `msolve` binary on PATH
// (or HF_MSOLVE_PATH).
#include "hyperflint/algebra/euler_chi.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using hyperflint::ChiCount;
using hyperflint::ChiStatus;

static int failures = 0;
#define CHECK(label, cond)                                            \
    do {                                                              \
        if (cond) {                                                   \
            std::printf("  ok    %s\n", label);                       \
        } else {                                                      \
            std::printf("  FAIL  %s\n", label);                       \
            ++failures;                                               \
        }                                                             \
    } while (0)

static std::string msolve_path()
{
    const char* p = std::getenv("HF_MSOLVE_PATH");
    return p ? p : "msolve";
}

int main()
{
    // ---- (0) staircase units ----
    {
        using V = std::vector<std::vector<unsigned long>>;
        ChiCount a = hyperflint::chi_staircase_count(V{{2, 0}, {0, 2}}, 2);
        CHECK("staircase {x^2, y^2} -> 4",
            a.status == ChiStatus::Finite && a.count == 4);
        ChiCount b = hyperflint::chi_staircase_count(V{{1, 1}}, 2);
        CHECK("staircase {x*y} -> positive-dim",
            b.status == ChiStatus::PositiveDim);
        ChiCount c = hyperflint::chi_staircase_count(V{{0, 0}}, 2);
        CHECK("staircase unit ideal -> 0",
            c.status == ChiStatus::Finite && c.count == 0);
        ChiCount d = hyperflint::chi_staircase_count(
            V{{3, 0}, {1, 1}, {0, 2}}, 2);
        // standard monomials: 1, x, x^2, y, xy? no: xy is a lead.
        // staircase of {x^3, xy, y^2}: {1, x, x^2, y} -> 4
        CHECK("staircase {x^3, x*y, y^2} -> 4",
            d.status == ChiStatus::Finite && d.count == 4);
    }

    // ---- (1) msolve roundtrip ----
    {
        auto leads = hyperflint::msolve_leading_exps(
            {"x^2+y", "y^2-3"}, {"x", "y"}, 65521, msolve_path());
        bool ok = leads.has_value() && leads->size() == 2;
        if (ok) {
            ChiCount c = hyperflint::chi_staircase_count(*leads, 2);
            ok = c.status == ChiStatus::Finite && c.count == 4;
        }
        CHECK("msolve {x^2+y, y^2-3} mod 65521 -> 4 standard monomials", ok);
    }

    const std::string box = "x1+x2+x3+x4+s*x1*x3+t*x2*x4";
    const std::vector<std::string> v4{"x1", "x2", "x3", "x4"};
    const std::vector<std::string> stp{"s", "t"};

    // ---- (2) box oracle ----
    {
        ChiCount g = hyperflint::chi_count_sectors(
            {box}, {101}, v4, stp, "0", 20260604, msolve_path());
        CHECK("box generic total == 3",
            g.status == ChiStatus::Finite && g.count == 3);
        ChiCount cs = hyperflint::chi_count_sectors(
            {box}, {101}, v4, stp, "s", 20260605, msolve_path());
        CHECK("box on s == 1",
            cs.status == ChiStatus::Finite && cs.count == 1);
        ChiCount cst = hyperflint::chi_count_sectors(
            {box}, {101}, v4, stp, "s+t", 20260606, msolve_path());
        CHECK("box on s+t == 2",
            cst.status == ChiStatus::Finite && cst.count == 2);
        ChiCount cf = hyperflint::chi_count_sectors(
            {box}, {101}, v4, stp, "s+7*t", 20260607, msolve_path());
        CHECK("box on fake s+7t == 3 (no drop)",
            cf.status == ChiStatus::Finite && cf.count == 3);
    }

    // ---- (3) par_generic_zero oracle (nonlinear Diophantine branch) ----
    {
        const std::string gpg =
            "x1*x3+x1*x4+x2*x3+x2*x4+x3*x4"
            "-m1*x1^2*x3-m1*x1^2*x4+(-m1-m2+s)*x1*x2*x3+(-m1-m2+s)*x1*x2*x4"
            "-m3*x1*x3^2+(-m1-m3-m4)*x1*x3*x4-m4*x1*x4^2-m2*x2^2*x3"
            "-m2*x2^2*x4-m3*x2*x3^2+(-m2-m3-m4)*x2*x3*x4-m4*x2*x4^2"
            "-m3*x3^2*x4-m4*x3*x4^2";
        const std::vector<std::string> pgp{"m1", "m2", "m3", "m4", "s"};
        ChiCount g = hyperflint::chi_count_sectors(
            {gpg}, {757}, v4, pgp, "0", 20260608, msolve_path());
        CHECK("pg generic total == 13",
            g.status == ChiStatus::Finite && g.count == 13);
        const std::string d3 =
            "m1^2-2*m1*m3-2*m1*m4+m3^2-2*m3*m4+m4^2";
        ChiCount cd = hyperflint::chi_count_sectors(
            {gpg}, {757}, v4, pgp, d3, 20260609, msolve_path());
        CHECK("pg on NONLINEAR D3 == 12 (chi drop)",
            cd.status == ChiStatus::Finite && cd.count == 12);
        ChiCount cf = hyperflint::chi_count_sectors(
            {gpg}, {757}, v4, pgp, "m1+7*m2-3*s", 20260610, msolve_path());
        CHECK("pg on fake m1+7m2-3s == 13 (no drop)",
            cf.status == ChiStatus::Finite && cf.count == 13);
    }

    // ---- (4) exponent-value independence ----
    {
        ChiCount e1 = hyperflint::chi_count_sectors(
            {box}, {7}, v4, stp, "0", 20260611, msolve_path());
        ChiCount e2 = hyperflint::chi_count_sectors(
            {box}, {104729}, v4, stp, "0", 20260612, msolve_path());
        CHECK("box exponent independence (7 vs 104729)",
            e1.status == ChiStatus::Finite &&
            e2.status == ChiStatus::Finite && e1.count == e2.count &&
            e1.count == 3);
        const std::vector<std::string> v3{"x1", "x2", "x3"};
        const std::string uc = "x1+x2+x3+1";
        const std::string fc = "s*x1*x3+t*x2";
        ChiCount s1 = hyperflint::chi_count_sectors(
            {uc, fc}, {103, 211}, v3, stp, "0", 20260613, msolve_path());
        ChiCount s2 = hyperflint::chi_count_sectors(
            {uc, fc}, {3001, 65537}, v3, stp, "0", 20260614, msolve_path());
        CHECK("split-pair exponent independence + value == 3 (Mma t20 log)",
            s1.status == ChiStatus::Finite &&
            s2.status == ChiStatus::Finite && s1.count == s2.count &&
            s1.count == 3);
    }

    std::printf(failures ? "EULER-CHI: %d FAILURES\n" : "EULER-CHI: ALL PASS\n",
        failures);
    return failures ? 1 : 0;
}
