// Track 8.4-probe (iter-60): CLI-level cross-process determinism probe
// for the hyperflint_sym handler.
//
// FEASIBILITY GATE for iter-61's planned promotion of hf_hyperflint_sym
// from "not yet declared in c_abi.h" to a real C ABI op delegating to
// hyperflint::handlers::hyperflint_sym (mirror of iter-58's
// hf_find_lr_orders promotion via the c_abi.cpp + splice_envelope
// pattern landed in iter-56 §T8.2).
//
// Why a probe before iter-61 commits to the full ABI op:
//   The hyperflint handler at src/bridge/handlers.cpp:929-1232 has a
//   substantially broader surface than the partial_fractions /
//   linear_factors / find_lr_orders handlers it would join:
//     - regex-based JSON parsing for `algebraic_letters`,
//       `check_divergences`, `canonical_emission`;
//     - env-flag-driven code paths (HF_MAX_THREADS_PER_CALL,
//       HF_PROBE_DUMP_DIR, HF_NARROW_CTX);
//     - pointer-keyed cache singletons cleared at handler entry
//       (AlgebraicLetterTable::global(), rhs_cache, linear_factors
//       cache, narrow_ctx flag) — handlers.cpp:1003,1016-1018;
//     - full integration via hyperflint::hyperflint_sym() returning
//       a RegulatorSym that is then emitted with timing_compute_s,
//       vars, and (when introduce_al) an algebraic_letters table.
//
//   Before declaring `char* hf_hyperflint_sym(const char*)` in c_abi.h
//   and locking a CLI-vs-ABI byte-identity snapshot ctest (the pattern
//   landed by iter-58 for find_lr_orders), we want positive evidence
//   that the CLI ITSELF emits byte-identical output across independent
//   processes for the same input, modulo the known wall-clock field.
//
// Why this is not redundant with the existing T4 omp-e2e-determinism
// test (test/integration/test_omp_e2e_determinism.py):
//   T4 (iter-57+B3 followup) gates the `result` field's sha256 across
//   OMP={1,2,13} thread counts and 6 independent subprocesses on the
//   findroots21_{a,b} fixtures. That covers thread-count invariance of
//   the integration kernel's RegulatorSym output. It does NOT cover the
//   WHOLE response shape: the per-call `timing_compute_s`, the `vars`
//   emission, and (load-bearing for iter-61 if introduce_al=true) the
//   `algebraic_letters` table emission, which iterates a singleton
//   container whose ordering invariants this probe is here to check.
//
//   A CLI-vs-ABI snapshot ctest (iter-61's deliverable) compares
//   stdout bytes, not sha256 of an extracted subfield. If the
//   non-result portion of the response is process-nondeterministic,
//   iter-61's snapshot would fail erratically. iter-60 catches that
//   here, before any header / ABI / golden-file commit.
//
// Method:
//   1. Use the same findroots21_b fixture T4 uses. Smallest known
//      W-non-empty integrator request in the repo (~3 ms compute);
//      exercises the introduce_al=true branch which emits the
//      algebraic_letters table.
//   2. Build the request envelope as a fixed std::string literal at
//      compile time (fixture content is small enough to inline; no
//      JSON-parser dependency in the test binary).
//   3. Invoke `hyperflint eval-json` twice via std::system() with a
//      tempfile-pair pipe. Each invocation is an independent OS
//      process: fresh heap, fresh thread pools, fresh static
//      initializers, fresh ASLR layout. This is exactly the regime
//      iter-61's snapshot ctest will hit when CMake runs both the
//      CLI test step and the ABI test step in sequence.
//   4. Strip the known wall-clock field `timing_compute_s` from both
//      stdouts. The strip helper uses the iter-59-widened char class
//      (accepts nan/inf payload chars so a clock-source pathology
//      surfaces as a SEPARATE bug rather than masking a partial strip).
//   5. Assert byte-identity of the two stripped responses. On
//      mismatch, dump both stripped responses + a localized
//      first-divergence sketch to stderr for iter-61 inspection.
//
// Probe outcome → iter-61 decision:
//   PASS  → iter-61 LANDs hf_hyperflint_sym ABI op + CLI-vs-ABI
//           snapshot ctest reusing the same strip helper.
//   FAIL  → iter-61 inspects the divergence dump and either:
//             (a) widens the strip helper to cover the additional
//                 non-deterministic field (if it's localized);
//             (b) declares the algebraic_letters emission order
//                 non-stable and pivots to a result-only sha256
//                 snapshot (a la T4) instead of byte-identity;
//             (c) pivots to §T7 (env-flag registry refactor) and
//                 defers §T8.4-real-op-1.
//
// Wall budget: ~80 ms (2 cells × ~35 ms cli compute + tempfile I/O +
// process-fork overhead). Tier-1 ctest by construction.
//
// Usage:
//   test_hyperflint_sym_response_determinism <path-to-hyperflint-cli>
// (CMake supplies the path via $<TARGET_FILE:hyperflint-cli>. The mzv
// data path comes from a compile-time -D injected by
// target_compile_definitions; see HyperFLINT/CMakeLists.txt.)
//
// Linked against nothing: this is a pure-stdlib smoke binary, no
// libhyperflint.a / libmimalloc weak externs needed.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>  // close, unlink

