// Phase γ.1: transport-neutral JSON handlers for HyperFLINT ops.
//
// Currently exposed:
//   - find_lr_orders
//
// Both the CLI (`hyperflint eval-json`) and the LibraryLink shared
// library call into these.  The CLI wrapper prints the returned string
// to stdout + newline; the LibraryLink wrapper returns it to Mma via
// MArgument_setUTF8String.  Errors travel in the returned JSON (`"error"`
// field), never stderr; neither transport aborts.

#include "hyperflint/bridge/handlers.hpp"
#include "hyperflint/bridge/env_flags.hpp"  // iter-94 Track-OMP bridge portion: HF_FLAG_MAX_THREADS_PER_CALL (NEW first bridge-domain env_flags header; §5.1 rule-1 BINDING placement)

#include "hyperflint/c_abi.h"  // HF_SCHEMA_VERSION SSOT (Track 8.1b chunk-1, iter-46)
#include "hyperflint/algebra/algebraic_letters.hpp"
#include "hyperflint/algebra/linear_factors.hpp"  // clear_linear_factors_cache
#include "hyperflint/algebra/partial_fractions.hpp"  // Track 8.1b chunk-2b iter-48
#include "hyperflint/algebra/shuffle.hpp"
#include "hyperflint/core/zw_table.hpp"  // Track 8.1b chunk-2b iter-48: handlers::partial_fractions transient
#include "hyperflint/convert/convert_hlog.hpp"
#include "hyperflint/convert/parse.hpp"
#include "hyperflint/reduce/mzv_expansion.hpp"   // HF basis-ctx campaign (PHASE_2 iter 10)
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/addpf_probe.hpp"  // period-tuples Phase 0 census
#include "hyperflint/core/symcoef.hpp"
#include "hyperflint/integrator/ctx_probe.hpp"
#include "hyperflint/integrator/env_flags.hpp"  // iter-77 Track-probe-ctx (cross-domain rule-3)
#include "hyperflint/integrator/hyper_int.hpp"
#include "hyperflint/integrator/integration_step.hpp"  // NarrowCtxTooNarrow
#include "hyperflint/integrator/lr_search.hpp"
#include "hyperflint/integrator/regularize.hpp"
#include "hyperflint/integrator/step_strategy.hpp"  // Track 6.5 wire-in iter-40
#include "hyperflint/reduce/mzv_reduce.hpp"             // clear_rhs_cache
#include "hyperflint/core/period_table.hpp"             // period_powers emission (Phase 2)
#include "hyperflint/reduce/period_scratch.hpp"          // period_tuples_enabled (Phase 2)
#include "hyperflint/runtime/narrow_ctx_flag.hpp"       // reset_narrow_ctx_flag
#include "hyperflint/runtime/env_flags.hpp"             // HF_FLAG_NARROW_CTX (iter-69)

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <flint/flint.h>
#include <iostream>
#include <memory>
#include <mutex>      // std::lock_guard/std::mutex (not transitively included on libstdc++)
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// Track 8.1 (iter-43): defensive fallback so handlers.cpp still
// compiles if some downstream target somehow links it without picking
// up the HF_VERSION_STRING compile-def from the hyperflint /
// hyperflint_nomp / hyperflint_nomp_sd libraries.  Production builds
// always supply HF_VERSION_STRING via CMakeLists.txt top-of-file
// HF_VERSION cache var.  "unknown" is a load-bearing sentinel: the
// Mma-side gate in SubTropica.wl warns on hf_version == "unknown" as
// "binary was not built with HF_VERSION_STRING — verify your build."
#ifndef HF_VERSION_STRING
#define HF_VERSION_STRING "unknown"
#endif

