// Track 8.1b chunk-2b (iter-48): byte-identical CLI vs C ABI snapshot.
//
// Locks the transport-neutral contract introduced by iter-48 chunk-2b:
// both `hyperflint eval-json` (CLI) and `hf_partial_fractions` (C ABI)
// route through `hyperflint::handlers::partial_fractions`, so their
// outputs differ exactly in the envelope (the schema_version +
// hf_version fields the C ABI prepends).  This test runs the same
// `partial_fractions` request through both transports and asserts
// they are byte-identical *modulo* the envelope strip.
//
// Failure modes this test catches:
//   - CLI and handler drift: someone edits one path without the other.
//   - Envelope drift: hf_partial_fractions stops emitting (or starts
//     emitting different) schema_version / hf_version fields.
//   - Field-order drift: the payload field sequence reorders on one
//     side but not the other (caller might silently break consumers).
//
// Usage:
//   test_c_abi_pfrac_cli_snapshot <path-to-hyperflint-cli>
// (CMake supplies the path via $<TARGET_FILE:hyperflint-cli> in the
// add_test() command.)
//
// Linked against libhyperflint.a, same as test_c_abi_pfrac.cpp.

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

// Strip the schema_version + hf_version envelope fields from a C ABI
// response.  Conservative implementation: find the
// `,"schema_version":<digits>,"hf_version":"<chars>"` substring and
// remove it.  Returns the input unchanged if the pattern is absent
// (which would be a separate test failure caught upstream by the
// regular `test_c_abi_pfrac` smoke).
std::string strip_envelope(const std::string& s) {
    const std::string marker = ",\"schema_version\":";
    size_t a = s.find(marker);
    if (a == std::string::npos) return s;
    // Skip past `,"schema_version":<digits>,`
    size_t b = a + marker.size();
    while (b < s.size() && std::isdigit(static_cast<unsigned char>(s[b]))) ++b;
    if (b >= s.size() || s[b] != ',') return s;
    // Now skip `,"hf_version":"<chars>"`
    const std::string hf_marker = ",\"hf_version\":\"";
    if (s.compare(b, hf_marker.size(), hf_marker) != 0) return s;
    size_t c = b + hf_marker.size();
    while (c < s.size() && s[c] != '"') {
        // Allow simple escapes; the only ones HF emits are " \ n r t.
        if (s[c] == '\\' && c + 1 < s.size()) c += 2;
        else ++c;
    }
    if (c >= s.size()) return s;
    // Now c points at the closing '"' of the hf_version value.
    return s.substr(0, a) + s.substr(c + 1);
}

// Run the CLI binary on a fixed request and return its stdout.  Uses
// a tempfile to feed the request because POSIX popen() is one-way.
std::string run_cli(const std::string& cli_path,
                     const std::string& request) {
    // Write request to a fresh tempfile under /tmp.
    char req_path[]  = "/tmp/hf_cli_snapshot_req_XXXXXX";
    char out_path[]  = "/tmp/hf_cli_snapshot_out_XXXXXX";
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
    // Strip the trailing newline that the CLI shim adds.
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

    // Fixed request shared by both transports.  Trivial 1/(x-1) so the
    // computation is sub-millisecond on both paths.
    const std::string req =
        "{\"op\":\"partial_fractions\","
         "\"f\":\"1/(x-1)\","
         "\"var\":\"x\","
         "\"vars\":[\"x\"]}";

    // ---- C ABI: invoke hf_partial_fractions, strip envelope ----
    char* abi_raw = hf_partial_fractions(req.c_str());
    check(abi_raw != nullptr, "A1 hf_partial_fractions: non-NULL return");
    std::string abi_full   = abi_raw ? std::string(abi_raw) : std::string();
    std::string abi_stripped = strip_envelope(abi_full);
    hf_free_string(abi_raw);
    check(abi_stripped != abi_full,
          "A2 envelope strip: schema_version+hf_version removed (output shrank)");

    // ---- CLI: spawn hyperflint binary, capture stdout ----
    std::string cli_out = run_cli(cli_path, req);
    check(!cli_out.empty(), "B1 CLI: non-empty stdout");
    check(cli_out.rfind("{\"op\":\"partial_fractions\"", 0) == 0,
          "B2 CLI: output starts with {\"op\":\"partial_fractions\"");
    check(cli_out.find("\"schema_version\":") == std::string::npos,
          "B3 CLI: no envelope (legacy CLI form intact)");

    // ---- Snapshot: byte-for-byte equality ----
    bool equal = (abi_stripped == cli_out);
    check(equal, "C1 byte-identical CLI vs C-ABI (envelope stripped)");
    if (!equal) {
        std::cerr << "    CLI bytes :       " << cli_out << "\n";
        std::cerr << "    ABI stripped :    " << abi_stripped << "\n";
    }

    std::printf("[SUMMARY] PASS=%d FAIL=%d\n", n_pass, n_fail);
    return (n_fail == 0) ? 0 : 1;
}
