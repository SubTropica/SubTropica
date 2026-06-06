// Track 7.2 iter-39 close falsifier: process-wide cache-registry coverage gate.
// iter-42 Track 7.2 fold (iter-39 Q2): Pattern G added (std::map family,
// namespace-scope analog of Pattern D). Currently zero matches in src/
// (no registry-class std::map cache exists), but Pattern G is in place so
// a future namespace-scope std::map cache cannot vacuous-pass the gate.
// A parser-level self-test (self_test_patterns below) exercises all 7
// patterns against synthetic inputs to confirm each regex CAN capture
// its documented shape, defending against pattern-G regex typos that
// would otherwise be unobservable until a real std::map cache is added.
//
// Asserts S_md == S_src where
//   S_md  = cache identifiers declared in HF_CACHE_REGISTRY_MD_PATH §2
//           (parser-A: markdown row regex `^\|\s*`(\w+)`\s*\|`).
//   S_src = cache identifiers captured by the 7-pattern source scan
//           (parser-B: patterns A..G per cache_registry.md §1.c / §4.b) over
//           HF_SRC_ROOT/src/, HF_SRC_ROOT/bridge/, HF_SRC_ROOT/include/
//           (.bak* excluded).
//
// STRUCTURAL_TAUTOLOGY_ROUND_TRIP defence (cache_registry.md §4.e):
//   parser-A reads markdown rows; parser-B reads 7 distinct C++ shapes
//   (thread_local unordered_map, sharded vector<unique_ptr<Shard>>,
//    accessor returning unordered_map&, namespace-scope unordered_map,
//    singleton-member unordered_map, OperatorMemo accessor, namespace-
//    scope std::map). Different file kinds, different regex grammars.
//    Either side can fail independently. self_test_patterns adds a third
//    transcription axis: synthetic-input fixtures, capturing intent.
//
// Exit 0 on equality (with "PASS: |S_md|=|S_src|=10" stderr line).
// Exit 1 with diff dump on any divergence or self-test failure.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef HF_CACHE_REGISTRY_MD_PATH
#  error "HF_CACHE_REGISTRY_MD_PATH must be set by CMake target_compile_definitions"
#endif
#ifndef HF_SRC_ROOT
#  error "HF_SRC_ROOT must be set by CMake target_compile_definitions"
#endif

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

// parser-A: markdown row enumerator. Matches `| `NAME` | ...` rows where
// NAME is a C++ identifier (\w+). Restricted to rows whose 4th |-separated
// column equals one of {A,B,C,D,E,F} (the pattern column per §2), so the
// regex never over-matches incidental backtick-wrapped names elsewhere in
// the doc (§5 cross-ref table, §1 prose).
std::set<std::string> parser_A_md(const fs::path& md_path) {
    std::set<std::string> S;
    std::string s = slurp(md_path);
    if (s.empty()) {
        std::fprintf(stderr,
            "parser-A: cannot read HF_CACHE_REGISTRY_MD_PATH=%s\n",
            md_path.string().c_str());
        return S;
    }
    // Pattern column accepts A..G (iter-42 widening for Pattern G,
    // std::map family). Currently zero §2 rows use pattern G; the
    // letter is reserved so a future std::map registry-class cache
    // can declare its row without parser-A regex churn.
    static const std::regex re(
        R"REG(^\|\s*`([A-Za-z_][A-Za-z0-9_]*)`\s*\|[^|]*\|[^|]*\|\s*([ABCDEFG])\s*\|)REG",
        std::regex::ECMAScript | std::regex::multiline);
    auto begin = std::sregex_iterator(s.begin(), s.end(), re);
    auto end   = std::sregex_iterator{};
    for (auto it = begin; it != end; ++it) S.insert((*it)[1].str());
    return S;
}

bool is_active_source(const fs::path& p) {
    static const std::set<std::string> exts = {".cpp", ".hpp", ".cc", ".h"};
    if (!fs::is_regular_file(p)) return false;
    auto e = p.extension().string();
    if (!exts.count(e)) return false;
    for (const auto& seg : p) {
        auto s = seg.string();
        if (s.find(".bak") != std::string::npos) return false;
    }
    return true;
}

