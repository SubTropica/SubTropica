// HF basis-ctx campaign — eager-expansion MZV table implementation.
//
// PHASE_1 scope (iter 4): loader + load-time invariants. The production
// table is empirically flat, so every RHS is parseable directly against
// basis_ctx; no recursive substitution is exercised by the production
// table. The chained-rule recursive path lands in iter 5 alongside the
// synthetic test fixtures.
//
// Design memo: notes/hf_mzv_weight_cap_2026-05-28/design.md (v2.1).
// Round-2 binding R-4: Rat-level substitution (not textual). Iter 4 ships
// a placeholder for the chained-rule path that throws "not yet
// implemented"; iter 5 fills it in with the substitute_var_rat +
// cross_ctx_transfer_rat machinery and the adversarial fixture test.

#include "hyperflint/reduce/mzv_expansion.hpp"

#include "hyperflint/reduce/mzv_reduce.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <regex>
#include <stdexcept>
#include <unordered_set>

namespace hyperflint {

// ---------------- weight_of_mzv_name ----------------

int weight_of_mzv_name(const std::string& name) {
    if (name == "Log2") return 1;
    if (name.size() < 5 || name.compare(0, 4, "mzv_") != 0) return -1;
    int w = 0;
    size_t i = 4;
    const size_t n = name.size();
    while (i < n) {
        // Optional 'm' prefix for negative index
        if (name[i] == 'm') ++i;
        // Must have at least one digit
        if (i >= n || !std::isdigit(static_cast<unsigned char>(name[i]))) {
            return -1;
        }
        int v = 0;
        while (i < n && std::isdigit(static_cast<unsigned char>(name[i]))) {
            v = 10 * v + (name[i] - '0');
            ++i;
        }
        w += v;
        if (i == n) break;
        if (name[i] != '_') return -1;
        ++i;
    }
    return w;
}

// ---------------- looks_like_mzv ----------------

bool looks_like_mzv(const std::string& tok) {
    if (tok == "Log2") return true;
    // Reuse weight_of_mzv_name's grammar; >=1 iff valid mzv_<seq>.
    return weight_of_mzv_name(tok) >= 1;
}

// ---------------- tokens_in ----------------

std::vector<std::string> tokens_in(const std::string& expr) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    const size_t n = expr.size();
    size_t i = 0;
    while (i < n) {
        char c = expr[i];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i + 1;
            while (j < n) {
                char d = expr[j];
                if (std::isalnum(static_cast<unsigned char>(d)) || d == '_') {
                    ++j;
                } else {
                    break;
                }
            }
            std::string tok = expr.substr(i, j - i);
            if (seen.insert(tok).second) {
                out.push_back(std::move(tok));
            }
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

// ---------------- build_basis_var_list ----------------

std::vector<std::string> build_basis_var_list(
    const MzvExpansionTable& exp,
    const std::vector<std::string>& user_vars) {
    std::unordered_set<std::string> seen(user_vars.begin(), user_vars.end());
    std::vector<std::string> out = user_vars;
    // Append basis CONTIGUOUSLY at the tail; preserves the basis-block
    // contiguity invariant required by cross_ctx_transfer_rat (design
    // §5.3 A-1).
    for (const auto& b : exp.basis_names) {
        if (seen.insert(b).second) {
            out.push_back(b);
        }
    }
    return out;
}

// ---------------- active expansion (PHASE_2 iter 10) ----------------

namespace {
// Process-global active expansion pointer; see design §5.3 + mzv_expansion.hpp.
// std::atomic for single-load semantics from OMP workers. The pointer is
// set once at bridge entry (main thread) and read concurrently from OMP
// parallel regions; never modified mid-call.
std::atomic<const MzvExpansionTable*> g_active_expansion{nullptr};
}  // namespace

const MzvExpansionTable* get_active_mzv_expansion() {
    return g_active_expansion.load(std::memory_order_acquire);
}

void set_active_mzv_expansion(const MzvExpansionTable* exp) {
    g_active_expansion.store(exp, std::memory_order_release);
}

// ---------------- assert_no_lhs_tokens (PHASE_3 / MF-3) ----------------

void assert_no_lhs_tokens(const std::string& payload,
                           const MzvExpansionTable* exp,
                           const std::string& site_name) {
    if (exp == nullptr) return;  // legacy callers without slim ctx active

    // Regex pattern from design.md v2.1 §5.5.
    //   \b                       identifier boundary
    //   mzv_                     literal prefix
    //   (?:m?\d+)                first index (optional 'm' for negative)
    //   (?:_m?\d+)*              additional underscore-separated indices
    //   | Log2                   alternation: also accept Log2
    //   \b                       identifier boundary
    static const std::regex mzv_pat(R"(\b(mzv_(?:m?\d+)(?:_m?\d+)*|Log2)\b)");

    auto begin = std::sregex_iterator(payload.begin(), payload.end(), mzv_pat);
    auto end   = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string tok = (*it)[1].str();
        // Branch A: basis name — OK.
        if (exp->basis_idx.count(tok)) continue;
        // Branch B: reducible LHS — REJECT.
        if (exp->expansion.count(tok)) {
            throw std::runtime_error(
                "Bridge input scanner (" + site_name +
                "): MZV reducible-LHS symbol '" + tok +
                "' in payload; slim-ctx world rejects bridge-side LHS "
                "(SubTropica payloads do not emit MZV LHS). "
                "If your workflow needs this, re-enable the deferred "
                "string preprocessor per design.md v2.1 §5.5.");
        }
        // Branch C: mzv_*-shaped but unknown — REJECT as out-of-table.
        throw std::runtime_error(
            "Bridge input scanner (" + site_name +
            "): unknown MZV-like token '" + tok +
            "' (not in basis, not in expansion table). Out-of-scope "
            "for the current MZV reduction table coverage.");
    }
}

// ---------------- cross_ctx_transfer_rat ----------------

Rat cross_ctx_transfer_rat(const Rat& src, const PolyCtx& dst_ctx) {
    // Fast path: zero Rat.
    if (src.is_zero()) {
        return Rat::zero_of(dst_ctx);
    }
    // Canonical-string round-trip. fmpq_mpoly_get_str_pretty produces
    // a string using src_ctx's variable NAMES (not indices); parsing
    // against dst_ctx resolves names via dst_ctx's name->index map.
    // Any name in the src string that isn't present in dst_ctx will
    // surface as a parse failure (FLINT throws via Rat::parse's
    // exception path).
    const std::string num_str = src.num().to_string();
    const std::string den_str = src.den().to_string();
    // Build the rational expression. Outer parens defend against unary-
    // minus or operator-precedence corner cases at the parser layer
    // (same R-4 hygiene as the design memo applies to any textual
    // round-trip).
    const std::string expr = "(" + num_str + ")/(" + den_str + ")";
    try {
        return Rat::parse(dst_ctx, expr);
    } catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("cross_ctx_transfer_rat: parse failure against ") +
            "dst_ctx; src expression '" + expr + "'; cause: " + e.what());
    }
}

