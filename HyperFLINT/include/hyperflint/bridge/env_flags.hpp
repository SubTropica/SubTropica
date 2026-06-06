// HF_FLAG_* macros for bridge-domain env flags.
//
// Track scope: OMP (docs/env_flags.md §T7 twenty-fourth chunk
// Track-OMP, iter-94; bridge-portion of an n=3 cluster split
// across two effect-domains under §5.1 rule-1) + diagnostic-dump
// partial bridge portion (docs/env_flags.md §T7 twenty-sixth
// chunk Track-diagnostic-dump partial, iter-96; n=1 of cluster=4:
// the legacy master verbose debug flag at the bridge CLI body
// parser entry of handle_shuffle_product, gating per-term
// debug trace emission while iterating shuffle-product input
// terms; the other three cluster members land in integrator/
// env_flags.hpp and the new core/env_flags_poly.hpp under
// iter-96 §5.1 rule-1 split-by-effect-domain placement — see
// iter-96 adversarial-reviewer Q-19 B1 substantive-pattern
// BINDING verdict).
//
// FIRST bridge-domain env_flags header (precedent: iter-77
// algebra/env_flags.hpp was the first algebra-domain env_flags
// header; iter-74 integrator/env_flags.hpp was the first
// integrator-domain header; this header continues the pattern of
// per-effect-domain headers, created from scratch when a domain
// gains its first VALUE-family macro and no host exists). Future
// bridge-domain clusters should grow here in place per the §5.1
// F7 soft intra-header growth bound, until enough macros / tracks
// force a subsystem-suffix split.
//
// PLACEMENT DECISION (iter-94 adversarial-reviewer BINDING verdict
// Q-19 B1 substantive-pattern dispatch). The Track-OMP cluster
// has three call sites across two effect-domains:
//   - HF_FLAG_OMP_PER_TERM_CLOSURE @ integrator effect-domain
//     (file-static cache inside src/integrator/integration_step.cpp,
//     gating an integrator inner-loop OMP parallel-for body).
//   - HF_FLAG_QOS_USER_INITIATED @ integrator effect-domain
//     (file-static thread_local one-shot guard inside
//     src/integrator/integration_step.cpp, gating macOS QoS
//     promotion of integrator's OMP worker threads).
//   - HF_FLAG_MAX_THREADS_PER_CALL @ bridge effect-domain
//     (src/bridge/handlers.cpp hyperflint_sym entry, gating
//     bridge-controlled per-call FLINT thread budget throttling
//     for memory-constrained Mma-driven dispatch).
// The reviewer's binding rationale: §5.1 rule-1 disposes the
// question under strict-priority resolution of rules 1-4. Each
// macro has an unambiguous single-effect-domain, so rule-1 fires
// before rule-3 (lower-level-domain tiebreaker) ever applies.
// HF_FLAG_MAX_THREADS_PER_CALL's mechanism alters process-global
// FLINT thread count, but the INTENT and the call-site origin are
// bridge-controlled throttling decisions; the process-globalness
// is a side-channel, not an effect-domain. The iter-71 PINNED
// lesson `iter71_section5_1_rule_1_places_by_effect_domain_not_
// mention_domain` applies here in its primary direction (effect-
// domain not mention-domain), and ALSO in its complementary
// direction (do not let process-global side-channels turn rule-1
// into a rule-3 trigger via the "everything-is-multi-domain"
// failure mode).
//
// NEW-HEADER-FOR-ONE-MACRO precedent: iter-77 created
// integrator/env_flags.hpp with two simultaneous macros (the
// Track-cache-toggle iter-74 kill-switch was already there as the
// header's first occupant; iter-77 added Track-probe-ctx four
// macros simultaneously). This bridge-domain header lands with
// one macro at iter-94. The reviewer ruled this acceptable:
// §5.1 does NOT encode a minimum-macro-count threshold for header
// creation, and inventing one now to dodge (β) would be ad hoc
// rule-bending. If iter-95+ brings a second bridge-effect macro
// (e.g. a future Track-bridge-rss or Track-bridge-cli flag), it
// co-locates trivially here.
//
// Pattern: VALUE-family per docs/env_flags.md §5. Each macro
// expands to the full std::getenv(<literal>) expression returning
// const char* (NULL when unset). String literals are preserved
// verbatim so the ctest target hf-env-flag-registry-coverage
// parser-B regex continues to find them. Set S_src is unchanged
// before and after the iter-94 refactor in element count; the
// HF_MAX_THREADS_PER_CALL literal just relocates from
// src/bridge/handlers.cpp to this header.
//
// Call sites:
//   HF_FLAG_MAX_THREADS_PER_CALL (iter-94 Track-OMP, bridge n=1)
//     — src/bridge/handlers.cpp hyperflint_sym entry; when set
//       and parseable as a positive integer N, calls
//       flint_set_num_threads(N) before any FLINT work in this
//       call. Memory operational lever: Mma master-kernel
//       STIntegrate dispatch sets HF_MAX_THREADS_PER_CALL=1 to
//       keep per-call peak RSS at ~970 MB (vs ~2.5 GB at OMP=13).
//       The call-site default-direction is "unset implies no
//       override" (FLINT default thread count is used); the
//       value-family is POSITIVE_INTEGER, NOT boolean ON/OFF
//       (sentinel HF_FLAG_MAX_THREADS_PER_CALL=0 is treated as
//       no-op because the atoi-then-(n>=1) guard requires a
//       positive integer).
//   HF_FLAG_DEBUG (iter-96 Track-diagnostic-dump partial, bridge
//   n=1 of cluster=4)
//     — bridge/cli/main.cpp line ~627, inline conditional inside
//       handle_shuffle_product's term-iteration loop after the
//       depth-balanced extraction of inner brace groups. When
//       TRUTHY (any non-null getenv pointer, regardless of value),
//       emits one stderr `[dbg]` block per term: the verbatim
//       term substring, the parsed coefficient string, and the
//       letters list, before constructing the WordlistTerm.
//       Scope-of-effect note: the bridge CLI legacy debug flag is
//       INTENTIONALLY narrow — this macro covers ONE call site,
//       which gates per-term tracing during shuffle_product input
//       parsing only. Other tools or libraries that share the
//       name HF_DEBUG (none identified within HF as of iter-96)
//       are unrelated; the macro and the call site coevolve, and
//       no cross-cutting "master verbose" semantics are assumed.
//       Call-site predicate-family: TRUTHY `if (std::getenv(...))`.
//       Default-when-unset = OFF. Docs §2 row 146 column
//       `unset⇒OFF` matches the call-site behavior verbatim
//       (no default-direction discrepancy). The docs §2 retire=Y
//       tag is STALE — the literal is still actively read at the
//       call site (cli/main.cpp:627). Logged as advisory per the
//       iter-86 lesson, dissolved per the iter-95 retire=Y
//       dissolution precedent (docs/§5.2 staleness, source
//       canonical). The reviewer Recommendation 3 calls out that
//       HF_DEBUG is the lowest-specificity name in the entire
//       HF_* registry; future readers should NOT assume this
//       gate's scope extends beyond the cli body parser without
//       explicit narrative caveat at any new call site they add.
//       Single-effect-domain (bridge/) and single TU.
//
// Family-isolation note (per iter-73 reviewer concerns_binding,
// extension of iter-65 F2 from cluster-level to header-level): this
// header holds VALUE-family macros only. If the bridge domain
// ever needs NAME-family macros, they MUST land in a sibling
// subsystem-suffix header (e.g. bridge/env_flags_<subsystem>.hpp)
// rather than co-reside with VALUE-family macros in this file.
// The iter-65 F2 hazard reader-confusion footgun applies
// symmetrically to either-direction family mixing.
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

#pragma once

#include <cstdlib>

#define HF_FLAG_MAX_THREADS_PER_CALL         std::getenv("HF_MAX_THREADS_PER_CALL")
#define HF_FLAG_DEBUG                        std::getenv("HF_DEBUG")
