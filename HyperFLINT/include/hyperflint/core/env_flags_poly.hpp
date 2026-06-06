// HF_FLAG_* macros for the core/poly-subsystem env flags.
//
// Track scope: Track-diagnostic-dump partial core/poly portion
// (docs/env_flags.md §T7 twenty-sixth chunk Track-diagnostic-dump
// partial, iter-96; n=1 of cluster=4: the discriminant-divisibility
// debug dump at the Poly::discriminant_in_var failure path; the
// other three cluster members land in integrator/env_flags.hpp and
// bridge/env_flags.hpp under iter-96 §5.1 rule-1 split-by-effect-
// domain placement — see iter-96 adversarial-reviewer Q-19 B1
// substantive-pattern BINDING verdict).
//
// THIRD core/ subsystem-suffix sibling header (precedent: iter-74
// core/env_flags_rat.hpp was the first core/ subsystem-suffix
// header for Track-cache-toggle core/rat subset; iter-76
// core/env_flags_reduce.hpp was the second for Track-reduce; this
// header at iter-96 is the third, and it is the
// iter-76-anticipated env_flags_poly.hpp literally named in the
// core/env_flags_reduce.hpp narrative at lines 36-41 as a
// concrete future-growth example of the `core/`-domain
// subsystem-suffix rule). Future core/poly-subsystem env flags
// (none identified as of iter-96 close) would grow here in place
// per the §5.1 F7 soft intra-header growth bound; non-poly core/
// subsystems continue to add their own sibling subsystem-suffix
// header rather than mixing into this one.
//
// SCOPE OF SUBSYSTEM-SUFFIX SPLITTING (iter-76 BINDING
// clarification, applied here at iter-96). The subsystem-suffix
// rule is `core/`-domain-specific. It is forced by the iter-65
// NAME-family precedent at core/env_flags.hpp: because the
// catch-all core/env_flags.hpp is NAME-family only (currently 14
// HF_FLAG_NAME_* macros for Track-cache-op-memo via the
// env_truthy/env_size wrapper machinery in operator_memo.cpp),
// every new VALUE-family cluster placed in core/ must split into
// its own sibling header to honor §5.1 family-isolation as
// codified by the iter-73 adversarial-reviewer concerns_binding
// verdict (header-level family-isolation, extension of iter-65
// F2 from cluster-level to header-level). Other domains
// (algebra/, bridge/, diagnostics/, instrumentation/, integrator/,
// runtime/) do NOT follow subsystem-suffix splitting; they
// continue to follow §5.1 F7 same-domain co-location.
//
// SUBSYSTEM-NAME CHOICE (iter-96 adversarial-reviewer
// Recommendation 1). The subsystem-suffix is `_poly` (matching
// the source TU subsystem name at src/core/poly.cpp) rather than
// `_debug` (which would re-introduce the family-suffix axis
// rejected at iter-75 codification §5.1 A1; "debug" is a
// macro-family classification, not a source-subsystem
// classification). The `_poly` choice keeps this header on the
// same subsystem axis as core/env_flags_rat.hpp and
// core/env_flags_reduce.hpp, so future poly-subsystem env flags
// aggregate here organically without re-litigating the
// subsystem-vs-family axis.
//
// ALTERNATIVES CONSIDERED (logged in the HyperFLINT development notes
// (internal) for permanent record of the iter-96 Q-19 B1
// substantive-pattern dispatch). (α) Mix HF_FLAG_DEBUG_DISC_DUMP into the catch-all
// core/env_flags.hpp alongside its 14 NAME-family macros —
// REJECTED for directly violating the iter-73 BINDING
// header-level family-isolation invariant. (γ) Reinterpret as
// NAME-family and add HF_FLAG_NAME_DEBUG_DISC_DUMP to
// core/env_flags.hpp — REJECTED with an additional-beyond-iter-65
// -F2 hazard: at this specific call site the macro's bound
// value is consumed as a filesystem path passed to
// std::ofstream, so the silent-default-off NAME-family footgun
// amplifies into silent-file-creation-under-the-literal-name
// (the iter-96 F-* advisory of permanent record:
// "NAME-family-by-convenience hazard amplifies when the bound
// value is consumed as a path"). (δ) Defer to iter-97 —
// REJECTED on work-doubling, context-loss, and Track-fragmentation
// grounds; β has no LAND-time cost beyond one new sibling header
// (a procedure already executed twice in core/ without incident).
// (ε) Route HF_DEBUG_DISC_DUMP to diagnostics/env_flags.hpp
// (mention-domain consolidation) — REJECTED by §5.1 rule-1
// strict-priority per iter-71 PINNED lesson
// `iter71_section5_1_rule_1_places_by_effect_domain_not_mention_
// domain`. The effect-domain of the poly.cpp:978 call site is
// unambiguously core/; the iter-75 diagnostics-off-spine rule-3
// amendment never overrides rule-1.
//
// FAMILY-ISOLATION enforcement. The §T9 oracle-1 (LANDED iter-75)
// asserts unique-home for every HF_FLAG_<NAME> #define across the
// repository, and oracle-2 asserts single-header for every
// "HF_<NAME>" literal. Together they catch the consequences of a
// family-mixing mistake (duplicate macros, multi-header literals)
// but do NOT directly assert per-header family purity. The §5.1
// family-isolation invariant is therefore enforced by convention
// at refactor time; a per-header family-purity ctest gate is
// deferred follow-up work (iter-74 F8b).
//
// Pattern: VALUE-family per docs/env_flags.md §5. Each macro
// expands to the full std::getenv(<literal>) expression returning
// const char* (NULL when unset). String literals are preserved
// verbatim so the ctest target hf-env-flag-registry-coverage
// parser-B regex continues to find them in this header. Set
// S_src is unchanged in element count; the literal just relocates
// from src/core/poly.cpp to this header.
//
// Call sites (refactored to use this macro):
//   HF_FLAG_DEBUG_DISC_DUMP (iter-96 Track-diagnostic-dump
//   partial, core/poly n=1 of cluster=4)
//     — src/core/poly.cpp line ~978, inside the
//       Poly::discriminant_in_var failure path. When set, the
//       bound value is used as a filesystem path (NOT as a
//       boolean) and the gate appends a diagnostic block to that
//       path: (f, f', lc, Res, var_idx, var_name, deg_in_var,
//       ctx_vars) so the divisibility a_n | Res(f,f') failure
//       (a polynomial identity in char 0 per Sylvester / GKZ
//       Prop 12.1.6) can be cross-checked against Mma. Capped at
//       10 dumps and serialized across threads via static
//       atomic + mutex. Call-site predicate-family: TRUTHY
//       `if (const char* path = std::getenv(...))`. The bound
//       value is then dereferenced as a path argument to
//       std::ofstream — see the (γ) rejection rationale above
//       for why this call shape makes NAME-family-by-convenience
//       a strictly-worse alternative. Default-when-unset = OFF
//       (no dump). Docs §2 row 147 column `unset⇒OFF` matches
//       the call-site behavior verbatim (no default-direction
//       discrepancy). The docs §2 retire=Y tag is STALE — the
//       literal is still actively read at the call site
//       (poly.cpp:978). Logged as advisory per the iter-86
//       lesson, dissolved per the iter-95 retire=Y dissolution
//       precedent (docs staleness, source canonical).
//       Single-effect-domain (core/) and single TU.

#pragma once

#include <cstdlib>

#define HF_FLAG_DEBUG_DISC_DUMP    std::getenv("HF_DEBUG_DISC_DUMP")
