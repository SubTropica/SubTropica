// Phase 5f: HyperFLINT top-level driver.
//
// Mirrors HyperIntica.wl:4913 (HyperInt) and HyperInt.mpl:1182
// (hyperInt). The function is named `hyperflint` on the C++ side to
// match the project name; the HyperIntica/HyperInt predecessors keep
// their original names as separate backends.
//
// Scope (Phase 5f-i):
//   hyperflint(ctx, input, var_indices) — integrate `input` over the
//   listed variables from 0 to ∞ in sequence. Each variable's
//   IntegrationStep produces a Regulator that becomes the next
//   iteration's ShuffleList input (a Regulator's `{coef, regkey}` is
//   directly reinterpretable as a ShuffleList entry `{coef, shuffle}`
//   since RegKey is just vector<Word>).
//
// Deferred to Phase 5f-iii:
//   * ConvertToHlogRegInf: symbolic-expression -> ShuffleList path
//     (HyperIntica.wl:3421). In Phase 5f-i the caller supplies the
//     ShuffleList directly — for bare-rational inputs this is the
//     singleton [(coef, empty)].
//   * Interval rescaling: `x -> {a, b}` variable substitutions
//     (HyperIntica.wl:4933-4948).

#pragma once

#include "hyperflint/integrator/integration_step.hpp"
#include "hyperflint/integrator/transform.hpp"
#include "hyperflint/reduce/mzv_reduce.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace hyperflint {

// Phase 6d-v-iv: SymCoef-producing driver.
//
// Runs integration_step for each variable up to the penultimate step;
// the final step uses integration_step_sym (positive-letter
// continuation). Output is a RegulatorSym — the SymCoef coefficients
// carry Pi, I, Log[n], delta[var] residues from the contour
// deformation. Intermediate steps assume their regulator outputs are
// Rat-reducible (i.e. no positive-letter continuation fires inside
// them); fixtures that violate this assertion lie outside the Phase
// 6d-v scope.
// DP.3 spectator projection (2026-06-03): `spectator_var_indices` lists
// the ctx indices of user variables that are never integrated (free
// kinematic parameters). The boundary-divergence check projects each
// accumulated bin onto the fibration basis over (un-integrated
// integration vars) UNION (spectators) before the zero test. Without
// the spectators, cross-letter cancellations that are functions of the
// free parameters are invisible to the per-term is_zero fallback and
// generic CONVERGENT integrands (e.g. 1/((x+1)(x+y)) over x) are
// falsely reported divergent -- the Mathematica reference
// (HyperIntica's TestZeroFunction) tests the bin as a function of all
// surviving variables and accepts them. Empty list preserves the old
// behavior.
RegulatorSym hyperflint_sym(const PolyCtx& ctx,
                              const ShuffleList& input,
                              const std::vector<size_t>& var_indices,
                              const MzvReductionTable& table,
                              bool introduce_algebraic_letters = false,
                              bool check_divergences = false,
                              const std::vector<size_t>& spectator_var_indices = {});

// Phase 5f-iii: interval rescaling for `var` from [from, to] to [0, ∞).
//
// Applies one of the five cases mirroring HyperIntica.wl:4365-4380.
// `from_str` / `to_str` are Rat-parseable strings plus the two
// sentinels "Infinity" and "-Infinity" (also accepts "+Infinity" and
// "-oo" / "oo"). After rescaling, the caller integrates `input`
// over `var` from 0 to ∞.
//
// Cases:
//   a == b                    → empty integral, returns {} (zero fn)
//   [-∞, ∞]                   → F(var) + F(-var), both over [0, ∞)
//   [a, ∞], a finite          → var -> a + var
//   [-∞, b], b finite         → var -> b - var, Jacobian -1
//   [a, -∞], a finite         → var -> a - var, Jacobian -1
//   [∞, b], b finite          → var -> b + var, Jacobian -1
//   [a, b], both finite       → var -> (a + b·var)/(1 + var);
//                                Jacobian (b - a)/(1 + var)²
//
// The substitution reuses `var` itself as the rescaled variable —
// callers integrate the rescaled entry over `[0, ∞)` with the
// original `var_idx`. Multiple variables are rescaled by repeated
// calls at the driver layer.
ShuffleList rescale_interval(const PolyCtx& ctx,
                              const ShuffleList& input,
                              size_t var_idx,
                              const std::string& from_str,
                              const std::string& to_str);

}  // namespace hyperflint