#ifndef HF_TEST_MZV_DATA
#error "HF_TEST_MZV_DATA must be defined (path to mzv_reductions.json)."
#endif

#define HF_STRINGIFY_INNER(x) #x
#define HF_STRINGIFY(x) HF_STRINGIFY_INNER(x)

namespace {

// Strip `,"timing_compute_s":<number>` from a response. Same widened
// char class as iter-59's strip_timing
// (test_c_abi_find_lr_orders_cli_snapshot.cpp): the digits/sign/
// decimal/exponent set is augmented with nan/inf payload chars so a
// clock-source pathology (CLOCK_MONOTONIC backwards under rebased VMs,
// malformed timespec arithmetic in the handler) surfaces as a
// separate bug — confusing a partial strip would otherwise mask it.
std::string strip_timing(const std::string& s) {
    const std::string marker = ",\"timing_compute_s\":";
    size_t a = s.find(marker);
    if (a == std::string::npos) return s;
    size_t b = a + marker.size();
    auto is_num_char = [](char c) {
        return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+'
            || c == 'e' || c == 'E'
            || c == 'n' || c == 'N' || c == 'a' || c == 'A'
            || c == 'i' || c == 'I' || c == 'f' || c == 'F'
            || c == '(' || c == ')' || c == '_';
    };
    while (b < s.size() && is_num_char(s[b])) ++b;
    return s.substr(0, a) + s.substr(b);
}

// Spawn the CLI as an independent child process. Each call goes
// through std::system => fresh OS process, fresh heap, fresh
// static initializers. The `tag` discriminates tempfile names so two
// invocations in this test never collide.
std::string run_cli(const std::string& cli_path,
                    const std::string& request,
                    int tag) {
    char req_path[96];
    char out_path[96];
    std::snprintf(req_path, sizeof(req_path),
                  "/tmp/hf_sym_det_req_%d_XXXXXX", tag);
    std::snprintf(out_path, sizeof(out_path),
                  "/tmp/hf_sym_det_out_%d_XXXXXX", tag);
    int rfd = ::mkstemp(req_path);
    int ofd = ::mkstemp(out_path);
    if (rfd < 0 || ofd < 0) {
        std::cerr << "[FAIL] mkstemp failed (tag=" << tag << ")\n";
        return "";
    }
    ::close(rfd);
    ::close(ofd);
    {
        std::ofstream f(req_path);
        f << request;
    }
    std::string cmd = cli_path + " eval-json < " + req_path
                    + " > " + out_path + " 2>/dev/null";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "[WARN] CLI exit code " << rc
                  << " (tag=" << tag << ")\n";
    }
    std::ifstream f(out_path);
    std::stringstream ss;
    ss << f.rdbuf();
    ::unlink(req_path);
    ::unlink(out_path);
    std::string out = ss.str();
    // CLI shim appends '\n' to handler output; strip for comparison.
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// Print a byte-level diff sketch focusing on the first divergence
// point. Lets a FAIL report show iter-61 exactly which field varied
// across processes (algebraic_letters table reorder?  some leaked
// PID/timestamp?  un-stripped float in result?).
void dump_first_divergence(const std::string& a, const std::string& b) {
    size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    for (; i < n; ++i) if (a[i] != b[i]) break;
    if (i == n && a.size() == b.size()) {
        std::cerr << "    (no divergence found, strings somehow unequal)\n";
        return;
    }
    size_t lo = (i > 64) ? (i - 64) : 0;
    size_t hi_a = std::min(a.size(), i + 96);
    size_t hi_b = std::min(b.size(), i + 96);
    std::cerr << "    first divergence at byte " << i
              << " (a.size=" << a.size()
              << ", b.size=" << b.size() << ")\n";
    std::cerr << "      run-1: ..." << a.substr(lo, hi_a - lo) << "...\n";
    std::cerr << "      run-2: ..." << b.substr(lo, hi_b - lo) << "...\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0]
                  << " <path-to-hyperflint-cli>\n";
        return 2;
    }
    const std::string cli_path = argv[1];
    const std::string mzv_data_path = HF_STRINGIFY(HF_TEST_MZV_DATA);

    int n_pass = 0, n_fail = 0;
    auto check = [&](bool cond, const char* tag) {
        if (cond) { std::printf("[PASS] %s\n", tag); ++n_pass; }
        else      { std::printf("[FAIL] %s\n", tag); ++n_fail; }
    };