// Pattern regexes: a single shared definition for both parser_B_src and the
// self-test (so the two never drift). Functions returning const& to local
// static prevent reordering bugs while keeping the regexes lazily compiled.
//
// Patterns use ECMAScript regex with multiline flag enabled where line-anchoring
// (`^`) is needed to distinguish namespace-scope decls from function-scope ones.
const std::regex& re_A() {
    // Pattern A: thread_local std::unordered_map<...> NAME;
    static const std::regex r(
        R"REG(thread_local\s+std::unordered_map<[^;>]*>\s+(\w+)\s*;)REG",
        std::regex::ECMAScript);
    return r;
}
const std::regex& re_B() {
    // Pattern B: std::vector<std::unique_ptr<XShard>> NAME;
    static const std::regex r(
        R"REG(std::vector<\s*std::unique_ptr<\w+>\s*>\s+(g_\w*shards?)\s*;)REG",
        std::regex::ECMAScript);
    return r;
}
const std::regex& re_C() {
    // Pattern C: namespace-scope accessor std::unordered_map<...>& NAME() { ... }
    static const std::regex r(
        R"REG(^std::unordered_map<[^;>]*>&\s+(\w+)\(\s*\)\s*\{)REG",
        std::regex::ECMAScript | std::regex::multiline);
    return r;
}
const std::regex& re_D() {
    // Pattern D: namespace-scope std::unordered_map<...> NAME; (NOT thread_local;
    // identifier must start with g_ to avoid capturing return-type-by-value sites).
    static const std::regex r(
        R"REG(^std::unordered_map<[^;>]*>\s+(g_\w+)\s*;)REG",
        std::regex::ECMAScript | std::regex::multiline);
    return r;
}
const std::regex& re_E() {
    // Pattern E: literal-allowlist for singleton private member.
    // iter-39 hardcodes the single known instance (AlgebraicLetterTable::content_index_).
    // Extending pattern E to a generic class-scoped scan is deferred to iter-40+;
    // adding a new singleton-member cache requires updating this regex.
    static const std::regex r(
        R"REG(\bstd::unordered_map<[^;>]*>\s+(content_index_)\s*;)REG",
        std::regex::ECMAScript);
    return r;
}
const std::regex& re_F() {
    // Pattern F: OperatorMemo<...>& NAME() (def or decl).
    static const std::regex r(
        R"REG(\bOperatorMemo<[^;>]*>&\s+(g_\w+)\(\s*\)\s*[\{;])REG",
        std::regex::ECMAScript);
    return r;
}
const std::regex& re_G() {
    // Pattern G (iter-42 fold per iter-39 Q2): namespace-scope std::map<...>
    // NAME; (analog of Pattern D for the std::map family). Currently zero
    // matches in src/ (no registry-class std::map cache exists), but the
    // pattern is in place so a future namespace-scope std::map cache cannot
    // vacuous-pass the coverage gate. As with pattern D, the `g_` prefix
    // anchor avoids capturing return-type-by-value sites and local
    // variables that happen to start a logical line with `std::map<`.
    static const std::regex r(
        R"REG(^std::map<[^;>]*>\s+(g_\w+)\s*;)REG",
        std::regex::ECMAScript | std::regex::multiline);
    return r;
}

// parser-B: 7-pattern source scan. Patterns documented in
// docs/cache_registry.md §1.c / §4.b. Each pattern captures one or more
// cache identifiers; the union goes into S_src.
std::set<std::string> parser_B_src(const fs::path& src_root) {
    std::set<std::string> S;

    std::vector<fs::path> subdirs = {
        src_root / "src", src_root / "bridge", src_root / "include"
    };
    for (const auto& d : subdirs) {
        if (!fs::exists(d)) continue;
        for (auto it = fs::recursive_directory_iterator(d);
             it != fs::recursive_directory_iterator{}; ++it) {
            if (!is_active_source(it->path())) continue;
            std::string txt = slurp(it->path());
            if (txt.empty()) continue;
            auto sweep = [&S, &txt](const std::regex& r) {
                auto begin = std::sregex_iterator(txt.begin(), txt.end(), r);
                auto end   = std::sregex_iterator{};
                for (auto m = begin; m != end; ++m) S.insert((*m)[1].str());
            };
            sweep(re_A());
            sweep(re_B());
            sweep(re_C());
            sweep(re_D());
            sweep(re_E());
            sweep(re_F());
            sweep(re_G());
        }
    }
    return S;
}