// ---------------- load_mzv_expansion ----------------

namespace {

// Build the basis_ctx PolyCtx over the basis names in canonical order
// (matching the JSON's "basis" array, which is itself weight-sorted on
// the production table). The basis is expected to include "Log2".
std::shared_ptr<PolyCtx> build_basis_ctx(
    const std::vector<std::string>& basis_names) {
    return std::make_shared<PolyCtx>(basis_names);
}

// Return the basis names, normalised so "Log2" is at index 0 (if present
// anywhere in the source list). The JSON's "basis" array typically lists
// Log2 first already; this normalisation is defensive.
std::vector<std::string> canonicalise_basis_names(
    const std::vector<std::string>& src) {
    std::vector<std::string> out;
    out.reserve(src.size());
    // Log2 first.
    bool has_log2 = false;
    for (const auto& b : src) {
        if (b == "Log2") { has_log2 = true; break; }
    }
    if (has_log2) out.push_back("Log2");
    // Then everything else in source order, skipping Log2 if it appears.
    for (const auto& b : src) {
        if (b == "Log2") continue;
        out.push_back(b);
    }
    return out;
}

}  // namespace

MzvExpansionTable load_mzv_expansion(const std::string& json_path,
                                      bool allow_chained) {
    // Phase 1: load the raw rule table via the existing parser.
    MzvReductionTable raw = load_mzv_reductions(json_path);

    // Phase 1.5: sort rules weight-ascending so chained references are
    // expanded in topological order. The production table is already
    // weight-ordered (one trivial inversion); the sort is a no-op for it
    // but is required for the chained-rule test fixtures where the
    // author may list rules in any order.
    std::stable_sort(raw.reductions.begin(), raw.reductions.end(),
                     [](const MzvReductionRule& a, const MzvReductionRule& b) {
                         return weight_of_mzv_name(a.lhs) <
                                weight_of_mzv_name(b.lhs);
                     });

    MzvExpansionTable out;

    // Phase 2: canonicalise basis names + build the basis PolyCtx.
    out.basis_names = canonicalise_basis_names(raw.basis);
    if (out.basis_names.empty()) {
        throw std::runtime_error(
            "load_mzv_expansion: empty basis list parsed from " + json_path);
    }
    out.basis_ctx = build_basis_ctx(out.basis_names);
    for (size_t i = 0; i < out.basis_names.size(); ++i) {
        out.basis_idx.emplace(out.basis_names[i], i);
    }

    // Phase 3: build LHS-name set for assertions.
    std::unordered_set<std::string> lhs_set;
    lhs_set.reserve(raw.reductions.size());
    for (const auto& rule : raw.reductions) {
        lhs_set.insert(rule.lhs);
    }

    // Phase 4: load-time invariants (strict-decrease + flatness +
    // A-9-tightened unknown-token rejection). Walk every rule's RHS,
    // tokenise, and check each token against the three categories.
    bool any_chained = false;
    for (const auto& rule : raw.reductions) {
        const int w_lhs = weight_of_mzv_name(rule.lhs);
        if (w_lhs < 0) {
            throw std::runtime_error(
                "load_mzv_expansion: rule LHS '" + rule.lhs +
                "' is not a valid mzv_* name");
        }
        for (const auto& tok : tokens_in(rule.rhs)) {
            // Branch 1: basis name (leaf in the substitution tree). OK at
            // any weight relationship; basis symbols don't recurse.
            if (out.basis_idx.count(tok)) continue;
            // Branch 2: another rule's LHS (chained reference). Must
            // have weight strictly less than w_lhs.
            if (lhs_set.count(tok)) {
                any_chained = true;
                const int w_tok = weight_of_mzv_name(tok);
                if (w_tok >= w_lhs) {
                    throw std::runtime_error(
                        "load_mzv_expansion: rule '" + rule.lhs +
                        "' (weight " + std::to_string(w_lhs) +
                        ") RHS references LHS '" + tok +
                        "' (weight " + std::to_string(w_tok) +
                        "); strict-decrease invariant violated");
                }
                continue;
            }
            // Branch 3 (A-9): mzv_*-shaped token that is neither basis
            // nor LHS. Fail loudly; almost certainly a typo or table
            // corruption.
            if (looks_like_mzv(tok)) {
                throw std::runtime_error(
                    "load_mzv_expansion: rule '" + rule.lhs +
                    "' RHS contains unknown MZV-shaped token '" + tok +
                    "' (neither basis nor LHS); table corruption or typo");
            }
            // Otherwise: arbitrary identifier (rare; should not occur in
            // a well-formed MZV table). OK in principle; let downstream
            // Rat::parse decide if it's valid.
        }
    }

    // Phase 5 (MF-2(i) production flatness invariant): if the table
    // contains chained rules and the caller didn't opt-in, refuse to
    // proceed. This catches upstream regeneration that silently switches
    // table semantics.
    if (any_chained && !allow_chained) {
        throw std::runtime_error(
            "load_mzv_expansion: table " + json_path +
            " contains chained rules (RHS references another LHS) but "
            "allow_chained=false (production-flatness invariant). The "
            "production HyperFLINT/data/mzv_reductions.json is empirically "
            "flat; pass allow_chained=true ONLY for synthetic test "
            "fixtures designed to exercise the recursive expansion path.");
    }

    // Phase 6: expand every rule into a basis-ctx Rat.
    //
    // Two paths, dispatched per-rule on whether the RHS references any
    // other LHS:
    //
    // (A) FLAT RHS (the only path the production table takes today): RHS
    //     mentions only basis names + Q-coefficients. Parse directly
    //     against basis_ctx. RHS=="0" sentinel for divergent ζ(1,…,1)
    //     short-circuits to Rat::zero_of(basis_ctx) per R-1.
    //
    // (B) CHAINED RHS (synthetic test fixtures and any future production
    //     table that becomes non-flat): RHS mentions previously-expanded
    //     LHS tokens. Build a temporary work_ctx = basis_names + every
    //     LHS token appearing in this rule's RHS (in source order).
    //     Parse rule.rhs as a Rat against work_ctx, then for each LHS
    //     token substitute its already-expanded basis Rat (transferred
    //     into work_ctx via cross_ctx_transfer_rat) via
    //     substitute_var_rat. After all substitutions, every LHS-named
    //     var has zero exponent; project back to basis_ctx via
    //     cross_ctx_transfer_rat. THIS IS THE R-4 RAT-LEVEL PATH:
    //     textual substitution is rejected; we use exact Rat algebra
    //     throughout, so operator precedence and unary-minus are
    //     handled by FLINT, not by string manipulation.
    //
    // Weight-ascending iteration (sort done in phase 2.5 below) ensures
    // every LHS token in any given RHS has already been expanded by the
    // time we reach this rule. Termination is guaranteed by the
    // strict-decrease invariant enforced in phase 4.
    for (const auto& rule : raw.reductions) {
        // RHS=="0" sentinel for divergent ζ(1,…,1) rules.
        if (rule.rhs == "0") {
            out.expansion.emplace(rule.lhs, Rat::zero_of(*out.basis_ctx));
            continue;
        }

        // Scan RHS tokens to determine path (A) vs (B).
        std::vector<std::string> lhs_refs_in_rhs;
        for (const auto& tok : tokens_in(rule.rhs)) {
            if (lhs_set.count(tok) && !out.basis_idx.count(tok)) {
                lhs_refs_in_rhs.push_back(tok);
            }
        }

        if (lhs_refs_in_rhs.empty()) {
            // Path (A): flat RHS; parse directly against basis_ctx.
            out.expansion.emplace(rule.lhs,
                                  Rat::parse(*out.basis_ctx, rule.rhs));
            continue;
        }

        // Path (B): chained RHS. R-4 Rat-level substitution.
        std::vector<std::string> work_vars = out.basis_names;
        for (const auto& lhs_tok : lhs_refs_in_rhs) {
            work_vars.push_back(lhs_tok);
        }
        PolyCtx work_ctx(work_vars);

        Rat rhs_rat = Rat::parse(work_ctx, rule.rhs);

        for (const auto& lhs_tok : lhs_refs_in_rhs) {
            const size_t lhs_idx = work_ctx.index_of(lhs_tok);
            if (lhs_idx == SIZE_MAX) {
                throw std::runtime_error(
                    "load_mzv_expansion: internal error — LHS token '" +
                    lhs_tok + "' lost from work_ctx for rule '" +
                    rule.lhs + "'");
            }
            // Previously-expanded basis Rat lives in basis_ctx; project
            // into work_ctx (which contains basis_names as a subset).
            auto exp_it = out.expansion.find(lhs_tok);
            if (exp_it == out.expansion.end()) {
                throw std::runtime_error(
                    "load_mzv_expansion: rule '" + rule.lhs +
                    "' references LHS '" + lhs_tok +
                    "' which has not been expanded yet "
                    "(weight-ascending sort missing or strict-decrease "
                    "invariant broken)");
            }
            Rat repl_in_work_ctx = cross_ctx_transfer_rat(exp_it->second,
                                                           work_ctx);
            rhs_rat = substitute_var_rat(work_ctx, rhs_rat,
                                          lhs_idx, repl_in_work_ctx);
        }

        // After all substitutions, every LHS-named var has zero
        // exponent. Project back to basis_ctx (narrowing transfer).
        // cross_ctx_transfer_rat will throw if any LHS-named var
        // survives with nonzero exponent (parse failure against
        // basis_ctx, which lacks those names).
        Rat expanded_basis = cross_ctx_transfer_rat(rhs_rat, *out.basis_ctx);
        out.expansion.emplace(rule.lhs, std::move(expanded_basis));
    }

    return out;
}

}  // namespace hyperflint
