// bench_parse_unknown_var.cpp
//
// R22 Step 1 / R23 follow-up: validate sentinel-tolerance prerequisites.
//
// Two phases:
//
//   Phase 1 (FLINT layer): confirm fmpq_mpoly_set_str_pretty returns
//   nonzero (does NOT abort) when the input expression references a
//   variable name that is not in the PolyCtx.  Established 2026-05-03
//   in the original chain-13 smoke test (6/6 PASS).
//
//   Phase 2 (HF C++ wrapper): R23 reviewer found that Poly::Poly(ctx,
//   expr) has a double-clear bug — the in-body fmpq_mpoly_clear
//   followed by the destructor's clear (after the throw triggers
//   stack unwinding of the partially-constructed object) can SIGTRAP
//   on real production RHS strings (compound expressions with
//   rational coefficients).  Phase 2 calls Poly::Poly directly
//   under fork-per-case so that a SIGTRAP from one case does not
//   take out the whole test.
//
// Phase 2 cases are real RHS strings sampled from
//   HyperFLINT/data/mzv_reductions.json
// — these are the strings that mzv_reduce.cpp:153 would parse at
// runtime under narrow ctx if we shipped the sentinel-tolerance
// design without fixing the Poly ctor first.
//
// Output:
//   - Phase 1: per-case rc and a summary verdict (legacy format).
//   - Phase 2: per-case child exit status, expected outcome, and
//     a summary verdict.  PASS = child exited cleanly with the
//     expected outcome (ctor success OR clean throw).  FAIL = child
//     died from a signal (SIGTRAP / SIGABRT / etc.) — which is the
//     bug under audit.
//
// Exit code: 0 if both phases pass, 1 otherwise.

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"

#include <flint/fmpq_mpoly.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace hf = hyperflint;

// =====================================================================
// Phase 1 — FLINT layer
// =====================================================================

struct FlintCase {
    std::vector<std::string> vars;
    std::string expr;
    int expected_rc;  // 0 = expect success, 1 = expect nonzero
    std::string note;
};

static int run_phase1() {
    std::vector<FlintCase> cases = {
        {{"x"},      "x",     0, "single var, valid"},
        {{"x"},      "y",     1, "unknown var (the load-bearing case)"},
        {{"x"},      "x*y",   1, "unknown var in compound expr"},
        {{"x", "y"}, "x*y",   0, "compound, both known"},
        {{"x"},      "1/2",   0, "pure numeric, no var"},
        {{"x"},      "2*x+3", 0, "polynomial in known var"},
    };

    int n_pass = 0, n_fail = 0;
    std::cout << "PHASE 1 (FLINT layer fmpq_mpoly_set_str_pretty)\n";
    std::cout << "case,vars,expr,expected_rc,actual_rc,verdict,note\n";
    for (size_t i = 0; i < cases.size(); ++i) {
        const FlintCase& c = cases[i];
        hf::PolyCtx ctx(c.vars);
        fmpq_mpoly_t p;
        fmpq_mpoly_init(p, ctx.raw());
        int rc = fmpq_mpoly_set_str_pretty(
            p, c.expr.c_str(), ctx.cvars(), ctx.raw());
        int actual_rc01 = (rc == 0) ? 0 : 1;
        bool pass = (actual_rc01 == c.expected_rc);
        if (pass) ++n_pass; else ++n_fail;
        std::cout << i+1 << ",\"";
        for (size_t k = 0; k < c.vars.size(); ++k) {
            if (k) std::cout << " ";
            std::cout << c.vars[k];
        }
        std::cout << "\",\"" << c.expr << "\","
                  << c.expected_rc << "," << rc
                  << "," << (pass ? "PASS" : "FAIL")
                  << ",\"" << c.note << "\"\n";
        fmpq_mpoly_clear(p, ctx.raw());
    }
    std::cout << "\nphase 1 summary: " << n_pass << "/"
              << (n_pass + n_fail) << " pass\n";
    return n_fail == 0 ? 0 : 1;
}

// =====================================================================
// Phase 2 — HF C++ Poly::Poly ctor under fork-per-case
// =====================================================================

// Exit codes from the child:
//   0 — Poly constructed successfully (no exception)
//   1 — Poly ctor threw std::runtime_error (clean throw)
//   2 — Poly ctor threw a different std::exception (unexpected but clean)
//   anything else / signal-death — bug under audit (SIGTRAP / SIGABRT)

