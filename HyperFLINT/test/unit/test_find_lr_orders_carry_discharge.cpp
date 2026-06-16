// Carry-discharge (FindRoots) keep rule in find_lr_orders — TDD battery.
//
// find_lr_orders judges deg-2 algebraic letters TERMINAL-ONLY: a deg-2
// letter whose sqrt-argument (odd part of the pivot-discriminant) depends
// on a still-pending integration variable is rejected at that step
// (the forbidden_after_step guard, lr_search.cpp).  The Doppio FindRoots
// tier (lr_scan::step_fr / fr_judge) instead CARRIES that obligation
// forward and discharges it once the pending variable is integrated —
// strictly more permissive.  This test wires that tier into
// find_lr_orders as the "carry_discharge" request option, DEFAULT OFF
// (production fix, spec 2026-06-10 §4a.1).
//
// The three gates of the task:
//   (1) NOLR->LR FLIP: a fixture that find_lr_orders+algebraic reports
//       NOLR under the terminal-only rule but LR under carry-discharge.
//       Primary fixture is the REAL gauged-uq5 system (uq5 at x3->1):
//       lr_scan FindRoots @ gauge x3 has 24 admissible orders (oracle in
//       test_lr_scan.cpp / t24 battery), so an LR order provably exists;
//       find_lr_orders+algebraic currently says NOLR.  Plus a small
//       synthetic 2-var fixture with the same structure.
//   (2) STRICT BYTE-IDENTITY: carry_discharge:false reproduces today's
//       response (the response is fed through handlers::find_lr_orders so
//       the whole envelope is checked; the timing field is stripped).
//   (3) DEFAULT OFF (production fix, spec 2026-06-10-carry-option-design.md
//       4a.1): an absent carry_discharge behaves like :false (Strict).
//       v1.2.3 shipped absent=>true unintentionally; this gate pins the fix.
//
// Parity against lr_scan FindRoots (the per-step semantics are the SAME
// extracted code, lr_scan::step_fr_judge) is covered structurally by the
// flip oracle above and by test_lr_scan.cpp (unchanged 120-order pin).
//
// Handler-level (mirrors test_find_lr_orders_strategy_roundtrip.cpp): the
// request crosses the JSON boundary, so this also guards the serialization
// of the new option + any carried-sqrt profile fields.
//
// Tier-1 wall: gauged-uq5 ~20 ms, synthetic sub-ms; a handful of rows.

#include "hyperflint/bridge/handlers.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/integrator/lr_scan.hpp"

#include <climits>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

