// Track 7.1 iter-37 Step 5 falsifier: env-flag registry coverage gate.
//
// Asserts S_md == S_src where
//   S_md  = names declared in HF_ENV_FLAGS_MD_PATH §2 (parser-A: markdown
//           row regex `^\|\s*`(HF_[A-Z0-9_]+)`\s*\|`).
//   S_src = names appearing as C-string literals "HF_[A-Z0-9_]+" in
//           *.cpp / *.hpp / *.cc / *.h under HF_SRC_ROOT/src/,
//           HF_SRC_ROOT/bridge/, HF_SRC_ROOT/include/  (.bak* excluded).
//
// STRUCTURAL_TAUTOLOGY_ROUND_TRIP defence (iter-36 audit §F refinement):
//   * parser-A reads markdown-table rows; parser-B reads C string literals.
//   * Different file kinds (.md vs .cpp/.hpp/.cc/.h), different regexes.
//   * The source side intentionally uses the LITERAL-STRING union pattern,
//     not `getenv("HF_...")`, so wrapper-based call sites
//     (env_truthy / env_size / OpDumper) are picked up. See iter-36 audit
//     Finding B (12 wrapper-based env vars in operator_memo.cpp + OpDumper).
//
// Exit 0 on equality. Exit 1 with diff dump on any divergence.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

#ifndef HF_ENV_FLAGS_MD_PATH
#  error "HF_ENV_FLAGS_MD_PATH must be set by CMake target_compile_definitions"
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

// parser-A: markdown row enumerator. Match `| `NAME` | ...` rows in §2 tables.
// Skips header / separator rows (no backtick-wrapped NAME), so it never
// over-matches the column headings.
std::set<std::string> parser_A_md(const fs::path& md_path) {
    std::set<std::string> S;
    std::string s = slurp(md_path);
    if (s.empty()) {
        std::fprintf(stderr,
            "parser-A: cannot read HF_ENV_FLAGS_MD_PATH=%s\n",
            md_path.string().c_str());
        return S;
    }
    static const std::regex re(R"(^\|\s*`(HF_[A-Z0-9_]+)`\s*\|)",
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
    // Exclude any path component containing ".bak" (e.g.,
    // foo.cpp.bak-pre-strip-20260504-232004). iter-36 Finding A: .bak
    // files survive from the iter-30 strip pivot and inflate the count.
    for (const auto& seg : p) {
        auto s = seg.string();
        if (s.find(".bak") != std::string::npos) return false;
    }
    return true;
}

// Comment-aware pre-pass for parser-B (iter-66 F3 hardening).
//
// Drops the content of // line-comments and /* ... */ block-comments while
// preserving the content of string literals (including raw strings
// R"delim(...)delim") and character literals. The state machine handles
// the string-in-comment edge case ("// not a comment" inside a string is
// preserved; // outside any string opens a line comment).
//
// Pre-iter-66 behavior: parser-B regexed raw file text, so any commented-out
// example of the form `// std::getenv("HF_OPERATOR_MEMO")` over-matched and
// inflated S_src. iter-63 / iter-64 / iter-65 each independently re-tripped
// this landmine while documenting new HF_FLAG[_NAME]_* macro headers. This
// pre-pass removes the landmine permanently.
//
// Scope / known limitations:
//   * L / u / U / u8 string-literal prefixes are tokenized as plain code
//     followed by a regular string; this never under-matches HF_X literals
//     and only fails to recognize raw-string semantics in those very rare
//     cases (none currently in HF source).
//   * iter-67 fold: C++ phase-2 line-splicing (backslash-newline) IS now
//     honored inside // line-comments (F1). A malformed R"<delim>" raw-
//     string introducer without the opening '(' is recovered cleanly back
//     to CODE state instead of wedging RAW to EOF (F2).
//   * Trigraphs are not emulated. HF source does not use them.
//   * Nested /* */ comments are not supported (C++ does not support them).
std::string strip_comments_cpp(const std::string& s) {
    auto is_id = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_';
    };
    std::string out;
    out.reserve(s.size());
    enum { CODE, LINE_C, BLOCK_C, STR, CH, RAW } st = CODE;
    std::string raw_delim;
    const size_t N = s.size();
    size_t i = 0;
    while (i < N) {
        const char c = s[i];
        const char n = (i + 1 < N) ? s[i + 1] : '\0';
        switch (st) {
        case CODE:
            if (c == '/' && n == '/') { st = LINE_C; i += 2; break; }
            if (c == '/' && n == '*') { st = BLOCK_C; i += 2; break; }
            if (c == '\'')             { out += c; st = CH; ++i; break; }
            if (c == '"') {
                const bool is_raw =
                    (i > 0 && s[i - 1] == 'R'
                     && (i < 2 || !is_id(s[i - 2])));
                if (is_raw) {
                    out += c; ++i;
                    raw_delim.clear();
                    while (i < N && s[i] != '(' && s[i] != '\n') {
                        raw_delim += s[i];
                        out += s[i];
                        ++i;
                    }
                    // iter-67 F2 fold: only enter RAW state if the opening
                    // '(' was actually present. A malformed R"NoParen" raw-
                    // string introducer (which is a compile error in real
                    // C++) used to wedge the state machine in RAW state with
                    // empty raw_delim, scanning to EOF for a matching )"
                    // that would never come.
                    if (i < N && s[i] == '(') { out += s[i]; ++i; st = RAW; }
                    else { st = CODE; raw_delim.clear(); }
                } else {
                    out += c; st = STR; ++i;
                }
                break;
            }
            out += c; ++i; break;
        case LINE_C:
            // iter-67 F1 fold: honor C++ phase-2 line-splicing inside //
            // line-comments. A backslash followed immediately by a newline
            // does NOT end the comment; both characters are spliced out and
            // the comment continues onto the next line. Without this, a
            // commented-out '// real getenv on next line: \' could leak a
            // following getenv("HF_X") into parser-B's match set.
            if (c == '\\' && n == '\n') { i += 2; break; }
            if (c == '\n') { out += c; st = CODE; }
            ++i; break;
        case BLOCK_C:
            if (c == '*' && n == '/') { st = CODE; i += 2; break; }
            if (c == '\n') out += c;
            ++i; break;
        case STR:
            out += c;
            if (c == '\\' && n != '\0') { out += n; i += 2; break; }
            if (c == '"') st = CODE;
            ++i; break;
        case CH:
            out += c;
            if (c == '\\' && n != '\0') { out += n; i += 2; break; }
            if (c == '\'') st = CODE;
            ++i; break;
        case RAW: {
            if (c == ')') {
                bool match = (i + 1 + raw_delim.size() < N
                              && s[i + 1 + raw_delim.size()] == '"');
                for (size_t k = 0; match && k < raw_delim.size(); ++k) {
                    if (s[i + 1 + k] != raw_delim[k]) { match = false; }
                }
                if (match) {
                    out += c;
                    for (size_t k = 0; k < raw_delim.size(); ++k)
                        out += s[i + 1 + k];
                    out += '"';
                    i += 2 + raw_delim.size();
                    st = CODE;
                    break;
                }
            }
            out += c; ++i; break;
        }
        }
    }
    return out;
}

