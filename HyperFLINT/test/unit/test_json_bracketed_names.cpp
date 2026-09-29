// Bracketed symbol names in JSON request arrays (2026-09-21,
// INV-JSON-STRING-ARRAYS).
//
// The bridge extracted every JSON string array with a regex that stopped at
// the first `]` byte, blind to string quoting.  A polynomial containing an
// indexed symbol (`mm[1]`, the standard multi-mass naming) therefore
// truncated the array at that element: the order search and verify_order ran
// on a mutilated polynomial set and certified orders the integrator then
// refused (problem.nb, 2026-09-18: the pipeline's own request parsed to
// nPolys [1,3] for groups of 8 and 9), and the `hyperflint` op silently
// skipped a bracketed integration variable or range endpoint.
//
// Gates:
//   (1) JSON layer: strict, structural, string-aware extraction (escapes,
//       nesting, exact top-level member, failure instead of a parsed prefix,
//       empty arrays preserved, mismatched bracket types refused).
//   (2) find_lr_orders: the synthetic oracle x^2 + mm[1]*x*y + y + 1 with the
//       bare variables gives nPolys [3], NOLR with letters off, and
//       verify_order ["x","y"] NOT-LR at step 0 degree 2 (before the fix:
//       nPolys [0], best_order ["y","x"], order_is_lr true); indexed names
//       first / last in a group and in coeff_vars alone; an empty
//       verify_order stays verify mode; a non-permutation is malformed; a
//       malformed request is a structured error; an inadmissible name is
//       refused.
//   (3) the real face of problem.nb (fixture, argv[1]): nPolys [8,9], a best
//       order whose first pivot is linear in F, verify of that order true,
//       verify of the x5-first order false with a forbidden dependence.
//   (4) plain-name control: nPolys [3] and the NOLR verdict unchanged.
//   (5) hyperflint: a bracketed integration variable is admissible
//       (the tokenizer merges name[ints]) and must be INTEGRATED, never
//       returned unintegrated; a bracketed range endpoint is not silently
//       dropped; Sqrt[MM] and m[k] are refused; numeric range endpoints
//       (expressions, not names) keep their values 1/2, 1/3, 1/3.
//   (6) sibling ops carry nPolys; an empty inner group is accepted and
//       scored as before (no new refusal); the variable-discovery scan of
//       partial_fractions / linear_factors keeps mm[1] whole.
//
// The response readers here are string-aware on purpose: those of the older
// tests (test_find_lr_orders_carry_discharge.cpp:66,109,120) repeat the
// first-`]` regex and cannot be reused for bracketed content.

#include "hyperflint/bridge/handlers.hpp"
#include "hyperflint/bridge/json_min.hpp"

#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef HF_TEST_MZV_DATA
#error "HF_TEST_MZV_DATA must be defined (path to mzv_reductions.json)."
#endif
#define HF_STRINGIFY_INNER(x) #x
#define HF_STRINGIFY(x) HF_STRINGIFY_INNER(x)

namespace {

int g_pass = 0, g_fail = 0;
void check(const char* label, bool cond, const std::string& diag = "") {
    if (cond) {
        std::cout << "[PASS] " << label << "\n";
        ++g_pass;
    } else {
        std::cerr << "[FAIL] " << label;
        if (!diag.empty()) std::cerr << " -- " << diag;
        std::cerr << "\n";
        ++g_fail;
    }
}

template <class Fn> bool throws(Fn f) {
    try { f(); return false; } catch (const std::exception&) { return true; }
}

bool has_error(const std::string& r) { return r.find("\"error\"") != std::string::npos; }

bool bool_field(const std::string& r, const char* key) {
    std::regex rx("\"" + std::string(key) + "\"\\s*:\\s*(true|false)");
    std::smatch m;
    return std::regex_search(r, m, rx) && m[1].str() == "true";
}

long int_field(const std::string& r, const char* key) {
    std::regex rx("\"" + std::string(key) + "\"\\s*:\\s*(-?[0-9]+)");
    std::smatch m;
    if (std::regex_search(r, m, rx)) return std::stol(m[1].str());
    return -999;
}

// Integer array field, e.g. "nPolys":[8,9] (integers only, never strings).
std::vector<long> int_array(const std::string& r, const char* key) {
    std::vector<long> out;
    std::regex rx("\"" + std::string(key) + "\"\\s*:\\s*\\[([0-9,\\s]*)\\]");
    std::smatch m;
    if (!std::regex_search(r, m, rx)) return out;
    std::stringstream ss(m[1].str());
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) out.push_back(std::stol(tok));
    return out;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const std::string kMzv = HF_STRINGIFY(HF_TEST_MZV_DATA);

std::string hf_req(const std::string& expr, const std::string& vars_int,
                   const std::string& vars, const std::string& extra = "") {
    return "{\"op\":\"hyperflint\",\"expr\":\"" + expr + "\",\"vars_int\":[" + vars_int +
           "],\"vars\":[" + vars + "]" + extra + ",\"mzv_data_path\":\"" + kMzv +
           "\",\"check_divergences\":false}";
}

}  // namespace

