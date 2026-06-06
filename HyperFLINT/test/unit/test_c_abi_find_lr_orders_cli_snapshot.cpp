// Track 8.4 (iter-58): byte-identical CLI vs C ABI snapshot for
// find_lr_orders.
//
// Locks the transport-neutral contract for the iter-58 promotion of
// hf_find_lr_orders from its iter-56 stub (which returned
// {"error":"chunk_3_pending"}) to a real delegating entry point: both
// `hyperflint eval-json` (CLI) and `hf_find_lr_orders` (C ABI) route
// through hyperflint::handlers::find_lr_orders.  Unlike the
// partial_fractions and linear_factors handlers, find_lr_orders already
// emits the {schema_version, hf_version} envelope inline (see
// src/bridge/handlers.cpp:700-702), so c_abi.cpp's splice_envelope
// returns the body unchanged (branch 1: envelope already present).
// Net invariant tested here: ABI bytes == CLI bytes (CLI trailing
// newline stripped, and `timing_compute_s` wall-clock field elided on
// both sides because each invocation measures its own elapsed time).
//
// Failure modes this test catches:
//   - CLI and C ABI drift: someone edits one path without the other.
//   - Envelope drift: hyperflint::handlers::find_lr_orders stops emitting
//     (or starts emitting different) schema_version / hf_version fields,
//     or splice_envelope incorrectly re-wraps an already-stamped body.
//   - Field-order drift: the response field sequence reorders on one
//     side but not the other.
//   - Regression of hf_find_lr_orders back to a stub (would surface as
//     ABI body starting with `{"op":"find_lr_orders","schema_version":1,
//     "hf_version":"...","error":"chunk_3_pending"}` while the CLI
//     emits the real LR-search response).
//
// Mirrors test_c_abi_linear_factors_cli_snapshot.cpp (iter-51 chunk-3b)
// and test_c_abi_pfrac_cli_snapshot.cpp (iter-48 chunk-2b).  The
// strip_timing and run_cli helpers are intentionally duplicated rather
// than factored into a shared header: each snapshot ctest is a
// self-contained smoke binary, consistent with the iter-50/51 lineage's
// "snapshot tests are independent" expectation.
//
// Usage:
//   test_c_abi_find_lr_orders_cli_snapshot <path-to-hyperflint-cli>
// (CMake supplies the path via $<TARGET_FILE:hyperflint-cli>.)
//
// Linked against libhyperflint.a, same as the pfrac and linear_factors
// snapshot ctests.

#include "hyperflint/c_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>  // close, unlink, mkstemp

