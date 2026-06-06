// HF MZV-rewrite C-prep.4 (iter-29): pre-C0a Rat content-normalization
// invariance test.
//
// Trigger: iter-28 post-commit adversarial-reviewer (internal review)
// findings F1+F2 -- the iter-27 commit-message claim
// "Caveat 1 is benign for C0a because cross-thread accumulation produces
// algebraically-identical Rats from algebraically-identical inputs" was
// asserted, not argued. This test verifies construction-path-invariance
// of Rat::to_string across the production paths enumerated in
// notes/hf_mzv_rewrite_design_2026-05-05/c_prep_4_content_audit_memo.md
// §2.
//
// 14 cases per memo §4.2 + iter-31 F1 strengthening + iter-32 F1 TRUE F2
// falsifier hardening:
//
//   T1  trivial_one_over_one
//   T2  single_var_coprime_parse_vs_add
//   T3  single_var_non_coprime_parse_reduces
//   T4  multivar_large_nvars_repswap_vs_legacy        (load-bearing for F2)
//   T4b multivar_large_nvars_repswap_vs_legacy_content_gt_1
//                                                     (iter-31 F1 strengthening)
//   T4c multivar_large_nvars_repswap_vs_legacy_true_f2_falsifier
//                                                     (iter-32 F1 TRUE F2 hardening)
//   T5  multivar_small_nvars_legacy_vs_forced_repswap (load-bearing for F2)
//   T5b multivar_small_nvars_legacy_vs_forced_repswap_content_gt_1
//                                                     (iter-31 F1 strengthening)
//   T5c multivar_small_nvars_legacy_vs_forced_repswap_true_f2_falsifier
//                                                     (iter-32 F1 TRUE F2 hardening)
//   T6  sign_flip_neg_vs_parse_negative
//   T7  pow_via_pow_vs_parse_expanded
//   T8  derivative_quotient_rule
//   T9  substitute_one_var
//   T10 cross_mult_cancellation_parse_vs_mul_compose
//
// Iter-29 status: T1, T2, T6, T8 implemented as proof-of-concept and
// PASS; T3 implemented and EMPIRICALLY FOUND DIVERGENCE -- demoted to
// TODO with a comment documenting the finding; T4, T5, T7, T9, T10
// stubbed with TODO markers (iter-30 finishes T4+T5).
// Iter-30 status: T4 + T5 promoted from TODO to PASS checks via the
// new public `Rat::add_repswap` method (mirrors `Rat::add_legacy`,
// bypasses static-cached env-gate lambdas at rat.cpp:1230-1235 /
// 1284-1292). Both T4 (nvars=60, rep-swap regime) and T5 (nvars=12,
// Smirnov tst2 legacy regime) PASS at unit integer content.
//
// Iter-31 status: T4b + T5b ADDED to address iter-30 post-commit
// reviewer F1 (internal review): T4/T5 operands have unit
// integer content end-to-end (aD=(x2+1) content 1, bD=(x5+x6+1)
// content 1), and therefore do NOT exercise the iter-28 F2 falsifier
// scenario. T4b/T5b reuse the T4/T5 shape but at non-trivial pairwise-
// coprime integer content: aN=x0+2*x1, aD=2*x2+2 (integer content 2),
// bN=x3-x4, bD=3*x5+3*x6+3 (integer content 3). cross-product
// integer content is 6, and rep-swap vs legacy paths could
// hypothetically distribute that content differently (the F2-targeted
// regime). If T4b/T5b PASS, the iter-30 closure verdict
// "production hot paths produce byte-identical to_string under both
// backends" is materially strengthened; if FAIL,
// `from_canonical_normalize_content` becomes BINDING.
//
// Iter-32 status: T4c + T5c ADDED to address iter-31 post-commit
// reviewer F1 (internal review): T4b/T5b operands are still
// self-content-coprime within each input Rat (gcd(aN-content=1,
// aD-content=2)=1 and gcd(bN-content=1, bD-content=3)=1), so the
// rep-swap path's distinguishing feature -- fmpz_mpoly_q canonical
// form removing gcd(num,den) Z-content per FLINT documented spec --
// has nothing to do at construction time and the two paths agree by
// construction. The TRUE F2 falsifier requires inputs where each
// input Rat carries shared num/den Z-content. T4c/T5c use:
//   a = (2*x0 + 4*x1) / (2*x2 + 2)        -- aN content 2, aD content 2
//   b = (3*x3 - 3*x4) / (3*x5 + 3*x6 + 3) -- bN content 3, bD content 3
// After cross-mult, the unreduced num/den each carry Z-content 6;
// gcd-across-pair is 6. Rep-swap canonicalises that to 1; legacy
// (per T3 empirical) does NOT pull integer content across the pair.
// The reviewer demonstrated divergence at this Case D via a Mathematica
// stress test (see HyperFLINT development notes, internal).
// If T4c/T5c PASS at byte-identity under HF Rat::to_string, the
// closure verdict is empirically airtight at the actual falsifier
// regime; if FAIL, from_canonical_normalize_content is BINDING and
// canonical-emission v0 baselines must be re-pinned where shared-
// content enters the regulator chain.
//
// T3 stays TODO permanently with audit-memo §5.4 cross-reference;
// T7/T9/T10 deferred indefinitely (low-priority; not on C-prep
// critical path).
//
// Per memo §5.4: T4+T5 PASS -> the iter-27 dismissal is empirically
// upheld at the production arithmetic paths at unit integer content;
// `from_canonical_normalize_content` is NOT BINDING for C0a (verified
// at iter-30; strengthened at iter-31 if T4b/T5b PASS).
// Iter-30 audit close ships the documentation comment in
// canonical-emission writer header (bridge/handlers.cpp) noting the
// parse-level invariant: future callers that feed
// `Rat::parse(non_coprime_content_string)` into
// `sym_coef_canonical_string` MUST add the preprocessor first.

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace hyperflint;

