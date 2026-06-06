// Phase 6a: MZV reduction.
//
// Loads HyperIntica's `mzvAllReductions` table (parsed by
// scripts/gen_mzv_reductions.py into data/mzv_reductions.json) and
// applies it as a fixed-point substitution on a Rat.
//
// Naming convention (shared with the data file):
//   Log[2]          -> Log2
//   Zeta[n]         -> mzv_<n>
//   Pi^(2k)         -> (6*mzv_2)^k
//   mzv[a, b, ...]  -> mzv_<aEnc>_<bEnc>_...
//   where iEnc is `i` for i>=0 and `m<|i|>` for i<0.
//
// Basis (weight <= 8, unevaluated to numbers):
//   Log2, mzv_2, mzv_3, mzv_5, mzv_7, mzv_3_5,
//   mzv_1_m3, mzv_1_m5, mzv_1_1_m3, mzv_1_1_1_m3.
//
// For Phase 6a, the caller is responsible for including relevant
// transcendentals in the Rat's PolyCtx. apply_mzv_reductions is a
// no-op on variables that aren't in the ctx; it's also a no-op on
// basis variables.

#pragma once

#include "hyperflint/core/rat.hpp"

#include <string>
#include <vector>

namespace hyperflint {

struct MzvReductionRule {
    std::string lhs;   // e.g. "mzv_1_m1"
    std::string rhs;   // Rat-parsable expression, e.g. "1/2*Log2^2"
};

struct MzvReductionTable {
    std::vector<MzvReductionRule> reductions;
    std::vector<std::string>      basis;
};

// Load the JSON table. Throws on parse error or missing file.
MzvReductionTable load_mzv_reductions(const std::string& json_path);

// Substitute one variable in a Rat with another Rat (exact symbolic
// substitution, no truncation). Treats the replacement as a full
// rational function; clears the variable's denominator structure by
// expanding num/den separately in powers of the substituted variable.
Rat substitute_var_rat(const PolyCtx& ctx,
                       const Rat& r,
                       size_t var_idx,
                       const Rat& replacement);

// Apply the mzv reductions to `r`, iterating until fixed point. A
// variable is "reducible" iff its name appears as an LHS in `table`
// and is present in `ctx`. Pure-basis Rats pass through untouched.
Rat apply_mzv_reductions(const PolyCtx& ctx,
                          const MzvReductionTable& table,
                          const Rat& r);

// Clear the process-global RHS-parse cache used by
// `apply_mzv_reductions` (`g_rhs_cache` in mzv_reduce.cpp).
//
// The cache keys on `(const PolyCtx*, const std::string*)`.  When
// a PolyCtx is destroyed, its raw pointer becomes dangling but the
// cache entries keyed by it survive.  If a future PolyCtx happens
// to land at the same heap address, the cache hit returns a Rat
// valued in the OLD ctx -- undefined behaviour.  This is a latent
// UB in the wide-ctx-everywhere code path too, but the narrow-ctx
// lever (HF_NARROW_CTX=1) creates short-lived PolyCtx-per-call,
// which makes the address-reuse far more likely.
//
// Recommended (R24 rev 2 / R25 review): wire this into the
// hyperflint_sym handler entry so the cache is reset every call.
// Cost: re-parses ~700 RHS strings on first cache miss per call
// (ms-scale at narrow ctx).  Wiring is deferred to chain 17+ along
// with the rest of the production-path Step 1 integration; the
// API ships in chain 16 as scaffolding so chain 17's source change
// is minimal.
void clear_rhs_cache();

// HF basis-ctx campaign (PHASE_4, 2026-05-28). Clear the process-global
// (ctx*, table*) → bool cache populated by `apply_mzv_reductions`'s no-op
// guard. Same hazard model as `clear_rhs_cache()`: PolyCtx instances
// are per-bridge-call (handlers.cpp:~1096), destroyed at handler exit;
// without this clear, a stale `(addr, table) → true` entry from a prior
// slim-ctx call can collide with a fresh wide-ctx PolyCtx that lands at
// the same heap address, causing `apply_mzv_reductions` to silently
// return identity on a wide-ctx input that should have been reduced.
// MUST be called at the entry of every bridge handler that may follow
// a prior call within the same process (LibraryLink transport).
void clear_ctx_has_no_lhs_cache();

// Union of (user_vars) + Log2 + every basis and reducible name in
// the table. This is the default PolyCtx for period-evaluator ops
// (zero_one_period, zero_inf_period) where intermediate mzv symbols
// may appear at any weight represented in the table.
std::vector<std::string> build_mzv_var_list(
    const MzvReductionTable& table,
    const std::vector<std::string>& user_vars);

// R20 Route (i): per-call narrow var list.  Walks the integrand
// string for MZV-symbol mentions, then takes the transitive closure
// over the reduction graph (rules whose LHS appears in the current
// set bring in their RHS-referenced symbols).  Returns user_vars +
// Log2 + only the MZV symbols actually reachable for this integrand.
// Output preserves canonical var ordering matching build_mzv_var_list
// (so cache keys keyed by var_idx are cross-face stable).
std::vector<std::string> build_narrow_var_list(
    const MzvReductionTable& table,
    const std::vector<std::string>& user_vars,
    const std::string& integrand_str);

}  // namespace hyperflint