int main(int argc, char** argv) {
    using hyperflint::jsonmin::json_array_interior;
    using hyperflint::jsonmin::json_member_present;
    using hyperflint::jsonmin::json_str_array;
    using hyperflint::jsonmin::json_subarrays;

    // ===== Gate 1: JSON layer =====
    {
        auto r = json_str_array("{\"k\":[\"a\\\"b\",\"c]d\",\"e[1]\",\"\"],\"z\":[\"x\"]}", "k");
        check("g1a escapes, ] inside a string, empty string kept",
              r.size() == 4 && r[0] == "a\"b" && r[1] == "c]d" && r[2] == "e[1]" && r[3] == "");
        check("g1b exact top-level member (key text as a value, similar keys)",
              json_str_array("{\"note\":\"vars\",\"other\":[\"wrong\"],\"vars_int\":[\"v\"],\"vars\":[\"x\"]}", "vars")
                  == std::vector<std::string>{"x"});
        check("g1c key text inside a value with a colon and bracket",
              json_str_array("{\"expr\":\"vars:[\\\"q\\\"]\",\"vars\":[\"x\",\"mm[1]\"]}", "vars").size() == 2);
        check("g1d nested member not matched at top level",
              json_str_array("{\"a\":{\"vars\":[\"inner\"]},\"vars\":[\"top\"]}", "vars")
                  == std::vector<std::string>{"top"});
        check("g1e empty array preserved", json_str_array("{\"coeff_vars\":[],\"x\":1}", "coeff_vars").empty());
        check("g1f absent member -> empty", json_str_array("{\"x\":1}", "coeff_vars").empty());
        check("g1g presence of an empty verify_order",
              json_member_present("{\"verify_order\":[]}", "verify_order") &&
                  !json_member_present("{\"x\":[]}", "verify_order"));
        check("g1h truncated document throws", throws([] { json_str_array("{\"k\":[\"a\",\"b", "k"); }));
        check("g1i non-string element throws", throws([] { json_str_array("{\"k\":[\"a\",1]}", "k"); }));
        check("g1j missing comma throws", throws([] { json_str_array("{\"k\":[\"a\" \"b\"]}", "k"); }));
        check("g1k invalid escape throws", throws([] { json_str_array("{\"k\":[\"a\\q\"]}", "k"); }));
        check("g1l backslash runs",
              json_str_array("{\"k\":[\"a\\\\\\\\b\",\"c\\\\\"]}", "k") == std::vector<std::string>{"a\\\\b", "c\\"});
        check("g1m unicode escape", json_str_array("{\"k\":[\"\\u0041\"]}", "k") == std::vector<std::string>{"A"});
        auto g = json_subarrays(json_array_interior("{\"groups\":[[\"a]\",\"x\"],[\"y[\"],[]],\"n\":2}", "groups"));
        check("g1n nested arrays with lone brackets inside strings, empty inner group kept",
              g.size() == 3 && json_str_array("{\"xs\":" + g[0] + "}", "xs").size() == 2 &&
                  json_str_array("{\"xs\":" + g[1] + "}", "xs")[0] == "y[" &&
                  json_str_array("{\"xs\":" + g[2] + "}", "xs").empty());
        check("g1o mismatched bracket types throw (a ] cannot close a {, nor } a [)",
              throws([] { json_str_array("{\"a\":[1},\"k\":[\"x\"]}", "k"); }) &&
                  throws([] { json_str_array("{\"a\":{\"b\":1],\"k\":[\"x\"]}", "k"); }));
        // codex referee: a surrogate pair decoded half by half gave CESU-8 bytes
        // without throwing; a lone surrogate is a malformed document.
        check("g1p surrogate pair \\uD83D\\uDE00 decodes to U+1F600 (f0 9f 98 80); lone surrogates throw",
              json_str_array("{\"k\":[\"\\uD83D\\uDE00\"]}", "k") == std::vector<std::string>{"\xF0\x9F\x98\x80"} &&
                  throws([] { json_str_array("{\"k\":[\"\\uD83Dx\"]}", "k"); }) &&
                  throws([] { json_str_array("{\"k\":[\"\\uDE00\"]}", "k"); }));
    }

    // ===== Gate 2: find_lr_orders synthetic oracle =====
    const std::string synth_br =
        "{\"op\":\"find_lr_orders\",\"groups\":[[\"x^2 + mm[1]*x*y + y + 1\",\"x\",\"y\"]],"
        "\"xvars\":[\"x\",\"y\"],\"coeff_vars\":[\"mm[1]\"]}";
    const std::string synth_pl =
        "{\"op\":\"find_lr_orders\",\"groups\":[[\"x^2 + mm1*x*y + y + 1\",\"x\",\"y\"]],"
        "\"xvars\":[\"x\",\"y\"],\"coeff_vars\":[\"mm1\"]}";
    {
        auto rb = hyperflint::handlers::find_lr_orders(synth_br);
        auto rp = hyperflint::handlers::find_lr_orders(synth_pl);
        check("g2a bracketed: nPolys [3]", int_array(rb, "nPolys") == std::vector<long>{3}, rb.substr(0, 300));
        check("g2b bracketed: NOLR with letters off, same verdict as the plain control",
              bool_field(rb, "nolr") && bool_field(rp, "nolr"), rb.substr(0, 300));
        auto vb = hyperflint::handlers::find_lr_orders(
            "{\"op\":\"find_lr_orders\",\"groups\":[[\"x^2 + mm[1]*x*y + y + 1\",\"x\",\"y\"]],"
            "\"xvars\":[\"x\",\"y\"],\"coeff_vars\":[\"mm[1]\"],\"verify_order\":[\"x\",\"y\"]}");
        check("g2c bracketed verify x-first: NOT LR at step 0 degree 2",
              !bool_field(vb, "order_is_lr") && int_field(vb, "verify_blocking_step") == 0 &&
                  int_field(vb, "verify_blocking_degree") == 2, vb.substr(0, 300));
        auto r3 = hyperflint::handlers::find_lr_orders(
            "{\"op\":\"find_lr_orders\",\"groups\":[[\"mm[1] + x\",\"x + y\",\"y + mm[2]\"]],"
            "\"xvars\":[\"x\",\"y\"],\"coeff_vars\":[\"mm[1]\",\"mm[2]\"]}");
        check("g2d indexed names first and last in a group: nPolys [3]",
              int_array(r3, "nPolys") == std::vector<long>{3}, r3.substr(0, 300));
        auto r4 = hyperflint::handlers::find_lr_orders(
            "{\"op\":\"find_lr_orders\",\"groups\":[[\"x + y\"],[\"y + 1\"]],"
            "\"xvars\":[\"x\",\"y\"],\"coeff_vars\":[\"mm[1]\",\"M[2]\"]}");
        check("g2e indexed coeff_vars with plain polynomials: nPolys [1,1]",
              int_array(r4, "nPolys") == std::vector<long>{1, 1}, r4.substr(0, 300));
        auto ve = hyperflint::handlers::find_lr_orders(
            "{\"op\":\"find_lr_orders\",\"groups\":[[\"x + y\"]],\"xvars\":[\"x\",\"y\"],"
            "\"coeff_vars\":[],\"verify_order\":[]}");
        check("g2f empty verify_order stays verify mode (malformed), never a free search",
              bool_field(ve, "verify_malformed") && !bool_field(ve, "order_is_lr") &&
                  json_str_array(ve, "best_order").empty(), ve.substr(0, 300));
        auto vp = hyperflint::handlers::find_lr_orders(
            "{\"op\":\"find_lr_orders\",\"groups\":[[\"x + y\"]],\"xvars\":[\"x\",\"y\"],"
            "\"coeff_vars\":[],\"verify_order\":[\"x\",\"x\"]}");
        check("g2g verify_order not a permutation of xvars -> malformed",
              bool_field(vp, "verify_malformed") && !bool_field(vp, "order_is_lr"), vp.substr(0, 300));
        auto bad = hyperflint::handlers::find_lr_orders(
            "{\"op\":\"find_lr_orders\",\"groups\":[[\"x + y\",1]],\"xvars\":[\"x\",\"y\"],\"coeff_vars\":[]}");
        check("g2h non-string group element -> structured error", has_error(bad), bad.substr(0, 300));
        auto sq = hyperflint::handlers::find_lr_orders(
            "{\"op\":\"find_lr_orders\",\"groups\":[[\"x + Sqrt[MM]\"]],\"xvars\":[\"x\"],\"coeff_vars\":[\"Sqrt[MM]\"]}");
        check("g2i inadmissible coefficient name Sqrt[MM] -> structured error", has_error(sq), sq.substr(0, 300));
        auto mk = hyperflint::handlers::find_lr_orders(
            "{\"op\":\"find_lr_orders\",\"groups\":[[\"x + m[k]\"]],\"xvars\":[\"x\"],\"coeff_vars\":[\"m[k]\"]}");
        check("g2j inadmissible coefficient name m[k] -> structured error", has_error(mk), mk.substr(0, 300));
        // codex referee: an EMPTY name collided with the "all admissible" sentinel.
        auto em = hyperflint::handlers::find_lr_orders(
            "{\"op\":\"find_lr_orders\",\"groups\":[[\"x + y\"]],\"xvars\":[\"x\",\"\"],\"coeff_vars\":[]}");
        check("g2k an empty name in xvars is refused", has_error(em), em.substr(0, 300));
        // codex referee: a control character in a refused name was echoed
        // unescaped into the error envelope, which was then invalid JSON.
        auto cc = hyperflint::handlers::find_lr_orders(
            "{\"op\":\"find_lr_orders\",\"groups\":[[\"x + y\"]],\"xvars\":[\"x\"],\"coeff_vars\":[\"y\\u0008\"]}");
        check("g2l a name with a control character is refused and the error envelope is valid JSON",
              has_error(cc) && !throws([&] { hyperflint::jsonmin::json_str_field(cc, "error"); }) &&
                  hyperflint::jsonmin::json_str_field(cc, "error").find('\b') != std::string::npos, cc.substr(0, 300));
    }

    // ===== Gate 3: the real face (fixture path from argv[1]) =====
    if (argc > 1) {
        const std::string body = read_file(argv[1]);
        auto r = hyperflint::handlers::find_lr_orders(body);
        check("g3a fixture nPolys [8,9]", int_array(r, "nPolys") == std::vector<long>{8, 9}, r.substr(0, 300));
        auto order = json_str_array(r, "best_order");
        check("g3b fixture best order starts with a variable linear in F (x1 or x2)",
              !order.empty() && (order[0] == "x1" || order[0] == "x2"), r.substr(0, 300));
        if (!order.empty()) {
            std::string vbody = body.substr(0, body.rfind('}')) + ",\"verify_order\":[";
            for (size_t i = 0; i < order.size(); ++i) vbody += (i ? ",\"" : "\"") + order[i] + "\"";
            vbody += "]}";
            auto v = hyperflint::handlers::find_lr_orders(vbody);
            check("g3c fixture: verify_order of the returned order is LR", bool_field(v, "order_is_lr"), v.substr(0, 300));
        }
        std::string x5body = body.substr(0, body.rfind('}')) +
                             ",\"verify_order\":[\"x5\",\"x6\",\"x3\",\"x2\",\"x1\"]}";
        auto v5 = hyperflint::handlers::find_lr_orders(x5body);
        check("g3d fixture: verify x5-first is NOT LR with a forbidden dependence",
              !bool_field(v5, "order_is_lr") && bool_field(v5, "verify_forbidden_dep"), v5.substr(0, 300));
    } else {
        std::cout << "[SKIP] gate 3 (no fixture path given)\n";
    }

    // ===== Gate 4: plain-name control =====
    {
        auto rp = hyperflint::handlers::find_lr_orders(synth_pl);
        check("g4a plain control carries nPolys [3] and the NOLR verdict",
              int_array(rp, "nPolys") == std::vector<long>{3} && bool_field(rp, "nolr"), rp.substr(0, 300));
    }

    // ===== Gate 5: hyperflint op with bracketed names =====
    {
        auto plain = hyperflint::handlers::hyperflint_sym(hf_req("1/(1+x)^2", "\"x\"", "\"x\""));
        check("g5a plain control integrates to 1", plain.find("\"coef\":\"1\"") != std::string::npos, plain.substr(0, 300));
        auto r = hyperflint::handlers::hyperflint_sym(hf_req("1/(1+x[1])^2", "\"x[1]\"", "\"x[1]\""));
        check("g5b bracketed integration variable is admissible and integrated to 1 (before the fix: returned unintegrated)",
              r.find("\"coef\":\"1\"") != std::string::npos && r.find("x[1]^2") == std::string::npos, r.substr(0, 300));
        auto p = hyperflint::handlers::hyperflint_sym(hf_req("1/((1+y)^2*(1+m[1])^2)", "\"y\",\"m[1]\"", "\"y\",\"m[1]\""));
        {
            // the response echoes its vars list, so test the result array only
            const size_t rs = p.find("\"result\":");
            const size_t re = p.find(",\"timing_compute_s\"");
            const std::string result = (rs != std::string::npos && re != std::string::npos && re > rs)
                                           ? p.substr(rs, re - rs) : p;
            check("g5c partial list [y, m[1]] integrates both (result 1, free of m[1])",
                  result.find("\"coef\":\"1\"") != std::string::npos && result.find("m[1]") == std::string::npos,
                  p.substr(0, 300));
        }
        auto rr = hyperflint::handlers::hyperflint_sym(
            hf_req("1/(1+x)^2", "\"x\"", "\"x\"", ",\"vars_int_from\":[\"a[1]\"],\"vars_int_to\":[\"b[1]\"]"));
        check("g5d bracketed range endpoints are not silently dropped (not the full-range value 1)",
              has_error(rr) || rr.find("\"coef\":\"1\"") == std::string::npos, rr.substr(0, 300));
        auto sq = hyperflint::handlers::hyperflint_sym(hf_req("1/(1+x)^2", "\"x\"", "\"x\",\"Sqrt[MM]\""));
        check("g5e inadmissible name Sqrt[MM] in vars -> structured error", has_error(sq), sq.substr(0, 300));
        auto mk = hyperflint::handlers::hyperflint_sym(hf_req("1/(1+x)^2", "\"x\"", "\"x\",\"m[k]\""));
        check("g5f inadmissible name m[k] in vars -> structured error", has_error(mk), mk.substr(0, 300));
        // Range endpoints are EXPRESSIONS and are never subject to the name
        // predicate: the first implementation refused every numeric range.
        // The three test/cross/fixtures/hf_rescale_*.json requests, replayed.
        auto z1 = hyperflint::handlers::hyperflint_sym(
            hf_req("1/(1+x)^2", "\"x\"", "\"x\"", ",\"vars_int_from\":[\"0\"],\"vars_int_to\":[\"1\"]"));
        check("g5g numeric range [0,1]: 1/(1+x)^2 -> 1/2 (hf_rescale_zero_one)",
              z1.find("\"coef\":\"(1/2)\"") != std::string::npos, z1.substr(0, 300));
        auto sh = hyperflint::handlers::hyperflint_sym(
            hf_req("1/(1+x)^2", "\"x\"", "\"x\"", ",\"vars_int_from\":[\"2\"],\"vars_int_to\":[\"Infinity\"]"));
        check("g5h range [2,Infinity]: 1/(1+x)^2 -> 1/3 (hf_rescale_shift)",
              sh.find("\"coef\":\"(1/3)\"") != std::string::npos, sh.substr(0, 300));
        auto ab = hyperflint::handlers::hyperflint_sym(
            "{\"op\":\"hyperflint\",\"f\":\"x^2\",\"vars_int\":[\"x\"],\"vars_int_from\":[\"0\"],"
            "\"vars_int_to\":[\"1\"],\"vars\":[\"x\"],\"mzv_data_path\":\"" + kMzv + "\"}");
        check("g5i hf_rescale_finite_ab verbatim (f key): x^2 on [0,1] -> 1/3",
              ab.find("\"coef\":\"(1/3)\"") != std::string::npos, ab.substr(0, 300));
        // A symbolic endpoint (an expression in a declared spectator) is
        // parsed by the rescaling: refused by the round-1 predicate, truncated
        // by the pre-fix regex.
        auto se = hyperflint::handlers::hyperflint_sym(
            hf_req("x^2", "\"x\"", "\"x\",\"mm[1]\"", ",\"vars_int_from\":[\"0\"],\"vars_int_to\":[\"mm[1]\"]"));
        check("g5j symbolic endpoint mm[1]: x^2 on [0, mm[1]] -> mm[1]^3/3",
              se.find("mm[1]^3") != std::string::npos && !has_error(se), se.substr(0, 300));
        // The engine's own atom tokens are not variable names (the decoder
        // would rewrite them: mzv_2 -> Zeta[2], Log2 -> Log[2], Wm_1 -> Wm[i]).
        auto t1 = hyperflint::handlers::hyperflint_sym(hf_req("1/(1+x)^2", "\"x\"", "\"x\",\"mzv_2\""));
        auto t2 = hyperflint::handlers::hyperflint_sym(hf_req("1/(1+x)^2", "\"x\"", "\"x\",\"Log2\""));
        auto t3 = hyperflint::handlers::find_lr_orders(
            "{\"op\":\"find_lr_orders\",\"groups\":[[\"x + Wm_1\"]],\"xvars\":[\"x\"],\"coeff_vars\":[\"Wm_1\"]}");
        check("g5k reserved atom tokens mzv_2, Log2, Wm_1 are refused as variable names (integrator and search)",
              has_error(t1) && has_error(t2) && has_error(t3), t1.substr(0, 200) + " | " + t3.substr(0, 200));
    }

    // ===== Gate 6: sibling ops carry nPolys; empty inner group accepted =====
    {
        auto e = hyperflint::handlers::find_lr_orders(
            "{\"op\":\"find_lr_orders\",\"groups\":[[],[\"x + y\",\"x\",\"y\"]],\"xvars\":[\"x\",\"y\"],\"coeff_vars\":[]}");
        check("g6a empty inner group accepted and reported as nPolys [0,3]",
              int_array(e, "nPolys") == std::vector<long>{0, 3} && !has_error(e), e.substr(0, 300));
        auto ft = hyperflint::handlers::factor_table(
            "{\"op\":\"factor_table\",\"groups\":[[\"x + mm[1]\",\"x\",\"y\"]],\"xvars\":[\"x\",\"y\"],"
            "\"coeff_vars\":[\"mm[1]\"],\"order\":[\"y\",\"x\"]}");
        check("g6b factor_table carries nPolys [3] with a bracketed name",
              int_array(ft, "nPolys") == std::vector<long>{3}, ft.substr(0, 300));
        check("g6c schema_version is 3 on every envelope",
              int_field(e, "schema_version") == 3 && int_field(ft, "schema_version") == 3);
        // The variable-discovery scan of partial_fractions / linear_factors
        // (no explicit "vars") merges an indexed name into one identifier, as
        // the tokenizer does; before the change it split mm[1] into mm and the
        // parse failed ("Poly: parse error").
        auto pf = hyperflint::handlers::partial_fractions(
            "{\"op\":\"partial_fractions\",\"f\":\"1/((x+mm[1])*(x+2))\",\"var\":\"x\"}");
        check("g6d partial_fractions autoscan discovers mm[1] (poles at -2 and -mm[1])",
              !has_error(pf) && pf.find("\"pole\":\"-mm[1]\"") != std::string::npos &&
                  pf.find("\"pole\":\"-2\"") != std::string::npos, pf.substr(0, 300));
        auto lf = hyperflint::handlers::linear_factors(
            "{\"op\":\"linear_factors\",\"poly\":\"(x+mm[1])*(x+2)\",\"var\":\"x\"}");
        check("g6e linear_factors autoscan discovers mm[1] (no error)", !has_error(lf), lf.substr(0, 300));
    }

    std::cout << "Summary: " << g_pass << " PASS / " << g_fail << " FAIL\n";
    return g_fail == 0 ? 0 : 1;
}
