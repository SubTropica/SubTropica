// Phase 6a: MZV reduction implementation.

#include "hyperflint/reduce/mzv_reduce.hpp"

#include "hyperflint/core/poly.hpp"
#include "hyperflint/runtime/narrow_ctx_flag.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace hyperflint {

namespace {

// --- Minimal JSON extraction matching the schema written by
// scripts/gen_mzv_reductions.py. Flat enough that we don't need a
// full parser.

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("load_mzv_reductions: cannot open " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

MzvReductionTable load_mzv_reductions(const std::string& json_path) {
    std::string text = read_file(json_path);

    MzvReductionTable out;

    // Parse "reductions": [ { "lhs": "...", "rhs": "..." }, ... ]
    std::regex re_reduction(
        "\\{\\s*\"lhs\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*"
        "\"rhs\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"\\s*\\}");
    auto begin = std::sregex_iterator(text.begin(), text.end(), re_reduction);
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        std::string lhs = (*it)[1];
        std::string rhs = (*it)[2];
        // Unescape any \"
        std::string unesc;
        unesc.reserve(rhs.size());
        for (size_t i = 0; i < rhs.size(); ++i) {
            if (rhs[i] == '\\' && i + 1 < rhs.size()) {
                unesc += rhs[++i];
            } else {
                unesc += rhs[i];
            }
        }
        out.reductions.push_back(MzvReductionRule{lhs, unesc});
    }

    // Parse "basis": [ "...", "...", ... ]
    std::regex re_basis_block("\"basis\"\\s*:\\s*\\[([^\\]]*)\\]");
    std::smatch m;
    if (std::regex_search(text, m, re_basis_block)) {
        std::string inner = m[1];
        std::regex re_name("\"([^\"]+)\"");
        for (auto it = std::sregex_iterator(inner.begin(), inner.end(), re_name);
             it != std::sregex_iterator(); ++it) {
            out.basis.push_back((*it)[1]);
        }
    }

    if (out.reductions.empty()) {
        throw std::runtime_error(
            "load_mzv_reductions: no reductions parsed from " + json_path);
    }
    return out;
}

Rat substitute_var_rat(const PolyCtx& ctx,
                       const Rat& r,
                       size_t var_idx,
                       const Rat& replacement) {
    // For each Poly p(var, others) of degree D in var, the substitution
    // is sum_{k=0..D} c_k(others) * replacement^k.  Both num and den
    // of `r` are substituted this way, yielding Rats; the final result
    // is num'/den'.
    auto sub_poly = [&](const Poly& p) -> Rat {
        long max_deg = p.degree_in_var(var_idx);
        if (max_deg < 0) return Rat::zero_of(ctx);
        long min_deg = p.min_exponent_in_var(var_idx);
        Rat acc{Poly::zero_of(ctx)};
        for (long k = min_deg; k <= max_deg; ++k) {
            Poly ck = p.coefficient_of_var(var_idx, k);
            if (ck.is_zero()) continue;
            Rat term{ck};
            if (k > 0) {
                term = term * replacement.pow(k);
            } else if (k < 0) {
                term = term / replacement.pow(-k);
            }
            acc = acc + term;
        }
        return acc;
    };
    Rat num_r = sub_poly(r.num());
    Rat den_r = sub_poly(r.den());
    return num_r / den_r;
}

namespace {

// Shared cache of previously-parsed RHS strings, keyed by
// (ctx*, rhs_str*). Both keys are pointer-stable: PolyCtx and
// MzvReductionTable live for the lifetime of a run and are not
// resized/reallocated during a call. Moved from thread_local to
// shared+mutex as part of the OpenMP integration-step plan:
// thread_local fragmented hit rate across parallel workers.
// unordered_map inserts don't invalidate references to existing
// values, but iterators do — so we only ever dereference under
// the lock.
struct RhsCacheKey {
    const PolyCtx*    ctx;
    const std::string* rhs;
    bool operator==(const RhsCacheKey& o) const {
        return ctx == o.ctx && rhs == o.rhs;
    }
};
struct RhsCacheKeyHash {
    size_t operator()(const RhsCacheKey& k) const noexcept {
        return std::hash<const void*>{}(k.ctx) ^
               (std::hash<const void*>{}(k.rhs) << 1);
    }
};
std::mutex g_rhs_mu;
std::unordered_map<RhsCacheKey, Rat, RhsCacheKeyHash> g_rhs_cache;

// Returns by value (copy). The alternative — returning a reference
// into the map — would be unsafe because a later insert by another
// thread could rehash and invalidate it. Rat copy is cheap
// (two same-ctx fmpq_mpoly_set calls).
//
// Tolerance branch (R24 rev 2 / chain 17, gated on
// `tolerance_enabled()` ⇔ HF_PARSE_TOLERANT=1): wraps the parse in
// `Rat::parse_or_none`.  On `nullopt` (parse failed because some
// referenced var is absent from the narrow ctx), set the global
// `g_narrow_ctx_too_narrow` flag and return a `Rat::zero_of(ctx)`
// safe-failure placeholder.  Do NOT cache the failure: a future
// retry with a wider ctx must not pick the zero out of the cache.
//
// The downstream `substitute_var_rat(ctx, current, rule.var_idx,
// Rat::zero_of)` may then call `Rat::div` with a zero denominator
// (when `current.den()` contained the LHS variable as a factor),
// which `Rat::div` reports as `std::runtime_error("Rat::div:
// division by zero")`.  Inside an OMP region that escape converts
// to `std::terminate` — this is why R24v2 also adds an OMP-region
// iteration-body try/catch wrapper (chain 17 step 3) at every
// parallel-for that may transitively call `parse_rhs_cached`.
Rat parse_rhs_cached(const PolyCtx& ctx, const std::string* rhs) {
    RhsCacheKey key{&ctx, rhs};
    {
        std::lock_guard<std::mutex> lk(g_rhs_mu);
        auto it = g_rhs_cache.find(key);
        if (it != g_rhs_cache.end()) return it->second;
    }
    // Parse outside the lock (expensive at wide ctx).
    if (tolerance_enabled()) {
        std::optional<Rat> parsed = Rat::parse_or_none(ctx, *rhs);
        if (!parsed.has_value()) {
            g_narrow_ctx_too_narrow.store(true,
                                            std::memory_order_relaxed);
            // Safe-failure placeholder: caller's substitution with
            // zero may div-by-zero downstream, caught by the
            // OMP-iteration-body try/catch.  Do NOT cache.
            return Rat::zero_of(ctx);
        }
        std::lock_guard<std::mutex> lk(g_rhs_mu);
        return g_rhs_cache.emplace(key, std::move(*parsed))
            .first->second;
    }
    Rat parsed = Rat::parse(ctx, *rhs);
    std::lock_guard<std::mutex> lk(g_rhs_mu);
    // Re-check: another thread may have populated the entry while we
    // were parsing. emplace leaves the existing entry untouched and
    // returns the iterator to whatever's there, so the parse we did
    // is harmlessly discarded if we lost the race.
    return g_rhs_cache.emplace(key, std::move(parsed)).first->second;
}

}  // namespace

void clear_rhs_cache() {
    // Drop every cached (ctx*, rhs*) → Rat entry.  Use case: chain-17+
    // wires this into hyperflint_sym handler entry to defuse the
    // pointer-reuse latent UB R25 flagged.  See header for full
    // rationale.
    std::lock_guard<std::mutex> lk(g_rhs_mu);
    g_rhs_cache.clear();
}

// HF basis-ctx campaign (PHASE_4, 2026-05-28): per-(ctx*, table*) cache
// of the "ctx contains no LHS from table" predicate. When true, the
// outer apply_mzv_reductions function is a true no-op — the ctx
// cannot host any LHS variable's nonzero exponent, so no rule can
// fire. Cache is amortised per bridge call per N-1 clarification
// (design.md v2.1 §5.4): one cold scan per fresh (ctx, table) pair;
// subsequent calls within the same pair hit in O(1). Mutex-protected
// because apply_mzv_reductions is called from within OMP regions in
// the integrator.
namespace {
std::mutex                                                g_ctx_lhs_mu;
std::unordered_map<const PolyCtx*,
                   std::unordered_map<const MzvReductionTable*, bool>>
                                                          g_ctx_has_no_lhs;

bool ctx_has_no_lhs_cached(const PolyCtx& ctx,
                            const MzvReductionTable& table) {
    {
        std::lock_guard<std::mutex> lk(g_ctx_lhs_mu);
        auto cit = g_ctx_has_no_lhs.find(&ctx);
        if (cit != g_ctx_has_no_lhs.end()) {
            auto tit = cit->second.find(&table);
            if (tit != cit->second.end()) return tit->second;
        }
    }
    // Cold path: build a hash set of LHS names + scan ctx.vars()
    // (O(|table.reductions| + |ctx.vars()|) once per pair).
    std::unordered_set<std::string> lhs_set;
    lhs_set.reserve(table.reductions.size());
    for (const auto& rule : table.reductions) lhs_set.insert(rule.lhs);
    bool none = true;
    for (const auto& v : ctx.vars()) {
        if (lhs_set.count(v)) { none = false; break; }
    }
    {
        std::lock_guard<std::mutex> lk(g_ctx_lhs_mu);
        g_ctx_has_no_lhs[&ctx][&table] = none;
    }
    return none;
}
}  // namespace

// PHASE_4 round-3 BLOCKER fix (2026-05-28): clear the per-(ctx*, table*)
// cache. Same hazard model as clear_rhs_cache. Must be called from
// every bridge handler entry that may follow a prior call within the
// same process — see handlers.cpp:1014-1027 commentary on the
// PolyCtx address-reuse hazard documented for the R24-rev2/chain-17
// pattern.
void clear_ctx_has_no_lhs_cache() {
    std::lock_guard<std::mutex> lk(g_ctx_lhs_mu);
    g_ctx_has_no_lhs.clear();
}

Rat apply_mzv_reductions(const PolyCtx& ctx,
                          const MzvReductionTable& table,
                          const Rat& r) {
    // HF basis-ctx campaign no-op guard (PHASE_4 / design §5.4):
    // when ctx has no LHS variable from table, the input Rat cannot
    // mention any LHS variable's nonzero exponent, so no reduction
    // is possible. Return identity. This is the path taken on every
    // slim-ctx call (HF_USE_BASIS_CTX=1) and on any legacy call
    // whose ctx happens to omit the LHS pool. Predicate cache is
    // amortised per (ctx*, table*) pair (N-1 clarification).
    if (ctx_has_no_lhs_cached(ctx, table)) return r;

    // Legacy wide-ctx path (algebraic-letters call sites and any
    // pre-PHASE_2 caller). Index reducible-var positions in `ctx`
    // (O(|vars|·|rules|) once per call, but the dominant cost in a
    // naive impl was parsing the 700 RHS strings into Rats every
    // time — `Rat::parse` routes through FLINT's
    // fmpq_mpoly_set_str_pretty which does a full polynomial parse
    // per call and, at 711 ctx variables, costs several ms each.
    // Profiling tst0 showed `apply_mzv_reductions` at ~16% of total
    // runtime, half of it in unused RHS parses. Defer parsing: only
    // parse a rule's RHS when we've established that its LHS
    // variable actually appears in `current`. Most inputs mention
    // only a handful of mzv_* symbols, so most rules fall through
    // at the is_zero-derivative check.
    const auto& vars = ctx.vars();

    struct Rule {
        size_t      var_idx;
        const std::string* rhs_str;   // not yet parsed
    };
    std::vector<Rule> active;
    active.reserve(table.reductions.size());

    std::unordered_map<std::string, size_t> var_idx;
    var_idx.reserve(vars.size());
    for (size_t i = 0; i < vars.size(); ++i) var_idx.emplace(vars[i], i);

    for (const auto& rule : table.reductions) {
        auto it = var_idx.find(rule.lhs);
        if (it == var_idx.end()) continue;
        active.push_back(Rule{it->second, &rule.rhs});
    }

    // Presence check: `Poly::degree_in_var(v) > 0` is true iff `v`
    // appears with a positive exponent in any monomial. This is O(nt)
    // via `fmpq_mpoly_degree_si` vs O(nt * nv) for the previous
    // `derivative(v).is_zero()` check — on tst0 the derivative path
    // was ~3% of total runtime all by itself.
    auto uses_var = [&](const Rat& rat, size_t v) {
        return rat.num().degree_in_var(static_cast<long>(v)) > 0 ||
               rat.den().degree_in_var(static_cast<long>(v)) > 0;
    };

    Rat current = r;
    for (int pass = 0; pass < 50; ++pass) {
        bool changed = false;
        for (const auto& rule : active) {
            if (!uses_var(current, rule.var_idx)) continue;
            // LHS var is present — parse the RHS now. Cache the parsed
            // Rat across calls: the table is stable and the ctx is
            // stable across a single integration run, so keying by
            // (ctx*, rhs_str*) gives a safe hit rate. Without this
            // cache, a rule that fires across many `apply_mzv_reductions`
            // calls re-parses its wide-ctx RHS every time — at 711
            // variables each parse costs several ms.
            Rat replacement = parse_rhs_cached(ctx, rule.rhs_str);
            current = substitute_var_rat(ctx, current, rule.var_idx,
                                          replacement);
            changed = true;
        }
        if (!changed) return current;
    }
    throw std::runtime_error(
        "apply_mzv_reductions: substitution didn't terminate in 50 passes");
}

std::vector<std::string> build_mzv_var_list(
    const MzvReductionTable& table,
    const std::vector<std::string>& user_vars) {
    std::unordered_set<std::string> seen(user_vars.begin(), user_vars.end());
    std::vector<std::string> out = user_vars;
    auto add = [&](const std::string& name) {
        if (seen.insert(name).second) out.push_back(name);
    };
    add("Log2");
    for (const auto& b : table.basis) add(b);
    for (const auto& r : table.reductions) add(r.lhs);
    return out;
}

std::vector<std::string> build_narrow_var_list(
    const MzvReductionTable& table,
    const std::vector<std::string>& user_vars,
    const std::string& integrand_str) {
    // R20 + empirical refinement: the integrator mints basis MZVs
    // (e.g. mzv_2 = zeta(2)) at runtime via in-thread Rat::parse calls;
    // those exceptions cross OMP boundaries and abort the process,
    // so we cannot rely on integrand-string discovery alone.
    // Conservative narrow: include the full basis (always live as
    // alphabet) but exclude reductions LHS (transient — replaced by
    // RHS as soon as introduced).  ~1.6x per-term storage reduction
    // vs build_full_var_list, low risk.
    // 1. Build the universe of MZV-related symbol names from the table.
    std::unordered_set<std::string> mzv_universe;
    mzv_universe.insert("Log2");
    for (const auto& b : table.basis) mzv_universe.insert(b);
    for (const auto& r : table.reductions) mzv_universe.insert(r.lhs);

    // 2. Map LHS -> rule index for fast lookup.
    std::unordered_map<std::string, size_t> lhs_to_rule;
    lhs_to_rule.reserve(table.reductions.size());
    for (size_t i = 0; i < table.reductions.size(); ++i) {
        lhs_to_rule.emplace(table.reductions[i].lhs, i);
    }

    // 3. Token extractor: pull C-style identifiers from a string and
    //    intersect with mzv_universe (cheap, no regex compile).
    auto add_words = [&](const std::string& s,
                         std::unordered_set<std::string>& seen,
                         std::vector<std::string>& worklist) {
        const size_t n = s.size();
        size_t i = 0;
        while (i < n) {
            char c = s[i];
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t j = i + 1;
                while (j < n) {
                    char d = s[j];
                    if (std::isalnum(static_cast<unsigned char>(d)) || d == '_') {
                        ++j;
                    } else break;
                }
                std::string w = s.substr(i, j - i);
                if (mzv_universe.count(w) && seen.insert(w).second) {
                    worklist.push_back(w);
                }
                i = j;
            } else {
                ++i;
            }
        }
    };

