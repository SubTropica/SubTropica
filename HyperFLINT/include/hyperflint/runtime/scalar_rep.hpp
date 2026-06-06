// HF_USE_SCALAR_REP: representation-selector for the HF MZV-as-scalar
// rewrite (design v2 §3.4, §3.5a).
//
// When the environment variable `HF_USE_SCALAR_REP=1` is set at process
// start, the per-step regulator chain in `transform_word_impl` /
// `transform_shuffle` builds its working type as `RegulatorSplit`
// (vector of `RegTermSplit{ SymCoefSplit coef; RegKey key; }`) instead
// of `RegulatorSym`. At each function's API boundary the working type
// is converted back to `RegulatorSym` via `SymCoefSplit::as_rat()`,
// so callers (`break_up_contour.cpp`, `regularize.cpp`,
// `integration_step.cpp`, `hyper_int.cpp`) are unaffected.
//
// `HF_USE_SCALAR_REP` is the representation knob; `HF_RAT_SPLIT_VERIFY`
// (see `runtime/rat_split_verify.hpp`) is the verifier-on/off knob.
// The two are independent: v1 representation can run with verify off
// for production, or with verify on for the per-commit gate.
//
// Phase-B B1.b lands the env-gate header + the dead `RegulatorSplit`
// helpers (`collect_regulator_split`, `shuffle_symbolic_split`,
// `canonicalize_regulator_split`, `scalar_mul_regulator_split`) — no
// call site is flipped at B1.b. Phase-B B1.c flips the dispatch and
// inserts the round-trip verifier site at the `as_rat()` boundary
// per design §3.5a.
//
// One cached read at first call, then a single `if` per check. With
// `HF_USE_SCALAR_REP` unset (or `=0`), the new dispatch is unreached
// and the production hot path runs unchanged.

#pragma once

namespace hyperflint::runtime {

// True iff `HF_USE_SCALAR_REP` is set in the environment to a non-empty,
// non-"0" value at process start. Cached on first call (thread-safe via
// std::call_once).
bool scalar_rep_enabled();

// True iff `HF_SCALAR_REP_REQUIRE_PERSISTENT` is set in the environment
// to a non-empty, non-"0" value at process start. Cached on first call.
//
// Iter-44 (2026-05-09): debug-build assertion gate that fires at the
// silent-regression trap identified by the iter-43 cold-start drift
// check (Concern 2). When this is on AND `scalar_rep_enabled()` is on,
// the integrator aborts at every callsite that still hands out a
// per-call/transient `ZWTable` instead of threading the persistent
// table from the driver-entry allocation (`hyperflint_sym`,
// iter-41 `7de106a02`). Default-OFF preserves current behavior bit-
// identically; turning the gate on is the local migration check used
// during the C0b/C0c cascade (iter-46+).
//
// The four assertion sites at iter-44 ship are:
//   - `break_up_contour_sym` body lambda (`reduce/break_up_contour.cpp`):
//     fires if `zw_tab` is null when `scalar_rep_enabled()` is on.
//   - `reglim_word/positive_letter` callsite
//     (`integrator/transform.cpp:589`).
//   - `close_positive_letters_in_regulator_sym` callsite
//     (`integrator/integration_step.cpp:983`).
//   - CLI bridge `op=break_up_contour_sym` callsite
//     (`bridge/cli/main.cpp:2075`).
//
// All four are intentionally fatal: the gate exists to stop the build
// at the precise file:line where a transitional per-call `ZWTable`
// allocation has not yet been migrated to a caller-supplied persistent
// table. Vacates entirely once the C0c.1 cascade lands and all
// callsites receive a non-null `zw_tab` from `hyperflint_sym`.
bool require_persistent_enabled();

}  // namespace hyperflint::runtime