enum ChildOutcome {
    OUT_OK_CONSTRUCT = 0,
    OUT_OK_THROW_RUNTIME = 1,
    OUT_OK_THROW_OTHER = 2
};

struct PolyCase {
    std::vector<std::string> vars;
    std::string expr;
    // Expected outcome:
    //   "OK"    — Poly should construct successfully
    //   "THROW" — Poly should throw std::runtime_error (parse error)
    std::string expected;
    std::string note;
};

static const char* outcome_label(int code) {
    switch (code) {
        case OUT_OK_CONSTRUCT:    return "OK_CONSTRUCT";
        case OUT_OK_THROW_RUNTIME: return "OK_THROW_RUNTIME";
        case OUT_OK_THROW_OTHER:   return "OK_THROW_OTHER";
        default:                   return "UNKNOWN";
    }
}

static int run_phase2() {
    // Real RHS strings from data/mzv_reductions.json mixed with the
    // basic unknown-var case the chain-13 smoke test already covered.
    std::vector<PolyCase> cases = {
        // baseline simple unknown-var (chain-13 smoke test scope)
        {{"x"},                              "y",
         "THROW", "single unknown var (chain-13 smoke test scope)"},
        // the smallest compound RHS that reviewer reported as SIGTRAP
        {{"x"},                              "1/2*Log2",
         "THROW", "compound w/ rational coef + unknown var (R23-flagged)"},
        // real RHS from mzv_reductions.json (line 13): mzv_m1_m1
        {{"Log2"},                           "1/2*Log2^2-1/2*mzv_2",
         "THROW", "real RHS, mzv_m1_m1, partial coverage (Log2 only)"},
        // real RHS from mzv_reductions.json (line 31): mzv_2_m1
        {{"Log2", "mzv_2"},                  "1/2*Log2*mzv_2-1/4*mzv_3",
         "THROW", "real RHS, mzv_2_m1, partial coverage (no mzv_3)"},
        // real RHS from mzv_reductions.json (line 39): mzv_m1_m1_m1
        {{"Log2"},                           "-(1/6*Log2^3-1/2*Log2*mzv_2+1/4*mzv_3)",
         "THROW", "real RHS, mzv_m1_m1_m1, only Log2 (no mzv_2, mzv_3)"},
        // sanity: same RHS but with all vars present should succeed
        {{"Log2", "mzv_2", "mzv_3"},         "1/2*Log2*mzv_2-1/4*mzv_3",
         "OK",    "all vars present — sanity check"},
        {{"Log2"},                           "1/6*Log2^3",
         "OK",    "Log2 only — sanity check"},
    };

    int n_pass = 0, n_fail = 0;
    std::cout << "\nPHASE 2 (HF C++ Poly::Poly ctor, fork-per-case)\n";
    std::cout << "case,vars,expr,expected,child_status,actual,verdict,note\n";

    for (size_t i = 0; i < cases.size(); ++i) {
        const PolyCase& c = cases[i];

        // Flush before fork to avoid double-printing the parent header
        std::cout.flush();
        pid_t pid = fork();
        if (pid == 0) {
            // === child ===
            // Suppress child's stdout so the report is parent-only.
            // Use write to /dev/null only if needed; default stdout
            // inherited is fine since we don't print from the child.
            try {
                hf::PolyCtx ctx(c.vars);
                hf::Poly p(ctx, c.expr);
                // ctor returned without throwing
                std::_Exit(OUT_OK_CONSTRUCT);
            } catch (const std::runtime_error&) {
                std::_Exit(OUT_OK_THROW_RUNTIME);
            } catch (const std::exception&) {
                std::_Exit(OUT_OK_THROW_OTHER);
            } catch (...) {
                std::_Exit(99);
            }
        }
        // === parent ===
        int status = 0;
        waitpid(pid, &status, 0);

        std::string vars_str;
        for (size_t k = 0; k < c.vars.size(); ++k) {
            if (k) vars_str += " ";
            vars_str += c.vars[k];
        }

        std::cout << i+1 << ",\"" << vars_str << "\",\""
                  << c.expr << "\",\"" << c.expected << "\",";

        bool pass = false;
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            std::cout << "exit_" << code << ",\""
                      << outcome_label(code) << "\",";
            // Map expected-string to acceptable codes.
            if (c.expected == "OK") {
                pass = (code == OUT_OK_CONSTRUCT);
            } else if (c.expected == "THROW") {
                pass = (code == OUT_OK_THROW_RUNTIME);
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            std::cout << "signal_" << sig << ",\"SIGNAL_DEATH\",";
            pass = false;
        } else {
            std::cout << "unknown_" << status << ",\"UNKNOWN\",";
            pass = false;
        }
        std::cout << (pass ? "PASS" : "FAIL")
                  << ",\"" << c.note << "\"\n";

        if (pass) ++n_pass; else ++n_fail;
    }

    std::cout << "\nphase 2 summary: " << n_pass << "/"
              << (n_pass + n_fail) << " pass\n";
    return n_fail == 0 ? 0 : 1;
}