// iter-42 fold (iter-39 Q2): exercise each parser-B regex against a synthetic
// declaration that matches its documented shape. Catches regex typos that
// would silently make a pattern capture nothing in real source (the
// "pattern compiles but never matches" vacuous-pass scenario). Pattern G
// currently has zero matches in real src/ (no std::map cache exists), so
// the self-test is its ONLY runtime confirmation that the regex captures
// correctly.
//
// STRUCTURAL_TAUTOLOGY_ROUND_TRIP third-axis (cache_registry.md §1.c spec
// prose + parser-B regex + this synthetic-fixture set = three independent
// transcriptions of the same pattern definition; any pair-wise divergence
// surfaces here, not at production-cache-add time).
bool self_test_patterns() {
    struct Case {
        const char* label;
        const std::regex* re;
        const char* synth;
        const char* want;
    };
    const Case cases[] = {
        {"A", &re_A(),
         "thread_local std::unordered_map<int,int> g_synth_A;",
         "g_synth_A"},
        {"B", &re_B(),
         "std::vector<std::unique_ptr<XShard>> g_synth_B_shards;",
         "g_synth_B_shards"},
        // C is line-anchored; embed the synthetic in a leading newline
        // so `^` matches at the start of the accessor signature.
        {"C", &re_C(),
         "\nstd::unordered_map<int,int>& g_synth_C() {",
         "g_synth_C"},
        {"D", &re_D(),
         "\nstd::unordered_map<int,int> g_synth_D;",
         "g_synth_D"},
        {"E", &re_E(),
         "std::unordered_map<int,int> content_index_;",
         "content_index_"},
        {"F", &re_F(),
         "OperatorMemo<int,int>& g_synth_F() {",
         "g_synth_F"},
        {"G", &re_G(),
         "\nstd::map<int,int> g_synth_G;",
         "g_synth_G"},
    };
    bool ok = true;
    for (const auto& c : cases) {
        const std::string s(c.synth);
        std::smatch m;
        if (!std::regex_search(s, m, *c.re) || m[1].str() != c.want) {
            std::fprintf(stderr,
                "self_test_patterns FAIL: pattern %s "
                "captured=\"%s\" want=\"%s\" synth=\"%s\"\n",
                c.label,
                m.empty() ? "<no match>" : m[1].str().c_str(),
                c.want,
                c.synth);
            ok = false;
        }
    }

    // iter-42 reviewer Q5 advisory fold: mutual-exclusivity check.
    // cache_registry.md §4.b L234 claims "Patterns are mutually exclusive at
    // each (file:line) site." A future regex relaxation (e.g., loosening
    // Pattern D to accept `thread_local`) could silently break this invariant
    // and S_src would coincidentally remain correct via overlapping captures.
    // Assert each synth string is matched by exactly ONE of the 7 patterns.
    for (const auto& c : cases) {
        const std::string s(c.synth);
        int n_match = 0;
        const char* matchers[7] = {};
        int idx = 0;
        for (const auto& other : cases) {
            std::smatch m2;
            if (std::regex_search(s, m2, *other.re)) {
                matchers[idx++] = other.label;
                ++n_match;
            }
        }
        if (n_match != 1) {
            std::fprintf(stderr,
                "self_test_patterns FAIL: mutual-exclusivity violation: "
                "synth for pattern %s matched %d patterns (",
                c.label, n_match);
            for (int i = 0; i < idx; ++i) {
                std::fprintf(stderr, "%s%s",
                    matchers[i],
                    (i + 1 < idx) ? "," : "");
            }
            std::fprintf(stderr, "); §4.b mutual-exclusivity invariant broken.\n");
            ok = false;
        }
    }
    return ok;
}

template <typename Set>
std::vector<std::string> set_diff(const Set& a, const Set& b) {
    std::vector<std::string> out;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(out));
    return out;
}

void dump(const char* label, const std::vector<std::string>& v) {
    std::fprintf(stderr, "%s [%zu]:\n", label, v.size());
    for (const auto& x : v) std::fprintf(stderr, "  %s\n", x.c_str());
}

} // namespace

int main(int argc, char** argv) {
    // argv[1] (if given) overrides the compile-time HF_CACHE_REGISTRY_MD_PATH.
    // The negative-control ctest (`hf-cache-registry-negative-control`) uses
    // this to point at a configure-time-mutated stripped copy of the registry
    // and asserts WILL_FAIL on the coverage gate.
    fs::path md  = (argc >= 2) ? fs::path(argv[1]) : fs::path(HF_CACHE_REGISTRY_MD_PATH);
    fs::path src = HF_SRC_ROOT;

    // iter-42 Q2 fold: run the parser-level self-test first. A failure
    // here would mean a parser-B regex no longer captures the shape
    // it documents in cache_registry.md §1.c — a contract drift between
    // spec and parser that must be fixed BEFORE evaluating S_md / S_src,
    // otherwise the coverage gate could pass vacuously on a broken regex.
    if (!self_test_patterns()) {
        std::fprintf(stderr,
            "FAIL: parser-B self-test failed; one or more pattern regexes "
            "do not capture their documented synthetic shape. Fix the "
            "regex (or the §1.c spec) before evaluating coverage.\n");
        return 1;
    }

    auto S_md  = parser_A_md(md);
    auto S_src = parser_B_src(src);

    std::fprintf(stderr,
        "hf-cache-registry-coverage: |S_md|=%zu |S_src|=%zu\n",
        S_md.size(), S_src.size());

    if (S_md.empty()) {
        std::fprintf(stderr,
            "FAIL: parser-A returned empty set (md path or §2 table broken)\n");
        return 1;
    }
    if (S_src.empty()) {
        std::fprintf(stderr,
            "FAIL: parser-B returned empty set (HF_SRC_ROOT broken or no sources)\n");
        return 1;
    }

    auto md_minus_src = set_diff(S_md, S_src);
    auto src_minus_md = set_diff(S_src, S_md);
    if (md_minus_src.empty() && src_minus_md.empty()) {
        std::fprintf(stderr, "PASS: registry covers source exactly (%zu names).\n",
                     S_md.size());
        return 0;
    }
    std::fprintf(stderr, "FAIL: registry/source mismatch.\n");
    if (!md_minus_src.empty()) dump("S_md \\ S_src (declared but not in source)", md_minus_src);
    if (!src_minus_md.empty()) dump("S_src \\ S_md (in source but not declared)", src_minus_md);
    return 1;
}