namespace hyperflint {
namespace handlers {

// Track 8.1 (iter-43): eval-json response schema version.
//
// Semantics (request side):
//   - Optional request field `"schema_version_min": <int>`.  If the
//     request asserts a minimum schema version greater than what this
//     binary serves, the handler returns an error_json with a
//     structured message — the caller (Mma / future C ABI) MUST gate
//     subsequent parses on the response NOT being an error before
//     reading the regular fields.
//
// Semantics (response side):
//   - Every successful response now carries:
//       "schema_version": <int>   (currently 1)
//       "hf_version": "<string>"  (from HF_VERSION_STRING, default
//                                  "unknown" if the build is older
//                                  than this define)
//   - Error responses also carry these fields so the caller can
//     diagnose mismatches against an *error* without first having to
//     parse a successful payload.
//
// Bump policy:
//   - Backwards-compatible field additions: keep schema_version=1.
//   - Renames, removals, or semantic changes to existing fields:
//     bump schema_version and update SubTropica.wl's
//     $SubTropicaHFSchemaVersionExpected in lockstep.
//
// Track 8.1 scope (iter-43): find_lr_orders body + error_json +
// error_json_op all stamp the envelope.  error_json_op was folded
// IN-ITER per iter-43 reviewer a735322b58cc29e6c Q1/Q8 advisory
// (the one-line shim is mechanical and avoids leaving an
// inconsistent-error-surface gap for hyperflint_sym).  Per-op
// success-path emission for hyperflint_sym remains a future-iter
// item if the op's response shape ever stabilises behind a documented
// schema; today it returns opaque payloads and adding the envelope
// would require an explicit response-builder refactor.  No silent
// inconsistency between error paths.
// §K.1 SSOT retrofit (iter-46 Track 8.1b chunk-1): kSchemaVersion sources
// HF_SCHEMA_VERSION from c_abi.h so the eval-json envelope (Track 8.1) and
// the forthcoming stable C ABI (Track 8.1b chunks 2-4) share a single bump
// point.  Replaces the literal `1` per iter-44 reviewer B3 BINDING fold.
constexpr int kSchemaVersion = HF_SCHEMA_VERSION;
inline const char* kHFVersion() { return HF_VERSION_STRING; }

namespace {

// ---------- minimal JSON helpers (duplicated from bridge/cli/main.cpp
// for now; promote to shared bridge/ utility if a third caller appears).

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

bool json_bool_field(const std::string& body, const std::string& key) {
    // Track 8.1b chunk-2b (iter-48): mirror of the c_abi.cpp anon-namespace
    // helper; promotion to shared utility deferred to chunk-4 per iter-44
    // plan §F.
    std::regex re("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (!std::regex_search(body, m, re)) return false;
    return m[1] == "true";
}

std::vector<std::string> autoscan_vars_single(const std::string& expr) {
    // Single-string autoscan: identifier-pattern scan used when the
    // caller doesn't supply an explicit "vars" array.  CLI's main.cpp
    // exposes an initializer-list variant; we keep this minimal form
    // local because partial_fractions only scans one expression.
    std::set<std::string> seen;
    std::regex re("[A-Za-z][A-Za-z0-9_]*");
    for (auto it = std::sregex_iterator(expr.begin(), expr.end(), re);
         it != std::sregex_iterator(); ++it) {
        seen.insert((*it)[0]);
    }
    return std::vector<std::string>(seen.begin(), seen.end());
}

std::vector<std::string> json_str_array(const std::string& body,
                                        const std::string& key) {
    std::regex re("\"" + key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
    std::smatch m;
    std::vector<std::string> out;
    if (!std::regex_search(body, m, re)) return out;
    std::string inner = m[1];
    std::regex re_item("\"((?:[^\"\\\\]|\\\\.)*)\"");
    for (auto it = std::sregex_iterator(inner.begin(), inner.end(), re_item);
         it != std::sregex_iterator(); ++it) {
        out.push_back((*it)[1]);
    }
    return out;
}

std::string extract_top_array(const std::string& body,
                               const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t k = body.find(search);
    if (k == std::string::npos) return {};
    size_t colon = body.find(':', k + search.size());
    if (colon == std::string::npos) return {};
    size_t bracket = body.find('[', colon);
    if (bracket == std::string::npos) return {};
    int depth = 1;
    size_t i = bracket + 1;
    while (i < body.size() && depth > 0) {
        if (body[i] == '[') depth++;
        else if (body[i] == ']') depth--;
        ++i;
    }
    if (depth != 0) return {};
    return body.substr(bracket + 1, i - bracket - 2);
}

std::string error_json(const std::string& msg) {
    // Track 8.1 (iter-43): error responses also carry schema_version
    // and hf_version so the caller can route on mismatch vs other
    // failure classes without first having to parse a successful
    // payload.  Field ordering matches the success path emitted by
    // find_lr_orders below: op, schema_version, hf_version, error.
    std::ostringstream o;
    o << "{\"op\":\"find_lr_orders\""
      << ",\"schema_version\":" << kSchemaVersion
      << ",\"hf_version\":\"" << json_escape(kHFVersion()) << "\""
      << ",\"error\":\"" << json_escape(msg) << "\"}";
    return o.str();
}

std::string error_json_op(const std::string& op, const std::string& msg) {
    // Track 8.1 (iter-43, in-iter fold per reviewer a735322b58cc29e6c
    // rec Q1/Q8): error_json_op now ALSO carries schema_version +
    // hf_version so the eval-json error envelope is uniform across
    // every op handler (find_lr_orders, hyperflint_sym, and any
    // future op).  Without this, a hyperflint_sym error path returns
    // a bare {"op":"hyperflint","error":"..."} and any C-ABI consumer
    // cannot diagnose whether the failure is schema / version /
    // anything else.  One-line shim; matches error_json above.
    std::ostringstream o;
    o << "{\"op\":\"" << json_escape(op) << "\""
      << ",\"schema_version\":" << kSchemaVersion
      << ",\"hf_version\":\"" << json_escape(kHFVersion()) << "\""
      << ",\"error\":\"" << json_escape(msg) << "\"}";
    return o.str();
}

// HF MZV-rewrite C-prep.4 (iter-32): F2 parse-boundary safety rail.
//
// Iter-31 post-commit advisory adversarial-reviewer F4 (agent
// a59aea4e506d5c178) flagged the JSON-bridge `Rat::parse` entry points
// at handlers.cpp:186/211/694 as the production entry vector for
// non-canonical-content Rat instances. T4c/T5c at iter-32 (test_rat_
// content_invariance.cpp) PASSED: even at TRUE F2 falsifier inputs
// where each input Rat carries shared num/den Z-content, the rep-swap
// and legacy backends produce byte-identical Rat::to_string. The
// canonical-emission writer (sym_coef_canonical_string in this file)
// is therefore not load-bearing on parse-level integer-content
// coprime-ness within each input Rat.
//
// This rail is defense-in-depth: in debug builds (NDEBUG unset), it
// asserts that every JSON-bridge-parsed Rat satisfies the to_string
// idempotency invariant -- i.e., that re-parsing the canonical
// to_string output produces a Rat with the same to_string. T1 of
// test_rat_content_invariance establishes this on synthetic inputs;
// this rail extends the check to every JSON-bridge production input.
//
// In release builds (NDEBUG set), this is a zero-cost no-op.
//
// See c_prep_4_content_audit_memo.md §5.6 (iter-32 closure rail).
inline void debug_check_parse_idempotent(
    [[maybe_unused]] const hyperflint::PolyCtx& ctx,
    [[maybe_unused]] const hyperflint::Rat& parsed,
    [[maybe_unused]] const char* site_tag) {
#ifndef NDEBUG
    const std::string& s1 = parsed.to_string();
    hyperflint::Rat r2 = hyperflint::Rat::parse(ctx, s1);
    if (s1 != r2.to_string()) {
        std::cerr << "[hyperflint][F2-parse-rail] to_string idempotency "
                     "broken at " << site_tag
                  << ": parsed=\"" << s1
                  << "\", reparsed=\"" << r2.to_string() << "\"\n";
        std::abort();
    }
#endif
}

// ---------- hyperflint_sym supporting helpers (migrated from
// bridge/cli/main.cpp anon namespace so the library + LibraryLink can
// both reach them).  Keep `static` / anon-namespace linkage; they're
// implementation detail of handlers.cpp.

std::string json_str_field(const std::string& body, const std::string& key) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
    std::smatch m;
    if (!std::regex_search(body, m, re)) return {};
    std::string v = m[1];
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char nx = v[++i];
            switch (nx) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                default:  out += nx;
            }
        } else {
            out += v[i];
        }
    }
    return out;
}

std::string resolve_mzv_data_path(const std::string& body) {
    std::string data_path = json_str_field(body, "mzv_data_path");
    if (!data_path.empty()) return data_path;
    const char* env = std::getenv("HYPERFLINT_DATA_DIR");
    if (env && *env) return std::string(env) + "/mzv_reductions.json";
    return "data/mzv_reductions.json";
}

// Phase γ.2: process-global MZV-table cache.  The CLI reloads the table
// every process; LibraryLink keeps the same process alive across Mma
// calls, so a per-process cache turns ~15 ms/call of JSON parsing into
// ~0.05 ms/call.  Keyed on path + mtime so a stale .dylib with a fresh
// reductions file still rebuilds.  Single-threaded CLI never races; the
// LibraryLink transport is called from the main Mma kernel thread and
// subkernel kernels get their own process with their own cache, so no
// mutex is needed.
const hyperflint::MzvReductionTable&
get_cached_mzv_table(const std::string& path) {
    static std::string cached_path;
    static std::time_t cached_mtime = 0;
    static std::unique_ptr<hyperflint::MzvReductionTable> cached;
    std::time_t mtime = 0;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) mtime = st.st_mtime;
    if (!cached || path != cached_path || mtime != cached_mtime) {
        cached = std::make_unique<hyperflint::MzvReductionTable>(
            hyperflint::load_mzv_reductions(path));
        cached_path  = path;
        cached_mtime = mtime;
    }
    return *cached;
}

hyperflint::Word parse_word(const hyperflint::PolyCtx& ctx,
                             const std::vector<std::string>& letters) {
    hyperflint::Word w;
    w.letters.reserve(letters.size());
    for (const auto& s : letters) {
        // HF MZV-rewrite C-prep.4 iter-32 F2 parse-boundary safety rail:
        // debug-only to_string idempotency check. See helper comment.
        hyperflint::Rat r = hyperflint::Rat::parse(ctx, s);
        debug_check_parse_idempotent(ctx, r, "parse_word/letter");
        w.letters.push_back(std::move(r));
    }
    return w;
}