namespace {

// Strip `,"timing_compute_s":<number>` from a response.  The handler
// records its own wall-clock duration between t0 and t1 around the
// lr_search::find_lr_orders call; CLI and ABI invocations run
// independently, so this single field is the load-bearing source of
// non-determinism between the two transports.  All other fields
// (best_order, score, nolr, strategy, nXVars, nGroups, nPolys, ...)
// are deterministic functions of the input.  The strip is conservative:
// returns input unchanged if the marker is absent (which would itself
// be a separate failure caught at the byte-identity check).
std::string strip_timing(const std::string& s) {
    const std::string marker = ",\"timing_compute_s\":";
    size_t a = s.find(marker);
    if (a == std::string::npos) return s;
    size_t b = a + marker.size();
    // The numeric value is a double serialized by std::ostringstream's
    // default float formatter — digits, optional sign, optional decimal
    // point, optional exponent (e/E + optional sign + digits).  iter-59
    // widening (Q-19 advisory A2): also accept `nan`, `inf`, `nan(...)`
    // payload characters that std::ostringstream emits for non-finite
    // doubles.  In practice timing_compute_s is a wall-clock delta which
    // is always finite and positive, but a clock-source pathology
    // (CLOCK_MONOTONIC backwards on rebased VMs, malformed timespec
    // arithmetic in the handler) would produce non-finite output and
    // surface as a snapshot mismatch with a confusing diff.  Widening
    // the char class eliminates that distraction; if non-finite
    // timing_compute_s does land in the response, that's a separate
    // handler-side bug surfaced cleanly by the byte-identity test rather
    // than masked by a strip that stops at the first non-digit.
    auto is_num_char = [](char c) {
        return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+'
            || c == 'e' || c == 'E'
            // non-finite payload: nan, NaN, inf, Inf, nan(payload)
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
    char req_path[] = "/tmp/hf_cli_lr_snapshot_req_XXXXXX";
    char out_path[] = "/tmp/hf_cli_lr_snapshot_out_XXXXXX";
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

    int n_pass = 0;
    int n_fail = 0;
    auto check = [&](bool cond, const char* tag) {
        if (cond) { std::printf("[PASS] %s\n", tag); ++n_pass; }
        else      { std::printf("[FAIL] %s\n", tag); ++n_fail; }
    };

    // Fixed request: same single-variable single-poly fixture used by
    // the iter-50/51 linear_factors snapshot, adapted to the find_lr_orders
    // schema.  Sub-millisecond on both transports; the response carries
    // a deterministic best_order/score/strategy decision plus the
    // schema_version+hf_version envelope the handler emits inline.
    const std::string req =
        "{\"op\":\"find_lr_orders\","
         "\"xvars\":[\"x\"],"
         "\"polys\":[\"x\"]}";

    // ---- C ABI: invoke hf_find_lr_orders, strip timing field ----
    char* abi_raw = hf_find_lr_orders(req.c_str());
    check(abi_raw != nullptr, "A1 hf_find_lr_orders: non-NULL return");
    std::string abi_full = abi_raw ? std::string(abi_raw) : std::string();
    hf_free_string(abi_raw);

    // The promoted entry point must NOT emit the iter-56 stub error;
    // catch the regression explicitly so a reverted commit fails loud.
    check(abi_full.find("\"chunk_3_pending\"") == std::string::npos,
          "A2 hf_find_lr_orders: not the iter-56 chunk_3_pending stub");

    // Inline envelope: handler emits schema_version and hf_version
    // directly (handlers.cpp:700-702), so splice_envelope must NOT
    // double-wrap.  Verify exactly one occurrence on each side.
    auto count_substr = [](const std::string& hay, const std::string& need) {
        size_t n = 0, pos = 0;
        while ((pos = hay.find(need, pos)) != std::string::npos) {
            ++n;
            pos += need.size();
        }
        return n;
    };
    check(count_substr(abi_full, "\"schema_version\":") == 1,
          "A3 ABI: schema_version appears exactly once (no double-wrap)");
    check(count_substr(abi_full, "\"hf_version\":") == 1,
          "A4 ABI: hf_version appears exactly once (no double-wrap)");

    std::string abi_stripped = strip_timing(abi_full);

    // ---- CLI: spawn hyperflint binary, capture stdout ----
    std::string cli_out = run_cli(cli_path, req);
    check(!cli_out.empty(), "B1 CLI: non-empty stdout");
    check(cli_out.rfind("{\"op\":\"find_lr_orders\"", 0) == 0,
          "B2 CLI: output starts with {\"op\":\"find_lr_orders\"");
    check(cli_out.find("\"schema_version\":") != std::string::npos,
          "B3 CLI: envelope present (handler emits inline)");

    std::string cli_stripped = strip_timing(cli_out);

    // ---- Snapshot: byte-for-byte equality (timing field elided) ----
    bool equal = (abi_stripped == cli_stripped);
    check(equal, "C1 byte-identical CLI vs C-ABI (timing_compute_s stripped)");
    if (!equal) {
        std::cerr << "    CLI stripped: " << cli_stripped << "\n";
        std::cerr << "    ABI stripped: " << abi_stripped << "\n";
    }

    std::printf("[SUMMARY] PASS=%d FAIL=%d\n", n_pass, n_fail);
    return (n_fail == 0) ? 0 : 1;
}
