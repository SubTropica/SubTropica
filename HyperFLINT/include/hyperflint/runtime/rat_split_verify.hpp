// HF_RAT_SPLIT_VERIFY: Phase-A round-trip verifier wiring.
//
// When the environment variable `HF_RAT_SPLIT_VERIFY=1` is set at
// process start, every per-step `RegulatorSym` produced by the
// integrator is run through the rat_split adapter (see
// core/rat_split.hpp) and asserted bit-identical to its source
// representation. Any divergence aborts with a structured error.
//
// The harness pays per-step overhead only when the env var is set
// (one cached read at first call, then a single `if` per check).
// Production fixtures with the gate OFF run unchanged.
//
// The narrow ctx `N` passed to the verifier is constructed at each
// call site from the wide ctx `F` minus the variables that the
// integrator treats as "external" (Mandelstam, mass, Wm/Wp). For
// Phase A, the external set is identified by the rule:
//   var_name does NOT match an MZV basis name AND var_name does
//   NOT appear in `feynman_var_indices` (the integrator's
//   integration list). The MZV basis name set is computed as the
//   transitive closure used by `build_mzv_var_list`.
//
// This is a build-profile-only check — Phase A scope. Commit (3)
// wires it; commits (4)/(5) extend the verifier to cover SymCoefSplit
// arithmetic ops once the new representation lands.

#pragma once

#include "hyperflint/core/poly.hpp"
#include "hyperflint/integrator/transform.hpp"  // RegulatorSym

#include <cstddef>
#include <vector>

namespace hyperflint {

// True iff `HF_RAT_SPLIT_VERIFY` is set in the environment to a
// non-empty, non-"0" value at process start. Cached on first call.
bool rat_split_verify_enabled();

// Run the rat_split round-trip on every `coef` in `r`, with `N`
// constructed from `F`'s variables minus the W-side names. On
// divergence, aborts with `std::abort()` after writing a structured
// JSON line to stderr (op, fixture-tag, offending Rat string, recon
// string, F.vars(), N.vars()).
//
// `feynman_var_indices` are the integration variables (the
// integrator's `var_indices`); they are kept on the N-side. Every
// other variable name that does NOT match an MZV-basis pattern is
// treated as W-side.
//
// No-op if `rat_split_verify_enabled()` returns false.
void verify_regulator_sym_rat_split(
    const RegulatorSym& r,
    const PolyCtx& F,
    const std::vector<size_t>& feynman_var_indices,
    const char* call_site_tag);

}  // namespace hyperflint
