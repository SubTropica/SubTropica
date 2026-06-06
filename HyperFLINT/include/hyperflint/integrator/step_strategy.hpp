// StepStrategy dispatch — declarative classification of the per-face
// integration-step strategy used by HF (and the orthogonal Mma-side
// fallback paths).  Track 6.3, iter-34 design.
//
// Status: HEADER-ONLY DESIGN AT ITER-34.  No .cpp yet.  iter-35 (or
// later) lands the implementation + ctest falsifier + updates ARCH §vi
// L316-337 to point at this header as the single source of truth.
//
// What this header is:
//   - The four-state enum that captures every distinct
//     integration-step path HF or its Mma caller can take per face.
//   - A struct `StepInputs` naming the inputs the decision rule reads.
//   - A declarative `pick_step_strategy(const StepInputs&)` that
//     returns the strategy.  Single source of truth, embeddable in a
//     ctest falsifier (iter-35).
//
// What this header is NOT (honest current state):
//   HF C++ today implements ONE of the four states directly: the
//   LR-search path (`find_lr_orders` in lr_search.hpp / .cpp,
//   eval-json-exposed via `find_lr_orders` op in handlers.cpp:449,
//   bridge driver bridge/cli/main.cpp:225 + :3236).  The
//   FindRoots=False / FindRoots=True axis splits that one path into
//   the LR_NoOpt / LR_OptOrdered enum values.  Fubini_Lungo and
//   Fubini_Espresso are NOT implemented in HF C++.  They denote the
//   two Mma-side fallback paths (`STFasterFubini2` and `STFubini`
//   resp., see SubTropica.wl:11042-11083 stDispatchFubini2 dispatcher
//   + SubTropica.wl:5357 / :16403 `MethodLR` user option).  When
//   `pick_step_strategy` returns one of those two values, the
//   integrator caller must delegate to the Mma side (today via
//   `STFindLROrdersHF` returning $Failed -> STFasterFubini2 fallback;
//   future HJ port may implement Lungo/Espresso natively).
//
// Why an enum at all if 2 of 4 states are unimplemented in HF C++:
//   The §19 falsifier (see HyperFLINT development notes, internal) asks
//   an outside party (HJ port author, any new HF backend) to be able to
//   choose the strategy by reading HF's documented decision rule, NOT by
//   reverse-engineering C++ control flow OR the Mma stDispatchFubini2
//   cascade.  This header IS that decision rule.  It captures the strategy
//   contract whether or not HF C++ itself executes every leg.
//
// Source-side anchor convention (Track 6.1):
//   /* @ARCH:step-strategy v=1 */ ... /* @ARCH:end step-strategy */
//   paired markers will wrap the strategy-dependent call sites once
//   iter-35+ wires `pick_step_strategy` into the integration driver.
//   ARCHITECTURE.md `<!-- @ARCH-DOC v=1 -->` reserves the section-id.

#pragma once

#include <cstddef>

