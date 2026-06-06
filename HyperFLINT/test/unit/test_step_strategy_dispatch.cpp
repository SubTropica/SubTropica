// Track 6.3 iter-35 ctest falsifier: hf-step-strategy-dispatch.
//
// Reads HyperFLINT/test/data/step_strategy_truth_table.json (path
// supplied at compile time via HF_TEST_DATA_DIR macro, defined by
// CMakeLists.txt entry below).  Parses the 6 rows via a minimal
// hand-rolled regex (no nlohmann/json dep — the JSON is hand-trivial
// and the parser stays in this TU).  For each row constructs a
// StepInputs, calls pick_step_strategy, and asserts the returned
// StepStrategy enum matches the row's "expected" string.
//
// Anti-tautology guarantee (STRUCTURAL_TAUTOLOGY_ROUND_TRIP defence,
// lessons_learned iter-29): the implementation in
// src/integrator/step_strategy.cpp does NOT read the JSON file.  This
// test is the only consumer of the JSON.  Independent ground truth
// chain: step_strategy.hpp truth-table comment (lines 150-155 spec)
// -> hand-typed JSON file (committed first per iter-35 step order)
// -> hand-typed .cpp implementation (committed second) -> this test
// (committed third).  Three independent transcriptions all gate the
// strategy decision rule.
//
// Pattern precedent: iter-27 test/unit/test_phase_timer.cpp (~165 LOC,
// 8 assertions, no googletest dep, plain main() returning 0 or 1).

#include "hyperflint/integrator/step_strategy.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef HF_TEST_DATA_DIR
#error "HF_TEST_DATA_DIR must be defined at compile time (set by CMakeLists.txt)"
#endif

namespace {

using hyperflint::integrator::StepInputs;
using hyperflint::integrator::StepStrategy;
using hyperflint::integrator::pick_step_strategy;

const char* strategy_name(StepStrategy s) {
    switch (s) {
        case StepStrategy::LR_OptOrdered:    return "LR_OptOrdered";
        case StepStrategy::LR_NoOpt:         return "LR_NoOpt";
        case StepStrategy::Fubini_Lungo:     return "Fubini_Lungo";
        case StepStrategy::Fubini_Espresso:  return "Fubini_Espresso";
    }
    return "UNKNOWN";
}

struct Row {
    bool lr_found;
    int degree_budget;
    std::string method_lr_hint;
    std::string expected;
    int source_line;  // 1-based row index in the JSON for failure diagnostics
};

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "FATAL: cannot open " << path << '\n';
        std::exit(2);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Minimal hand-rolled regex parse of the 6 JSON object lines.  The
// JSON file is hand-written and regular: one object per line, fields
// in a fixed order.  Each match captures (lr_found, degree_budget,
// method_lr_hint, expected).  Whitespace-tolerant.
std::vector<Row> parse_rows(const std::string& json_text) {
    std::vector<Row> out;
    // {"lr_found": <true|false>, "degree_budget": <1|2>,
    //  "method_lr_hint": "<word>", "expected": "<word>"}
    std::regex rx(
        R"~(\{\s*"lr_found":\s*(true|false)\s*,)~"
        R"~(\s*"degree_budget":\s*(\d+)\s*,)~"
        R"~(\s*"method_lr_hint":\s*"([A-Za-z_]+)"\s*,)~"
        R"~(\s*"expected":\s*"([A-Za-z_]+)"\s*\})~"
    );
    auto begin = std::sregex_iterator(json_text.begin(), json_text.end(), rx);
    auto end   = std::sregex_iterator();
    int idx = 0;
    for (auto it = begin; it != end; ++it) {
        Row r;
        r.lr_found       = ((*it)[1].str() == "true");
        r.degree_budget  = std::atoi((*it)[2].str().c_str());
        r.method_lr_hint = (*it)[3].str();
        r.expected       = (*it)[4].str();
        r.source_line    = ++idx;
        out.push_back(r);
    }
    return out;
}

bool run_row(const Row& r) {
    StepInputs in;
    in.degree_budget = r.degree_budget;
    in.n_factors    = 0;  // iter-34 decision rule does not gate on this
    in.n_letters    = 0;  // ditto
    in.lr_found     = r.lr_found;
    in.method_lr_hint =
        (r.method_lr_hint == "Espresso")
            ? StepInputs::MethodLR::Espresso
            : StepInputs::MethodLR::Lungo;

    StepStrategy got = pick_step_strategy(in);
    const char* got_name = strategy_name(got);

    if (r.expected == got_name) {
        std::cout << "[PASS] row " << r.source_line
                  << " lr_found=" << (r.lr_found ? "true" : "false")
                  << " db=" << r.degree_budget
                  << " hint=" << r.method_lr_hint
                  << " -> " << got_name << '\n';
        return true;
    }
    std::cerr << "[FAIL] row " << r.source_line
              << " lr_found=" << (r.lr_found ? "true" : "false")
              << " db=" << r.degree_budget
              << " hint=" << r.method_lr_hint
              << "  expected=" << r.expected
              << "  got=" << got_name << '\n';
    return false;
}

}  // namespace

int main() {
    const std::string data_dir = HF_TEST_DATA_DIR;
    const std::string json_path = data_dir + "/step_strategy_truth_table.json";
    const std::string json_text = slurp(json_path);
    const std::vector<Row> rows = parse_rows(json_text);

    if (rows.size() != 6) {
        std::cerr << "[FAIL] expected 6 rows in " << json_path
                  << ", parsed " << rows.size() << '\n';
        return 1;
    }

    int n_pass = 0;
    int n_fail = 0;
    for (const auto& r : rows) {
        if (run_row(r)) ++n_pass; else ++n_fail;
    }

    std::cout << "Summary: " << n_pass << " PASS / "
              << n_fail << " FAIL out of " << rows.size() << '\n';
    return (n_fail == 0) ? 0 : 1;
}
