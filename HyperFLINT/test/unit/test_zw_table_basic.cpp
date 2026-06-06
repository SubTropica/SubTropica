// HF MZV-rewrite Phase A commit (4): ZWTable arithmetic + dedup +
// merge-determinism unit tests.
// Design ref: notes/hf_mzv_rewrite_design_2026-05-05/design.md
//             §3.3, §3.6a.

#include "hyperflint/algebra/poly_struct_hash.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/zw_table.hpp"
#include "hyperflint/runtime/zw_aggregate.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace hyperflint;

namespace {

bool g_pass = true;

void check(bool cond, const std::string& tag) {
    if (cond) {
        std::cout << "[OK ] " << tag << "\n";
    } else {
        std::cout << "[FAIL] " << tag << "\n";
        g_pass = false;
    }
}

void test_dedup() {
    PolyCtx F({"x", "y", "s"});
    ZWTable tab(F);

    // ZW_ONE should already be set up at handle 0.
    check(tab.size() == 1, "dedup: size after ctor (just ZW_ONE)");
    check(tab.get(ZW_ONE).is_one(),
          "dedup: get(ZW_ONE) is the polynomial 1");

    // Intern the same polynomial twice — should dedup.
    ZWHandle h1 = tab.intern(Poly(F, "x + 1"), ZWTable::Intent::NumIntent);
    ZWHandle h2 = tab.intern(Poly(F, "x + 1"), ZWTable::Intent::NumIntent);
    check(h1 == h2, "dedup: same poly returns same handle");
    check(tab.size() == 2,
          "dedup: size unchanged on second intern of same content");

    // Different poly, different handle.
    ZWHandle h3 = tab.intern(Poly(F, "x + s"),
                             ZWTable::Intent::DenIntent);
    check(h3 != h1 && h3 != ZW_ONE,
          "dedup: distinct content gets distinct handle");
    check(tab.size() == 3, "dedup: size = 3 after distinct content");

    // Re-intern the polynomial 1 — should resolve to ZW_ONE.
    ZWHandle h4 = tab.intern(Poly::one_of(F), ZWTable::Intent::NumIntent);
    check(h4 == ZW_ONE, "dedup: intern(1) returns ZW_ONE");
}

void test_arith() {
    PolyCtx F({"x", "y"});
    ZWTable tab(F);

    ZWHandle hx = tab.intern(Poly(F, "x"), ZWTable::Intent::NumIntent);
    ZWHandle hy = tab.intern(Poly(F, "y"), ZWTable::Intent::NumIntent);

    ZWHandle hxy = tab.multiply(hx, hy);
    check(tab.get(hxy).to_string() == "x*y",
          "arith: multiply(x, y) = x*y");

    // Commutativity via cache: multiply(y, x) hits the same handle.
    ZWHandle hyx = tab.multiply(hy, hx);
    check(hyx == hxy, "arith: multiply commutes via cache");

    // Multiplicative identity: ZW_ONE * h == h, h * ZW_ONE == h.
    check(tab.multiply(ZW_ONE, hx) == hx, "arith: ZW_ONE * x = x");
    check(tab.multiply(hx, ZW_ONE) == hx, "arith: x * ZW_ONE = x");

    // Multiplicative absorbing: ZW_ZERO * h == ZW_ZERO.
    check(tab.multiply(ZW_ZERO, hx) == ZW_ZERO, "arith: 0 * x = 0");
    check(tab.multiply(hx, ZW_ZERO) == ZW_ZERO, "arith: x * 0 = 0");

    // Addition: x + y; cache; identity; cancellation to ZW_ZERO.
    ZWHandle h_xpy = tab.add(hx, hy);
    check(tab.get(h_xpy).to_string() == "x + y",
          "arith: add(x, y) = x + y");
    ZWHandle h_xpy_again = tab.add(hy, hx);
    check(h_xpy == h_xpy_again, "arith: add commutes via cache");

    // h + ZW_ZERO == h.
    check(tab.add(ZW_ZERO, hx) == hx, "arith: 0 + x = x");
    check(tab.add(hx, ZW_ZERO) == hx, "arith: x + 0 = x");

    // negate(x) gives -x; add(x, -x) = ZW_ZERO.
    ZWHandle h_negx = tab.negate(hx);
    check(tab.get(h_negx).to_string() == "-x", "arith: negate(x) = -x");
    ZWHandle h_zero_via_cancel = tab.add(hx, h_negx);
    check(h_zero_via_cancel == ZW_ZERO, "arith: x + (-x) = ZW_ZERO");

    // negate ZW_ZERO is ZW_ZERO.
    check(tab.negate(ZW_ZERO) == ZW_ZERO, "arith: negate(ZW_ZERO) = ZW_ZERO");
}

void test_opaque() {
    PolyCtx F({"x", "y", "s"});
    ZWTable tab(F);

    ZWHandle h_op = tab.intern_opaque(Poly(F, "x*s + y"),
                                       Poly(F, "x - 1"));
    check(ZWTable::is_opaque(h_op), "opaque: is_opaque flag set");
    check((h_op & ~ZW_OPAQUE_BIT) == 0,
          "opaque: first opaque handle has raw idx 0");

    const auto& e = tab.get_opaque(h_op);
    check(e.num_F.to_string() == "x*s + y",
          "opaque: get_opaque preserves num");
    check(e.den_F.to_string() == "x - 1",
          "opaque: get_opaque preserves den");

    // Dedup on (num, den) content.
    ZWHandle h_op2 = tab.intern_opaque(Poly(F, "x*s + y"),
                                        Poly(F, "x - 1"));
    check(h_op2 == h_op, "opaque: dedup on identical (num,den)");

    ZWHandle h_op3 = tab.intern_opaque(Poly(F, "x*s + y"),
                                        Poly(F, "x + 1"));
    check(h_op3 != h_op, "opaque: distinct den => distinct handle");
}

void test_merge_determinism() {
    PolyCtx F({"x", "y", "s"});

    auto build_some = [&](const std::vector<std::string>& strs) {
        ZWTable t(F);
        std::vector<ZWHandle> hs;
        for (const auto& s : strs) {
            hs.push_back(t.intern(Poly(F, s),
                                  ZWTable::Intent::NumIntent));
        }
        return std::make_pair(std::move(t), std::move(hs));
    };

    // Two secondary tables with the SAME content set but different
    // intern orders. Their post-merge byte image into a fresh primary
    // must be identical.
    auto a = build_some({"x + s", "y*s", "x*y", "x + y"});
    auto b = build_some({"x*y", "y*s", "x + y", "x + s"});

    ZWTable primary_a(F);
    auto remap_a = primary_a.merge_into(a.first);
    ZWTable primary_b(F);
    auto remap_b = primary_b.merge_into(b.first);

    check(primary_a.size() == primary_b.size(),
          "merge: same size after merging permuted-secondary");

    // For determinism: walk primary_a's entries and compare strings
    // to primary_b's same-index entries.
    bool entries_match = true;
    for (size_t i = 0; i < primary_a.size(); ++i) {
        if (primary_a.get(static_cast<ZWHandle>(i)).to_string() !=
            primary_b.get(static_cast<ZWHandle>(i)).to_string()) {
            entries_match = false;
            std::cout << "    diff at index " << i
                      << ": A=" << primary_a.get(static_cast<ZWHandle>(i)).to_string()
                      << " vs B=" << primary_b.get(static_cast<ZWHandle>(i)).to_string()
                      << "\n";
            break;
        }
    }
    check(entries_match,
          "merge: entries are byte-identical regardless of intern order");

    // Remap correctness: applying the remap to look up `x + y` in
    // each secondary should land on the same primary handle.
    auto find_xpy_handle = [&](const auto& tab_pair) {
        const auto& strs = (&tab_pair == &a) ?
            std::vector<std::string>{"x + s", "y*s", "x*y", "x + y"} :
            std::vector<std::string>{"x*y", "y*s", "x + y", "x + s"};
        for (size_t i = 0; i < strs.size(); ++i) {
            if (strs[i] == "x + y") return tab_pair.second[i];
        }
        return ZW_ONE;
    };
    ZWHandle src_a = find_xpy_handle(a);
    ZWHandle src_b = find_xpy_handle(b);
    ZWHandle dst_a = remap_a[src_a];
    ZWHandle dst_b = remap_b[src_b];
    check(dst_a == dst_b,
          "merge: remap places same content at same primary handle");
}

// Phase B precondition (2): direct exercise of `poly_struct_compare`,
// the canonical-content comparator that replaces the prior
// `to_string()` lex-tiebreak in `ZWTable::merge_into` (and its opaque
// twin). Property checks: total order on canonical-fmpq_mpoly content
// (reflexive, antisymmetric, distinguishes the four content axes:
// constant, exponent, coefficient, term count). Equality on content
// must NOT depend on the source string (canonicalisation idempotency).
void test_poly_struct_compare() {
    PolyCtx F({"x", "y"});

    Poly p_const_one(F, "1");
    Poly p_const_two(F, "2");

    Poly p_x(F, "x");
    Poly p_2x(F, "2*x");
    Poly p_x2(F, "x^2");
    Poly p_y(F, "y");

    Poly p_xpy(F, "x + y");
    Poly p_xpy_other(F, "y + x");      // canonicalises to "x + y"

    // (a) Reflexivity: cmp(p, p) == 0 across the four axes.
    check(poly_struct_compare(p_x, p_x) == 0,
          "compare: reflexive on x");
    check(poly_struct_compare(p_xpy, p_xpy) == 0,
          "compare: reflexive on x + y");

    // (b) Canonicalisation idempotency: two distinct source strings
    //     that canonicalise to the same fmpq_mpoly compare equal.
    //     Subtle property: `to_string()` of both yields the same
    //     string, so this would also pass under the prior tiebreak,
    //     but it's the precondition the new comparator must preserve.
    check(poly_struct_compare(p_xpy, p_xpy_other) == 0,
          "compare: canonicalisation idempotency (x+y == y+x)");

    // (c) Antisymmetry: cmp(a, b) and cmp(b, a) have opposite signs
    //     for each axis of difference.
    auto opposite_signs = [](int a, int b) {
        return (a < 0 && b > 0) || (a > 0 && b < 0);
    };
    check(opposite_signs(poly_struct_compare(p_const_one, p_const_two),
                         poly_struct_compare(p_const_two, p_const_one)),
          "compare: antisymmetric on constants");
    check(opposite_signs(poly_struct_compare(p_x, p_x2),
                         poly_struct_compare(p_x2, p_x)),
          "compare: antisymmetric on exponent");
    check(opposite_signs(poly_struct_compare(p_x, p_2x),
                         poly_struct_compare(p_2x, p_x)),
          "compare: antisymmetric on coefficient");
    check(opposite_signs(poly_struct_compare(p_x, p_xpy),
                         poly_struct_compare(p_xpy, p_x)),
          "compare: antisymmetric on term count");

    // (d) Distinguishes the four content axes (just check non-zero).
    check(poly_struct_compare(p_const_one, p_const_two) != 0,
          "compare: distinguishes constant scalars");
    check(poly_struct_compare(p_x, p_x2) != 0,
          "compare: distinguishes exponents");
    check(poly_struct_compare(p_x, p_2x) != 0,
          "compare: distinguishes integer coefficients");
    check(poly_struct_compare(p_x, p_xpy) != 0,
          "compare: distinguishes term counts");
    check(poly_struct_compare(p_x, p_y) != 0,
          "compare: distinguishes by variable index");
}

// Iter-33 (C-prep.1) per iter-22 amendment §3.2: per-table counters
// are split into three disjoint buckets so the §6.3 fallback rate is
// measurable even before the predicate goes hot. Verify that:
//   (a) `intern()` increments only `intern_regular_calls()`;
//   (b) `intern_opaque()` increments only `intern_opaque_calls()`;
//   (c) `bump_would_have_been_opaque()` increments only its own bucket;
//   (d) the legacy `intern_calls()` getter returns the SUM of (a)+(b),
//       preserving its existing diagnostic semantics in
//       `test_rat_split_roundtrip.cpp`.
void test_counter_split() {
    PolyCtx F({"x", "y", "s"});
    ZWTable tab(F);

    // Fresh table: ZW_ONE is constructed in the ctor body but does NOT
    // go through the `intern()` accounting path — all three counters
    // start at zero.
    check(tab.intern_regular_calls() == 0,
          "counter_split: intern_regular_calls() == 0 after ctor");
    check(tab.intern_opaque_calls() == 0,
          "counter_split: intern_opaque_calls() == 0 after ctor");
    check(tab.would_have_been_opaque_calls() == 0,
          "counter_split: would_have_been_opaque_calls() == 0 after ctor");
    check(tab.intern_calls() == 0,
          "counter_split: legacy intern_calls() == 0 after ctor");

    // Two regular interns (one of which is a dedup hit).
    tab.intern(Poly(F, "x + 1"), ZWTable::Intent::NumIntent);
    tab.intern(Poly(F, "x + 1"), ZWTable::Intent::NumIntent);
    check(tab.intern_regular_calls() == 2,
          "counter_split: regular bucket counts dedup hits");
    check(tab.intern_opaque_calls() == 0,
          "counter_split: regular intern leaves opaque bucket alone");
    check(tab.would_have_been_opaque_calls() == 0,
          "counter_split: regular intern leaves would-have bucket alone");
    check(tab.intern_calls() == 2,
          "counter_split: legacy intern_calls() == regular + opaque (2+0)");

    // One opaque intern.
    tab.intern_opaque(Poly(F, "x*s + y"), Poly(F, "x - 1"));
    check(tab.intern_regular_calls() == 2,
          "counter_split: opaque intern leaves regular bucket alone");
    check(tab.intern_opaque_calls() == 1,
          "counter_split: opaque bucket counts opaque intern");
    check(tab.would_have_been_opaque_calls() == 0,
          "counter_split: opaque intern leaves would-have bucket alone");
    check(tab.intern_calls() == 3,
          "counter_split: legacy intern_calls() == regular + opaque (2+1)");

    // Three caller-side bumps (the §6.3-predicate-fired-but-took-regular
    // case). Default arg n=1, then explicit n=5.
    tab.bump_would_have_been_opaque();
    tab.bump_would_have_been_opaque(5);
    check(tab.would_have_been_opaque_calls() == 6,
          "counter_split: bump_would_have_been_opaque() default + n=5");
    check(tab.intern_regular_calls() == 2,
          "counter_split: bump leaves regular bucket alone");
    check(tab.intern_opaque_calls() == 1,
          "counter_split: bump leaves opaque bucket alone");
    check(tab.intern_calls() == 3,
          "counter_split: legacy intern_calls() ignores would-have bucket");
}

// Iter-34 (C-prep.1 plumbing): exercises the explicit move ctor +
// move-assignment zero-out invariant introduced alongside the
// destructor-flush-to-aggregate path. Without the explicit zero-out
// the moved-from destructor would re-flush counts that the moved-into
// table will eventually flush itself, double-counting the
// `hf_zw_aggregate:` totals.
//
// Strategy: peek the global aggregate before and after a deliberate
// scope, where we (a) move-construct, (b) move-assign, then let both
// scopes destruct. If the moved-from objects re-flushed, the global
// `total_intern_regular` would observe `2 * built_count` instead of
// `built_count`. The deltas this test asserts therefore directly
// witness that double-counting does not occur.
void test_move_ctor_zero_out() {
    PolyCtx F({"x", "y", "s"});

    std::uint64_t before_regular = 0;
    std::uint64_t before_distinct = 0;
    detail::zw_aggregate_peek(&before_regular, nullptr, nullptr,
                              &before_distinct);

    {
        // Build A with 3 regular interns + 1 opaque intern.
        ZWTable tabA(F);
        tabA.intern(Poly(F, "x + 1"), ZWTable::Intent::NumIntent);
        tabA.intern(Poly(F, "x + s"), ZWTable::Intent::DenIntent);
        tabA.intern(Poly(F, "y + 1"), ZWTable::Intent::NumIntent);
        tabA.intern_opaque(Poly(F, "x*y"), Poly(F, "y - 1"));
        tabA.bump_would_have_been_opaque(2);

        check(tabA.intern_regular_calls() == 3,
              "move_ctor: pre-move regular_calls == 3");
        check(tabA.intern_opaque_calls() == 1,
              "move_ctor: pre-move opaque_calls == 1");
        check(tabA.would_have_been_opaque_calls() == 2,
              "move_ctor: pre-move would_have_been_opaque == 2");

        // Move-construct tabB from tabA.
        ZWTable tabB(std::move(tabA));

        check(tabB.intern_regular_calls() == 3,
              "move_ctor: dst regular_calls preserved across move");
        check(tabB.intern_opaque_calls() == 1,
              "move_ctor: dst opaque_calls preserved across move");
        check(tabB.would_have_been_opaque_calls() == 2,
              "move_ctor: dst would_have_been_opaque preserved across move");
        check(tabA.intern_regular_calls() == 0,
              "move_ctor: src regular_calls zeroed");
        check(tabA.intern_opaque_calls() == 0,
              "move_ctor: src opaque_calls zeroed");
        check(tabA.would_have_been_opaque_calls() == 0,
              "move_ctor: src would_have_been_opaque zeroed");

        // Move-assign tabC <- tabB.
        ZWTable tabC(F);
        tabC.intern(Poly(F, "s + 1"), ZWTable::Intent::DenIntent);
        // tabC pre-move-assign: 1 regular intern; that flushed-on-
        // overwrite contribution will land in the aggregate
        // independently of tabA/B/C's own flushes. We only assert
        // the final state of tabC, not its pre-state in the global.
        tabC = std::move(tabB);
        check(tabC.intern_regular_calls() == 3,
              "move_assign: dst regular_calls preserved");
        check(tabC.intern_opaque_calls() == 1,
              "move_assign: dst opaque_calls preserved");
        check(tabC.would_have_been_opaque_calls() == 2,
              "move_assign: dst would_have_been_opaque preserved");
        check(tabB.intern_regular_calls() == 0,
              "move_assign: src regular_calls zeroed");

        // Scope exit destructs tabA (zeroed; no-op flush), tabB
        // (zeroed; no-op flush), tabC (flushes 3 / 1 / 2).
    }

    std::uint64_t after_regular = 0;
    std::uint64_t after_distinct = 0;
    detail::zw_aggregate_peek(&after_regular, nullptr, nullptr,
                              &after_distinct);

    // tabC contributes 3 regular intern calls (originally tabA's).
    // tabC's pre-move-assign self had 1 regular intern that was
    // flushed at the move-assignment site (operator= flush-then-
    // overwrite path). Total regular delta: 3 + 1 = 4. Without the
    // zero-out invariant the moved-from objects would re-flush their
    // pre-zero counts, blowing the delta to 3+3+1+1+... and the
    // assertion would fire.
    check((after_regular - before_regular) == 4,
          "move_ctor: aggregate regular delta == 4 (no double-count)");
    // distinct entries: tabC ends up with tabA's table (4 entries:
    // ZW_ONE + 3 distinct interns); tabC's pre-move-assign self had
    // 2 entries (ZW_ONE + 1 intern). 4 + 2 = 6.
    check((after_distinct - before_distinct) == 6,
          "move_ctor: aggregate distinct delta == 6 (no double-count)");
}

}  // namespace

int main() {
    test_dedup();
    test_arith();
    test_opaque();
    test_merge_determinism();
    test_poly_struct_compare();
    test_counter_split();
    test_move_ctor_zero_out();
    std::cout << "\n[summary] " << (g_pass ? "PASS" : "FAIL") << "\n";
    return g_pass ? 0 : 1;
}