// parser-B: C-string literal-union scan. Recurses HF_SRC_ROOT/{src,bridge,include}.
// Distinct regex from parser-A (no backticks; quoted-string form).
//
// iter-66 F3: runs strip_comments_cpp(text) as a pre-pass to remove the
// recurring landmine of documentation prose that quotes "HF_X" literals
// inside comments. Without the pre-pass, those prose mentions were
// over-matched and inflated S_src.
std::set<std::string> parser_B_src(const fs::path& src_root) {
    std::set<std::string> S;
    static const std::regex re(R"REG("(HF_[A-Z0-9_]+)")REG");
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
            const std::string stripped = strip_comments_cpp(txt);
            auto begin = std::sregex_iterator(stripped.begin(), stripped.end(), re);
            auto end   = std::sregex_iterator{};
            for (auto m = begin; m != end; ++m) S.insert((*m)[1].str());
        }
    }
    return S;
}

// iter-75 §T9 verification-oracle (closes iter71_A4; escalated by iter-73
// reviewer and iter-74 F8a advisory). Two new gates on top of the existing
// |S_md|=|S_src| set-equality check:
//
//   oracle-1: every `#define HF_FLAG_<NAME>` line in include/hyperflint/
//             appears in exactly one header. Catches partial-relocation
//             regressions where a macro is duplicated across env_flags
//             headers with potentially conflicting bodies. Set-equality
//             cannot catch this because the env-var literal still appears
//             in S_src exactly once per name (the duplicate macro bodies
//             merge in std::set).
//
//   oracle-2: for every "HF_<NAME>" literal that appears in any header
//             under include/hyperflint/, that literal appears in at most
//             one header. Dual of oracle-1: a macro could be moved from
//             one env_flags_<subsystem>.hpp to another while a stale copy
//             of the literal lingers in the original header (e.g., inside
//             a stub body that was left behind by a maintainer mid-
//             refactor). Without this gate, set-equality would still pass.
//
// Hazard becomes more concrete after iter-74's 3-header Option ζ split
// for the Track-cache-toggle cluster (algebra/env_flags.hpp +
// core/env_flags_rat.hpp + integrator/env_flags.hpp): the §5.1 family-
// isolation invariant now relies on three independent <domain>/env_flags
// homes each owning their respective macros uniquely. iter-71 single-
// header creation did not need this gate; iter-74's 3-header split does.
//
// Both gates are *preventive*: as of iter-74's LAND, neither catches a
// current defect (53 distinct HF_FLAG_* macros, each in exactly one
// header; 58 distinct "HF_<NAME>" literals in headers, each in exactly
// one header).
std::map<std::string, std::vector<fs::path>>
scan_hf_flag_defines(const fs::path& include_root) {
    std::map<std::string, std::vector<fs::path>> by_name;
    static const std::regex re(
        R"REG(^[ \t]*#[ \t]*define[ \t]+(HF_FLAG_[A-Z0-9_]+)\b)REG",
        std::regex::ECMAScript | std::regex::multiline);
    if (!fs::exists(include_root)) return by_name;
    for (auto it = fs::recursive_directory_iterator(include_root);
         it != fs::recursive_directory_iterator{}; ++it) {
        if (!is_active_source(it->path())) continue;
        const std::string txt = slurp(it->path());
        if (txt.empty()) continue;
        const std::string stripped = strip_comments_cpp(txt);
        auto begin = std::sregex_iterator(stripped.begin(), stripped.end(), re);
        auto end   = std::sregex_iterator{};
        for (auto m = begin; m != end; ++m)
            by_name[(*m)[1].str()].push_back(it->path());
    }
    return by_name;
}