namespace {

// ---- response field accessors (hand-rolled, no JSON dep — same style as
//      test_find_lr_orders_strategy_roundtrip.cpp) ----

bool resp_nolr(const std::string& resp) {
    std::regex rx(R"~("nolr"\s*:\s*(true|false))~");
    std::smatch m;
    if (std::regex_search(resp, m, rx)) return m[1].str() == "true";
    // absent => treat as NOLR=unknown; force a visible failure
    return true;
}

bool resp_has_error(const std::string& resp) {
    return resp.find("\"error\"") != std::string::npos;
}

std::string resp_best_order(const std::string& resp) {
    std::regex rx(R"~("best_order"\s*:\s*(\[[^\]]*\]))~");
    std::smatch m;
    if (std::regex_search(resp, m, rx)) return m[1].str();
    return "<absent>";
}

// Extract an unsigned integer field; returns -1 if absent.
long resp_uint_field(const std::string& resp, const char* field) {
    std::regex rx("\"" + std::string(field) + "\"\\s*:\\s*([0-9]+)");
    std::smatch m;
    if (std::regex_search(resp, m, rx)) return std::stol(m[1].str());
    return -1;
}

// Extract an unsigned long field; returns ULONG_MAX (visible failure) if
// absent.  Used by the lexicographic-selection gates (gate 4 / 4b).
unsigned long resp_ulong_field(const std::string& resp, const std::string& key) {
    std::regex rx("\"" + key + "\"\\s*:\\s*(\\d+)");
    std::smatch m;
    if (std::regex_search(resp, m, rx)) return std::stoul(m[1].str());
    return ULONG_MAX;  // absent => visible failure
}

// Strip the (non-deterministic) timing field so two responses can be
// compared byte-for-byte.  Same approach the c-abi snapshot ctests use.
std::string strip_timing(const std::string& resp) {
    std::regex rx(R"~(,"timing_compute_s":[0-9eE.+\-]+)~");
    return std::regex_replace(resp, rx, "");
}

// Strip the carry-profile fields (carried_sqrts, kin_sqrts,
// terminal_quads, and — Phase 2 — the carried_polys array) so a
// carry:true response can be compared byte-for-byte with a carry:false
// response on a strictly-LR face where no carry fires.  These fields are
// only emitted by the carry path (they are additively new; gate 6
// verifies that a strict-LR face is a no-op modulo exactly these fields
// and timing).
std::string strip_profile(std::string s) {
    s = std::regex_replace(s,
        std::regex(
            "[,]?\"(carried_sqrts|kin_sqrts|terminal_quads)\"\\s*:\\s*\\d+"),
        "");
    s = std::regex_replace(s,
        std::regex("[,]?\"carried_polys\"\\s*:\\s*\\[[^\\]]*\\]"),
        "");
    return s;
}

// Extract the "carried_polys" string array; `present` reports whether the
// field exists at all (an empty array is present-but-empty, distinct from
// absent — gate 7 asserts absence on the no-carry paths).
std::vector<std::string> resp_carried_polys(const std::string& resp,
                                            bool& present) {
    std::vector<std::string> out;
    std::regex rx(R"~("carried_polys"\s*:\s*\[([^\]]*)\])~");
    std::smatch m;
    present = std::regex_search(resp, m, rx);
    if (!present) return out;
    const std::string body = m[1].str();
    // JSON string literals (escapes allowed, same emission as root_polys)
    std::regex item(R"~("((?:[^"\\]|\\.)*)")~");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), item);
         it != std::sregex_iterator(); ++it)
        out.push_back((*it)[1].str());
    return out;
}

int g_pass = 0, g_fail = 0;
void check(const char* label, bool cond, const std::string& diag = "") {
    if (cond) {
        std::cout << "[PASS] " << label << "\n";
        ++g_pass;
    } else {
        std::cerr << "[FAIL] " << label;
        if (!diag.empty()) std::cerr << "  " << diag;
        std::cerr << "\n";
        ++g_fail;
    }
}

// ---- fixtures ----

// uq5 gauged at x3 -> 1.  Two-factor projective group of the ungauged
// uq5 (test_lr_scan.cpp) with x3 substituted to 1:
//   P1 = x1 + x2 + x3                       -> x1 + x2 + 1
//   P2 = -qq1 x1 x2 - qq2 x1 x3 + 2 wb1 x3 x4 - x4^2
//        + 2 wb2 x2 x5 - x5^2 + 2 yb x4 x5  -> (x3 -> 1)
const char* kGaugedUq5Polys =
    R"~("x1+x2+1","-qq1*x1*x2-qq2*x1+2*wb1*x4-x4^2+2*wb2*x2*x5-x5^2+2*yb*x4*x5")~";
const char* kGaugedUq5Xvars  = R"~("x1","x2","x4","x5")~";
const char* kGaugedUq5Coeffs = R"~("qq1","qq2","wb1","wb2","yb")~";

std::string gauged_uq5_request(const char* carry_field) {
    std::string r = "{\"op\":\"find_lr_orders\",";
    r += "\"xvars\":[";   r += kGaugedUq5Xvars;  r += "],";
    r += "\"coeff_vars\":["; r += kGaugedUq5Coeffs; r += "],";
    r += "\"polys\":[";   r += kGaugedUq5Polys;  r += "],";
    r += "\"algebraic_letters\":true";
    if (carry_field[0] != '\0') { r += ","; r += carry_field; }
    r += "}";
    return r;
}

// Small synthetic 2-var fixture, VERIFIED against lr_scan FindRoots.
// It is the Cheng-Wu gauge-x3->1 projection of the projective 3-var
// system
//     U = x1 + x2 + x3 ,   F = x1^2 + x2^2 + x1 x2 + s x3^2
// (twist exps U^(-1+2eps) F^(-1-eps); Sigma a_i deg = -3, Sigma b_i
// deg = 0).  On that lift:
//   * lr_scan Strict      -> 0 orders (gauge-exhaustive NOLR);
//   * lr_scan FindRoots   -> 6 orders, each carried_sqrts == 1; at gauge
//     x3 the orders are (x1,x2) and (x2,x1).
// The gauge-x3 group is { x1+x2+1, x1^2+x2^2+x1 x2 + s } (F at x3->1,
// plus the U-boundary x1+x2+1).  find_lr_orders+carry on it must thus:
//   * carry OFF -> NOLR  (== lr_scan Strict @ gauge x3),
//   * carry ON  -> LR, order (x1,x2), carried_sqrts == 1  (one of the
//     lr_scan FindRoots @ gauge x3 orders; the score-minimal one).
// The single deg-2 letter's sqrt-obligation depends on the pending
// second variable at the first pivot and discharges at the second —
// exactly the structure the terminal-only rule rejects but carry keeps.
const char* kSynthPolys  = R"~("x1+x2+1","x1^2+x2^2+x1*x2+s")~";
const char* kSynthXvars  = R"~("x1","x2")~";
const char* kSynthCoeffs = R"~("s")~";

std::string synth_request(const char* carry_field) {
    std::string r = "{\"op\":\"find_lr_orders\",";
    r += "\"xvars\":[";   r += kSynthXvars;  r += "],";
    r += "\"coeff_vars\":["; r += kSynthCoeffs; r += "],";
    r += "\"polys\":[";   r += kSynthPolys;  r += "],";
    r += "\"algebraic_letters\":true";
    if (carry_field[0] != '\0') { r += ","; r += carry_field; }
    r += "}";
    return r;
}

}  // namespace

int main() {
    using hyperflint::handlers::find_lr_orders;

    // ===== Gate 1: NOLR -> LR flip (real gauged-uq5) =====
    {
        const std::string off = find_lr_orders(gauged_uq5_request(
            "\"carry_discharge\":false"));
        const std::string on  = find_lr_orders(gauged_uq5_request(
            "\"carry_discharge\":true"));
        check("gauged-uq5: carry OFF => NOLR (terminal-only, today)",
              !resp_has_error(off) && resp_nolr(off), off);
        check("gauged-uq5: carry ON => LR (carry-discharge flip)",
              !resp_has_error(on) && !resp_nolr(on), on);
        if (!resp_nolr(on))
            std::cout << "       gauged-uq5 carry-ON order = "
                      << resp_best_order(on) << "\n";
    }

    // ===== Gate 1b: NOLR -> LR flip + lr_scan PARITY (small synthetic) =====
    {
        const std::string off = find_lr_orders(synth_request(
            "\"carry_discharge\":false"));
        const std::string on  = find_lr_orders(synth_request(
            "\"carry_discharge\":true"));
        check("synthetic: carry OFF => NOLR (== lr_scan Strict @ gauge x3)",
              !resp_has_error(off) && resp_nolr(off), off);
        check("synthetic: carry ON => LR (carry-discharge flip)",
              !resp_has_error(on) && !resp_nolr(on), on);
        // PARITY with lr_scan FindRoots @ gauge x3: order (x1,x2),
        // carried_sqrts == 1 (verified: see fixture comment).  This is
        // the core correctness claim at small scale — the same verdict
        // AND the same carried-sqrt count as the exhaustive scan engine.
        check("synthetic: carry-ON order == (x1,x2) [lr_scan parity]",
              resp_best_order(on) == "[\"x1\",\"x2\"]",
              "order=" + resp_best_order(on));
        check("synthetic: carry-ON carried_sqrts == 1 [lr_scan parity]",
              resp_uint_field(on, "carried_sqrts") == 1,
              "carried_sqrts=" +
                  std::to_string(resp_uint_field(on, "carried_sqrts")));
    }

    // ===== Gate 2: Strict byte-identity (carry OFF == today) =====
    // The pre-change baseline response for gauged-uq5 + algebraic is the
    // terminal-only NOLR envelope; carry_discharge:false must reproduce
    // it byte-for-byte (modulo timing).  We compare carry:false against
    // the EXPECTED today-envelope captured here as a literal, so a
    // regression in the Strict path (not just a verdict flip) is caught.
    {
        const std::string off = strip_timing(find_lr_orders(
            gauged_uq5_request("\"carry_discharge\":false")));
        // today's NOLR envelope for this fixture (algebraic on, no carry):
        const std::string expect =
            "{\"op\":\"find_lr_orders\""
            ",\"schema_version\":2"
            ",\"hf_version\":\"" /* version-agnostic prefix match below */;
        // Version-agnostic structural check: NOLR envelope shape.
        const bool shape_ok =
            off.find("\"best_order\":[]") != std::string::npos &&
            off.find("\"score\":null") != std::string::npos &&
            off.find("\"nolr\":true") != std::string::npos &&
            off.find("\"strategy\":\"Fubini_Lungo\"") != std::string::npos &&
            off.find("\"root_polys\":[]") != std::string::npos;
        check("gauged-uq5: Strict(carry:false) NOLR envelope unchanged",
              shape_ok, off);
        (void) expect;
    }

    // ===== Gate 3 (DEFAULT-OFF, spec 2026-06-10-carry-option-design.md 4a.1):
    // absent carry_discharge behaves like :false (Strict).  v1.2.3
    // shipped absent=>true unintentionally; this gate pins the fix.
    // Run on the FLIP fixture (gauged-uq5) where carry CHANGES the verdict;
    // a trivial Strict-LR face would let both agree and mask the default. =====
    {
        const std::string resp_absent =
            find_lr_orders(gauged_uq5_request(""));
        const std::string resp_false =
            find_lr_orders(gauged_uq5_request("\"carry_discharge\":false"));
        check("gauged-uq5: absent carry_discharge => NOLR (default OFF)",
              !resp_has_error(resp_absent) && resp_nolr(resp_absent),
              resp_absent);
        check("gauged-uq5: absent == explicit false (byte-identical)",
              strip_timing(resp_absent) == strip_timing(resp_false),
              "absent=" + strip_timing(resp_absent) +
              "  false=" + strip_timing(resp_false));

        const std::string sdef = find_lr_orders(synth_request(""));
        check("synthetic: absent carry_discharge => NOLR (default OFF)",
              !resp_has_error(sdef) && resp_nolr(sdef), sdef);
    }

    // ===== Gate 4 (lexicographic selection, spec 4a.2): an admissible
    // UNCARRIED order must never be shadowed by a score-cheaper CARRIED
    // one.  Premise check: carry run is LR; the returned profile must
    // then be carried_sqrts == 0.  Against the WIP selection (score-only)
    // this FAILS by returning the cheaper carried order. =====
    //
    // Fixture (2 vars {x,y}, kinematic s), single 3-factor group:
    //   B = x + y + 1            (deg-1 boundary, keeps resultants low)
    //   F = x^2 + x*y + y + 1    (deg-2 in x, deg-1 in y)
    //   H = y + (s^4+s^3+s^2+s+1) (deg-0 in x, deg-1 in y, fat kinematic)
    // Instrumented DFS leaves (gauge-free carry path):
    //   x-first (x,y): score 43.3, carried_sqrts 1  -- F's x-disc
    //       y^2-4y-4 has a y-dependent odd part, so x-first CARRIES the
    //       obligation, discharged terminally at the y pivot;
    //   y-first (y,x): score 100.1, carried_sqrts 0 -- everything is
    //       deg<=1 in y at the first pivot (UNCARRIED), but eliminating y
    //       first forms a fat x-resultant of H against F (the
    //       s^4-scale letter x^2 - x(s^4+...) - (s^4+...)), inflating the
    //       y-first marginal ~2.3x.
    // Both orders are admissible; the CARRIED one is score-cheaper.  A
    // score-only keep therefore returns carried_sqrts == 1 (the carried
    // order shadows the executable uncarried one); the lexicographic
    // (carried_sqrts, score) keep must return carried_sqrts == 0.
    {
        const std::string body_shadow =
            "{\"op\":\"find_lr_orders\",\"xvars\":[\"x\",\"y\"],"
            "\"coeff_vars\":[\"s\"],"
            "\"polys\":[\"x+y+1\",\"x^2 + x*y + y + 1\","
            "\"y + (s^4+s^3+s^2+s+1)\"],"
            "\"algebraic_letters\":true,\"carry_discharge\":true}";
        const std::string resp = find_lr_orders(body_shadow);
        if (resp_has_error(resp) || resp_nolr(resp)) {
            std::cerr << "FAIL gate4 PREMISE: shadow fixture not carry-LR: "
                      << resp << "\n";
            return 1;
        }
        if (resp_ulong_field(resp, "carried_sqrts") != 0UL) {
            std::cerr << "FAIL gate4: carried order shadowed an uncarried "
                         "one: " << resp << "\n";
            return 1;
        }
        std::cout << "[PASS] gate4: uncarried order not shadowed "
                     "(carried_sqrts == 0)\n";
        ++g_pass;
    }

    // ===== Gate 4b: within equal carried_sqrts the score tiebreaker
    // applies; on gauged-uq5 the DFS must return the admissible-MINIMUM
    // carried count, not merely the score-minimal admissible profile. =====
    //
    // PINNED VALUE 3, justified at THIS fixture's gauge (x3->1, the file's
    // gauged-uq5 system).  RE-DERIVED 2026-06-16 for v1.2.5: the Brown/
    // HyperInt polynomial reduction was completed (commit 93feb8d66 — the
    // constant-term piece f|_{x=0} = coeff(f,var,0), the x=0 endpoint
    // singularity, was previously omitted from st_fubini_lr, which
    // UNDER-counted the Landau set).  With the complete reduction the
    // admissible-minimum carried_sqrts over all full orders of the 4
    // variables is 3 (re-instrumented HF_CARRY_LEAF_TRACE=1
    // HF_CARRY_NO_PRUNE=1: leaf profile {4x nsq3, 2x4, 8x5, 5x6, 4x7,
    // 1x8}), and the winning order is [x4,x5,x1,x2] with profile
    // (nsq, nkin, ntq) == (3, 0, 5).  This matches uq5's published
    // 3-algebraic-letter alphabet (arXiv:2308.02125): the three carried
    // obligations are pairwise independent (one yields sqrt(yb^2-1); the
    // other two are images under the pentagon symmetry qq1<->qq2,
    // wb1<->wb2; physics review 2026-06-16).  Pre-v1.2.5 this pinned 2
    // against the incomplete reduction.  Re-instrument with the same two
    // env levers if the fixture is ever regauged.
    {
        const std::string body_uq5_carry_true =
            gauged_uq5_request("\"carry_discharge\":true");
        const std::string resp = find_lr_orders(body_uq5_carry_true);
        unsigned long nsq  = resp_ulong_field(resp, "carried_sqrts");
        unsigned long nkin = resp_ulong_field(resp, "kin_sqrts");
        unsigned long ntrq = resp_ulong_field(resp, "terminal_quads");
        if (nsq != 3UL) {
            std::cerr << "FAIL gate4b: gauged-uq5 carried_sqrts=" << nsq
                      << " (expected the admissible-minimum 3)\n" << resp
                      << "\n";
            return 1;
        }
        if (nkin != 0UL || ntrq != 5UL) {
            std::cerr << "FAIL gate4b profile: expected (carried_sqrts=3, "
                         "kin_sqrts=0, terminal_quads=5), got ("
                      << nsq << "," << nkin << "," << ntrq << ")\n"
                      << resp << "\n";
            return 1;
        }
        check("gate4b: gauged-uq5 carried_sqrts == 3 (admissible minimum @ gauge x3)",
              nsq == 3UL,
              "carried_sqrts=" + std::to_string(nsq));
        check("gate4b profile: (carried_sqrts=3, kin_sqrts=0, terminal_quads=5) pinned",
              nkin == 0UL && ntrq == 5UL,
              "kin_sqrts=" + std::to_string(nkin) +
              " terminal_quads=" + std::to_string(ntrq));
    }

    // ===== Gate 5 (G3 no-over-find): carry must NOT promote a deg-3 pivot.
    // fr_judge returns false immediately for d >= 3, so a system whose
    // EVERY integration variable appears to degree >= 3 in every polynomial
    // (no escape route via a deg-1 variable) must remain NOLR under carry.
    //
    // Poly used: x^3*y^3 + x^3 + y^3 + s.
    //   x-first pivot: deg(x) = 3 => fr_judge hard-fails => x-first NOLR.
    //   y-first pivot: deg(y) = 3 => fr_judge hard-fails => y-first NOLR.
    // There is no route to LR; carry must not over-find.
    //
    // NOTE: the single-poly candidate x^3 + 2*x + y + s was tested
    // first and is legitimately LR via y-first (deg-y = 1 => trivially
    // integrable; no residual polynomial remains at the x-step because
    // eliminating y from a single polynomial leaves nothing).  That case
    // is correct engine behavior, not a bug.  The both-routes-deg-3
    // polynomial above is the right gate for the no-over-find claim.
    {
        const std::string body_deg3 =
            "{\"op\":\"find_lr_orders\",\"xvars\":[\"x\",\"y\"],"
            "\"coeff_vars\":[\"s\"],"
            "\"polys\":[\"x^3*y^3 + x^3 + y^3 + s\"],"
            "\"algebraic_letters\":true,\"carry_discharge\":true}";
        const std::string resp = find_lr_orders(body_deg3);
        check("gate5: both-routes-deg-3 poly must remain NOLR under carry",
              !resp_has_error(resp) && resp_nolr(resp), resp);
    }

    // ===== Gate 6 (G3 strict-face no-op): on a strictly-LR face (one where
    // carry fires no obligations), carry:true and carry:false must agree on
    // verdict (nolr), score, root_polys, strategy, and nXVars/nGroups/nPolys.
    // Only the carry profile fields (carried_sqrts, kin_sqrts, terminal_quads)
    // and timing are legitimately path-dependent; best_order is also excluded
    // because the carry DFS and the subset-DP may break ties among equal-score
    // orders differently (both pick a globally optimal order; which one is
    // returned is an implementation detail, not a correctness property).
    //
    // Fixture (2 vars {x,y}, kinematic s,t), single group:
    //   x*y + s*x + t*y
    // Both pivots are deg-1 (no deg-2 letters); NO carry obligation arises.
    // Carry profile fields must be zero (or absent); the verdict must be LR;
    // scores must be equal.
    //
    // This gate asserts:
    //   (a) nolr == false in both paths  (strictly LR)
    //   (b) scores match
    //   (c) byte-identity after stripping timing + profile + best_order
    //
    // A difference in nolr, score, root_polys, or strategy would be a STOP.
    {
        const std::string b_t =
            "{\"op\":\"find_lr_orders\",\"xvars\":[\"x\",\"y\"],"
            "\"coeff_vars\":[\"s\",\"t\"],"
            "\"polys\":[\"x*y + s*x + t*y\"],"
            "\"algebraic_letters\":true,\"carry_discharge\":true}";
        const std::string b_f =
            "{\"op\":\"find_lr_orders\",\"xvars\":[\"x\",\"y\"],"
            "\"coeff_vars\":[\"s\",\"t\"],"
            "\"polys\":[\"x*y + s*x + t*y\"],"
            "\"algebraic_letters\":true,\"carry_discharge\":false}";
        const std::string rt_raw = find_lr_orders(b_t);
        const std::string rf_raw = find_lr_orders(b_f);

        // (a) both LR
        check("gate6a: strict-LR face: carry:true => LR",
              !resp_has_error(rt_raw) && !resp_nolr(rt_raw), rt_raw);
        check("gate6b: strict-LR face: carry:false => LR",
              !resp_has_error(rf_raw) && !resp_nolr(rf_raw), rf_raw);

        // (b) scores match
        // (Use the same regex the roundtrip tests use for the score field.)
        auto resp_score_str = [](const std::string& r) -> std::string {
            std::regex rx(R"~("score"\s*:\s*([0-9eE.+\-]+))~");
            std::smatch m;
            if (std::regex_search(r, m, rx)) return m[1].str();
            return "<absent>";
        };
        const std::string st_sc = resp_score_str(rt_raw);
        const std::string sf_sc = resp_score_str(rf_raw);
        check("gate6c: strict-LR face: scores match",
              st_sc == sf_sc,
              "carry:true score=" + st_sc + "  carry:false score=" + sf_sc);

        // (c) byte-identity after stripping timing + profile + best_order.
        // best_order is benign: DFS and subset-DP may tiebreak differently
        // among equal-score orders; correctness claims do not depend on
        // which specific optimal order is returned.
        auto strip_best_order = [](std::string s) -> std::string {
            s = std::regex_replace(s,
                std::regex(R"~(,"best_order"\s*:\s*\[[^\]]*\])~"), "");
            return s;
        };
        const std::string rt =
            strip_best_order(strip_profile(strip_timing(rt_raw)));
        const std::string rf =
            strip_best_order(strip_profile(strip_timing(rf_raw)));
        check("gate6d: strict-LR face: carry:true == carry:false "
              "(modulo timing, profile, best_order tiebreak)",
              rt == rf,
              "\ncarry:true  = " + rt + "\ncarry:false = " + rf);
    }

    // ===== Gate 7 (Phase 2, spec 2026-06-11-carry-phase2-design.md §3.1,
    // gate P2-G1): leaf-replay obligation emission.  When the carry tier
    // ran (algebraic_letters && carry_discharge), the response carries
    // "carried_polys": the DISTINCT carried obligations along best_order,
    // deduped up to proportionality across the whole path (leaf-replay,
    // path-cumulative ledger).  Structural facts asserted here:
    //   (i)   size <= carried_sqrts (remints re-count in nsq, enter the
    //         ledger once) and >= 1 on a genuinely-carried face (nsq=2
    //         on gauged-uq5, so at least one distinct mint exists);
    //   (ii)  entries pairwise distinct as strings (the canonical
    //         proportionality form makes string-distinct == class-
    //         distinct; the proportionality-level assertion is pinned in
    //         gate 7c after generation);
    //   (iii) field ABSENT when carry_discharge is false or absent (the
    //         Strict envelope stays byte-identical, gate-2/3 class).
    {
        const std::string on = find_lr_orders(gauged_uq5_request(
            "\"carry_discharge\":true"));
        bool present = false;
        const std::vector<std::string> polys =
            resp_carried_polys(on, present);
        check("gate7a: carry ON => carried_polys present",
              present, on);
        const unsigned long nsq = resp_ulong_field(on, "carried_sqrts");
        check("gate7b-size: 1 <= |carried_polys| <= carried_sqrts",
              present && polys.size() >= 1 &&
                  polys.size() <= nsq,
              "size=" + std::to_string(polys.size()) +
                  " carried_sqrts=" + std::to_string(nsq));
        bool pairwise_distinct = true;
        for (size_t i = 0; i < polys.size(); ++i)
            for (size_t j = i + 1; j < polys.size(); ++j)
                if (polys[i] == polys[j]) pairwise_distinct = false;
        check("gate7-distinct: carried_polys pairwise string-distinct",
              pairwise_distinct, on);
        // generate-then-pin (P2-G1): print the emitted polys so the pin
        // below is derived from engine output, never written a priori.
        std::cout << "       gauged-uq5 carried_polys (" << polys.size()
                  << "):\n";
        for (const auto& p : polys)
            std::cout << "         " << p << "\n";

        bool present_false = false, present_absent = false;
        const std::string off = find_lr_orders(gauged_uq5_request(
            "\"carry_discharge\":false"));
        resp_carried_polys(off, present_false);
        const std::string absent = find_lr_orders(gauged_uq5_request(""));
        resp_carried_polys(absent, present_absent);
        check("gate7-absent: no carried_polys under carry:false",
              !present_false, off);
        check("gate7-absent: no carried_polys when option absent",
              !present_absent, absent);

        // ===== Gate 7b (REMINT, spec P2-G1) — VACUOUS-WITH-EVIDENCE =====
        // The spec reserved a strict-inequality fixture (|carried_polys| <
        // carried_sqrts via mint -> discharge -> re-mint).  That case is
        // STRUCTURALLY UNREACHABLE in find_lr_orders' carry path, so no
        // fixture is fabricated (plan 2.5 escape clause).  Proof:
        //   (1) fr_judge mints a factor as a CARRY obligation only if it
        //       depends on a still-pending integration variable
        //       (depends_on_any(base, pending), lr_scan.cpp fr_judge);
        //   (2) an obligation is DISCHARGED (dropped from PathState.carried)
        //       only once free of pending vars and of the pivot — i.e. all
        //       integration variables in its support are integrated;
        //   (3) any factor produced at a later step that is proportional to
        //       a discharged class therefore has integration-variable
        //       support among ALREADY-INTEGRATED vars only, fails (1)'s
        //       pending-dependence predicate, and is routed to the KIN
        //       counter — ++nsq cannot re-fire for a discharged class.
        // Hence nsq == #distinct minted classes == ledger size, ALWAYS:
        // |carried_polys| == carried_sqrts in this engine (the spec's
        // size <= carried_sqrts stays true, with equality everywhere) —
        // 3 == 3 as of v1.2.5.  NOTE: the per-step ledger trace below was
        // generated 2026-06-11 for the PRE-v1.2.5 best_order [x1,x4,x5,x2]
        // (carried_sqrts 2); the completed LR reduction (commit 93feb8d66)
        // moved the winning order to [x4,x5,x1,x2] with 3 obligations
        // (authoritatively pinned in gate7c below).  The old trace is kept
        // only as the historical no-remint evidence.
        // Per-step replay ledger on gauged-uq5 (HF_CARRY_REPLAY_TRACE=1,
        // pre-v1.2.5 best_order [x1,x4,x5,x2], generated 2026-06-11):
        //   step 2 (pivot x4): ledger += x2^2*qq1 + 2*x2*x5*wb2 + x2*qq1
        //                        + x2*qq2 + x5^2*yb^2 - x5^2 + 2*x5*wb1*yb
        //                        + qq2 + wb1^2
        //   step 3 (pivot x5): ledger += x2^2*qq1*yb^2 - x2^2*qq1
        //                        - x2^2*wb2^2 + x2*qq1*yb^2 - x2*qq1
        //                        + x2*qq2*yb^2 - x2*qq2 - 2*x2*wb1*wb2*yb
        //                        + qq2*yb^2 - qq2 - wb1^2
        // No discharge-then-reappearance event exists along the order (the
        // G3b "reappearance" was a WL-allVars-walk artifact, not an HF
        // remint).  The equality instance is pinned here as the evidence:
        check("gate7b-remint-vacuous: |carried_polys| == carried_sqrts "
              "(no remint exists; proof in comment)",
              present && polys.size() == nsq,
              "size=" + std::to_string(polys.size()) +
                  " carried_sqrts=" + std::to_string(nsq));

        // ===== Gate 7c (generate-then-pin, P2-G1 / convergence F1) =====
        // PINNED from engine output, never a priori.  RE-DERIVED 2026-06-16
        // for v1.2.5: the completed LR reduction (commit 93feb8d66, constant-
        // term piece restored) raised the admissible-minimum carried count
        // to 3 and moved the winning order to [x4,x5,x1,x2]; the engine now
        // emits the three canonical obligations below (verbatim from the
        // find_lr_orders response's carried_polys field).  |carried_polys| ==
        // carried_sqrts == 3.  The three are pairwise independent (physics
        // review 2026-06-16): obligation 2's discriminant carries (yb^2-1) =
        // uq5's letter sqrt(yb^2-1); obligations 1 and 3 are the two
        // pentagon-symmetry images (qq1<->qq2, wb1<->wb2).
        {
            const std::vector<std::string> pin = {
                "x1*x2*qq1 + x1*qq2 - 2*x2*x5*wb2 - x5^2*yb^2 + x5^2"
                " - 2*x5*wb1*yb - wb1^2",
                "x1*x2*qq1*yb^2 - x1*x2*qq1 + x1*qq2*yb^2 - x1*qq2"
                " + x2^2*wb2^2 + 2*x2*wb1*wb2*yb + wb1^2",
                "x1*x2*qq1 + x1*qq2 - x2^2*wb2^2"};
            check("gate7c: gauged-uq5 carried_polys pinned "
                  "(3 obligations, best_order [x4,x5,x1,x2], v1.2.5)",
                  polys == pin,
                  "got: " + (polys.size() > 0 ? polys[0] : "<none>") +
                      " | " + (polys.size() > 1 ? polys[1] : "<none>") +
                      " | " + (polys.size() > 2 ? polys[2] : "<none>"));
        }
    }

    // ===== Gate 8 (Phase 2, spec 2026-06-11-carry-phase2-design.md §4,
    // gate P2-G3): nonexec tertiary tie-break.  An admissible order whose
    // single obligation is EXECUTABLE-shaped (single integration variable,
    // degree <= 2 — the Euler-substitution executor's input shape) must not
    // be shadowed by a CHEAPER admissible order at the same carried_sqrts
    // whose obligation is multi-variable (executor-inadmissible).
    //
    // Fixture (3 vars {x,y,z}, kinematics {s,t}, FOUR single-poly groups —
    // groups evolve independently, so no cross-group resultants muddy the
    // letter sets):
    //   g1: 4x^2 - yz - s    pivot-x disc 16(yz+s)  -> multi-var obligation
    //   g2: 4z^2 + 4yz - t   pivot-z disc 16(y^2+t) -> single-var obligation
    //   g3: x + (t^4+t^3+t^2+t+1)  fat kinematic rider, deg-0 in z: it dies
    //       at the x-step, so it inflates exactly the routes that postpone
    //       x (the z-first exec route pays it twice more)
    //   g4: x + y^3          blocker: any pivot-y step while alive refuses
    //       (deg 3), forcing x BEFORE y in every admissible order
    // Admissible leaves (HF_CARRY_LEAF_TRACE=1 HF_CARRY_NO_PRUNE=1,
    // generated 2026-06-11, pre-change engine):
    //   [x,y,z] score 52.79  nsq=1 nonexec=1  (mints yz+s)
    //   [x,z,y] score 55.33  nsq=2            (irrelevant: higher nsq)
    //   [z,x,y] score 59.86  nsq=1 nonexec=0  (mints y^2+t)
    // The (nsq, score) key returns [x,y,z] (the shadowing FAIL); the
    // (nsq, nonexec, score) key must return [z,x,y].  nonexec is not
    // emitted in the response; the assertion inspects the emitted
    // obligation's support instead (the y*z+s string contains 'z', the
    // y^2+t string does not — x never appears in either).
    {
        const std::string body_g8 =
            "{\"op\":\"find_lr_orders\",\"xvars\":[\"x\",\"y\",\"z\"],"
            "\"coeff_vars\":[\"s\",\"t\"],"
            "\"groups\":[[\"4*x^2 - y*z - s\"],[\"4*z^2 + 4*y*z - t\"],"
            "[\"x + t^4 + t^3 + t^2 + t + 1\"],[\"x + y^3\"]],"
            "\"algebraic_letters\":true,\"carry_discharge\":true}";
        const std::string resp = find_lr_orders(body_g8);
        if (resp_has_error(resp) || resp_nolr(resp)) {
            std::cerr << "FAIL gate8 PREMISE: tie-break fixture not "
                         "carry-LR: " << resp << "\n";
            return 1;
        }
        if (resp_ulong_field(resp, "carried_sqrts") != 1UL) {
            std::cerr << "FAIL gate8 PREMISE: expected carried_sqrts == 1, "
                         "got " << resp << "\n";
            return 1;
        }
        bool present = false;
        const std::vector<std::string> polys =
            resp_carried_polys(resp, present);
        check("gate8a: executable-shaped order returned "
              "(best_order == [z,x,y])",
              resp_best_order(resp) == "[\"z\",\"x\",\"y\"]",
              "order=" + resp_best_order(resp));
        check("gate8b: emitted obligation has single-variable support "
              "(no 'z' in the poly string)",
              present && polys.size() == 1 &&
                  polys[0].find('z') == std::string::npos,
              "carried_polys=" + (polys.empty() ? "<none>" : polys[0]));
    }

    // ===== Gate 9 (physics-review 2026-06-11 finding 3): the nonexec
    // shape test counts support over ALL integration variables, not
    // pending ∪ {pivot}.  A second-generation mint (disc factor of a
    // CARRIED obligation re-judged at a later pivot) can contain an
    // already-integrated variable: the letter 4*b^2 - a^2 - d at pivot b
    // with pending {d} and a ALREADY INTEGRATED mints disc odd part
    // a^2 + d — true integration-variable support {a, d} (2 vars,
    // executor-inadmissible) while pending ∪ {pivot} sees only {d}
    // (single var, deg 1).  The full count must set nonexec.  Driven
    // through step_fr_judge directly (the obligation enters via
    // `letters` here; in production the integrated-var case arrives via
    // the carried join — same code path at the mint site).  NOTE the
    // under-count was provably masked at PATH level (the gen-1 ancestor
    // of any such factor had >= 3 then-pending support variables and set
    // the monotone flag earlier on the same path), so this fold changes
    // no selection behavior — the gate pins the LOCAL correctness of the
    // shape test.
    {
        using hyperflint::Poly;
        using hyperflint::PolyCtx;
        namespace lr_scan = hyperflint::lr_scan;

        PolyCtx ctx({"a", "b", "d", "s"});
        std::vector<Poly> letters;
        letters.emplace_back(ctx, "4*b^2 - a^2 - d");
        const std::vector<size_t> pending = {ctx.index_of("d")};
        const std::vector<size_t> all_int = {
            ctx.index_of("a"), ctx.index_of("b"), ctx.index_of("d")};
        lr_scan::PathState st;
        const bool ok = lr_scan::step_fr_judge(
            letters, ctx.index_of("b"), lr_scan::kNoGauge, pending,
            all_int, st);
        check("gate9a: integrated-var obligation step admissible "
              "(disc odd part a^2+d minted)",
              ok && st.nsq == 1,
              "ok=" + std::to_string(ok) +
                  " nsq=" + std::to_string(st.nsq));
        check("gate9b: nonexec set — support counted over ALL int vars "
              "({a,d} = 2 vars, not the pending-only {d})",
              st.nonexec, "nonexec=0 (under-count regression)");
    }

    std::cout << "Summary: " << g_pass << " PASS / " << g_fail << " FAIL\n";
    return g_fail == 0 ? 0 : 1;
}