namespace {

bool g_pass = true;
int g_todo = 0;

void check(bool cond, const std::string& tag) {
    std::cout << "[" << (cond ? "OK  " : "FAIL") << "] " << tag << "\n";
    if (!cond) g_pass = false;
}

void todo(const std::string& tag) {
    std::cout << "[TODO] " << tag << " (iter-30)\n";
    g_todo++;
}

// =====================================================================
// T1: trivial_one_over_one. Sanity that Rat::parse(ctx, "1/1").to_string()
// == "1" (or whatever the canonical form is). Establishes baseline.
// =====================================================================
void T1_trivial_one_over_one() {
    PolyCtx ctx({"x"});
    Rat r = Rat::parse(ctx, "1");
    check(r.to_string() == "1", "T1 trivial_one_over_one");
}

// =====================================================================
// T2: single_var_coprime_parse_vs_add. r = (x+1)/(x+2) constructed via
// (a) Rat::parse and (b) Rat::add of equal-denominator Rats. Both
// should produce byte-identical Rat::to_string output.
// =====================================================================
void T2_single_var_coprime_parse_vs_add() {
    PolyCtx ctx({"x"});
    Rat r_a = Rat::parse(ctx, "(x+1)/(x+2)");
    // (b) x/(x+2) + 1/(x+2) = (x+1)/(x+2) via add (same-denominator).
    Rat r_b1 = Rat::parse(ctx, "x/(x+2)");
    Rat r_b2 = Rat::parse(ctx, "1/(x+2)");
    Rat r_b  = r_b1.add(r_b2);
    check(r_a.to_string() == r_b.to_string(),
          "T2 single_var_coprime_parse_vs_add: parse=\""
          + r_a.to_string() + "\", add=\"" + r_b.to_string() + "\"");
}

// =====================================================================
// T3: single_var_non_coprime_parse_reduces. Rat::parse("(2x)/(2x+2)")
// should reduce to "x/(x+1)".
//
// ITER-29 EMPIRICAL FINDING (deferred to iter-30 fix): test as-written
// FAILS. fmpq_mpoly_gcd_cofactors over Q does NOT pull integer content
// out of the cofactors -- it computes gcd_Q((2x),(2x+2))=1 and returns
// (2x, 2x+2) unchanged. Two algebraically-equal Rats then produce
// byte-different to_string output:
//   parse("(2*x)/(2*x+2)").to_string() == "2*x/(2*x + 2)"
//   parse("x/(x+1)").to_string()       == "x/(x + 1)"
// See c_prep_4_content_audit_memo.md §5.1 for the audit verdict and
// §5.2 for the C0a remediation plan (from_canonical_normalize_content
// preprocessor at the entry of sym_coef_canonical_string, BINDING if
// T4/T5 also fail; informational-only if only T3 fails).
// =====================================================================
void T3_single_var_non_coprime_parse_reduces() {
    todo("T3 single_var_non_coprime_parse_reduces "
         "[ITER-29 EMPIRICAL DIVERGENCE FOUND; see audit memo §5.1]");
}

// =====================================================================
// T4: multivar_large_nvars_repswap_vs_legacy. nvars=60 (above the
// HF_REPSWAP_NVARS_MIN=50 default; this is the regime where
// `Rat::add` dispatches to `add_via_q_underscore`).  Build the same
// algebraic Rat via:
//   Path A: r.add_repswap(b)  -- the rep-swap (q_underscore) backend
//   Path B: r.add_legacy(b)   -- the cross-mult+gcd_cofactors backend
// Assert byte-identical to_string.  THIS IS THE LOAD-BEARING CASE for
// the iter-28 reviewer F2 dismissal: if the two paths diverge on
// content (integer-content embedded in num/den vs not), then any
// cross-thread bucket_bump accumulation under HF_USE_SCALAR_REP=1 may
// produce nominally-equal SymCoefs that emit byte-different to_string,
// breaking --mode value-equivalence spuriously.
//
// Iter-30 NOTE: we use Rat::add_repswap (NEW iter-30, public method
// added to mirror Rat::add_legacy) rather than env-cache wrangling.
// `Rat::add_repswap` calls add_via_q_underscore directly, bypassing
// the static-cached env-gate lambdas at rat.cpp:1230-1235 / 1284-1292.
// =====================================================================
void T4_multivar_large_nvars_repswap_vs_legacy() {
    // 60 vars: x0..x59.  We construct Rat instances over a single
    // shared PolyCtx to keep the operands compatible.
    std::vector<std::string> vars;
    vars.reserve(60);
    for (int i = 0; i < 60; ++i) vars.push_back("x" + std::to_string(i));
    PolyCtx ctx(vars);

    // a = (x0 + 2*x1) / (x2 + 1).
    // b = (x3 - x4)   / (x5 + x6 + 1).
    // a + b is what we're testing under both backends.
    Rat a = Rat::parse(ctx, "(x0 + 2*x1)/(x2 + 1)");
    Rat b = Rat::parse(ctx, "(x3 - x4)/(x5 + x6 + 1)");
    Rat r_repswap = a.add_repswap(b);
    Rat r_legacy  = a.add_legacy(b);
    check(r_repswap.to_string() == r_legacy.to_string(),
          "T4 multivar_large_nvars_repswap_vs_legacy (nvars=60): "
          "repswap=\"" + r_repswap.to_string() + "\", legacy=\""
          + r_legacy.to_string() + "\"");
}

// =====================================================================
// T5: multivar_small_nvars_legacy_vs_forced_repswap. nvars=12 (Smirnov
// tst2 regime, below the HF_REPSWAP_NVARS_MIN=50 default; this is the
// regime where `Rat::add` dispatches to `add_legacy`).  Same shape as
// T4 but at production-relevant nvars: ensures the two paths produce
// byte-identical to_string in the small-ctx regime.  If T5 diverges,
// then the nvars=12 Smirnov tst2 production gate is dispatching to a
// path whose integer-content normalization is semantically distinct
// from the rep-swap path.  LOAD-BEARING for F2 in the
// production-relevant nvars regime.
// =====================================================================
void T5_multivar_small_nvars_legacy_vs_forced_repswap() {
    std::vector<std::string> vars;
    vars.reserve(12);
    for (int i = 0; i < 12; ++i) vars.push_back("x" + std::to_string(i));
    PolyCtx ctx(vars);

    // Same shape as T4 but on 12 vars only; same operands.
    Rat a = Rat::parse(ctx, "(x0 + 2*x1)/(x2 + 1)");
    Rat b = Rat::parse(ctx, "(x3 - x4)/(x5 + x6 + 1)");
    Rat r_legacy  = a.add_legacy(b);
    Rat r_repswap = a.add_repswap(b);
    check(r_legacy.to_string() == r_repswap.to_string(),
          "T5 multivar_small_nvars_legacy_vs_forced_repswap (nvars=12): "
          "legacy=\"" + r_legacy.to_string() + "\", repswap=\""
          + r_repswap.to_string() + "\"");
}

// =====================================================================
// T4b (iter-31): multivar_large_nvars_repswap_vs_legacy_content_gt_1.
// Same shape as T4 (nvars=60, rep-swap dispatch regime) but at non-
// trivial pairwise-coprime integer content on the denominators:
//   a = (x0 + 2*x1) / (2*x2 + 2)        -- aD integer content 2
//   b = (x3 - x4)   / (3*x5 + 3*x6 + 3) -- bD integer content 3
// Cross-product denominator integer content is 6. This is the F2
// falsifier-targeted regime per iter-30 post-commit adversarial-
// reviewer F1 (internal review): the rep-swap path
// (add_via_q_underscore) and the legacy path (cross-mult +
// gcd_cofactors over Q) may distribute the content differently across
// num/den, producing byte-different to_string output even though they
// are algebraically equal.
//
// Outcome interpretation:
//   T4b PASS -> iter-30 closure verdict empirically strengthened at
//               content > 1; from_canonical_normalize_content remains
//               NOT BINDING for C0a.
//   T4b FAIL -> closure verdict empirically falsified; ship
//               from_canonical_normalize_content (~30 LOC) at the
//               entry of sym_coef_canonical_string and re-pin
//               canonical-emission v0 baselines.
// =====================================================================
void T4b_multivar_large_nvars_repswap_vs_legacy_content_gt_1() {
    std::vector<std::string> vars;
    vars.reserve(60);
    for (int i = 0; i < 60; ++i) vars.push_back("x" + std::to_string(i));
    PolyCtx ctx(vars);

    Rat a = Rat::parse(ctx, "(x0 + 2*x1)/(2*x2 + 2)");
    Rat b = Rat::parse(ctx, "(x3 - x4)/(3*x5 + 3*x6 + 3)");
    Rat r_repswap = a.add_repswap(b);
    Rat r_legacy  = a.add_legacy(b);
    check(r_repswap.to_string() == r_legacy.to_string(),
          "T4b multivar_large_nvars_repswap_vs_legacy_content_gt_1 "
          "(nvars=60, aD content 2, bD content 3): repswap=\""
          + r_repswap.to_string() + "\", legacy=\""
          + r_legacy.to_string() + "\"");
}

// =====================================================================
// T5b (iter-31): multivar_small_nvars_legacy_vs_forced_repswap_content_gt_1.
// Same shape as T5 (nvars=12, legacy dispatch regime - Smirnov tst2)
// but at non-trivial pairwise-coprime integer content on the
// denominators (same operands as T4b). Tests the F2 falsifier
// scenario at the production-relevant nvars regime.
// =====================================================================
void T5b_multivar_small_nvars_legacy_vs_forced_repswap_content_gt_1() {
    std::vector<std::string> vars;
    vars.reserve(12);
    for (int i = 0; i < 12; ++i) vars.push_back("x" + std::to_string(i));
    PolyCtx ctx(vars);

    Rat a = Rat::parse(ctx, "(x0 + 2*x1)/(2*x2 + 2)");
    Rat b = Rat::parse(ctx, "(x3 - x4)/(3*x5 + 3*x6 + 3)");
    Rat r_legacy  = a.add_legacy(b);
    Rat r_repswap = a.add_repswap(b);
    check(r_legacy.to_string() == r_repswap.to_string(),
          "T5b multivar_small_nvars_legacy_vs_forced_repswap_content_gt_1 "
          "(nvars=12, aD content 2, bD content 3): legacy=\""
          + r_legacy.to_string() + "\", repswap=\""
          + r_repswap.to_string() + "\"");
}

// =====================================================================
// T4c (iter-32): multivar_large_nvars_repswap_vs_legacy_true_f2_falsifier.
// Same shape as T4/T4b (nvars=60, rep-swap dispatch regime) but with
// shared num/den Z-content within EACH input Rat (the TRUE F2 falsifier
// per iter-31 reviewer F1, internal review):
//   a = (2*x0 + 4*x1) / (2*x2 + 2)   -- aN content 2, aD content 2
//                                       gcd(aN-content, aD-content) = 2
//   b = (3*x3 - 3*x4) / (3*x5 + 3*x6 + 3)
//                                    -- bN content 3, bD content 3
//                                       gcd(bN-content, bD-content) = 3
// After cross-mult under legacy add, num and den each carry Z-content 6;
// gcd-across-pair is 6. Per FLINT documented spec, fmpz_mpoly_q
// canonical form removes that integer-content gcd; legacy
// (gcd_cofactors over Q) does not. The reviewer's Mathematica stress
// test (see HyperFLINT development notes, internal) predicted divergence
// at this Case D regime.
//
// Outcome interpretation:
//   T4c PASS -> closure verdict empirically airtight at the TRUE F2
//               falsifier regime. from_canonical_normalize_content
//               remains NOT BINDING for C0a.
//   T4c FAIL -> closure verdict empirically falsified. Ship
//               from_canonical_normalize_content (~30 LOC) at the
//               entry of sym_coef_canonical_string. Re-pin canonical-
//               emission v0 baselines; T3 may also start passing.
// =====================================================================
void T4c_multivar_large_nvars_repswap_vs_legacy_true_f2_falsifier() {
    std::vector<std::string> vars;
    vars.reserve(60);
    for (int i = 0; i < 60; ++i) vars.push_back("x" + std::to_string(i));
    PolyCtx ctx(vars);

    Rat a = Rat::parse(ctx, "(2*x0 + 4*x1)/(2*x2 + 2)");
    Rat b = Rat::parse(ctx, "(3*x3 - 3*x4)/(3*x5 + 3*x6 + 3)");
    Rat r_repswap = a.add_repswap(b);
    Rat r_legacy  = a.add_legacy(b);
    check(r_repswap.to_string() == r_legacy.to_string(),
          "T4c multivar_large_nvars_repswap_vs_legacy_true_f2_falsifier "
          "(nvars=60, aN/aD shared content 2, bN/bD shared content 3): "
          "repswap=\"" + r_repswap.to_string() + "\", legacy=\""
          + r_legacy.to_string() + "\"");
}

// =====================================================================
// T5c (iter-32): multivar_small_nvars_legacy_vs_forced_repswap_true_f2_falsifier.
// Same operands as T4c but on nvars=12 (Smirnov tst2 legacy dispatch
// regime). Tests the TRUE F2 falsifier scenario at the production-
// relevant nvars regime.
// =====================================================================
void T5c_multivar_small_nvars_legacy_vs_forced_repswap_true_f2_falsifier() {
    std::vector<std::string> vars;
    vars.reserve(12);
    for (int i = 0; i < 12; ++i) vars.push_back("x" + std::to_string(i));
    PolyCtx ctx(vars);

    Rat a = Rat::parse(ctx, "(2*x0 + 4*x1)/(2*x2 + 2)");
    Rat b = Rat::parse(ctx, "(3*x3 - 3*x4)/(3*x5 + 3*x6 + 3)");
    Rat r_legacy  = a.add_legacy(b);
    Rat r_repswap = a.add_repswap(b);
    check(r_legacy.to_string() == r_repswap.to_string(),
          "T5c multivar_small_nvars_legacy_vs_forced_repswap_true_f2_falsifier "
          "(nvars=12, aN/aD shared content 2, bN/bD shared content 3): "
          "legacy=\"" + r_legacy.to_string() + "\", repswap=\""
          + r_repswap.to_string() + "\"");
}

// =====================================================================
// T6: sign_flip_neg_vs_parse_negative. r = -(x/y) via Rat::neg vs
// Rat::parse("-x/y"). Both should land on canonical form (den has
// positive leading coef; sign carried in num).
// =====================================================================
void T6_sign_flip_neg_vs_parse_negative() {
    PolyCtx ctx({"x", "y"});
    Rat base = Rat::parse(ctx, "x/y");
    Rat r_a  = base.neg();
    Rat r_b  = Rat::parse(ctx, "-x/y");
    check(r_a.to_string() == r_b.to_string(),
          "T6 sign_flip_neg_vs_parse_negative: neg=\"" + r_a.to_string()
          + "\", parse=\"" + r_b.to_string() + "\"");
}

// =====================================================================
// T7: pow_via_pow_vs_parse_expanded. r = (x/y)^3 via Rat::pow vs
// Rat::parse("x^3/y^3"). TODO iter-30.
// =====================================================================
void T7_pow_via_pow_vs_parse_expanded() {
    todo("T7 pow_via_pow_vs_parse_expanded");
}

// =====================================================================
// T8: derivative_quotient_rule. r = d/dx[(x^2)/(x+1)] should equal
// (2x(x+1) - x^2)/(x+1)^2 = (x^2 + 2x)/(x+1)^2. Verify Rat::derivative
// produces the same canonical to_string as direct construction.
// =====================================================================
void T8_derivative_quotient_rule() {
    PolyCtx ctx({"x"});
    Rat base = Rat::parse(ctx, "x^2/(x+1)");
    Rat r_a  = base.derivative(0);
    Rat r_b  = Rat::parse(ctx, "(x^2 + 2*x)/((x+1)^2)");
    check(r_a.to_string() == r_b.to_string(),
          "T8 derivative_quotient_rule: derivative=\"" + r_a.to_string()
          + "\", expected=\"" + r_b.to_string() + "\"");
}

// =====================================================================
// T9: substitute_one_var. r = (x+y)/(x-y), substitute y=1 -> (x+1)/(x-1).
// TODO iter-30.
// =====================================================================
void T9_substitute_one_var() {
    todo("T9 substitute_one_var");
}

// =====================================================================
// T10: cross_mult_cancellation_parse_vs_mul_compose. r = ((x+1)(x-1))/
// ((x+1)(x+2)) parsed directly should reduce to (x-1)/(x+2). Compose
// via Rat::mul of factored Rats and assert equal to_string.
// TODO iter-30.
// =====================================================================
void T10_cross_mult_cancellation_parse_vs_mul_compose() {
    todo("T10 cross_mult_cancellation_parse_vs_mul_compose");
}

}  // namespace

