// LR-order search: find the variable-integration order that makes an
// Euler integrand linearly reducible.  C++ port of SubTropica's
// STFasterFubini2 (SubTropica.wl:11050-11149) for the single-group,
// FindRoots=False, LeafCountLinear MVP scope.
//
// Phase β.2 scope:
//   - single polynomial group (SubTropica's "groupMembers == 1" case)
//   - Heuristic = LeafCountLinear
//   - FindRoots = False
//   - Proportionality dedup via canonical form (Phase-α.2 semantics,
//     stricter than Mma's PPQ; equivalent over ℚ)
//   - Subsets DP with bitmask-indexed memoization; O(2^n * n) states

#pragma once

#include <limits>   // std::numeric_limits<double>::infinity() for the ScorePruneFactor
                    // default; macOS libc++ pulls it in transitively, Linux libstdc++ does not.

#include "hyperflint/core/poly.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hyperflint {
namespace lr_search {

// Budget safety net (2026-06-20).  The exhaustive default LR search
// (score_prune_factor == +inf) can wedge on a single uninterruptible
// FLINT discriminant/resultant on a huge multivariate letter: 100% of
// the wall is one fmpq_mpoly_resultant FFT poly-mul that ignores Mma
// aborts and SIGTERM (only SIGKILL stops it).  find_lr_orders reads two
// env vars (HF_LR_TIME_BUDGET_S, HF_LR_MAX_OPERAND_TERMS; both default 0
// = UNLIMITED) and throws this typed exception so the exhaustive default
// bails CLEANLY instead of hanging.  Both defaults unset => the budget
// checks are inert and behavior is byte-identical to the pre-change
// engine.  The handler catches it and serializes a DISTINCT failed
// response (budget_exceeded + error), NEVER a NOLR verdict.
struct LrBudgetExceeded : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Per-face kinematic-divisor collector (order-resolved singularities
// pipeline; notes/ristretto/order_resolved_singularities.md, step 2).
// An OPT-IN side channel into the LR walk: when a non-null pointer is
// threaded through find_lr_orders / st_fubini_lr, every IRREDUCIBLE
// factor (of any leading coefficient, discriminant, or pairwise
// resultant produced by the DP) that is FREE OF ALL integration
// variables is canonicalized (via Poly::canonical_prop_form, the same
// proportionality representative the dedup uses) and recorded.  These
// are exactly the kinematic divisors the face can develop a singularity
// on, at ANY degree --- the collector observes factors BEFORE/ASIDE
// from the deg-2 letter cap that bounds the LR VERDICT, and it NEVER
// alters control flow.  A null collector (the default) adds no work and
// leaves the response byte-identical (gate #1).
//
// `integration_vars` is the set of integration-variable ORIGINAL-CTX
// indices (xvar_indices); a factor is "kinematic-only" iff its used-var
// set is disjoint from this set.  `seen` dedups by canonical-form
// string; `ordered` preserves first-encounter order for a stable,
// reproducible response.  No chi-vetting here --- the Mathematica side
// owns vetting/comparison (notes/ristretto, Implementation step 2).
struct SingCollector {
    std::unordered_set<size_t>      integration_vars;
    std::unordered_set<std::string> seen;
    std::vector<std::string>        ordered;

    // Observe an irreducible factor base `b` (already a Poly in the
    // shared PolyCtx).  Records iff `b` is non-numeric and free of every
    // integration variable.  Canonical form = canonical_prop_form (the
    // dedup's proportionality representative), so s - 4*mm and
    // -(s - 4*mm) collapse to one entry.
    void observe(const Poly& b);
};

// Result shape.  `order.empty() && score == +inf` means NOLR (no
// linearly reducible order exists for this input).
//
// `root_polys` (Phase 7-vii): when the search accepted deg-2 steps
// (i.e., `allow_algebraic_letters` was true), this collects the
// distinct deg-2 polynomials that appeared as the "current poly in
// var" at the var-removal step.  These are the polys the integration
// stage will turn into Wm/Wp pairs (HF's `linear_factors` allocates
// AlgebraicLetterTable entries for them).  Mma's STFasterFubini2
// returns the same list as `result[[2]]` when FindRoots=True, and
// downstream STIntegrate consumers (STApplyRootFactoring) need it
// to record where to introduce algebraic letters.  Empty for the
// FindRoots=False / `allow_algebraic_letters=false` path.
struct LrResult {
    std::vector<size_t> order;       // xvar ORIGINAL-CTX indices
    double              score;
    std::vector<Poly>   root_polys;  // deg-2 polys accepted during walk
    // Carried-sqrt profile of the chosen order (carry_discharge tier
    // only; all zero on the Strict / deg<=1 paths).  Mirrors
    // lr_scan::ScanOrder's {carried_sqrts, kin_sqrts, terminal_quads}.
    unsigned long carried_sqrts = 0;
    unsigned long kin_sqrts = 0;
    unsigned long terminal_quads = 0;
    // Distinct carried obligations along best_order, deduped up to
    // proportionality across the whole path (leaf-replay, spec
    // 2026-06-11-carry-phase2 §3.1); size <= carried_sqrts (remints
    // re-count in nsq, enter the ledger once).  Canonical
    // proportionality forms.  Empty on the Strict / deg<=1 paths and
    // whenever carry_discharge is off.
    std::vector<Poly> obligation_polys;
    // false iff a finite score_prune_factor actually discarded subsets
    // during the DP: a NOLR result with search_complete == false is an
    // incomplete search, not a proof that no reducible order exists
    // (issue #52 round 5).  Exhaustive searches (the default) keep true.
    bool search_complete = true;
    bool nolr() const { return order.empty() && score >= 1e300; }
};

// STFubiniLR port: inner polynomial kernel.  Given a list of polys
// (intermediate state for some subset) and a pivot variable, return
// the list of factor bases of {lc(f, v) : f ∈ polys, deg(f,v) >= 0}
// ∪ {disc(f, v) : f ∈ polys, deg(f,v) >= 1}
// ∪ {res(f1, f2, v) : f1, f2 ∈ polys, deg(f1,v) ≥ 1, deg(f2,v) ≥ 1}
// after (a) factoring each via fmpq_mpoly_factor and (b) dedup via
// canonical-form equivalence (proportionality over ℚ).
// `sings` (default nullptr): optional per-face kinematic-divisor
// collector.  When non-null, every irreducible factor produced at this
// step that is free of all integration variables is recorded into it
// (see SingCollector).  Pure side channel: the returned letter list and
// all control flow are identical whether `sings` is null or not.
std::vector<Poly> st_fubini_lr(const std::vector<Poly>& polys, size_t var_idx,
                               SingCollector* sings = nullptr);

// Proportionality dedup: drop numeric / zero polys, group by canonical
// form, return one representative per equivalence class.
std::vector<Poly> dedup_proportional(const std::vector<Poly>& polys);

// Deficiency-3 cure (2026-06-06): reset the per-request step/factor
// memo layers inside st_fubini_lr (value-preserving caches keyed on
// poly strings; see notes/hf_lr_search_deficiencies.md).  Called
// automatically at find_lr_orders entry; external drivers that loop
// st_fubini_lr directly (lr_scan) must call it once per request to
// bound memo memory.  Opt-outs: HF_LR_STEP_MEMO=0 / HF_LR_FACTOR_MEMO=0.
void reset_lr_memos();

// Shared admissibility test for the LR DP and the factor-table builder
// (spec docs/superpowers/specs/2026-06-11-stfactorpredictor-design.md):
// degree 1..max_deg in the pivot, plus the deg-2 forbidden-variable
// guard mirrored from the runtime check in linear_factors.cpp.
// `forbidden_after` lists wide-context variable indices still
// un-integrated AFTER this step; the caller computes it (DP: complement
// of the subset bitmask; factor-table builder: order[k+1..]).  Both
// callers pass the same set on a full permutation, which a ctest
// asserts.  Returns false for pivot-free polys (d < 1): they are not
// letters in the pivot (the DP lets them pass through the step; the
// builder skips them).
bool lr_letter_admissible(const Poly& p, size_t var_idx,
                          const std::vector<size_t>& forbidden_after,
                          long max_deg);

// Reset the HF_LR_TRACE accumulators (g_lr_trace).  find_lr_orders
// does this internally on entry; external drivers of st_fubini_lr
// (factor_table handler) call it alongside reset_lr_memos so stats
// from a prior call in the same process don't bleed.
void reset_lr_trace();

// Reset the budget safety net (g_lr_budget) from the environment
// (HF_LR_TIME_BUDGET_S / HF_LR_MAX_OPERAND_TERMS; both default 0 =
// UNLIMITED).  find_lr_orders and verify_order_is_lr call this on entry
// so the per-call steady_clock deadline starts fresh; external drivers
// of st_fubini_lr (factor_table handler) must call it alongside
// reset_lr_memos / reset_lr_trace so a stale deadline (already in the
// past) from a prior budgeted call in the SAME process cannot throw
// LrBudgetExceeded spuriously.  With both env vars unset it leaves the
// budget inert (no-op checks).
void reset_lr_budget();

// issue #52 round 3 (item 9): budget checkpoints for op bodies OUTSIDE
// st_fubini_lr (lr_scan keep-rules, factor_table build).  Each is a
// no-op when the corresponding env var is unset (results/state identical;
// wall-clock timing fields may differ by the cost of the check itself).
// `where`/`what` land in the thrown LrBudgetExceeded message.  NOTE: no
// time checkpoint is exported into fr_judge -- it is shared with
// find_lr_orders' carry paths, where the loader-defaulted 180 s budget
// would add a live abort point inside the production search (round-3
// review finding 8).  A pair-form (product) helper was considered and
// dropped: every pairwise site is already covered inside st_fubini_lr
// (codex round-3 verification).
void lr_budget_check_time(const char* where);
void lr_budget_check_operand(const char* what, std::size_t n_terms);

// N-way intersection under proportionality equivalence.  Picks
// representatives from the FIRST list.  Matches the semantics of Mma's
// `Intersection[..., SameTest -> ProportionalPolynomialsQLR]` up to
// representative choice (dedup result is the same equivalence set).
std::vector<Poly> intersect_proportional(
    const std::vector<std::vector<Poly>>& lists);

// Proxy for Mma's `LeafCount[list-of-polys]`.  Counts atoms in a
// hypothetical Mma-style tree: for each monomial, 1 for each variable
// with exponent >= 1, +1 for each variable with exponent >= 2, and
// +1 for a non-unit coefficient.  Summed over monomials across all
// polys in the list.  Within ~10% of Mma's `LeafCount` on typical
// Symanzik inputs; the exact value doesn't matter, only the relative
// ranking of candidate orders under MinimalBy does.
long leaf_count_proxy(const std::vector<Poly>& polys);

// Top-level entry point.  `group_polys` is the input list-of-lists:
// for the single-group MVP it has one element.  `xvar_indices` lists
// the integration-variable indices into the shared PolyCtx.
//
// `allow_algebraic_letters` (Phase 7-vii): when true, deg-2 polys are
// accepted at each LR step.  HF's integrator already allocates Wm/Wp
// at integration time via `linear_factors`, so the LR pass only needs
// to admit deg-2 as a valid step shape.  When false (the classic
// FindRoots=False semantics) only linear (deg ≤ 1) polys are accepted.
//
// `sings` (default nullptr): optional per-face kinematic-divisor
// collector (order-resolved singularities pipeline).  When non-null, it
// is seeded with the integration-variable index set (so it can decide
// which factors are kinematic-only) and threaded through every
// st_fubini_lr call, accumulating the canonical irreducible kinematic
// divisors of the whole DP.  Null (the default) => no extra work and a
// byte-identical verdict path.  The collector never changes the LR
// order/score (it only observes factors the walk already computes).
//
// `carry_discharge` (2026-06-07): the Doppio FindRoots keep rule.  Only
// active when `allow_algebraic_letters` is true (deg-2 letters allowed);
// for the deg<=1 path it is a no-op.  When OFF (the Strict tier) the
// subset-DP judges deg-2 letters TERMINAL-ONLY: a deg-2 letter whose
// sqrt-obligation depends on a still-pending variable is rejected at
// that step (forbidden_after_step).  When ON, that obligation is
// instead CARRIED forward and discharged at a later pivot — the exact
// per-step semantics of lr_scan::step_fr_judge / fr_judge, run here as a
// per-path DFS over the same S-marginal set_table (no Cheng-Wu gauge:
// the production per-gauge integrand is already gauge-fixed upstream).
// This is strictly more permissive: it flips faces that are Strict-NOLR
// but carried-LR.  The path-dependence (a carried obligation depends on
// the whole order, not just the subset reached) is why this cannot be
// threaded through the best-score subset-DP memo and is realized as a
// DFS.  When ON the result's `root_polys` carries the deg-2 letters
// encountered along the chosen ((carried_sqrts, score)-minimal) order, and the
// LrResult.carried_sqrts profile is populated.  carry_discharge=false
// (the DEFAULT) reproduces the Strict subset-DP byte-for-byte
// (regression gate); the handler passes the request value explicitly,
// so this default agrees with the handler's default-OFF (spec 4a.1).
// score_prune_factor: relative branch-and-bound cutoff over the subset DP.
// At each subset size, any subset whose best-order score exceeds
// score_prune_factor * (lowest score at that size) is pruned: it is never
// used as a parent, so the (most expensive) next-size reductions extending
// it are never computed.  Default = +inf (no pruning; classic behavior).
LrResult find_lr_orders(
    const std::vector<std::vector<Poly>>& group_polys,
    const std::vector<size_t>& xvar_indices,
    bool allow_algebraic_letters = false,
    SingCollector* sings = nullptr,
    bool carry_discharge = false,
    double score_prune_factor = std::numeric_limits<double>::infinity());

// Result of verifying ONE specific integration order (no search).
struct OrderVerifyResult {
    bool        is_lr = false;          // the given order is linearly reducible
    int         blocking_step = -1;     // 0-based step that failed (-1 if LR)
    long        blocking_degree = 0;    // degree of the offending letter in the pivot
    bool        forbidden_dep = false;  // failure was a deg-2 forbidden-var dependence
    std::string blocking_letter;        // canonical form of the offending letter ("" if LR)
    bool        malformed = false;      // order is not a permutation of xvar_indices
};

// Verify whether ONE SPECIFIC order (order_var_indices, a permutation of
// xvar_indices in the intended integration sequence) is linearly reducible,
// WITHOUT enumerating/searching any other order.  Walks the order
// sequentially: starting from group_polys, at each pivot it (a) checks every
// current letter has degree <= max_deg in the pivot (max_deg = 1 strict, or 2
// when allow_algebraic_letters, with the same forbidden-pending-variable
// rejection as find_lr_orders' Step B), then (b) advances the letter set via
// st_fubini_lr (single path, no intersection).  Returns is_lr plus the
// blocking step/letter on the first failure.  Cost is O(n) st_fubini_lr calls
// (one per pivot), not the O(2^n) subset-DP -- the cheap certification the
// carry executor's order-pinning guard needs (it can confirm the PINNED order
// is LR directly, instead of a full free search + best-order comparison).
// carry_discharge is intentionally NOT supported here: the post-substitution
// transformed term must be LR in the strict / FindRoots tier (no obligation
// to carry); verifying that is exactly the safety condition.
OrderVerifyResult verify_order_is_lr(
    const std::vector<std::vector<Poly>>& group_polys,
    const std::vector<size_t>& xvar_indices,
    const std::vector<size_t>& order_var_indices,
    bool allow_algebraic_letters = false);

}  // namespace lr_search
}  // namespace hyperflint
