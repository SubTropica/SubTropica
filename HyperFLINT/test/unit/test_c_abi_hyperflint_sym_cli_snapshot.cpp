// Track 8.4-real-op-1 (iter-61): byte-identical CLI vs C ABI snapshot
// for hyperflint_sym (the integrator op).
//
// Locks the transport-neutral contract for the iter-61 promotion of
// hf_hyperflint_sym from "not yet declared in c_abi.h" to a real
// delegating C ABI entry point: both `hyperflint eval-json` (CLI) and
// hf_hyperflint_sym (C ABI) route through
// hyperflint::handlers::hyperflint_sym.
//
// Why this is a hybrid of the iter-48 pfrac and iter-58 find_lr_orders
// snapshot patterns:
//   - Like partial_fractions / linear_factors (NOT like find_lr_orders),
//     the hyperflint_sym handler emits a CLI-form body WITHOUT the
//     {schema_version, hf_version} envelope on its happy path
//     (src/bridge/handlers.cpp:1164-1181). So the ABI side has the
//     envelope spliced in by c_abi.cpp::splice_envelope (branch 2), and
//     this test strips it to compare against CLI bytes. (This is the
//     pfrac/linear_factors pattern.)
//   - Unlike partial_fractions / linear_factors, the handler also emits
//     a wall-clock-dependent `timing_compute_s` field on its happy
//     path (handlers.cpp:1168). So this test ALSO strips
//     `timing_compute_s` from both sides, reusing the iter-59-widened
//     strip_timing helper from the find_lr_orders snapshot and the
//     iter-60 cross-process determinism probe.
//
// Net invariant tested here:
//     strip_timing(strip_envelope(ABI_bytes))
//   ==
//     strip_timing(CLI_bytes)
//
// where the CLI bytes have had their trailing `\n` removed (the CLI
// shim at bridge/cli/main.cpp::handle_hyperflint appends one). The
// iter-60 cross-process determinism probe
// (test/unit/test_hyperflint_sym_response_determinism.cpp) is the
// feasibility evidence for the right-hand side: it asserts CLI bytes
// are byte-stable across independent processes modulo
// `timing_compute_s` on the same findroots21_b fixture this test
// reuses. The probe PASSED 8/8 at iter-60 commit 231a063e4.
//
// Failure modes this test catches:
//   - CLI and C ABI drift: someone edits one path without the other.
//   - Envelope drift: hf_hyperflint_sym stops emitting (or starts
//     emitting different) schema_version / hf_version fields, or
//     splice_envelope incorrectly double-wraps an already-stamped
//     body.
//   - Field-order drift: the response field sequence reorders on one
//     side but not the other.
//   - Regression of the algebraic_letters table emission order (the
//     load-bearing unknown that iter-60's probe was specifically built
//     to gate).
//   - Regression of hf_hyperflint_sym to a stub that returns an
//     immediate error envelope without invoking the handler.
//
// The strip helpers (strip_envelope and strip_timing) are intentionally
// duplicated from the iter-48 pfrac and iter-58 find_lr_orders snapshot
// tests rather than factored into a shared header: each snapshot ctest
// is a self-contained smoke binary, consistent with the iter-50/51
// lineage's "snapshot tests are independent" expectation.
//
// Usage:
//   test_c_abi_hyperflint_sym_cli_snapshot <path-to-hyperflint-cli>
// (CMake supplies the path via $<TARGET_FILE:hyperflint-cli>. The mzv
// data path comes from a compile-time -D injected by
// target_compile_definitions; see HyperFLINT/CMakeLists.txt.)
//
// Linked against libhyperflint.a (for the C ABI symbols), same as the
// pfrac / linear_factors / find_lr_orders snapshot ctests.

#include "hyperflint/c_abi.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>  // close, unlink, mkstemp

#ifndef HF_TEST_MZV_DATA
#error "HF_TEST_MZV_DATA must be defined (path to mzv_reductions.json)."
#endif

#define HF_STRINGIFY_INNER(x) #x
#define HF_STRINGIFY(x) HF_STRINGIFY_INNER(x)