hyperflint::ShuffleList parse_shuffle_list(const hyperflint::PolyCtx& ctx,
                                           const std::string& body,
                                           const std::string& key) {
    hyperflint::ShuffleList input;
    std::string inner = extract_top_array(body, key);
    if (inner.empty()) return input;
    int depth = 0;
    size_t start = 0;
    std::vector<std::string> entries;
    for (size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '{') {
            if (depth == 0) start = i;
            depth++;
        } else if (inner[i] == '}') {
            depth--;
            if (depth == 0) entries.push_back(inner.substr(start, i - start + 1));
        }
    }
    for (const auto& e : entries) {
        std::string coef_s = json_str_field(e, "coef");
        // HF MZV-rewrite C-prep.4 iter-32 F2 parse-boundary safety rail.
        hyperflint::Rat coef_r = hyperflint::Rat::parse(ctx, coef_s);
        debug_check_parse_idempotent(ctx, coef_r,
                                      "parse_shuffle_list/coef");
        hyperflint::ShuffleEntry ent{std::move(coef_r), {}};
        std::string sh_inner = extract_top_array(e, "shuffle");
        if (!sh_inner.empty()) {
            int wd = 0;
            size_t ws = 0;
            for (size_t i = 0; i < sh_inner.size(); ++i) {
                if (sh_inner[i] == '[') {
                    if (wd == 0) ws = i;
                    wd++;
                } else if (sh_inner[i] == ']') {
                    wd--;
                    if (wd == 0) {
                        std::string w_json = sh_inner.substr(ws, i - ws + 1);
                        std::string wrapped = "{\"w\":" + w_json + "}";
                        auto letters = json_str_array(wrapped, "w");
                        ent.shuffle.push_back(parse_word(ctx, letters));
                    }
                }
            }
        }
        input.push_back(std::move(ent));
    }
    return input;
}

std::string sym_coef_to_mma_string(const hyperflint::SymCoef& s) {
    if (s.is_zero()) return "0";
    std::ostringstream o;
    bool first = true;
    for (const auto& m : s.terms()) {
        std::string pre = m.prefactor.to_string();
        bool needs_paren = pre.find('+') != std::string::npos
                        || pre.find('-') != std::string::npos
                        || pre.find('/') != std::string::npos
                        || pre.find('*') != std::string::npos;
        if (!first) o << " + ";
        if (needs_paren) o << "(" << pre << ")"; else o << pre;
        if (m.pi_power == 1)      o << "*Pi";
        else if (m.pi_power != 0) o << "*Pi^" << m.pi_power;
        if (m.i_power == 1)       o << "*I";
        else if (m.i_power != 0)  o << "*I^" << m.i_power;
        for (const auto& kv : m.log_powers) {
            if (kv.second == 1) o << "*Log[" << kv.first << "]";
            else                o << "*Log[" << kv.first << "]^" << kv.second;
        }
        for (const auto& kv : m.delta_powers) {
            if (kv.second == 1) o << "*delta[" << kv.first << "]";
            else                o << "*delta[" << kv.first << "]^" << kv.second;
        }
        // Period-tuples Phase 2: render period generators by their atom
        // names (same tokens the wide-ctx path embeds in the Rat string,
        // so the Mathematica-side parser reuses its existing handling).
        for (const auto& kv : m.period_powers) {
            const std::string& nm =
                hyperflint::PeriodTable::instance().key_for(kv.first);
            if (kv.second == 1) o << "*" << nm;
            else                o << "*" << nm << "^" << kv.second;
        }
        first = false;
    }
    return o.str();
}

// Phase 7-vi-b: serialize the process-global AlgebraicLetterTable so
// the Mma caller can register matching entries in
// HyperIntica`$HyperAlgebraicLetterTable (index-remapped onto
// $HyperAlgebraicLetterCounter) and keep SimplifyWithVieta /
// GetAlgebraicBackSubRules / stEchoAlgebraicLettersSummary working
// unchanged on Wm[i]/Wp[i] symbols that came out of HF.  Each entry
// emits the raw degree-2 polynomial data; Mma derives WmValue/WpValue
// locally as (-b ∓ √disc)/(2·lc).
//
// Output shape: JSON array of objects with keys
//   "idx":      1-based HF-local index  (remapped on Mma side)
//   "poly":     the degree-2 polynomial as a parseable string
//   "var":      name of the special variable inside `poly`
//   "lc":       leading coefficient string
//   "sum":      Vieta sum (-b/lc) string
//   "product":  Vieta product (c/lc) string
//   "disc":     discriminant (b² - 4·lc·c) string
std::string emit_algebraic_letter_table() {
    const auto& tbl = hyperflint::AlgebraicLetterTable::global();
    std::ostringstream o;
    o << "[";
    bool first = true;
    for (long i : tbl.indices()) {
        const auto& e = tbl.at(i);
        if (!first) o << ",";
        first = false;
        o << "{\"idx\":"     << e.idx
          << ",\"poly\":\""  << json_escape(e.polynomial.to_string())     << "\""
          << ",\"var\":\""   << json_escape(e.polynomial.ctx().vars()[e.var_idx]) << "\""
          << ",\"lc\":\""    << json_escape(e.lc.to_string())              << "\""
          << ",\"sum\":\""   << json_escape(e.sum_value.to_string())       << "\""
          << ",\"product\":\"" << json_escape(e.product_value.to_string()) << "\""
          << ",\"disc\":\""  << json_escape(e.discriminant.to_string())    << "\""
          << "}";
    }
    o << "]";
    return o.str();
}

std::string emit_regulator_sym(const hyperflint::RegulatorSym& r) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < r.size(); ++i) {
        if (i) o << ",";
        o << "{\"coef\":\"" << json_escape(sym_coef_to_mma_string(r[i].coef)) << "\""
          << ",\"key\":[";
        for (size_t j = 0; j < r[i].key.size(); ++j) {
            if (j) o << ",";
            o << "[";
            for (size_t k = 0; k < r[i].key[j].size(); ++k) {
                if (k) o << ",";
                o << "\"" << json_escape(r[i].key[j][k].to_string()) << "\"";
            }
            o << "]";
        }
        o << "]}";
    }
    o << "]";
    return o.str();
}

// HF MZV-rewrite C-prep.4 (iter-27) -- canonical-emission writer.
//
// Path (c) per c_prep_4_scoping_memo.md: enforce structural-canonical
// input by calling SymCoef::canonicalize() before emitting. On already-
// canonical input the byte sequence matches sym_coef_to_mma_string
// exactly; on non-canonical input (post-OMP-merge cross-thread reorder
// at C0a) it absorbs the structural-permutation drift into a single
// canonical byte sequence.
//
// REFINEMENT, not a weakening: the comparator stays sha256 cross-cell
// match on the canonical-emission output; on inputs that already pass
// bit-identity today this writer produces the same bytes as
// sym_coef_to_mma_string. On structurally-distinct-but-algebraically-
// equivalent inputs the comparator still rejects (e.g. Pi^(2k) vs zeta
// absorption is NOT performed by canonicalize()). See iter-27 drift
// check M1 + memo §5 case 4.
//
// Iter-30 (HF MZV-rewrite C-prep.4 content audit close,
// c_prep_4_content_audit_memo.md §5.4): T4+T5 verified that the two
// production hot paths (`Rat::add_repswap` == `add_via_q_underscore`
// and `Rat::add_legacy` == cross-mult+gcd_cofactors) produce
// byte-identical `Rat::to_string` on algebraically-equal operands at
// both nvars=60 (rep-swap dispatch regime) and nvars=12 (Smirnov tst2
// legacy dispatch regime).  The Rat instances reaching this writer in
// production come exclusively through these arithmetic paths plus the
// `Rat::Rat(Poly,Poly)` ctor on arithmetic results, all of which
// preserve content normalization across paths.
//
// **Parse-level invariant (T3 caveat)**: `fmpq_mpoly_get_str_pretty`
// is integer-content-faithful for Rats produced by the production
// arithmetic paths, but NOT for Rats produced by `Rat::parse` of an
// arbitrary non-coprime-content input string (e.g.
// `Rat::parse("(2*x)/(2*x+2)")` lands on `"2*x/(2*x + 2)"` while
// `Rat::parse("x/(x+1)")` lands on `"x/(x + 1)"`).  If a future caller
// starts feeding `Rat::parse(non_coprime_content_string)` directly
// into `sym_coef_canonical_string`, add `from_canonical_normalize_content`
// (~30 LOC: pull integer content out of num and den, divide both by
// the common gcd_Z, re-emit) as a preprocessor at the writer entry
// before doing so. See c_prep_4_content_audit_memo.md §5 for the
// audit verdict and §5.2 for the remediation sketch.
std::string sym_coef_canonical_string(const hyperflint::SymCoef& s) {
    return sym_coef_to_mma_string(s.canonicalize());
}

