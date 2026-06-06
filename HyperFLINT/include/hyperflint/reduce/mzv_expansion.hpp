// HF basis-ctx campaign — eager-expansion MZV table.
//
// Loads HyperIntica's mzvAllReductions table and PRE-EXPANDS every LHS
// into a basis-only Rat at load time. The integrator can then mint MZV
// symbols by O(1) hash lookup into a slim PolyCtx (basis + user_vars,
// no LHS variables), eliminating the wide-ctx per-term FLINT cost
// measured at 23.4% of tst2 default-build wall (Phase 0.5 profile).
//
// Design memo: notes/hf_mzv_weight_cap_2026-05-28/design.md (v2.1).
//
// PHASE_1 scope (iter 4): loader scaffolding + load-time invariants
//   (strict-decrease, table-flatness, A-9-tightened unknown-token
//    rejection). The production table is empirically flat (0/700
//    chained), so for iter 4 every RHS is parsed directly against
//    basis_ctx; no Rat-level substitution machinery is exercised.
//   Iter 5 adds cross_ctx_transfer_rat + Rat::substitute_var_rat
//   chains for the synthetic chained-rule test fixtures.

#pragma once

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hyperflint {

// Slim eager-expansion table: every LHS in the source MzvReductionTable
// is pre-expanded into a Rat over a basis-only PolyCtx of width
// |basis|+1 (the +1 is Log2). At mint time in periods.cpp:to_mzv_one_word,
// the lookup expansion[lhs_name] returns the basis Rat directly.
//
// Lifecycle: built ONCE per process (or per test) via load_mzv_expansion;
// shared by all integrator threads as a `const MzvExpansionTable&`. The
// underlying basis_ctx is shared_ptr-held so that the basis-ctx Rats in
// `expansion` remain valid for the lifetime of the table.
struct MzvExpansionTable {
    // Slim PolyCtx containing only basis vars (Log2 + the 9 MZV basis
    // elements). Width = 10 on the current production table.
    std::shared_ptr<PolyCtx> basis_ctx;

    // Canonical basis-variable ordering (matches basis_ctx->vars()).
    // First entry is always "Log2" (weight 1).
    std::vector<std::string> basis_names;

    // The set of basis names as a hash-set for O(1) lookups. Used by
    // load-time assertions and by build_basis_var_list.
    std::unordered_map<std::string, size_t> basis_idx;

    // expansion[lhs_name] = basis-only Rat over basis_ctx. Populated for
    // every rule in the source table. For divergent ζ(1,...,1) rules
    // (RHS == "0"), the value is Rat::zero_of(basis_ctx).
    std::unordered_map<std::string, Rat> expansion;
};

// Load the JSON table and build the eager-expansion table.
//
// Throws on:
//   - missing file or unparseable JSON
//   - load-time strict-decrease violation (a non-basis LHS-named token
//     appears in some RHS at weight >= LHS weight; design.md §3.2)
//   - A-9: an mzv_*-shaped token appears in some RHS that is neither
//     in basis nor an LHS in the table (typo or table corruption)
//   - allow_chained=false (default) and the table contains chained
//     rules (RHS references another LHS); design.md §5.1 MF-2(i)
//     production-table flatness invariant
//   - weight-grading assertion failure on the final expanded polynomial
//
// `allow_chained` defaults to false — the production table at
// HyperFLINT/data/mzv_reductions.json is empirically flat, and a future
// upstream regeneration that introduces chained rules should be rejected
// with a clear error so the regeneration script can be audited. The
// synthetic test fixtures (test/data/mzv_reductions_chained_*.json) set
// allow_chained=true to exercise the recursive expansion code path.
MzvExpansionTable load_mzv_expansion(const std::string& json_path,
                                      bool allow_chained = false);

// Compute the transcendental weight of an MZV/Log2 symbol name.
//   weight("Log2") = 1
//   weight("mzv_<i1>_..._<in>") = sum_k |i_k|   (m prefix => negation)
// Returns -1 for non-matching names.
int weight_of_mzv_name(const std::string& name);

// Test whether a token matches the mzv_* identifier grammar:
//   mzv_(?:m?\d+)(?:_m?\d+)*
// Used by the A-9-tightened load-time assertion to reject unknown
// mzv_-shaped tokens that are neither basis nor LHS.
bool looks_like_mzv(const std::string& tok);

// Extract C-style identifier tokens from a Rat-parseable expression
// string. Returns identifiers in source order (with duplicates removed
// after the first occurrence). Used by the load-time assertions to
// scan each rule's RHS for LHS-name references.
std::vector<std::string> tokens_in(const std::string& expr);

// Build the slim PolyCtx variable list for the integrator:
//   user_vars + basis_names  (basis appended CONTIGUOUSLY at the tail)
//
// The contiguity invariant is required by cross_ctx_transfer_rat
// (design.md §5.3 A-1) so that the basis sub-block has a stable relative
// ordering across different user_vars permutations.
std::vector<std::string> build_basis_var_list(
    const MzvExpansionTable& exp,
    const std::vector<std::string>& user_vars);

// Transfer a Rat from src_ctx into dst_ctx, where every var name in
// src_ctx also appears in dst_ctx (possibly at different indices).
//
// PHASE_1 implementation: canonical-string round-trip
// (`fmpq_mpoly_get_str_pretty` on each src Poly + `Rat::parse` against
// dst_ctx). Correct under any var-index permutation because the
// canonical string uses var NAMES not indices. Cost: O(num_terms ·
// num_vars_dst) per call. Acceptable for loader use (one-time startup);
// PHASE_2 mint-site migration may need a per-term FLINT-level path
// (iterate `fmpq_mpoly_get_term_*`, repack against dst_ctx) for
// hot-path performance — deferred until profiling shows it's needed.
//
// Throws std::runtime_error if dst_ctx is missing a var that appears
// with nonzero exponent in src — i.e., the transfer is non-injective
// on the support.
Rat cross_ctx_transfer_rat(const Rat& src, const PolyCtx& dst_ctx);

// Process-global active expansion table (PHASE_2 iter 10).
//
// To avoid threading `const MzvExpansionTable*` through every layer of
// the integrator (break_up_contour → transform → integration_step →
// hyper_int → ...), the bridge entry point sets a process-global
// pointer that the mint site (to_mzv_one_word) consults when its
// explicit `expansion` parameter is nullptr. OMP semantics are safe:
// the pointer is set once on the main thread at bridge entry, never
// modified during the call, and reset at bridge exit via an RAII
// scope guard. OMP workers read the pointer atomically (single
// pointer-sized load).
//
// Default state: nullptr. When nullptr, the mint site falls through
// arm 2 to arm 3 (legacy Rat::parse) per design §5.3.
const MzvExpansionTable* get_active_mzv_expansion();
void                     set_active_mzv_expansion(const MzvExpansionTable* exp);

// RAII guard: sets the active expansion on construction; restores
// previous value on destruction. Use at bridge entry sites where
// HF_USE_BASIS_CTX=1 selects the slim-ctx code path.
struct ActiveMzvExpansionScope {
    explicit ActiveMzvExpansionScope(const MzvExpansionTable* new_exp)
        : prev_(get_active_mzv_expansion()) {
        set_active_mzv_expansion(new_exp);
    }
    ~ActiveMzvExpansionScope() { set_active_mzv_expansion(prev_); }
    ActiveMzvExpansionScope(const ActiveMzvExpansionScope&) = delete;
    ActiveMzvExpansionScope& operator=(const ActiveMzvExpansionScope&) = delete;
private:
    const MzvExpansionTable* prev_;
};

// PHASE_3 / MF-3: bridge input scanner.
//
// Tokenises `payload` for mzv_* / Log2 identifiers (regex
// \b(mzv_(?:m?\d+)(?:_m?\d+)*|Log2)\b) and validates each match
// against `exp`:
//   - basis name (e.g. mzv_2, Log2): OK (basis var legitimate in
//     slim-ctx world; integrator mints these via arm 1).
//   - reducible LHS (in exp.expansion map): REJECT — slim-ctx world
//     does not accept LHS tokens at bridge layer; SubTropica payloads
//     do not emit MZV LHS (verified by round-2 adversarial). If a
//     future workflow needs this, re-enable the deferred string
//     preprocessor per design §5.5.
//   - mzv_*-shaped but not in basis nor in expansion: REJECT as
//     out-of-table (weight may exceed table coverage, or typo).
//
// Throws std::runtime_error on rejection with a clear message
// identifying the site, token, and rejection reason. Called once
// per bridge invocation at hyperflint_sym entry when
// HF_USE_BASIS_CTX=1. No-op when `exp` is nullptr (legacy callers
// without slim ctx active).
void assert_no_lhs_tokens(const std::string& payload,
                           const MzvExpansionTable* exp,
                           const std::string& site_name);

}  // namespace hyperflint