namespace {

// Strip the schema_version + hf_version envelope fields from a C ABI
// response body.  Conservative implementation: find the
// `,"schema_version":<digits>,"hf_version":"<chars>"` substring and
// remove it.  Returns the input unchanged if the pattern is absent
// (which would be a separate failure caught upstream by the smoke /
// envelope-present assertions).  Mirrors the strip_envelope helper in
// test_c_abi_pfrac_cli_snapshot.cpp (iter-48).
std::string strip_envelope(const std::string& s) {
    const std::string marker = ",\"schema_version\":";
    size_t a = s.find(marker);
    if (a == std::string::npos) return s;
    size_t b = a + marker.size();
    while (b < s.size() && std::isdigit(static_cast<unsigned char>(s[b]))) ++b;
    if (b >= s.size() || s[b] != ',') return s;
    const std::string hf_marker = ",\"hf_version\":\"";
    if (s.compare(b, hf_marker.size(), hf_marker) != 0) return s;
    size_t c = b + hf_marker.size();
    while (c < s.size() && s[c] != '"') {
        if (s[c] == '\\' && c + 1 < s.size()) c += 2;
        else ++c;
    }
    if (c >= s.size()) return s;
    return s.substr(0, a) + s.substr(c + 1);
}

// Strip `,"timing_compute_s":<number>` from a response. Same widened
// char class as iter-59's strip_timing in
// test_c_abi_find_lr_orders_cli_snapshot.cpp and the iter-60 probe:
// the digits/sign/decimal/exponent set is augmented with nan/inf
// payload chars so a clock-source pathology surfaces as a separate
// bug rather than masking a partial strip.
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

// Run the CLI binary on a fixed request and return its stdout.  Uses
// a tempfile pair because POSIX popen() is one-way.
std::string run_cli(const std::string& cli_path,
                    const std::string& request) {
    char req_path[] = "/tmp/hf_cli_hsym_snapshot_req_XXXXXX";
    char out_path[] = "/tmp/hf_cli_hsym_snapshot_out_XXXXXX";
    int rfd = ::mkstemp(req_path);
    int ofd = ::mkstemp(out_path);
    if (rfd < 0 || ofd < 0) {
        std::cerr << "[FAIL] mkstemp failed\n";
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
        std::cerr << "[WARN] CLI returned exit code " << rc << "\n";
    }
    std::ifstream f(out_path);
    std::stringstream ss;
    ss << f.rdbuf();
    ::unlink(req_path);
    ::unlink(out_path);
    std::string out = ss.str();
    // CLI shim appends '\n' to handler output; strip it for comparison.
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <path-to-hyperflint-cli>\n";
        return 2;
    }
    const std::string cli_path = argv[1];
    const std::string mzv_data_path = HF_STRINGIFY(HF_TEST_MZV_DATA);

    int n_pass = 0;
    int n_fail = 0;
    auto check = [&](bool cond, const char* tag) {
        if (cond) { std::printf("[PASS] %s\n", tag); ++n_pass; }
        else      { std::printf("[FAIL] %s\n", tag); ++n_fail; }
    };

    // findroots21_b inline fixture — same one iter-60's cross-process
    // determinism probe used and the same one T4 omp-e2e-determinism
    // gates. Smallest W-non-empty integrator request in the repo
    // (~3 ms compute). `algebraic_letters:true` keeps the introduce_al
    // code path live so the response includes the algebraic_letters
    // table whose container-iteration determinism is the load-bearing
    // unknown that iter-60 cleared.
    const std::string req =
        "{\"op\":\"hyperflint\","
         "\"expr\":\"-1/2*1/(mm + 2*mm*x3 - s12*x3 + mm*x3^2 - 4*mm*x4 "
                  "+ s12*x4 + s23*x4)^2\","
         "\"vars_int\":[\"x4\",\"x3\"],"
         "\"vars\":[\"x4\",\"x3\",\"mm\",\"s12\",\"s23\"],"
         "\"algebraic_letters\":true,"
         "\"mzv_data_path\":\""
       + mzv_data_path
       + "\"}";