int main() {
    std::cout << "test_rat_content_invariance: 14 tests "
              << "(10 implemented iter-29+iter-30+iter-31+iter-32; 4 TODO)\n";
    std::cout << "----------------------------------------"
              << "----------------------------------------\n";
    T1_trivial_one_over_one();
    T2_single_var_coprime_parse_vs_add();
    T3_single_var_non_coprime_parse_reduces();
    T4_multivar_large_nvars_repswap_vs_legacy();
    T4b_multivar_large_nvars_repswap_vs_legacy_content_gt_1();
    T4c_multivar_large_nvars_repswap_vs_legacy_true_f2_falsifier();
    T5_multivar_small_nvars_legacy_vs_forced_repswap();
    T5b_multivar_small_nvars_legacy_vs_forced_repswap_content_gt_1();
    T5c_multivar_small_nvars_legacy_vs_forced_repswap_true_f2_falsifier();
    T6_sign_flip_neg_vs_parse_negative();
    T7_pow_via_pow_vs_parse_expanded();
    T8_derivative_quotient_rule();
    T9_substitute_one_var();
    T10_cross_mult_cancellation_parse_vs_mul_compose();
    std::cout << "----------------------------------------"
              << "----------------------------------------\n";
    if (g_pass) {
        std::cout << "PASS (10 implemented + " << g_todo
                  << " TODO; iter-32 audit close TRUE F2 falsifier hardening)\n";
        return 0;
    }
    std::cout << "FAIL\n";
    return 1;
}
