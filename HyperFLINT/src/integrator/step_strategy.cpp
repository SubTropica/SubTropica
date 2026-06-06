// StepStrategy dispatch implementation.  Track 6.3, iter-35.
//
// Decision rule is transcribed by hand from the truth-table block in
// step_strategy.hpp (the header is the single source of truth).  This
// .cpp deliberately does NOT read test/data/step_strategy_truth_table.json
// — the JSON is hand-written test data, the header table is the
// authoritative spec, and the .cpp is an independent third transcription.
// STRUCTURAL_TAUTOLOGY_ROUND_TRIP defence (lessons_learned iter-29):
// three independent representations (header text, JSON file, .cpp code);
// the ctest falsifier hf-step-strategy-dispatch and the doc-vs-data lint
// (internal tooling) catch divergence.
//
// Honest current state (iter-35): HF C++ only executes the LR_* legs
// natively.  When pick_step_strategy returns Fubini_Lungo or
// Fubini_Espresso, the caller must delegate to the Mma side or a
// future HJ-native port (see step_strategy.hpp lines 24-31).  This .cpp
// is a pure classifier and does not invoke the integration step itself.

#include "hyperflint/integrator/step_strategy.hpp"

#include <cassert>

namespace hyperflint {
namespace integrator {

StepStrategy pick_step_strategy(const StepInputs& in) {
    // degree_budget outside {1, 2} is undefined per header doc.
    assert(in.degree_budget == 1 || in.degree_budget == 2);

    if (in.lr_found) {
        // method_lr_hint deliberately ignored when LR succeeded.
        return (in.degree_budget == 2)
            ? StepStrategy::LR_OptOrdered
            : StepStrategy::LR_NoOpt;
    }

    // lr_found == false -> degree_budget ignored; hint gates.
    return (in.method_lr_hint == StepInputs::MethodLR::Espresso)
        ? StepStrategy::Fubini_Espresso
        : StepStrategy::Fubini_Lungo;
}

}  // namespace integrator
}  // namespace hyperflint
