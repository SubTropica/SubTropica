// verify_order_is_lr multi-group false-negative — regression battery.
//
// THE BUG (2026-06-17, HyperInt-adjudicated; notes/verify_multigroup_bug/).
// verify_order_is_lr walked a SINGLE reduction path along the order
// (cur = st_fubini_lr(cur, pivot)), which over-approximates the Landau set:
// st_fubini_lr emits ALL pairwise resultants with no compatibility graph, so a
// single path accumulates order-dependent SPURIOUS letters (e.g. x8^2+x8+1)
// that are not genuine singularities.  A spurious deg-2 letter made verify
// reject genuinely-LR multi-group (lazy-sum) orders that find_lr_orders (the
// SEARCH) and HyperInt both accept.  The fix reads letters from the SAME
// intersection-refined subset table the SEARCH uses (a letter survives a subset
// only if produced along EVERY pivot path into it), so verify agrees with the
// SEARCH by construction (and with HyperInt on every order the SEARCH accepts).
//
// Gates (all post JSON request bodies to hyperflint::handlers::find_lr_orders,
// the same boundary the Mma VerifyOrder option / carry order-pinning guard
// crosses):
//   (1) FIX — multi-group flip: the real e26 face (2 groups, 7 vars, the
//       simplest repro) in its search-found order is LR.  The OLD single-path
//       code returned order_is_lr=false here (the false-negative); the fix
//       returns true.  HyperInt: LR (2/2 groups).
//   (2) NOT A RUBBER-STAMP — genuine multi-group not-LR: a synthetic 2-group
//       face whose first group carries the irreducible quadratic 1+x+x^2 must
//       be rejected (order_is_lr=false) in any order that pivots on x.  This
//       deg-2 obstruction is real (HyperInt rejects it under strict, non-
//       algebraic LR); the fix must NOT certify it.
//   (3) FAST-ACCEPT path — genuine LR with no spurious blocker: a synthetic
//       2-group all-linear face is LR; the single-path screen accepts it
//       directly (O(n), no O(2^n) table build).
//   (4) malformed input: an order that is not a permutation of xvars is
//       reported malformed (and not silently certified).
//
// Tier-1 wall: e26 sub-100 ms, synthetics sub-ms.

#include "hyperflint/bridge/handlers.hpp"

#include <iostream>
#include <regex>
#include <string>

namespace {

// ---- response field accessors (hand-rolled, same style as
//      test_find_lr_orders_carry_discharge.cpp) ----

// Tri-state for order_is_lr: +1 true, 0 false, -1 absent.
int resp_order_is_lr(const std::string& resp) {
    std::regex rx(R"~("order_is_lr"\s*:\s*(true|false))~");
    std::smatch m;
    if (std::regex_search(resp, m, rx)) return m[1].str() == "true" ? 1 : 0;
    return -1;
}

bool resp_verify_malformed(const std::string& resp) {
    std::regex rx(R"~("verify_malformed"\s*:\s*(true|false))~");
    std::smatch m;
    if (std::regex_search(resp, m, rx)) return m[1].str() == "true";
    return false;
}

bool resp_has_error(const std::string& resp) {
    return resp.find("\"error\"") != std::string::npos;
}

std::string resp_blocking_letter(const std::string& resp) {
    std::regex rx(R"~("verify_blocking_letter"\s*:\s*"([^"]*)")~");
    std::smatch m;
    if (std::regex_search(resp, m, rx)) return m[1].str();
    return "<absent>";
}

// ---- fixtures ----

// The real e26__ord2_face6 face: 2 lazy-sum groups, 7 integration variables
// (x2,x4,x5,x6,x7,x8,x9), no masses.  Verbatim from
// notes/verify_multigroup_bug/faces/e26__ord2_face6.json (groups =
// Join[faceGroup, xvars]).  In the search-found order the single-path walk
// blocks on the spurious letter x8^2+x8+1; the intersection drops it.
std::string e26_verify_request(const std::string& order_quoted_csv) {
    return std::string(R"~({"op":"find_lr_orders","schema_version_min":1,"groups":[)~")
        + R"~(["1 + x2","1 + x4",)~"
        + R"~("x5*x6 + x5*x7 + x6*x7 + x5*x6*x7 + x5*x6*x8 + x5*x7*x8 + x6*x7*x8 + x5*x9 + x7*x9 + x5*x7*x9 + x5*x8*x9 + x7*x8*x9",)~"
        + R"~("x5*x6*x7 + x5*x6*x8 + x5*x7*x8 + x6*x7*x8 + x5*x6*x9 + x6*x7*x9 + x5*x6*x7*x9 + x5*x8*x9 + x5*x6*x8*x9 + x7*x8*x9 + x5*x7*x8*x9 + x6*x7*x8*x9",)~"
        + R"~("x2","x4","x5","x6","x7","x8","x9"],)~"
        + R"~(["1 + x2","1 + x4","x5 + x7 + x5*x7 + x5*x8 + x7*x8","1 + x9","x6 + x9",)~"
        + R"~("x2","x4","x5","x6","x7","x8","x9"]],)~"
        + R"~("xvars":["x2","x4","x5","x6","x7","x8","x9"],"coeff_vars":[],)~"
        + R"~("verify_order":[)~" + order_quoted_csv + "]}";
}

// Synthetic 2-group face over (x,y).  Group 0 carries the irreducible quadratic
// 1+x+x^2 (genuine deg-2-in-x obstruction); group 1 is linearly reducible.  An
// LR order requires EVERY group to pass, so any order that pivots on x is
// not-LR.  (HyperInt rejects 1+x+x^2 under strict non-algebraic LR.)
std::string synth_notlr_request(const std::string& order_quoted_csv) {
    return std::string(R"~({"op":"find_lr_orders","schema_version_min":1,"groups":[)~")
        + R"~(["1 + x + x^2","x","y"],["1 + y","x","y"]],)~"
        + R"~("xvars":["x","y"],"coeff_vars":[],)~"
        + R"~("verify_order":[)~" + order_quoted_csv + "]}";
}

// Synthetic 2-group all-linear face over (x,y): every letter is linear in every
// variable, so it is LR in any order and the single-path screen accepts it.
std::string synth_lr_request(const std::string& order_quoted_csv) {
    return std::string(R"~({"op":"find_lr_orders","schema_version_min":1,"groups":[)~")
        + R"~(["x","1 + x","y"],["y","1 + y","x"]],)~"
        + R"~("xvars":["x","y"],"coeff_vars":[],)~"
        + R"~("verify_order":[)~" + order_quoted_csv + "]}";
}

int g_failures = 0;
void expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "  FAIL: " << msg << "\n"; ++g_failures; }
    else std::cout << "  ok: " << msg << "\n";
}

}  // namespace