std::map<std::string, std::set<fs::path>>
scan_header_literal_locations(const fs::path& include_root) {
    std::map<std::string, std::set<fs::path>> by_lit;
    static const std::regex re(R"REG("(HF_[A-Z0-9_]+)")REG");
    if (!fs::exists(include_root)) return by_lit;
    for (auto it = fs::recursive_directory_iterator(include_root);
         it != fs::recursive_directory_iterator{}; ++it) {
        if (!is_active_source(it->path())) continue;
        const std::string txt = slurp(it->path());
        if (txt.empty()) continue;
        const std::string stripped = strip_comments_cpp(txt);
        auto begin = std::sregex_iterator(stripped.begin(), stripped.end(), re);
        auto end   = std::sregex_iterator{};
        for (auto m = begin; m != end; ++m)
            by_lit[(*m)[1].str()].insert(it->path());
    }
    return by_lit;
}

template <typename Set>
std::vector<std::string> diff(const Set& a, const Set& b) {
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

int main() {
    fs::path md  = HF_ENV_FLAGS_MD_PATH;
    fs::path src = HF_SRC_ROOT;
    auto S_md  = parser_A_md(md);
    auto S_src = parser_B_src(src);

    std::fprintf(stderr,
        "hf-env-flag-registry-coverage: |S_md|=%zu |S_src|=%zu\n",
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

    auto md_minus_src = diff(S_md, S_src);
    auto src_minus_md = diff(S_src, S_md);
    if (!md_minus_src.empty() || !src_minus_md.empty()) {
        std::fprintf(stderr, "FAIL: registry/source mismatch.\n");
        if (!md_minus_src.empty()) dump("S_md \\ S_src (declared but not in source)", md_minus_src);
        if (!src_minus_md.empty()) dump("S_src \\ S_md (in source but not declared)", src_minus_md);
        return 1;
    }
    std::fprintf(stderr, "PASS: registry covers source exactly (%zu names).\n",
                 S_md.size());

    // iter-75 §T9 verification-oracle gates (closes iter71_A4; preventive
    // — neither gate catches a current defect at iter-75 LAND, both are
    // proactive guardrails against post-iter-74 partial-relocation hazards).
    const fs::path include_root = src / "include" / "hyperflint";
    auto macro_defs = scan_hf_flag_defines(include_root);
    auto lit_locs   = scan_header_literal_locations(include_root);

    std::fprintf(stderr,
        "hf-env-flag-registry-coverage: oracle-1 |#define HF_FLAG_*|=%zu  "
        "oracle-2 |\"HF_*\" header-literals|=%zu\n",
        macro_defs.size(), lit_locs.size());

    int n_dup_macros = 0;
    for (const auto& kv : macro_defs) {
        if (kv.second.size() != 1) {
            std::fprintf(stderr,
                "FAIL: oracle-1 — macro \"%s\" #define'd in %zu headers:\n",
                kv.first.c_str(), kv.second.size());
            for (const auto& p : kv.second)
                std::fprintf(stderr, "  %s\n", p.string().c_str());
            ++n_dup_macros;
        }
    }

    int n_multi_header_lits = 0;
    for (const auto& kv : lit_locs) {
        if (kv.second.size() > 1) {
            std::fprintf(stderr,
                "FAIL: oracle-2 — literal \"%s\" appears in %zu headers under "
                "include/hyperflint/:\n",
                kv.first.c_str(), kv.second.size());
            for (const auto& p : kv.second)
                std::fprintf(stderr, "  %s\n", p.string().c_str());
            ++n_multi_header_lits;
        }
    }

    if (n_dup_macros + n_multi_header_lits > 0) {
        std::fprintf(stderr,
            "FAIL: oracle gates — %d duplicate-macro and %d multi-header-literal "
            "violation(s).\n",
            n_dup_macros, n_multi_header_lits);
        return 1;
    }
    std::fprintf(stderr,
        "PASS: oracle-1 (macro-define unique-home) + oracle-2 (literal "
        "single-header invariant).\n");
    return 0;
}