// =====================================================================
// Phase 3 — Rat::parse_or_none API smoke test (chain 16)
// =====================================================================
//
// New static method introduced in chain 16 as part of the R24 rev 2
// scaffolding.  Wraps Rat::parse in a try/catch(std::runtime_error)
// and returns std::optional<Rat>.  Production paths in chain 17+
// will wire it into parse_rhs_cached and to_mzv_one_word so the
// "missing variable inside OMP region" case becomes recoverable
// instead of std::terminate.
//
// Phase 3 doesn't fork — these cases just confirm the wrapper
// correctly translates throws into nullopt, and successful parses
// yield a Rat with the expected num.is_zero() / is_one() shape.

struct RonCase {
    std::vector<std::string> vars;
    std::string expr;
    bool expect_some;  // true = expect some Rat, false = expect nullopt
    std::string note;
};

static int run_phase3() {
    std::vector<RonCase> cases = {
        {{"x"}, "y",                              false, "unknown var"},
        {{"x"}, "1/2*Log2",                       false, "real RHS, missing Log2"},
        {{"Log2"}, "1/2*Log2^2-1/2*mzv_2",        false, "real RHS, partial coverage"},
        {{"x"}, "x",                              true,  "known var"},
        {{"x"}, "1/2",                            true,  "pure numeric"},
        {{"Log2", "mzv_2"}, "1/2*Log2*mzv_2",     true,  "real RHS, all known"},
    };

    int n_pass = 0, n_fail = 0;
    std::cout << "\nPHASE 3 (Rat::parse_or_none API smoke test)\n";
    std::cout << "case,vars,expr,expect_some,actual,verdict,note\n";

    for (size_t i = 0; i < cases.size(); ++i) {
        const RonCase& c = cases[i];
        std::string vars_str;
        for (size_t k = 0; k < c.vars.size(); ++k) {
            if (k) vars_str += " ";
            vars_str += c.vars[k];
        }
        hf::PolyCtx ctx(c.vars);
        std::optional<hf::Rat> result = hf::Rat::parse_or_none(ctx, c.expr);
        const bool got_some = result.has_value();
        const bool pass = (got_some == c.expect_some);
        if (pass) ++n_pass; else ++n_fail;
        std::cout << i+1 << ",\"" << vars_str << "\",\"" << c.expr << "\","
                  << (c.expect_some ? "some" : "none") << ","
                  << (got_some ? "some" : "none") << ","
                  << (pass ? "PASS" : "FAIL") << ",\""
                  << c.note << "\"\n";
    }
    std::cout << "\nphase 3 summary: " << n_pass << "/"
              << (n_pass + n_fail) << " pass\n";
    return n_fail == 0 ? 0 : 1;
}

int main() {
    int p1 = run_phase1();
    int p2 = run_phase2();
    int p3 = run_phase3();

    std::cout << "\n=====================================================\n";
    if (p1 == 0 && p2 == 0 && p3 == 0) {
        std::cout << "VERDICT: PASS — both FLINT layer and HF C++ wrapper "
                     "tolerate parse failures cleanly. The sentinel-"
                     "tolerance design (notes/hf_rss_campaign/"
                     "r22_step1_sentinel_design.md) prerequisites hold.\n";
        return 0;
    }
    std::cout << "VERDICT: FAIL — ";
    if (p1) std::cout << "FLINT layer behaves unexpectedly. ";
    if (p2) std::cout << "HF C++ Poly ctor signal-deaths or "
                         "produces wrong outcome on at least one case "
                         "(R23 double-clear bug or related). ";
    if (p3) std::cout << "Rat::parse_or_none API misbehaves on at "
                         "least one case (chain-16 scaffolding broken). ";
    std::cout << "Investigate before proceeding with sentinel-"
                 "tolerance Step 1 implementation.\n";
    return 1;
}