int main() {
    using hyperflint::handlers::find_lr_orders;

    // Gate (1): THE FIX.  e26 in its search-found order is LR.  Pre-fix this
    // returned order_is_lr=false (single-path blocked on spurious x8^2+x8+1).
    {
        std::cout << "[gate 1] e26 multi-group search-found order -> LR\n";
        const std::string resp = find_lr_orders(
            e26_verify_request(R"~("x7","x5","x6","x9","x8","x4","x2")~"));
        expect(!resp_has_error(resp), "no handler error");
        expect(resp_order_is_lr(resp) == 1,
               "order_is_lr == true (was false pre-fix; HyperInt: LR 2/2)");
    }

    // Gate (2): NOT A RUBBER-STAMP.  Genuine deg-2 obstruction must be rejected.
    {
        std::cout << "[gate 2] synthetic 1+x+x^2 multi-group order -> not-LR\n";
        const std::string resp = find_lr_orders(
            synth_notlr_request(R"~("x","y")~"));
        expect(!resp_has_error(resp), "no handler error");
        expect(resp_order_is_lr(resp) == 0,
               "order_is_lr == false (genuine deg-2 letter 1+x+x^2 in x)");
        expect(resp_blocking_letter(resp).find("x^2") != std::string::npos ||
               resp_blocking_letter(resp).find("x2") != std::string::npos,
               "blocking letter is the quadratic in x");
    }

    // Gate (3): FAST-ACCEPT path.  All-linear multi-group face is LR.
    {
        std::cout << "[gate 3] synthetic all-linear multi-group order -> LR\n";
        const std::string resp = find_lr_orders(
            synth_lr_request(R"~("x","y")~"));
        expect(!resp_has_error(resp), "no handler error");
        expect(resp_order_is_lr(resp) == 1, "order_is_lr == true (single-path accept)");
    }

    // Gate (4): malformed order (not a permutation of xvars) is flagged.
    {
        std::cout << "[gate 4] malformed order (missing/extra var) -> verify_malformed\n";
        const std::string resp = find_lr_orders(
            synth_lr_request(R"~("x","x")~"));  // duplicate, not a permutation
        expect(resp_verify_malformed(resp), "verify_malformed == true");
        expect(resp_order_is_lr(resp) != 1, "not silently certified LR");
    }

    if (g_failures) {
        std::cerr << "\nVERIFY-MULTIGROUP REGRESSION: " << g_failures
                  << " failure(s)\n";
        return 1;
    }
    std::cout << "\nVERIFY-MULTIGROUP REGRESSION: all gates passed\n";
    return 0;
}