std::string emit_regulator_sym_canonical(const hyperflint::RegulatorSym& r) {
    // canonicalize_regulator_sym (break_up_contour.cpp:222) sorts by
    // regkey_content_key, collects duplicate keys, drops zero-coef
    // entries; the inner key entries are canonicalize_regkey-sorted.
    hyperflint::RegulatorSym canon = hyperflint::canonicalize_regulator_sym(r);
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < canon.size(); ++i) {
        if (i) o << ",";
        o << "{\"coef\":\""
          << json_escape(sym_coef_canonical_string(canon[i].coef)) << "\""
          << ",\"key\":[";
        for (size_t j = 0; j < canon[i].key.size(); ++j) {
            if (j) o << ",";
            o << "[";
            for (size_t k = 0; k < canon[i].key[j].size(); ++k) {
                if (k) o << ",";
                o << "\"" << json_escape(canon[i].key[j][k].to_string()) << "\"";
            }
            o << "]";
        }
        o << "]}";
    }
    o << "]";
    return o.str();
}

}  // namespace

// ---------- find_lr_orders ----------

std::string find_lr_orders(const std::string& body) {
    try {
        // Track 8.1 (iter-43): optional request-side gate.  If the
        // caller asserts a minimum schema version greater than what
        // this binary supports, fail fast before any algebraic work.
        // The error_json path also stamps schema_version + hf_version
        // so the caller can diagnose the mismatch without ambiguity.
        // Negative or non-numeric values fall through to std::stoi
        // throwing — caught by the outer try/catch as a parse error.
        {
            std::regex re_sv(
                R"~("schema_version_min"\s*:\s*([0-9]+))~");
            std::smatch m_sv;
            if (std::regex_search(body, m_sv, re_sv)) {
                const int requested = std::stoi(m_sv[1]);
                if (requested > kSchemaVersion) {
                    return error_json(
                        "schema_version_min=" + std::to_string(requested)
                        + " exceeds supported schema_version="
                        + std::to_string(kSchemaVersion)
                        + " (hf_version=" + kHFVersion() + ")");
                }
            }
        }

        auto xvars = json_str_array(body, "xvars");
        if (xvars.empty()) return error_json("need \"xvars\"");
        auto coeff_vars = json_str_array(body, "coeff_vars");

        // Group shape: either "groups":[[...],[...]] (multi-group)
        // or "polys":[...] (single-group convenience).
        std::vector<std::vector<std::string>> group_strs;
        std::string groups_inner = extract_top_array(body, "groups");
        if (!groups_inner.empty()) {
            int depth = 0;
            size_t start = 0;
            for (size_t i = 0; i < groups_inner.size(); ++i) {
                if (groups_inner[i] == '[') {
                    if (depth == 0) start = i;
                    depth++;
                } else if (groups_inner[i] == ']') {
                    depth--;
                    if (depth == 0) {
                        std::string sub_obj = "{\"xs\":" +
                            groups_inner.substr(start, i - start + 1) + "}";
                        group_strs.push_back(json_str_array(sub_obj, "xs"));
                    }
                }
            }
        } else {
            auto polys_str = json_str_array(body, "polys");
            if (polys_str.empty()) return error_json("need \"polys\" or \"groups\"");
            group_strs.push_back(std::move(polys_str));
        }
        if (group_strs.empty()) return error_json("empty group list");

        std::vector<std::string> all_vars = xvars;
        for (const auto& cv : coeff_vars) all_vars.push_back(cv);

        hyperflint::PolyCtx ctx(all_vars);
        std::vector<size_t> xvar_indices;
        xvar_indices.reserve(xvars.size());
        for (const auto& v : xvars) xvar_indices.push_back(ctx.index_of(v));

        std::vector<std::vector<hyperflint::Poly>> group_polys;
        group_polys.reserve(group_strs.size());
        for (size_t g = 0; g < group_strs.size(); ++g) {
            std::vector<hyperflint::Poly> parsed;
            parsed.reserve(group_strs[g].size());
            for (const auto& s : group_strs[g]) {
                try {
                    parsed.emplace_back(ctx, s);
                } catch (const std::exception& e) {
                    return error_json(std::string("poly parse failed in group ")
                        + std::to_string(g) + ": " + e.what());
                }
            }
            group_polys.push_back(std::move(parsed));
        }

        // Phase 7-vii: optional algebraic_letters flag.
        bool allow_al = false;
        {
            std::regex re("\"algebraic_letters\"\\s*:\\s*(true|false)");
            std::smatch m;
            if (std::regex_search(body, m, re)) {
                allow_al = (m[1] == "true");
            }
        }

        hyperflint::lr_search::LrResult result;
        double compute_s = 0.0;
        auto t0 = std::chrono::steady_clock::now();
        result = hyperflint::lr_search::find_lr_orders(
            group_polys, xvar_indices, allow_al);
        auto t1 = std::chrono::steady_clock::now();
        compute_s = std::chrono::duration<double>(t1 - t0).count();

        /* @ARCH:step-strategy v=1 */
        // Track 6.5 / 6.3-followup wire-in (iter-40, satisfies iter-35 rec 3
        // BINDING by iter-45 contract).  Construct StepInputs from the
        // request body + LR-search result, classify via pick_step_strategy
        // (single source of truth in
        // include/hyperflint/integrator/step_strategy.hpp), and emit the
        // resulting StepStrategy enum name in the response JSON.  Mma
        // callers consume this field to dispatch LR_NoOpt / LR_OptOrdered
        // (HF-native) vs Fubini_Lungo / Fubini_Espresso (Mma-delegated
        // fallback) without re-deriving the rule.  See ARCHITECTURE.md §vi.
        std::string method_lr_hint_str = "Lungo";  // Mma default
        {
            std::regex re_hint(
                R"~("method_lr_hint"\s*:\s*"([A-Za-z_]+)")~");
            std::smatch m_hint;
            if (std::regex_search(body, m_hint, re_hint)) {
                method_lr_hint_str = m_hint[1].str();
            }
        }
        std::size_t n_factors_total = 0;
        for (const auto& gp : group_polys) n_factors_total += gp.size();
        hyperflint::integrator::StepInputs strategy_inputs;
        strategy_inputs.degree_budget = allow_al ? 2 : 1;
        strategy_inputs.n_factors     = n_factors_total;
        strategy_inputs.n_letters     = xvar_indices.size();
        strategy_inputs.lr_found      = !result.nolr();
        strategy_inputs.method_lr_hint =
            (method_lr_hint_str == "Espresso")
                ? hyperflint::integrator::StepInputs::MethodLR::Espresso
                : hyperflint::integrator::StepInputs::MethodLR::Lungo;
        const hyperflint::integrator::StepStrategy strategy_choice =
            hyperflint::integrator::pick_step_strategy(strategy_inputs);
        const char* strategy_name = "UNKNOWN";
        switch (strategy_choice) {
            case hyperflint::integrator::StepStrategy::LR_OptOrdered:
                strategy_name = "LR_OptOrdered"; break;
            case hyperflint::integrator::StepStrategy::LR_NoOpt:
                strategy_name = "LR_NoOpt"; break;
            case hyperflint::integrator::StepStrategy::Fubini_Lungo:
                strategy_name = "Fubini_Lungo"; break;
            case hyperflint::integrator::StepStrategy::Fubini_Espresso:
                strategy_name = "Fubini_Espresso"; break;
        }
        /* @ARCH:end step-strategy */

        std::ostringstream o;
        // Track 8.1 (iter-43): schema_version + hf_version emitted
        // immediately after "op" so Mma callers and any future C ABI
        // consumer can gate on the field before doing the full parse.
        // Field-ordering rationale: keeps the version envelope at the
        // head of the JSON so a streaming parser can short-circuit on
        // mismatch without scanning the rest of the payload.
        o << "{\"op\":\"find_lr_orders\""
          << ",\"schema_version\":" << kSchemaVersion
          << ",\"hf_version\":\"" << json_escape(kHFVersion()) << "\""
          << ",\"best_order\":[";
        for (size_t i = 0; i < result.order.size(); ++i) {
            if (i) o << ",";
            const size_t ctx_idx = result.order[i];
            o << "\"" << json_escape(all_vars[ctx_idx]) << "\"";
        }
        o << "],\"score\":";
        if (result.nolr() || !std::isfinite(result.score)) {
            o << "null";
        } else {
            o << result.score;
        }
        o << ",\"nolr\":" << (result.nolr() ? "true" : "false")
          << ",\"strategy\":\"" << strategy_name << "\""
          << ",\"timing_compute_s\":" << compute_s
          << ",\"nXVars\":" << xvars.size()
          << ",\"nGroups\":" << group_polys.size()
          << ",\"nPolys\":[";
        for (size_t g = 0; g < group_polys.size(); ++g) {
            if (g) o << ",";
            o << group_polys[g].size();
        }
        o << "]";
        // Phase 7-vii: emit the deg-2 polys collected during the LR
        // walk so the SubTropica side knows which polys to introduce
        // as Wm/Wp at integration time.  Mma's STFasterFubini2 returns
        // these as `result[[2]]` under FindRoots=True.
        if (allow_al) {
            o << ",\"root_polys\":[";
            for (size_t i = 0; i < result.root_polys.size(); ++i) {
                if (i) o << ",";
                o << "\"" << json_escape(result.root_polys[i].to_string()) << "\"";
            }
            o << "]";
        }
        o << "}";
        return o.str();
    } catch (const std::exception& e) {
        return error_json(e.what());
    } catch (...) {
        return error_json("unknown exception in find_lr_orders");
    }
}