    // 4. Initialise seen with user_vars + Log2; scan integrand for
    //    MZV-symbol mentions; then transitively close over the
    //    reduction graph (rules whose LHS is in seen contribute their
    //    RHS-referenced vars).
    std::unordered_set<std::string> seen(user_vars.begin(), user_vars.end());
    seen.insert("Log2");
    // Conservative: include all basis elements unconditionally — they
    // are the integrator's working alphabet and may be introduced via
    // in-OMP Rat::parse calls that no caller-side try/catch can
    // intercept.
    for (const auto& b : table.basis) seen.insert(b);
    std::vector<std::string> worklist;
    add_words(integrand_str, seen, worklist);

    // Empty-seed safety fallback (2026-05-28 abort fix).
    //
    // Integrand-string discovery is only a valid signal for the minted
    // MZV set when the integrand literally names the symbols it will
    // use (pure-MZV-arithmetic requests). For an *integration* request
    // the minted symbols (e.g. mzv_1_2) are produced internally by the
    // path-to-MZV encoding of the integration words, NOT present in the
    // integrand string. In that case the scan above finds nothing, the
    // narrow ctx omits the reduction-LHS placeholders, and the
    // integrator's later `Rat::parse(ctx, "mzv_1_2")` throws
    // `Poly: parse error: mzv_1_2`, which escapes the OMP region as
    // `std::terminate` (SIGABRT). This was a shipped foot-gun on every
    // Smirnov-shaped fixture (integrand contains zero MZV tokens).
    //
    // When discovery yields an empty MZV seed we cannot narrow safely,
    // so fall back to the full var list. This preserves the narrow win
    // on inputs that DO name their MZVs (the pentagon-gauge case) and
    // is safe-by-construction everywhere else. Note: this does not make
    // narrow-ctx provably sound for the *partial* case (integrand names
    // some MZVs but the integration mints others not reachable from
    // them); HF_NARROW_CTX remains advisory, recommended only for
    // pure-MZV-arithmetic inputs.
    if (worklist.empty()) {
        return build_mzv_var_list(table, user_vars);
    }

