// bench_parse_factored: evidence for the HF perf-campaign "stay-factored" lever.
//
// Premise (campaign findings F3): for an input (NUM)^p/(DEN)^q the engine's
// Rat::parse hands the WHOLE string to FLINT's fmpq_mpoly_set_str_pretty, which
// eagerly EXPANDS the powers (fmpz_mpoly_pow_fps) into fully-expanded
// multivariate polynomials at PARSE time -- dominating the wall and timing out
// before the integration loop is even reached (native 1m-qbox: 0 step traces in
// 45s, 100% pow_fps across 3 sample windows).
//
// This bench proves the avoidable cost: parsing the NUM and DEN *bases* (without
// the powers) is milliseconds, while expanding them to ^p/^q is what explodes.
// It also gates the algebra: materializing numbase^p / denbase^q via FactoredRat
// equals Rat::parse of the full string (value-equal), so a deferred-power
// representation is sound.
//
// This is a PURELY ADDITIVE diagnostic (no existing code path changed).
// Run directly; exit 0 = the correctness gate passed.
//
// Usage: hyperflint-bench-parse-factored <numbase.txt> <denbase.txt> <vars.csv>
//        (files hold the base strings and the comma-separated var list)

#include "hyperflint/core/factored_rat.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/poly.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace hyperflint;

namespace {
int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ \
                            << "  " << #cond << "\n"; ++g_failures; } } while (0)

// Representation-independent value equality (Rat is lowest-terms but not
// content-canonical): N_u*D_v == N_v*D_u. Mirrors test_factored_rat.cpp.
bool value_equal(const Rat& u, const Rat& v) {
    return u.num().mul(v.den()).equal(v.num().mul(u.den()));
}

std::string slurp(const char* path) {
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == ',') { out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

int main(int argc, char** argv) {
    // ---- Correctness gate (small, self-contained): deferred-power == parse ---
    {
        PolyCtx ctx({"a", "x"});
        Poly nb(ctx, "a + x");          // numerator base
        Poly db(ctx, "1 + x");          // denominator base
        const long p = 2, q = 3;
        // Deferred representation: numerator nb^p over factored den {(db, q)}.
        FactoredRat fr = FactoredRat::from_poly(
            nb.pow(static_cast<unsigned long>(p)));
        fr.push_factor(db, q);
        Rat mat = fr.materialize_to_rat();
        Rat ref = Rat::parse(ctx, "(a + x)^2/(1 + x)^3");
        CHECK(value_equal(mat, ref));
        // And the denominator really is kept factored (one factor, exp q).
        CHECK(fr.den_factors().size() == 1);
        CHECK(fr.den_factors()[0].exp == q);
        std::cout << "[gate] deferred-power materialize == Rat::parse : "
                  << (g_failures == 0 ? "PASS" : "FAIL") << "\n";
    }

    // ---- Premise demo on the real qbox bases (if provided) ------------------
    if (argc >= 4) {
        std::string numbase = slurp(argv[1]);
        std::string denbase = slurp(argv[2]);
        std::vector<std::string> vars = split_csv(slurp(argv[3]));
        PolyCtx ctx(vars);

        auto t0 = std::chrono::steady_clock::now();
        Poly nb(ctx, numbase);
        double t_num = ms_since(t0);

        auto t1 = std::chrono::steady_clock::now();
        Poly db(ctx, denbase);
        double t_den = ms_since(t1);

        std::cout << "[qbox] vars=" << vars.size()
                  << "  numbase: " << nb.n_terms() << " terms parsed in "
                  << t_num << " ms\n"
                  << "[qbox] denbase: " << db.n_terms() << " terms parsed in "
                  << t_den << " ms\n"
                  << "[qbox] base-parse total = " << (t_num + t_den)
                  << " ms  (vs Rat::parse of (numbase)^3/(denbase)^5 which the\n"
                  << "       campaign measured as a TIMEOUT: 100% fmpz_mpoly_pow_fps,\n"
                  << "       0 integration steps reached). The power expansion, not\n"
                  << "       the parse, is the wall -> deferring it is the lever.\n";
        // Optional: show how explosive even ONE power is (denbase^2), bounded.
        auto t2 = std::chrono::steady_clock::now();
        Poly db2 = db.pow(2);
        double t_db2 = ms_since(t2);
        std::cout << "[qbox] denbase^2 expand: " << db2.n_terms()
                  << " terms in " << t_db2 << " ms (one power; ^5 is far worse)\n";
    } else {
        std::cout << "[qbox] (no base files given; ran correctness gate only)\n";
    }

    std::cout << (g_failures == 0 ? "ALL PASS\n" : "FAILURES\n");
    return g_failures == 0 ? 0 : 1;
}