// ---------- partial_fractions ----------

// Track 8.1b chunk-2b (iter-48): transport-neutral partial_fractions
// handler.  Factored out of bridge/cli/main.cpp:916 per iter-44 plan §B
// chunk-2.  Returns the CLI-form response (no envelope, no trailing
// newline) so the CLI shim can print + newline; the C ABI wrapper in
// src/bridge/c_abi.cpp splices the schema_version + hf_version envelope
// in front of the payload (or, in a future chunk, calls a sibling
// envelope-emitting variant directly).
//
// Field ordering and content match the legacy main.cpp output verbatim
// so the iter-48 byte-identical CLI snapshot ctest gates regressions.
// Error reporting: on missing `f` or `var`, returns
// `{"op":"partial_fractions","error":"<msg>"}` (no envelope —
// matches the legacy `std::cerr` + non-zero exit-code path semantically
// but moves the message into JSON so non-CLI callers can route on it).
std::string partial_fractions(const std::string& body) {
    try {
        std::string f   = json_str_field(body, "f");
        std::string var = json_str_field(body, "var");
        if (f.empty() || var.empty()) {
            return std::string(
                "{\"op\":\"partial_fractions\","
                "\"error\":\"need \\\"f\\\" and \\\"var\\\" fields\"}");
        }
        auto vars = json_str_array(body, "vars");
        if (vars.empty()) vars = autoscan_vars_single(f);
        bool present = false;
        for (const auto& v : vars) if (v == var) { present = true; break; }
        if (!present) vars.push_back(var);

        // Phase 7-vi-a: same `algebraic_letters` flag as the other
        // pipeline stages; when true, deg-2 irreducible denominators
        // split into Wm/Wp pairs.
        bool introduce_al = json_bool_field(body, "algebraic_letters");
        std::vector<std::string> ctx_vars = introduce_al
            ? hyperflint::build_algebraic_letter_var_list(vars)
            : vars;
        hyperflint::PolyCtx ctx(ctx_vars);
        hyperflint::Rat r = hyperflint::Rat::parse(ctx, f);
        size_t idx = 0;
        for (; idx < ctx_vars.size(); ++idx) {
            if (ctx_vars[idx] == var) break;
        }
        // Iter-52 C0c.1 Increment β convention: caller-side fresh
        // transient ZWTable.  Bridge/CLI handlers don't share state
        // across requests, so a per-call transient is appropriate.
        auto _lf_zw = std::make_shared<hyperflint::ZWTable>(ctx);
        // Fully qualified call: the enclosing namespace is
        // hyperflint::handlers, where `partial_fractions` would name
        // *this* function via the unqualified-lookup rule.  Use the
        // global `::hyperflint::partial_fractions` to reach the
        // algebra-level algorithm in algebra/partial_fractions.hpp.
        auto pf = ::hyperflint::partial_fractions(r, idx, _lf_zw,
                                                  introduce_al);

        std::ostringstream o;
        o << "{\"op\":\"partial_fractions\""
          << ",\"var\":\"" << json_escape(var) << "\""
          << ",\"polynomial_part\":\""
          << json_escape(pf.polynomial_part.to_string()) << "\""
          << ",\"poles\":[";
        for (size_t i = 0; i < pf.poles.size(); ++i) {
            if (i) o << ",";
            const auto& P = pf.poles[i];
            o << "{\"pole\":\"" << json_escape(P.pole.to_string()) << "\""
              << ",\"multiplicity\":" << P.multiplicity
              << ",\"coefs\":[";
            for (size_t j = 0; j < P.coefs.size(); ++j) {
                if (j) o << ",";
                o << "\"" << json_escape(P.coefs[j].to_string()) << "\"";
            }
            o << "]}";
        }
        o << "],\"vars\":[";
        for (size_t i = 0; i < vars.size(); ++i) {
            if (i) o << ",";
            o << "\"" << json_escape(vars[i]) << "\"";
        }
        o << "]}";
        return o.str();
    } catch (const std::exception& e) {
        return error_json_op("partial_fractions", e.what());
    } catch (...) {
        return error_json_op("partial_fractions",
                             "unknown exception in partial_fractions");
    }
}

// ---------- linear_factors ----------

