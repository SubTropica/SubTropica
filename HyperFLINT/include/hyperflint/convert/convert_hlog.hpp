// Phase 3-b: the Hlog[var, word] branch of ConvertToHlogRegInf.
//
// Seven-case dispatch mirroring HyperIntica.wl:3429-3446 (and the
// equivalent Maple branches in hyperInt/HyperInt.mpl:2268-2308):
//
//   1. word == []                 -> Regulator{{1, {}}}
//   2. var == 0                   -> Regulator{}  (divergent base)
//   3. all letters == 0           -> log-power formula (§2.2.1)
//   4. word[-1] == 0              -> reg_tail_expr regularization
//                                    + recurse on rebuilt sum (§2.2.2)
//   5. word[0] == var             -> throw ConvertFailed ($Failed)
//   6. all letters == word[0]     -> log-power formula (§2.2.1)
//   7. otherwise                  -> general convergent case (§2.2.3)
//
// Case ordering is load-bearing; Case 3 preempts Case 4 preempts
// Case 5 (physics-review finding #3).

#pragma once

#include "hyperflint/convert/expr.hpp"
#include "hyperflint/integrator/transform.hpp"   // Regulator, RegKey, RegTerm
#include "hyperflint/symbols/word.hpp"

#include <stdexcept>
#include <vector>

namespace hyperflint {
namespace convert {

class ConvertFailed : public std::runtime_error {
public:
    explicit ConvertFailed(const std::string& msg)
        : std::runtime_error("convert_to_hlog_reg_inf: " + msg) {}
};

// AST-level regularization. Returns a list of { Expr coef, Word w }
// pairs whose Q-linear combination
//
//   sum_i coef_i * Hlog[var, w_i]
//
// equals Hlog[var, word] modulo the shuffle-regularization identity
// for the letter `letter_to_strip` (always 0 in the Case-4 caller
// of convert_to_hlog_reg_inf_hlog). The substitute `subst` appears
// as `subst^k/k!` factors in the coefs where `k` is the number of
// stripped copies of `letter_to_strip` on that branch.
//
// **Postcondition.** Every emitted `w` has `w.letters.back() !=
// letter_to_strip` (or is empty). This is the infinite-recursion
// guard for Case 4: when the Case-4 handler re-enters
// convert_to_hlog_reg_inf on the rebuilt sum, the Hlog sub-terms'
// words never re-trigger Case 4 on the same tail.
//
// Mirrors HyperIntica.wl:2499-2520 (`RegTail`), with two
// adaptations:
//   - Single-word input (Case 4 calls RegTail on a singleton
//     wordlist, so we skip the outer wordlist loop).
//   - Expr-valued coefs (Mma carries the symbolic substitute as a
//     multiplicative factor in the coef column; HF's Rat cannot
//     hold a symbolic Hlog[var, {0}], so we thread the substitute
//     through as an Expr).
struct RegTailExprTerm {
    Expr coef;
    Word w;
};

std::vector<RegTailExprTerm>
reg_tail_expr(const Word&           word,
               const Rat&            letter_to_strip,
               const Expr&           subst,
               const PolyCtx&        ctx);

// Hlog[var, word] 7-case dispatcher. Returns a Regulator.
Regulator
convert_to_hlog_reg_inf_hlog(const Rat&                 var,
                              const std::vector<Rat>&    word,
                              const PolyCtx&             ctx);

// Top-level converter — dispatches on Expr kind. Phase 3-b
// implementation covers Leaf, Plus, Times, Power, and Hlog (enough
// for Case-4 re-entry on the rebuilt sum). Phase 3-c extends to
// the full user-facing driver.
Regulator
convert_to_hlog_reg_inf(const Expr& e, const PolyCtx& ctx);

}  // namespace convert
}  // namespace hyperflint