    while (!worklist.empty()) {
        const std::string current = std::move(worklist.back());
        worklist.pop_back();
        auto it = lhs_to_rule.find(current);
        if (it != lhs_to_rule.end()) {
            const auto& rule = table.reductions[it->second];
            add_words(rule.rhs, seen, worklist);
        }
    }

    // 5. Build output: user_vars first (preserving caller order),
    //    then Log2, then MZV symbols sorted by their position in the
    //    canonical (full) build_mzv_var_list — so per-face caches keyed
    //    by var_idx are cross-face stable when faces touch the same
    //    set.
    std::vector<std::string> out = user_vars;
    std::unordered_set<std::string> in_out(user_vars.begin(), user_vars.end());
    auto append_if_new = [&](const std::string& s) {
        if (in_out.insert(s).second) out.push_back(s);
    };
    if (seen.count("Log2")) append_if_new("Log2");

    std::vector<std::string> full = build_mzv_var_list(table, user_vars);
    std::unordered_map<std::string, size_t> full_idx;
    full_idx.reserve(full.size());
    for (size_t i = 0; i < full.size(); ++i) full_idx[full[i]] = i;
    std::vector<std::string> mzv_extras;
    for (const auto& s : seen) {
        if (s == "Log2") continue;
        if (in_out.count(s)) continue;
        mzv_extras.push_back(s);
    }
    std::sort(mzv_extras.begin(), mzv_extras.end(),
              [&](const std::string& a, const std::string& b) {
                  auto ia = full_idx.find(a);
                  auto ib = full_idx.find(b);
                  size_t va = ia != full_idx.end()
                              ? ia->second : std::numeric_limits<size_t>::max();
                  size_t vb = ib != full_idx.end()
                              ? ib->second : std::numeric_limits<size_t>::max();
                  return va < vb;
              });
    for (auto& s : mzv_extras) append_if_new(s);
    return out;
}

}  // namespace hyperflint