// Track 8.1b chunk-3 (iter-50): transport-neutral linear_factors
// handler.  Factored out of bridge/cli/main.cpp:933 per iter-44 plan §B
// chunk-3.  Mirror of partial_fractions above (chunk-2b iter-48):
// returns the CLI-form response (no envelope, no trailing newline), the
// CLI shim prints + newline, and the C ABI wrapper in
// src/bridge/c_abi.cpp splices the schema_version + hf_version envelope
// for external consumers.
//
// Field ordering and content match the legacy main.cpp output verbatim
// (constant, linear, nonlinear, vars) so the iter-51 chunk-3b byte-
// identical CLI snapshot ctest will gate regressions.  Error reporting:
// on missing `poly` or `var`, returns
// `{"op":"linear_factors","error":"<msg>"}` (no envelope — matches the
// chunk-2b partial_fractions convention; replaces the legacy
// std::cerr + non-zero exit-code path with a JSON error response so
// non-CLI callers can route on it).
std::string linear_factors(const std::string& body) {
    try {
        std::string poly_s = json_str_field(body, "poly");
        std::string var    = json_str_field(body, "var");
        if (poly_s.empty() || var.empty()) {
            return std::string(
                "{\"op\":\"linear_factors\","
                "\"error\":\"need \\\"poly\\\" and \\\"var\\\" fields\"}");
        }
        auto user_vars = json_str_array(body, "vars");
        if (user_vars.empty()) user_vars = autoscan_vars_single(poly_s);
        bool present = false;
        for (const auto& v : user_vars) if (v == var) { present = true; break; }
        if (!present) user_vars.push_back(var);

        // Phase 7-ii: when introduce_algebraic_letters=true, the deg-2
        // branch needs Wm_<i> / Wp_<i> atoms in the PolyCtx.
        bool introduce_al = json_bool_field(body, "introduce_algebraic_letters");
        std::vector<std::string> vars = introduce_al
            ? hyperflint::build_algebraic_letter_var_list(user_vars)
            : user_vars;

        hyperflint::PolyCtx ctx(vars);
        hyperflint::Poly p(ctx, poly_s);
        size_t idx = 0;
        for (; idx < vars.size(); ++idx) if (vars[idx] == var) break;
        // Iter-52 C0c.1: caller-side fresh transient ZWTable for the
        // mandatory `zw_tab` parameter (Option A).  Bridge/CLI handlers
        // don't share state across requests, so a per-call transient is
        // appropriate, mirroring the pre-iter-52 lambda-internal
        // allocation semantics.
        auto _lf_zw = std::make_shared<hyperflint::ZWTable>(p.ctx());
        // axis-C-lf-constant-defer: standalone bridge-CLI op needs the
        // serialised constant for compare.py cross-tests, so pass
        // compute_constant=true.  Integration callers default to false.
        // Fully qualified call: the enclosing namespace is
        // hyperflint::handlers, where `linear_factors` would name *this*
        // function via unqualified lookup; use ::hyperflint::linear_factors
        // to reach the algebra-level algorithm in
        // algebra/linear_factors.hpp.
        auto lf = ::hyperflint::linear_factors(p, idx, _lf_zw, introduce_al,
                                                /*compute_constant=*/true);

        std::ostringstream o;
        o << "{\"op\":\"linear_factors\""
          << ",\"constant\":\"" << json_escape(lf.constant) << "\""
          << ",\"linear\":[";
        for (size_t i = 0; i < lf.linear.size(); ++i) {
            if (i) o << ",";
            o << "[" << lf.linear[i].multiplicity
              << ",\"" << json_escape(lf.linear[i].pole.num().to_string()) << "\""
              << ",\"" << json_escape(lf.linear[i].pole.den().to_string()) << "\"]";
        }
        o << "],\"nonlinear\":[";
        for (size_t i = 0; i < lf.nonlinear.size(); ++i) {
            if (i) o << ",";
            o << "[" << lf.nonlinear[i].multiplicity
              << ",\"" << json_escape(lf.nonlinear[i].polynomial.to_string()) << "\""
              << "," << lf.nonlinear[i].degree_in_var << "]";
        }
        o << "],\"vars\":[";
        for (size_t i = 0; i < vars.size(); ++i) {
            if (i) o << ",";
            o << "\"" << json_escape(vars[i]) << "\"";
        }
        o << "]}";
        return o.str();
    } catch (const std::exception& e) {
        return error_json_op("linear_factors", e.what());
    } catch (...) {
        return error_json_op("linear_factors",
                             "unknown exception in linear_factors");
    }
}

// ---------- hyperflint_sym ----------