namespace hyperflint {
namespace integrator {

// Enum surface.  Four mutually-exclusive states; pick_step_strategy
// returns exactly one for any well-formed StepInputs.
//
// LR_OptOrdered     LR-search succeeded AND deg-2 algebraic letters
//                   accepted (`allow_algebraic_letters == true`,
//                   equivalent to Mma's per-face FindRoots = True).
//                   HF C++ executes this directly via
//                   find_lr_orders + integrator post-LR step.  "Opt"
//                   = the optimized (Wm/Wp-rich) step shape.
//
// LR_NoOpt          LR-search succeeded AND only linear (deg <= 1)
//                   steps accepted (FindRoots = False).  HF C++
//                   executes this directly.  "NoOpt" = no algebraic-
//                   letter optimization, classic LR.
//
// Fubini_Lungo      LR-search returned NOLR (no linearly reducible
//                   order exists for this face) AND the user/cascade
//                   selected the Lungo (= "long") fallback algorithm
//                   (Mma's STFasterFubini2; older
//                   discriminant/resultant + global dedup family).
//                   NOT in HF C++.  Caller must delegate to Mma or
//                   future HJ-native Lungo port.
//
// Fubini_Espresso   LR-search returned NOLR AND the user selected the
//                   Espresso (= "short", "compact") fallback (Mma's
//                   STFubini family, newer iterative algorithm).  NOT
//                   in HF C++.  Caller must delegate.
enum class StepStrategy {
    LR_OptOrdered,
    LR_NoOpt,
    Fubini_Lungo,
    Fubini_Espresso,
};

// Inputs to the decision rule.  Names follow the strategy spec §6.3 / ARCH §vi
// "(degree_budget, n_factors, n_letters)" terminology; mapping to HF
// source-side identifiers documented inline.
//
// Field semantics (declarative; no hidden state):
//
//   degree_budget          1 -> reject deg-2 steps (FindRoots=False
//                          semantics; HF's
//                          `allow_algebraic_letters = false`); 2 ->
//                          accept deg-2 steps (FindRoots=True;
//                          `allow_algebraic_letters = true`).  Values
//                          outside {1, 2} are reserved for future
//                          higher-degree budgets and currently
//                          UB-flagged by pick_step_strategy.
//
//   n_factors              Total polynomial count across all groups,
//                          i.e. sum_g group_polys[g].size() where
//                          group_polys matches handlers.cpp:490 and
//                          lr_search.cpp:207 `group_polys`.  Spec
//                          name "n_factors"; HF source uses
//                          "n_polys_total" as the working variable
//                          (see iter-35 .cpp).
//
//   n_letters              Number of integration variables, i.e.
//                          xvar_indices.size() in lr_search.cpp:214.
//                          Spec name "n_letters"; HF source uses
//                          "n_xvars" as the working variable.
//
//   lr_found               True iff a prior find_lr_orders() call
//                          returned !result.nolr().  The caller is
//                          responsible for invoking find_lr_orders;
//                          this enum does not re-run the search.
//                          When `lr_found == false` the caller is
//                          expected to populate `method_lr_hint`.
//
//   method_lr_hint         "Lungo" or "Espresso", forwarded from
//                          Mma's `MethodLR` user option
//                          (SubTropica.wl:16403).  Consulted only
//                          when `lr_found == false`.  When
//                          `lr_found == true` this field is ignored.
//                          Default ("Lungo") matches Mma default.
//
//   Note on the spec triple `(degree_budget, n_factors, n_letters)`:
//   under the iter-34 decision rule, neither `n_factors` nor
//   `n_letters` actually changes the strategy choice — both fields
//   are recorded for shape-validation in the iter-35 ctest falsifier
//   and for future heuristic refinement (the strategy spec §6.3 anticipates
//   degree-budget / factor-count / letter-cardinality thresholds may
//   later split LR_NoOpt vs LR_OptOrdered along problem-size axes,
//   not just along the FindRoots toggle).  The iter-34 design KEEPS
//   the fields in StepInputs to commit to the contract; the iter-35
//   implementation MAY ignore them as long as the test passes.
struct StepInputs {
    int          degree_budget;  // 1 or 2 (= max accepted step degree)
    std::size_t  n_factors;      // sum_g |group_polys[g]|
    std::size_t  n_letters;      // |xvar_indices|
    bool         lr_found;       // !find_lr_orders().nolr()
    enum class MethodLR { Lungo, Espresso } method_lr_hint;
};

// Decision rule (single source of truth; iter-35 implements):
//
//   lr_found  | degree_budget | method_lr_hint | StepStrategy
//   ----------+---------------+----------------+----------------
//   true      | 1             | (ignored)      | LR_NoOpt
//   true      | 2             | (ignored)      | LR_OptOrdered
//   false     | (any)         | Lungo          | Fubini_Lungo
//   false     | (any)         | Espresso       | Fubini_Espresso
//
// Notes:
//   - `n_factors` and `n_letters` do not gate any cell at iter-34; see
//     StepInputs doc.  iter-35+ may refine.
//   - `degree_budget` outside {1, 2} is currently undefined.  iter-35
//     implementation must assert/clamp at the boundary.
//   - Symmetric exhaustion: 2 x 2 x 2 = 8 cells in the truth table,
//     but `method_lr_hint` is ignored when `lr_found == true`,
//     collapsing 8 -> 6 distinct (lr_found, degree_budget,
//     method_lr_hint) tuples.  Each maps to exactly one of 4 enum
//     values.  No undefined cells.
StepStrategy pick_step_strategy(const StepInputs& in);

// Falsifier contract (iter-35 ctest target `hf-step-strategy-dispatch`):
//
//   The ctest binary will read a JSON file
//   `test/data/step_strategy_truth_table.json` containing the 6
//   distinct (lr_found, degree_budget, method_lr_hint) tuples paired
//   with their expected StepStrategy value (encoded as one of the 4
//   enum-name strings).  The binary calls pick_step_strategy on each
//   row and asserts the returned enum matches.
//
//   STRUCTURAL_TAUTOLOGY_ROUND_TRIP defence (lessons_learned iter-29):
//   the JSON file is hand-written and committed BEFORE
//   pick_step_strategy.cpp is written; the .cpp implementation does
//   NOT read the JSON.  Independent ground truth = the table in this
//   header comment + the JSON file; the .cpp implementation must
//   match BOTH.  Drift between header-table-text and JSON content is
//   caught by a separate doc-vs-data lint to be added at iter-35.

}  // namespace integrator
}  // namespace hyperflint
