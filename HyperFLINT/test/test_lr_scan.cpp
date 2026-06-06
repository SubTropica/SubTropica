// Doppio-port phase 3 (gauge scan + conic + carry tiers) — oracle battery.
//
// Mathematica cross-pins (t24_gauge_scan.wl + t22/t23):
//   uq5 Strict scan: 0 orders (gauge-exhaustive NOLR at all 5 gauges);
//   uq5 FindRoots scan: 120 orders total, all 5 gauges represented, the
//   collaborator order (x2,x3,x1,x4) present at gauge x5 with
//   carried_sqrts == 2; fr_judge unit verdicts from t23.
//
// Run directly; exit 0 = pass.  msolve only needed for the chi-filter
// variant (not exercised here; the filter has its own battery).
#include "hyperflint/integrator/lr_scan.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <vector>

using namespace hyperflint;
using namespace hyperflint::lr_scan;

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
    // ---- (1) projectivity detection ----
    {
        PolyCtx ctx({"x1", "x2", "x3", "x4", "x5",
                     "qq1", "qq2", "wb1", "wb2", "yb"});
        std::vector<Poly> g;
        g.emplace_back(ctx, "x1+x2+x3");
        g.emplace_back(ctx,
            "-qq1*x1*x2-qq2*x1*x3+2*wb1*x3*x4-x4^2+2*wb2*x2*x5-x5^2"
            "+2*yb*x4*x5");
        std::vector<std::vector<Poly>> gp{g};
        std::vector<size_t> xv{0, 1, 2, 3, 4};
        // (1+2eps)*1 + (-3-eps)*2 = -5 - 0*eps  ==  -n
        CHECK("uq5 physical weights projective",
            projective_input(gp, xv, {{{1, 2}, {-3, -1}}}));
        CHECK("uq5 wrong weights rejected",
            !projective_input(gp, xv, {{{1, 0}, {-2, 0}}}));
        PolyCtx bctx({"x1", "x2", "x3", "x4", "s", "t"});
        std::vector<Poly> bg;
        bg.emplace_back(bctx, "x1+x2+x3+x4+s*x1*x3+t*x2*x4");
        CHECK("inhomogeneous box rejected",
            !projective_input({bg}, {0, 1, 2, 3}, {{{-2, -1}}}));
    }

    // ---- (2) conic_rationalizable units (dpSquareQ semantics) ----
    {
        PolyCtx ctx({"x1", "x4", "s", "qq1"});
        CHECK("A=x4^2, C=1 rationalizable",
            conic_rationalizable(Poly(ctx, "x4^2*x1^2+s*x1+1"), 0));
        CHECK("A=qq1 not a square: rejected",
            !conic_rationalizable(Poly(ctx, "qq1*x1^2+s*x1+1"), 0));
        CHECK("A=4 square, C=9 square: rationalizable",
            conic_rationalizable(Poly(ctx, "4*x1^2+s*x1+9"), 0));
        CHECK("A=2 not a square (sqrt 2 irrational): rejected",
            !conic_rationalizable(Poly(ctx, "2*x1^2+s*x1+1"), 0));
        CHECK("A=-1 not a square: rejected",
            !conic_rationalizable(Poly(ctx, "-x1^2+s*x1+1"), 0));
    }

    // ---- (3) fr_judge units (t23 ports) ----
    {
        PolyCtx ctx({"x1", "x4", "s", "t", "yb", "gg"});
        std::vector<size_t> pend{1};        // x4 pending
        std::vector<size_t> allx{0, 1};
        // zero-disc double line gg*(x1-x4)^2: accept, no counters
        FrJudgment r1 = fr_judge(
            Poly(ctx, "gg*x1^2-2*gg*x1*x4+gg*x4^2"), 0, pend, allx);
        CHECK("zero-disc gg(x1-x4)^2 accepted clean",
            r1.ok && r1.carry.empty() && r1.kin == 0 && r1.term == 0);
        // mixed disc: x1^2 - (s-t)(x4^2-yb): carry the x4 part, kin == 1
        FrJudgment r2 = fr_judge(
            Poly(ctx, "x1^2-s*x4^2+t*x4^2+s*yb-t*yb"), 0, pend, allx);
        CHECK("mixed disc carries x4-part + kin",
            r2.ok && r2.carry.size() == 1 && r2.kin == 1);
        // pure-kinematic disc: x1^2 - (s-t): kin sqrt letter
        FrJudgment r3 = fr_judge(Poly(ctx, "x1^2-s+t"), 0, pend, allx);
        CHECK("pure-kinematic disc -> kin == 1, no carry",
            r3.ok && r3.carry.empty() && r3.kin == 1);
        // terminal quadratic
        FrJudgment r4 = fr_judge(Poly(ctx, "s*x4^2+t*x4+1"), 1, {}, allx);
        CHECK("terminal quadratic -> term == 1", r4.ok && r4.term == 1);
        // deg-3 blocks
        FrJudgment r5 = fr_judge(Poly(ctx, "x1^3-s"), 0, pend, allx);
        CHECK("deg-3 blocks", !r5.ok);
    }

    // ---- (4) uq5 Strict scan: gauge-exhaustive NOLR ----
    PolyCtx uctx({"x1", "x2", "x3", "x4", "x5",
                  "qq1", "qq2", "wb1", "wb2", "yb"});
    std::vector<Poly> ug;
    ug.emplace_back(uctx, "x1+x2+x3");
    ug.emplace_back(uctx,
        "-qq1*x1*x2-qq2*x1*x3+2*wb1*x3*x4-x4^2+2*wb2*x2*x5-x5^2"
        "+2*yb*x4*x5");
    std::vector<std::vector<Poly>> ugp{ug};
    std::vector<size_t> uxv{0, 1, 2, 3, 4};
    std::vector<std::vector<ScanExponent>> uexp{{{1, 2}, {-3, -1}}};
    {
        ScanResult rs = find_lr_orders_scan(ugp, uxv, uexp,
            KeepRule::Strict);
        CHECK("uq5 strict scan ran (projective)", rs.projective);
        CHECK("uq5 strict scan: 0 orders (t24 pin)", rs.orders.empty());
    }

    // ---- (5) uq5 FindRoots scan: the t24 pins ----
    {
        ScanResult rf = find_lr_orders_scan(ugp, uxv, uexp,
            KeepRule::FindRoots);
        CHECK("uq5 FR scan: 120 orders exactly (t24 pin)",
            rf.orders.size() == 120 && !rf.truncated);
        std::set<size_t> gauges;
        for (const auto& o : rf.orders) gauges.insert(o.gauge);
        CHECK("all 5 gauges represented", gauges.size() == 5);
        bool collab = false;
        for (const auto& o : rf.orders) {
            // (x2, x3, x1, x4) at gauge x5: ctx indices (1, 2, 0, 3), 4
            if (o.gauge == 4 &&
                o.order == std::vector<size_t>{1, 2, 0, 3}) {
                collab = true;
                CHECK("collaborator order carried_sqrts == 2 (t22 pin)",
                    o.carried_sqrts == 2);
                break;
            }
        }
        CHECK("collaborator order (x2,x3,x1,x4) @ gauge x5 present",
            collab);
    }

    std::printf(failures ? "LR-SCAN: %d FAILURES\n" : "LR-SCAN: ALL PASS\n",
        failures);
    return failures ? 1 : 0;
}