std::string hyperflint_sym(const std::string& body) {
    // Memory operational lever: per-call FLINT thread count from env
    // HF_MAX_THREADS_PER_CALL.  When set, calls
    // flint_set_num_threads(N) before any FLINT work in this call.
    // Use case: Mma master-kernel STIntegrate dispatch needs single-
    // threaded HF to keep per-call peak RSS at ~970 MB (vs ~2.5 GB
    // at OMP=13).  Env-driven so it's overridable per-process; the
    // SubTropica.wl bridge sets it explicitly when calling HF in
    // memory-constrained contexts (master kernel, parallel
    // subkernels).  Not gated by introduce_al / narrow ctx — works
    // on every code path.
    // iter-94 Track-OMP macro-layer LAND: env-var literal relocated to
    // bridge/env_flags.hpp under §5.1 rule-1 BINDING from adversarial-
    // reviewer iter-94 Q-19 B1 substantive-pattern dispatch (this is the
    // FIRST bridge-domain env_flags header). The POSITIVE_INTEGER value-
    // family semantics (atoi-then-(n>=1) guard) are preserved verbatim.
    if (const char* mt = HF_FLAG_MAX_THREADS_PER_CALL) {
        if (mt && *mt) {
            int n = std::atoi(mt);
            if (n >= 1) flint_set_num_threads(n);
        }
    }
    // R20 step 2 instrumentation: env-gated request-body dump.
    // When HF_PROBE_DUMP_DIR is set to a directory, every entry of
    // hyperflint_sym writes its JSON body to <dir>/face_<NNN>.json
    // where NNN is an atomic per-process counter. Used for the
    // wide-ctx campaign's per-face variance probe.
    if (const char* dir = HF_FLAG_PROBE_DUMP_DIR) {
        if (dir && *dir) {
            static std::atomic<int> g_probe_dump_counter{0};
            int idx = g_probe_dump_counter.fetch_add(1);
            char fname[2048];
            std::snprintf(fname, sizeof(fname),
                "%s/face_%03d_pid%d.json",
                dir, idx, getpid());
            FILE* f = std::fopen(fname, "w");
            if (f) {
                std::fwrite(body.data(), 1, body.size(), f);
                std::fclose(f);
            }
        }
    }
    try {
        auto user_vars = json_str_array(body, "vars");
        auto vars_int = json_str_array(body, "vars_int");
        auto vars_int_from = json_str_array(body, "vars_int_from");
        auto vars_int_to   = json_str_array(body, "vars_int_to");
        if (user_vars.empty()) {
            for (const auto& v : vars_int) user_vars.push_back(v);
        }
        for (const auto& vi : vars_int) {
            bool present = false;
            for (const auto& v : user_vars) if (v == vi) { present = true; break; }
            if (!present) user_vars.push_back(vi);
        }
        if (user_vars.empty()) user_vars.push_back("x");

        const bool have_ranges =
            !vars_int_from.empty() || !vars_int_to.empty();
        if (have_ranges) {
            if (vars_int_from.size() != vars_int.size() ||
                vars_int_to.size()   != vars_int.size()) {
                return error_json_op("hyperflint",
                    "vars_int_from/vars_int_to length mismatch with vars_int");
            }
        }

        bool introduce_al = false;
        {
            std::regex re("\"algebraic_letters\"\\s*:\\s*(true|false)");
            std::smatch m;
            if (std::regex_search(body, m, re)) {
                introduce_al = (m[1] == "true");
            }
        }
        // Always clear the singleton at entry — required for correctness
        // under in-process transport (LibraryLink).  Cheap when unused.
        hyperflint::AlgebraicLetterTable::global().clear();

        // R24 rev 2 / chain 17 — defuse pointer-reuse latent UB on the
        // (ctx*, rhs_str*) caches.  When a `PolyCtx` is destroyed at
        // call boundary, its raw pointer becomes dangling but cache
        // entries keyed by it survive.  A future `PolyCtx` placed at
        // the same heap address by the allocator would produce a key
        // collision and return a `Rat` valued in the OLD ctx (UB,
        // crashes possible, silent wrong answers possible).  Clearing
        // at handler entry costs one ms-scale wide-ctx parse pass on
        // first miss per call but eliminates the hazard entirely.  Also
        // reset the narrow-ctx flag so a flag set by a previous failed
        // call (in-process LibraryLink) cannot poison this call.
        hyperflint::clear_rhs_cache();
        hyperflint::clear_linear_factors_cache();
        hyperflint::reset_narrow_ctx_flag();
        // PHASE_4 round-3 BLOCKER fix (2026-05-28): same hazard as
        // clear_rhs_cache. The (ctx*, table*) cache populated by
        // apply_mzv_reductions's no-op guard must be cleared at
        // bridge entry so stale entries keyed by a now-destroyed
        // PolyCtx address cannot collide with a freshly-allocated
        // PolyCtx at the same heap address.
        hyperflint::clear_ctx_has_no_lhs_cache();

        std::string data_path = resolve_mzv_data_path(body);
        // Phase γ.2: cached MZV reduction table.  First call loads;
        // subsequent calls in the same process reuse.  ~15 ms → ~0.05
        // ms on the Phase-γ micro-bench.  CLI invocation (one process
        // per call) sees no benefit but also no regression.
        const hyperflint::MzvReductionTable& table = get_cached_mzv_table(data_path);

        // HF basis-ctx campaign (PHASE_2 iter 10): env-gated opt-in to
        // the slim-ctx path. When HF_USE_BASIS_CTX=1, build a slim ctx
        // (basis + user_vars, no LHS) AND install the active expansion
        // table so to_mzv_one_word's arm-2 fires at every mint. Mutex
        // with HF_NARROW_CTX (slim is a strictly tighter narrow); mutex
        // with introduce_al (algebraic letters retain wide ctx per
        // design §5.2 A-2 carve-out).
        const char* slim_env = std::getenv("HF_USE_BASIS_CTX");
        const bool use_slim = slim_env && *slim_env && slim_env[0] != '0'
            && !introduce_al;
        const hyperflint::MzvExpansionTable* slim_exp = nullptr;
        if (use_slim) {
            // Cache the expansion table as a process singleton so
            // repeated bridge calls reuse the same instance (the
            // basis_ctx inside is shared_ptr-held; safe across calls).
            // C1 round-3 advisory fold (2026-05-28): static-init is
            // thread-safe (C++11) but the rebuild-on-data_path-change
            // block is not atomic. Mutex-protect for defence in depth
            // even though LibraryLink documents main-thread-only entry.
            static std::mutex                                  cached_exp_mu;
            static std::unique_ptr<hyperflint::MzvExpansionTable> cached_exp;
            static std::string                                 cached_exp_path;
            {
                std::lock_guard<std::mutex> lk(cached_exp_mu);
                if (!cached_exp || cached_exp_path != data_path) {
                    cached_exp = std::make_unique<hyperflint::MzvExpansionTable>(
                        hyperflint::load_mzv_expansion(data_path));
                    cached_exp_path = data_path;
                }
                slim_exp = cached_exp.get();
            }
        }

        // R20 Route (i): narrow per-call PolyCtx, env-gated for opt-in.
        // Discovers actually-touched MZV symbols by scanning integrand
        // string + transitive closure on reduction graph.  ~3-7x per-term
        // RSS reduction at the cost of ~ms-scale discovery.  Only safe
        // when introduce_al is False (algebraic letters need full pool).
        std::vector<std::string> base_vars;
        std::string expr_str_for_discovery = json_str_field(body, "expr");
        const char* narrow_env = HF_FLAG_NARROW_CTX;
        const bool use_narrow = narrow_env && *narrow_env && narrow_env[0] != '0'
            && !introduce_al && !expr_str_for_discovery.empty()
            && !use_slim;  // slim takes precedence
        if (hyperflint::period_tuples_enabled() && !introduce_al) {
            // Period-tuples Phase 2 (spec 2026-06-04 §2.1): SLIM ctx --
            // kinematic vars only; period content lives structurally in
            // SymMonomial::period_powers via the scratch-ring mint.
            // introduce_al (Wm/Wp) requests fall back to the legacy wide
            // path below (pilot exclusion, spec §2.5).
            base_vars = user_vars;
        } else if (use_slim) {
            base_vars = hyperflint::build_basis_var_list(*slim_exp, user_vars);
        } else if (use_narrow) {
            base_vars = hyperflint::build_narrow_var_list(
                table, user_vars, expr_str_for_discovery);
        } else {
            base_vars = introduce_al
                ? hyperflint::build_full_var_list(table, user_vars)
                : hyperflint::build_mzv_var_list(table, user_vars);
        }
        // Period-tuples Phase 0 Task 0.2: kinematic prefix length for the
        // density census (atoms occupy indices >= user_vars.size()).
        hyperflint::addpf_probe::set_user_var_count(user_vars.size());
        // HF_CTX_PAD_VARS (period-tuples Phase 0 falsifier, 2026-06-04):
        // append N synthetic never-referenced variables to measure the
        // width-attributable share of RSS/wall directly (same fixture,
        // PAD=0 vs PAD=N, diff peak RSS). Values must be byte-identical
        // since the pad vars never appear in any polynomial. Default OFF.
        if (const char* pad_env = std::getenv("HF_CTX_PAD_VARS")) {
            const long npad = std::strtol(pad_env, nullptr, 10);
            for (long i = 0; i < npad; ++i)
                base_vars.push_back("hfpad_" + std::to_string(i));
        }
        // RAII guard: install active expansion for the lifetime of this
        // bridge call. Mint site reads it via get_active_mzv_expansion()
        // as a fallback when no explicit expansion parameter is provided.
        std::unique_ptr<hyperflint::ActiveMzvExpansionScope> exp_scope;
        if (use_slim) {
            exp_scope = std::make_unique<
                hyperflint::ActiveMzvExpansionScope>(slim_exp);
            // PHASE_3 / MF-3: scan the full body for any LHS / out-of-
            // table MZV token before parsing begins. Cheap (~µs against
            // a 3KB JSON). Throws with clear site identification.
            hyperflint::assert_no_lhs_tokens(body, slim_exp,
                                              "hyperflint_sym");
        }

        std::unique_ptr<hyperflint::PolyCtx> ctx_holder;
        std::vector<std::string> vars;
        hyperflint::ShuffleList input;

        std::string f_str    = json_str_field(body, "f");
        std::string expr_str = json_str_field(body, "expr");
        {
            int given = (!expr_str.empty() ? 1 : 0) + (!f_str.empty() ? 1 : 0)
                + (!extract_top_array(body, "wordlist").empty() ? 1 : 0);
            if (given > 1) {
                std::cerr << "hyperflint: warning: multiple input forms "
                             "provided (expr/f/wordlist); expr > f > wordlist "
                             "priority applied\n";
            }
        }

        if (!expr_str.empty()) {
            try {
                auto parsed = hyperflint::convert::parse_expression(
                    expr_str, base_vars);
                hyperflint::Regulator reg = hyperflint::convert::convert_to_hlog_reg_inf(
                    parsed.expr, *parsed.ctx);
                vars = std::move(parsed.augmented_vars);
                ctx_holder = std::move(parsed.ctx);
                input.reserve(reg.size());
                for (auto& t : reg) {
                    input.push_back(hyperflint::ShuffleEntry{
                        std::move(t.coef), std::move(t.key)});
                }
            } catch (const hyperflint::convert::ConvertFailed& e) {
                std::ostringstream o;
                o << "{\"op\":\"hyperflint\",\"failed\":true"
                  << ",\"reason\":\"" << json_escape(e.what()) << "\""
                  << ",\"vars\":[";
                for (size_t i = 0; i < base_vars.size(); ++i) {
                    if (i) o << ",";
                    o << "\"" << json_escape(base_vars[i]) << "\"";
                }
                o << "]}";
                return o.str();
            } catch (const hyperflint::convert::ParseError& e) {
                std::ostringstream o;
                o << "{\"op\":\"hyperflint\",\"failed\":true"
                  << ",\"reason\":\"" << json_escape(e.what()) << "\""
                  << ",\"vars\":[";
                for (size_t i = 0; i < base_vars.size(); ++i) {
                    if (i) o << ",";
                    o << "\"" << json_escape(base_vars[i]) << "\"";
                }
                o << "]}";
                return o.str();
            }
        } else {
            vars = base_vars;
            ctx_holder = std::make_unique<hyperflint::PolyCtx>(vars);
            hyperflint::PolyCtx& ctx_ref = *ctx_holder;
            if (!f_str.empty()) {
                // HF MZV-rewrite C-prep.4 iter-32 F2 parse-boundary
                // safety rail (top-level f_str entry).
                hyperflint::Rat f_r = hyperflint::Rat::parse(ctx_ref, f_str);
                debug_check_parse_idempotent(ctx_ref, f_r,
                                              "main_handler/f_str");
                input.push_back(hyperflint::ShuffleEntry{std::move(f_r), {}});
            } else {
                input = parse_shuffle_list(ctx_ref, body, "wordlist");
            }
        }
        hyperflint::PolyCtx& ctx = *ctx_holder;

        std::vector<size_t> var_indices;
        for (const auto& vi : vars_int) {
            size_t idx = 0;
            for (; idx < vars.size(); ++idx) if (vars[idx] == vi) break;
            var_indices.push_back(idx);
        }

        if (have_ranges) {
            for (size_t k = 0; k < vars_int.size(); ++k) {
                const std::string& from = vars_int_from[k];
                const std::string& to   = vars_int_to[k];
                if (from == "0" && (to == "Infinity" || to == "+Infinity" ||
                                     to == "oo"))
                    continue;
                input = hyperflint::rescale_interval(ctx, input, var_indices[k],
                                                      from, to);
                if (input.empty()) break;
            }
        }

        // DP.3 (divergence policy, 2026-06-03): the intended bare-request
        // default is TRUE, but the flip is BLOCKED on HF-DIVCHECK-PARITY
        // (the scan false-positives on generic multi-pole convergent
        // integrands) -- see bridge/cli/main.cpp and
        // notes/hf_divcheck_parity.md. Flip only after parity.
        bool check_div = false;
        {
            std::regex re("\"check_divergences\"\\s*:\\s*(true|false)");
            std::smatch m;
            if (std::regex_search(body, m, re)) check_div = (m[1] == "true");
        }

        // HF MZV-rewrite C-prep.4 (iter-27): opt-in canonical-emission
        // writer. Default false preserves existing JSON-byte output.
        bool canonical_emission = false;
        {
            std::regex re("\"canonical_emission\"\\s*:\\s*(true|false)");
            std::smatch m;
            if (std::regex_search(body, m, re))
                canonical_emission = (m[1] == "true");
        }

        // DP.3 spectator projection (2026-06-03): user variables that are
        // never integrated (free kinematic parameters) feed the
        // divergence check's fibration-basis zero test. Built from
        // base_vars (the user-supplied "vars" list, BEFORE atom-pool
        // augmentation) so MZV constants / Log2 / minted letters are
        // never projected over. Cheap: only computed when the check is
        // armed; empty when every user var is integrated.
        std::vector<size_t> spectator_idx;
        if (check_div) {
            for (const auto& bv : base_vars) {
                bool is_int = false;
                for (const auto& vi : vars_int)
                    if (vi == bv) { is_int = true; break; }
                if (is_int) continue;
                for (size_t i = 0; i < vars.size(); ++i)
                    if (vars[i] == bv) { spectator_idx.push_back(i); break; }
            }
        }

        try {
            auto _bench_t0 = std::chrono::steady_clock::now();
            hyperflint::RegulatorSym out = hyperflint::hyperflint_sym(
                ctx, input, var_indices, table, introduce_al, check_div,
                spectator_idx);
            auto _bench_t1 = std::chrono::steady_clock::now();
            double compute_s =
                std::chrono::duration<double>(_bench_t1 - _bench_t0).count();
            // Round-19 wide-ctx probe dump.
            if (hyperflint::ctx_probe_enabled()) {
                std::cerr << hyperflint::ctx_probe_dump_and_clear(ctx)
                          << "\n";
            }
            std::ostringstream o;
            o << "{\"op\":\"hyperflint\",\"result\":"
              << (canonical_emission
                      ? emit_regulator_sym_canonical(out)
                      : emit_regulator_sym(out))
              << ",\"timing_compute_s\":" << compute_s
              << ",\"vars\":[";
            for (size_t i = 0; i < vars.size(); ++i) {
                if (i) o << ",";
                o << "\"" << json_escape(vars[i]) << "\"";
            }
            o << "]";
            // Phase 7-vi-b: emit the algebraic-letter table when the
            // caller turned it on.  Always emit (even if empty) so the
            // Mma-side parser has a stable response shape.
            if (introduce_al) {
                o << ",\"algebraic_letters\":" << emit_algebraic_letter_table();
            }
            o << "}";
            return o.str();
        } catch (const hyperflint::NarrowCtxTooNarrow&) {
            // R24 rev 2 / chain 17 — narrow ctx is missing some MZV
            // variable referenced at runtime.  Emit a structured-error
            // JSON so the Mma-side `STHyperFlint` can detect it and
            // re-issue `RunProcess` with `HF_NARROW_CTX=0`
            // (out-of-process retry — no in-process recursion, no
            // setenv MT-unsafety).  R1: cleanup FLINT pools so the
            // failed call doesn't leak per-thread arenas across
            // LibraryLink retries.
            flint_cleanup_master();
            std::ostringstream o;
            o << "{\"op\":\"hyperflint\",\"narrow_ctx_insufficient\":true"
              << ",\"vars\":[";
            for (size_t i = 0; i < vars.size(); ++i) {
                if (i) o << ",";
                o << "\"" << json_escape(vars[i]) << "\"";
            }
            o << "]}";
            return o.str();
        } catch (const hyperflint::HyperFLINTDivergentIntegral& e) {
            // R26 R1 -- release FLINT thread-pool arenas on the failure
            // path so subsequent LibraryLink calls don't see leaked RSS.
            flint_cleanup_master();
            std::ostringstream o;
            o << "{\"op\":\"hyperflint\",\"divergent\":true"
              << ",\"reason\":\"" << json_escape(e.what()) << "\""
              << ",\"vars\":[";
            for (size_t i = 0; i < vars.size(); ++i) {
                if (i) o << ",";
                o << "\"" << json_escape(vars[i]) << "\"";
            }
            o << "]}";
            return o.str();
        } catch (const hyperflint::IntegrationStepFailed&) {
            flint_cleanup_master();  // R26 R1 -- see above.
            std::ostringstream o;
            o << "{\"op\":\"hyperflint\",\"failed\":true,\"vars\":[";
            for (size_t i = 0; i < vars.size(); ++i) {
                if (i) o << ",";
                o << "\"" << json_escape(vars[i]) << "\"";
            }
            o << "]}";
            return o.str();
        }
    } catch (const std::exception& e) {
        flint_cleanup_master();  // R26 R1 -- catch-all failure path.
        return error_json_op("hyperflint", e.what());
    } catch (...) {
        flint_cleanup_master();  // R26 R1 -- unknown-exception failure path.
        return error_json_op("hyperflint", "unknown exception");
    }
}

}  // namespace handlers
}  // namespace hyperflint