    // findroots21_b fixture inlined: smallest W-non-empty integrator
    // request in the repo. Same fixture T4 omp-e2e-determinism uses;
    // T4 already shows the `result` field is sha256-stable across
    // OMP={1,2,13} thread counts and 6 fresh processes. Iter-60 here
    // extends that to FULL-stdout byte-identity (modulo
    // timing_compute_s) which is the invariant iter-61's snapshot
    // ctest will lock.
    //
    // Notes on field choice:
    //   - `algebraic_letters: true` keeps the introduce_al code path
    //     active; that's the larger surface and the one whose
    //     map-traversal-order determinism we need to gate before
    //     iter-61 commits to byte-identity.
    //   - mzv_data_path is injected at compile time via -D from
    //     CMake so the test is hermetic w.r.t. CWD.
    std::string request =
        "{\"op\":\"hyperflint\","
         "\"expr\":\"-1/2*1/(mm + 2*mm*x3 - s12*x3 + mm*x3^2 - 4*mm*x4 "
                  "+ s12*x4 + s23*x4)^2\","
         "\"vars_int\":[\"x4\",\"x3\"],"
         "\"vars\":[\"x4\",\"x3\",\"mm\",\"s12\",\"s23\"],"
         "\"algebraic_letters\":true,"
         "\"mzv_data_path\":\""
       + mzv_data_path
       + "\"}";

    // Cell-1: first independent CLI invocation.
    std::string out_a = run_cli(cli_path, request, 1);
    check(!out_a.empty(), "A1 run-1 non-empty stdout");
    check(out_a.rfind("{\"op\":\"hyperflint\"", 0) == 0,
          "A2 run-1 starts with {\"op\":\"hyperflint\"");
    // Sanity: the introduce_al=true path emits the algebraic_letters
    // field unconditionally (handlers.cpp:1179 "Always emit (even if
    // empty) so the Mma-side parser has a stable response shape"). If
    // this check fails, the fixture has been miswired (e.g.,
    // mzv_data_path resolution silently fell back to "data/..."
    // relative to CWD and missed). Catch it loudly before the
    // determinism step.
    check(out_a.find("\"algebraic_letters\":") != std::string::npos,
          "A3 run-1 emits algebraic_letters field (introduce_al active)");
    check(out_a.find("\"timing_compute_s\":") != std::string::npos,
          "A4 run-1 emits timing_compute_s (handler took happy path)");

    // Cell-2: second independent CLI invocation. Same request, same
    // CLI binary, different OS process (different PID, different
    // ASLR slide, different std::random_device state, different OMP
    // team-creation outcome if OMP_NUM_THREADS were nondefault, fresh
    // FLINT thread pool).
    std::string out_b = run_cli(cli_path, request, 2);
    check(!out_b.empty(), "B1 run-2 non-empty stdout");
    check(out_b.rfind("{\"op\":\"hyperflint\"", 0) == 0,
          "B2 run-2 starts with {\"op\":\"hyperflint\"");

    // ---- THE GATE ----
    // Strip the single known wall-clock field and assert byte-identity.
    std::string sa = strip_timing(out_a);
    std::string sb = strip_timing(out_b);

    bool equal = (sa == sb);
    check(equal,
          "C1 byte-identical CLI vs CLI (timing_compute_s stripped)"
          " — iter-61 feasibility gate for hf_hyperflint_sym snapshot");
    if (!equal) {
        std::cerr << "  --- C1 FAIL: cross-process non-determinism in "
                  << "hyperflint_sym CLI response ---\n";
        std::cerr << "  iter-61 must EITHER widen the strip helper "
                  << "(if divergence is in a localized field) OR pivot "
                  << "from byte-identity to result-only sha256 (if the "
                  << "algebraic_letters table emission is not order-stable).\n";
        dump_first_divergence(sa, sb);
        std::cerr << "  FULL stripped run-1 (" << sa.size() << " B):\n    "
                  << sa << "\n";
        std::cerr << "  FULL stripped run-2 (" << sb.size() << " B):\n    "
                  << sb << "\n";
    }

    // Lengths should also match exactly (a divergence in a numeric
    // field's width — say, integer reordering producing different
    // serialized digit counts — would surface here). Distinct from
    // C1 because string equality already implies length equality;
    // breaking it out as its own assertion makes a length-only
    // failure mode visible in the test log.
    check(sa.size() == sb.size(),
          "C2 stripped response byte-lengths match exactly");

    std::printf("[SUMMARY] PASS=%d FAIL=%d\n", n_pass, n_fail);
    if (n_fail == 0) {
        std::printf("[ITER-60-PROBE] hyperflint_sym CLI response is "
                    "process-deterministic modulo timing_compute_s. "
                    "iter-61 may LAND hf_hyperflint_sym + CLI-vs-ABI "
                    "byte-identity snapshot ctest.\n");
    } else {
        std::printf("[ITER-60-PROBE] hyperflint_sym CLI response is NOT "
                    "process-deterministic with the current strip "
                    "policy. iter-61 must widen the strip OR drop "
                    "byte-identity for result-only sha256.\n");
    }
    return (n_fail == 0) ? 0 : 1;
}
