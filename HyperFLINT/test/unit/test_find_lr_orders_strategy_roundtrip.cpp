// iter-41 Track 6.5 full PASS: JSON-boundary roundtrip ctest for the
// "strategy" field emitted by hyperflint::handlers::find_lr_orders
// (added iter-40 by src/bridge/handlers.cpp::find_lr_orders).
//
// Independence from hf-step-strategy-dispatch (iter-35 / iter-40 lesson
// dormant_abstraction_letter_vs_spirit defence):
//   - hf-step-strategy-dispatch calls pick_step_strategy(StepInputs)
//     DIRECTLY via the C++ header.  It never crosses the JSON boundary.
//   - This test posts JSON request bodies to handlers::find_lr_orders,
//     reads the response JSON, and asserts on the "strategy":"<name>"
//     field.  It is the ONLY ctest that gates the JSON-serialization
//     of the strategy enum.  A handler-side regression that produces
//     the right StepStrategy internally but mis-encodes the field
//     (typo in switch, missed case, dropped field on optional branch)
//     would slip past hf-step-strategy-dispatch and surface here.
//
// STRUCTURAL_TAUTOLOGY_ROUND_TRIP chain at this layer:
//   step_strategy.hpp truth-table comment (spec)
//     -> handlers.cpp switch + JSON emission (this layer)
//     -> hand-typed expected strings below (this test).
// Three independent transcriptions of the same decision rule, none
// reading the others at runtime.
//
// All 6 truth-table rows are reachable via JSON-level inputs:
//   { polys=["x1","x2","x1+x2"], xvars=["x1","x2"] }     -> LR (db=1 with
//      method_lr_hint default Lungo and Espresso, db=2 with algebraic
//      letters enabled)
//   { polys=["1+x1^2+x2^2"], xvars=["x1","x2"] }         -> NOLR (db=1)
// Polys hand-confirmed by direct CLI invocation iter-41 Step 2.
//
// Track 8.1 (iter-43) extension — versioned eval-json envelope check.
// Five additional rows assert the schema_version + hf_version envelope:
//   row7: success path response contains "schema_version":1
//   row8: success path response contains a non-empty "hf_version"
//   row9: schema_version_min=2 fast-fails with error mentioning "exceeds"
//   row10: schema_version_min=1 still succeeds (lower-bound roundtrip)
//   row11 (iter-43 reviewer Q2 fold): contiguous-prefix substring
//     check on the documented op -> schema_version -> hf_version
//     field ordering — closes the STRUCTURAL_TAUTOLOGY_ROUND_TRIP gap
//     where the audit doc claimed load-bearing ordering but no test
//     ever asserted position (regex predicates match anywhere).
// These rows guard against silent regressions in the iter-43 schema
// envelope and against future bumps of kSchemaVersion that would
// require updating $STHFSchemaVersionExpected in SubTropica.wl in
// lockstep.
//
// Tier-1 wall: ~few-ms each, 11 rows total well under 0.5 s.

#include "hyperflint/bridge/handlers.hpp"

#include <algorithm>  // std::min — iter-43 envelope-prefix diag head trim
#include <iostream>
#include <regex>
#include <string>
#include <vector>

