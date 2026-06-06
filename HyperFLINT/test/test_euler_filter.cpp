// Euler chi-drop letter filter (Doppio C port, phase 2) — oracle battery.
//
// Fixtures from the validated Mathematica reference:
//   (1) Lee-Pomeransky box (t18 oracle): letters s, t, s+t GENUINE at the
//       full subset; planted fake s+7t FICTITIOUS (dropped).
//   (2) split Symanzik pair {U, F} + boundary augmentation (t20 (3)
//       battery): same verdicts {T, T, T, F} — and ALL factors are
//       homogeneous in the subset vars, so this exercises the
//       C*-quotient auto-chart.
//   (3) uq5 (the first natural in-crawl catch, triple-verified
//       2026-06-03): at S = {x2,x3,x4,x5} of the 5-variable quadruple,
//       bare `yb` is FICTITIOUS; the control letter
//       wb1^2+wb2^2-2*wb1*wb2*yb is GENUINE.
//   (4) boundary-monomial exemption: x1 survives the filter unjudged.
//   (5) find_lr_orders OFF/ON parity on the massless box: identical
//       best order + score (the box table letters are all genuine), and
//       OFF-mode runs with the filter code fully dormant.
//
// Run directly; exit 0 = pass.  Needs `msolve` on PATH.
#include "hyperflint/algebra/euler_filter.hpp"
#include "hyperflint/integrator/lr_search.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using hyperflint::ChiFilterCache;
using hyperflint::Poly;
using hyperflint::PolyCtx;

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

int main()
{
    // ---- (1) LP box, t18 verdicts ----
    {
        PolyCtx ctx({"x1", "x2", "x3", "x4", "s", "t"});
        std::vector<Poly> group;
        group.emplace_back(ctx, "x1+x2+x3+x4+s*x1*x3+t*x2*x4");
        for (const char* b : {"x1", "x2", "x3", "x4"})
            group.emplace_back(ctx, b);
        std::vector<size_t> S{0, 1, 2, 3};
        ChiFilterCache cache;
        CHECK("box: s genuine",
            chi_letter_genuine(group, S, Poly(ctx, "s"), cache));
        CHECK("box: t genuine",
            chi_letter_genuine(group, S, Poly(ctx, "t"), cache));
        CHECK("box: s+t genuine",
            chi_letter_genuine(group, S, Poly(ctx, "s+t"), cache));
        CHECK("box: fake s+7t FICTITIOUS",
            !chi_letter_genuine(group, S, Poly(ctx, "s+7*t"), cache));
    }

    // ---- (2) split pair {U, F} (homogeneous: auto-chart fires) ----
    {
        PolyCtx ctx({"x1", "x2", "x3", "x4", "s", "t"});
        std::vector<Poly> group;
        group.emplace_back(ctx, "x1+x2+x3+x4");
        group.emplace_back(ctx, "s*x1*x3+t*x2*x4");
        for (const char* b : {"x1", "x2", "x3", "x4"})
            group.emplace_back(ctx, b);
        std::vector<size_t> S{0, 1, 2, 3};
        ChiFilterCache cache;
        CHECK("split: s genuine",
            chi_letter_genuine(group, S, Poly(ctx, "s"), cache));
        CHECK("split: t genuine",
            chi_letter_genuine(group, S, Poly(ctx, "t"), cache));
        CHECK("split: s+t genuine",
            chi_letter_genuine(group, S, Poly(ctx, "s+t"), cache));
        CHECK("split: fake s+7t FICTITIOUS",
            !chi_letter_genuine(group, S, Poly(ctx, "s+7*t"), cache));
    }

    // ---- (3) uq5 intermediate yb catch ----
    {
        PolyCtx ctx({"x1", "x2", "x3", "x4", "x5",
                     "qq1", "qq2", "wb1", "wb2", "yb"});
        std::vector<Poly> group;
        group.emplace_back(ctx, "x1+x2+x3");
        group.emplace_back(ctx,
            "-qq1*x1*x2-qq2*x1*x3+2*wb1*x3*x4-x4^2+2*wb2*x2*x5-x5^2"
            "+2*yb*x4*x5");
        for (const char* b : {"x1", "x2", "x3", "x4", "x5"})
            group.emplace_back(ctx, b);
        std::vector<size_t> S{1, 2, 3, 4};   // {x2, x3, x4, x5}
        ChiFilterCache cache;
        CHECK("uq5: bare yb FICTITIOUS at S={x2,x3,x4,x5}",
            !chi_letter_genuine(group, S, Poly(ctx, "yb"), cache));
        CHECK("uq5: wb1^2+wb2^2-2*wb1*wb2*yb GENUINE",
            chi_letter_genuine(group, S,
                Poly(ctx, "wb1^2+wb2^2-2*wb1*wb2*yb"), cache));
    }

    // ---- (4) boundary exemption through the list filter ----
    {
        PolyCtx ctx({"x1", "x2", "x3", "x4", "s", "t"});
        std::vector<Poly> group;
        group.emplace_back(ctx, "x1+x2+x3+x4+s*x1*x3+t*x2*x4");
        for (const char* b : {"x1", "x2", "x3", "x4"})
            group.emplace_back(ctx, b);
        std::vector<size_t> S{0, 1};
        ChiFilterCache cache;
        std::vector<Poly> letters;
        letters.emplace_back(ctx, "x3");        // boundary: exempt
        letters.emplace_back(ctx, "s");         // judged (kept or not)
        auto kept = hyperflint::chi_filter_letters(group, S, letters, cache);
        bool has_x3 = false;
        for (const auto& L : kept)
            if (L.to_string() == Poly(ctx, "x3").to_string()) has_x3 = true;
        CHECK("boundary monomial x3 exempt (survives)", has_x3);
    }

    // ---- (5) find_lr_orders OFF/ON parity on the box ----
    {
        PolyCtx ctx({"x1", "x2", "x3", "x4", "s", "t"});
        std::vector<Poly> group;
        group.emplace_back(ctx, "x1+x2+x3+x4+s*x1*x3+t*x2*x4");
        for (const char* b : {"x1", "x2", "x3", "x4"})
            group.emplace_back(ctx, b);
        std::vector<std::vector<Poly>> groups{group};
        std::vector<size_t> xv{0, 1, 2, 3};

        unsetenv("HF_EULER_FILTER");
        auto off = hyperflint::lr_search::find_lr_orders(groups, xv, false);
        setenv("HF_EULER_FILTER", "1", 1);
        auto on = hyperflint::lr_search::find_lr_orders(groups, xv, false);
        unsetenv("HF_EULER_FILTER");
        CHECK("find_lr_orders OFF/ON: same order",
            off.order == on.order && !off.nolr() && !on.nolr());
        CHECK("find_lr_orders OFF/ON: same score",
            off.score == on.score);
    }

    std::printf(failures ? "EULER-FILTER: %d FAILURES\n"
                         : "EULER-FILTER: ALL PASS\n", failures);
    return failures ? 1 : 0;
}