    // ---- C ABI: invoke hf_hyperflint_sym ----
    char* abi_raw = hf_hyperflint_sym(req.c_str());
    check(abi_raw != nullptr, "A1 hf_hyperflint_sym: non-NULL return");
    std::string abi_full = abi_raw ? std::string(abi_raw) : std::string();
    hf_free_string(abi_raw);

    // Smoke: the C ABI body must (a) start with the envelope-stamped
    // prefix `{"op":"hyperflint","schema_version":...` (splice_envelope
    // branch-2 insertion) and (b) include the algebraic_letters field
    // (introduce_al active — confirms mzv_data_path resolution worked
    // and the handler took the happy path, not divergent/narrow/failed).
    check(abi_full.rfind("{\"op\":\"hyperflint\"", 0) == 0,
          "A2 ABI: starts with {\"op\":\"hyperflint\"");
    check(abi_full.find("\"schema_version\":") != std::string::npos,
          "A3 ABI: schema_version envelope spliced in (branch 2)");
    check(abi_full.find("\"hf_version\":") != std::string::npos,
          "A4 ABI: hf_version envelope spliced in (branch 2)");
    check(abi_full.find("\"algebraic_letters\":") != std::string::npos,
          "A5 ABI: emits algebraic_letters field (introduce_al active)");
    check(abi_full.find("\"timing_compute_s\":") != std::string::npos,
          "A6 ABI: emits timing_compute_s (handler took happy path)");

    // Envelope must appear EXACTLY ONCE (no double-wrap regression in
    // splice_envelope). Mirrors iter-58 find_lr_orders A3/A4 checks.
    auto count_substr = [](const std::string& hay, const std::string& need) {
        size_t n = 0, pos = 0;
        while ((pos = hay.find(need, pos)) != std::string::npos) {
            ++n;
            pos += need.size();
        }
        return n;
    };
    check(count_substr(abi_full, "\"schema_version\":") == 1,
          "A7 ABI: schema_version appears exactly once (no double-wrap)");
    check(count_substr(abi_full, "\"hf_version\":") == 1,
          "A8 ABI: hf_version appears exactly once (no double-wrap)");

    // Strip envelope (ABI-only — CLI has no envelope to strip) then
    // strip timing (both sides).
    std::string abi_no_env = strip_envelope(abi_full);
    check(abi_no_env != abi_full,
          "A9 envelope strip: schema_version+hf_version removed (output shrank)");
    std::string abi_stripped = strip_timing(abi_no_env);

    // ---- CLI: spawn hyperflint binary, capture stdout ----
    std::string cli_out = run_cli(cli_path, req);
    check(!cli_out.empty(), "B1 CLI: non-empty stdout");
    check(cli_out.rfind("{\"op\":\"hyperflint\"", 0) == 0,
          "B2 CLI: output starts with {\"op\":\"hyperflint\"");
    check(cli_out.find("\"schema_version\":") == std::string::npos,
          "B3 CLI: no envelope (handler emits CLI-form bare on happy path)");
    check(cli_out.find("\"algebraic_letters\":") != std::string::npos,
          "B4 CLI: emits algebraic_letters field (introduce_al active)");
    check(cli_out.find("\"timing_compute_s\":") != std::string::npos,
          "B5 CLI: emits timing_compute_s (handler took happy path)");

    std::string cli_stripped = strip_timing(cli_out);

    // ---- Snapshot: byte-for-byte equality (envelope+timing stripped) ----
    bool equal = (abi_stripped == cli_stripped);
    check(equal,
          "C1 byte-identical CLI vs C-ABI (envelope+timing_compute_s stripped)");
    if (!equal) {
        std::cerr << "    CLI stripped : " << cli_stripped << "\n";
        std::cerr << "    ABI stripped : " << abi_stripped << "\n";
    }

    // Length-only check separated from C1 to make a length-mismatch
    // failure mode (numeric digit count drift, say) clearly visible
    // in the test log even when C1 also fails.
    check(abi_stripped.size() == cli_stripped.size(),
          "C2 stripped response byte-lengths match exactly");

    std::printf("[SUMMARY] PASS=%d FAIL=%d\n", n_pass, n_fail);
    return (n_fail == 0) ? 0 : 1;
}