namespace {

struct Row {
    const char* label;
    std::string request;
    std::string expected_strategy;
};

// Extract "strategy":"<name>" from the response.  Hand-rolled regex
// rather than nlohmann/json to avoid pulling a dep into this test —
// the field is a simple top-level string in a deterministic position
// in the emitted response (handlers.cpp:584).
std::string extract_strategy(const std::string& resp) {
    std::regex rx(R"~("strategy"\s*:\s*"([A-Za-z_]+)")~");
    std::smatch m;
    if (std::regex_search(resp, m, rx)) {
        return m[1].str();
    }
    return std::string("<MISSING>");
}

bool run_row(const Row& r) {
    const std::string resp =
        hyperflint::handlers::find_lr_orders(r.request);
    const std::string got = extract_strategy(resp);
    if (got == r.expected_strategy) {
        std::cout << "[PASS] " << r.label
                  << "  strategy=" << got << '\n';
        return true;
    }
    std::cerr << "[FAIL] " << r.label
              << "  expected=" << r.expected_strategy
              << "  got=" << got
              << "\n        response: " << resp << '\n';
    return false;
}

// Track 8.1 (iter-43): envelope-row probes against the
// schema_version + hf_version response fields.  EnvelopeRow.check
// returns true iff the row's expectation is satisfied; the test
// driver wraps it in the same PASS/FAIL bookkeeping as run_row.
struct EnvelopeRow {
    const char* label;
    std::string request;
    // Predicate over the response string.  Returns true on PASS.
    // Each predicate also returns a short human-readable diagnosis
    // via the diag out-param so we can print it on FAIL.
    bool (*predicate)(const std::string& resp, std::string& diag);
};

// Pre-built predicates ---------------------------------------------------

bool resp_has_schema_version_1(const std::string& resp, std::string& diag) {
    std::regex rx(R"~("schema_version"\s*:\s*([0-9]+))~");
    std::smatch m;
    if (!std::regex_search(resp, m, rx)) {
        diag = "schema_version field absent";
        return false;
    }
    const std::string got = m[1].str();
    if (got != "1") {
        diag = "schema_version=" + got + " (expected 1)";
        return false;
    }
    diag = "schema_version=1 present";
    return true;
}

bool resp_has_nonempty_hf_version(const std::string& resp, std::string& diag) {
    std::regex rx(R"~("hf_version"\s*:\s*"([^"]*)")~");
    std::smatch m;
    if (!std::regex_search(resp, m, rx)) {
        diag = "hf_version field absent";
        return false;
    }
    const std::string got = m[1].str();
    if (got.empty()) {
        diag = "hf_version is empty string";
        return false;
    }
    // Track 8.1 (iter-43): "unknown" is the sentinel the
    // HF_VERSION_STRING #ifndef fallback emits when the library was
    // compiled without -DHF_VERSION_STRING.  Production builds set it
    // from CMake's HF_VERSION cache var; "unknown" in a tier-1 ctest
    // means the CMake plumbing regressed and must be fixed before
    // shipping.
    if (got == "unknown") {
        diag = "hf_version='unknown' — CMake HF_VERSION plumbing regressed";
        return false;
    }
    diag = "hf_version='" + got + "'";
    return true;
}

bool resp_is_schema_too_low_error(const std::string& resp, std::string& diag) {
    std::regex rx_err(R"~("error"\s*:\s*"([^"]*)")~");
    std::smatch m;
    if (!std::regex_search(resp, m, rx_err)) {
        diag = "error field absent (expected fast-fail)";
        return false;
    }
    const std::string err = m[1].str();
    if (err.find("exceeds supported") == std::string::npos) {
        diag = "error='" + err + "' (expected 'exceeds supported')";
        return false;
    }
    diag = "error='" + err + "'";
    return true;
}

// Track 8.1 (iter-43, in-iter fold per reviewer Q2): contiguous-prefix
// substring check makes the documented "op -> schema_version ->
// hf_version" field order actually testable.  The pure regex
// predicates above accept ANY position; without this row the
// load-bearing-ordering language in handlers.cpp is convention-only
// (a STRUCTURAL_TAUTOLOGY_ROUND_TRIP gap by iter-39/40 axis-3).
// Asserting the *prefix* of the response (not a full equality) keeps
// the predicate robust to additive field changes after hf_version.
bool resp_has_envelope_prefix(const std::string& resp, std::string& diag) {
    const std::string expected_prefix =
        "{\"op\":\"find_lr_orders\","
        "\"schema_version\":1,"
        "\"hf_version\":\"";
    if (resp.compare(0, expected_prefix.size(), expected_prefix) != 0) {
        // Show the first ~120 chars for diagnosis; the response
        // could be arbitrarily long.
        std::string head = resp.substr(0, std::min<size_t>(120, resp.size()));
        diag = "response does not start with documented envelope prefix; "
               "head='" + head + "'";
        return false;
    }
    diag = "envelope prefix correct (op -> schema_version -> hf_version)";
    return true;
}

bool resp_has_strategy_field(const std::string& resp, std::string& diag) {
    std::string s = extract_strategy(resp);
    if (s == "<MISSING>") {
        diag = "strategy field absent — schema_version_min=1 should "
               "still serve a normal response";
        return false;
    }
    diag = "strategy='" + s + "'";
    return true;
}

bool run_envelope(const EnvelopeRow& r) {
    const std::string resp =
        hyperflint::handlers::find_lr_orders(r.request);
    std::string diag;
    const bool ok = r.predicate(resp, diag);
    if (ok) {
        std::cout << "[PASS] " << r.label
                  << "  " << diag << '\n';
        return true;
    }
    std::cerr << "[FAIL] " << r.label
              << "  " << diag
              << "\n        response: " << resp << '\n';
    return false;
}

}  // namespace

int main() {
    const std::string lr_polys =
        R"~("xvars":["x1","x2"],"polys":["x1","x2","x1+x2"])~";
    const std::string nolr_polys =
        R"~("xvars":["x1","x2"],"polys":["1+x1^2+x2^2"])~";

    const std::vector<Row> rows = {
        // Row 1: lr_found=true, db=1, hint default Lungo -> LR_NoOpt
        {"row1_lr_db1_lungo",
         "{\"op\":\"find_lr_orders\"," + lr_polys + "}",
         "LR_NoOpt"},

        // Row 2: lr_found=true, db=1, hint=Espresso -> LR_NoOpt
        // (db=1 wins: LR with no algebraic letters is the cheapest plan.)
        {"row2_lr_db1_espresso",
         "{\"op\":\"find_lr_orders\"," + lr_polys +
             ",\"method_lr_hint\":\"Espresso\"}",
         "LR_NoOpt"},

        // Row 3: lr_found=true, db=2 (algebraic_letters), hint default
        // Lungo -> LR_OptOrdered
        {"row3_lr_db2_lungo",
         "{\"op\":\"find_lr_orders\"," + lr_polys +
             ",\"algebraic_letters\":true}",
         "LR_OptOrdered"},

        // Row 4: lr_found=true, db=2, hint=Espresso -> LR_OptOrdered
        {"row4_lr_db2_espresso",
         "{\"op\":\"find_lr_orders\"," + lr_polys +
             ",\"algebraic_letters\":true,\"method_lr_hint\":\"Espresso\"}",
         "LR_OptOrdered"},

        // Row 5: lr_found=false, db=1, hint default Lungo -> Fubini_Lungo
        {"row5_nolr_lungo",
         "{\"op\":\"find_lr_orders\"," + nolr_polys + "}",
         "Fubini_Lungo"},

        // Row 6: lr_found=false, db=1, hint=Espresso -> Fubini_Espresso
        {"row6_nolr_espresso",
         "{\"op\":\"find_lr_orders\"," + nolr_polys +
             ",\"method_lr_hint\":\"Espresso\"}",
         "Fubini_Espresso"},
    };

    int n_pass = 0;
    int n_fail = 0;
    for (const auto& r : rows) {
        if (run_row(r)) ++n_pass; else ++n_fail;
    }

    // Track 8.1 (iter-43): versioned eval-json envelope rows.
    // Reuse the LR_NoOpt-fast request body for the success-path
    // envelope checks (lr_polys + db=1 + hint default Lungo).  The
    // schema_version_min rows use a minimal valid body so the gate
    // fires before any algebraic work — keeps each row at sub-ms wall.
    const std::vector<EnvelopeRow> envelope_rows = {
        // Row 7: success path must carry "schema_version":1.
        {"row7_envelope_schema_version_1",
         "{\"op\":\"find_lr_orders\"," + lr_polys + "}",
         resp_has_schema_version_1},

        // Row 8: success path must carry a non-empty, non-"unknown"
        // "hf_version" string.  Negative-control for the
        // #ifndef-fallback sentinel.
        {"row8_envelope_hf_version_nonempty",
         "{\"op\":\"find_lr_orders\"," + lr_polys + "}",
         resp_has_nonempty_hf_version},

        // Row 9: schema_version_min=2 must fast-fail with an error
        // whose message contains "exceeds supported".  Asserts the
        // C++-side request gate at handlers.cpp::find_lr_orders.
        {"row9_envelope_schema_version_min_2_rejects",
         "{\"op\":\"find_lr_orders\",\"schema_version_min\":2," +
             lr_polys + "}",
         resp_is_schema_too_low_error},

        // Row 10: schema_version_min=1 must roundtrip a normal
        // success response (with a strategy field).  Asserts the gate
        // is a >, not a >=, so callers asserting "I need schema 1+"
        // do not regress when the binary already serves schema 1.
        {"row10_envelope_schema_version_min_1_passes",
         "{\"op\":\"find_lr_orders\",\"schema_version_min\":1," +
             lr_polys + "}",
         resp_has_strategy_field},

        // Row 11 (iter-43 reviewer Q2 fold): contiguous-prefix check
        // makes the documented "op -> schema_version -> hf_version"
        // field order actually testable.  Closes the
        // STRUCTURAL_TAUTOLOGY_ROUND_TRIP gap flagged by reviewer
        // a735322b58cc29e6c (the audit doc claimed "load-bearing
        // ordering" but no ctest predicate ever asserted position).
        {"row11_envelope_contiguous_prefix",
         "{\"op\":\"find_lr_orders\"," + lr_polys + "}",
         resp_has_envelope_prefix},
    };

    for (const auto& r : envelope_rows) {
        if (run_envelope(r)) ++n_pass; else ++n_fail;
    }

    const size_t total = rows.size() + envelope_rows.size();
    std::cout << "Summary: " << n_pass << " PASS / "
              << n_fail << " FAIL out of " << total << '\n';
    return (n_fail == 0) ? 0 : 1;
}
