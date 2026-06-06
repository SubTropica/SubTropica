// HF_FLAG_* macros for Track-mimalloc environment flags.
//
// Track scope: mimalloc (docs/env_flags.md §2 Track-mimalloc, 8 entries
// total; iter-3, iter-4, phase-1, pre-iter1, iter-38/39/41/43 vintage).
// This header defines macros for all 8 entries; every call site is a
// plain `std::getenv()` invocation inside a static initializer, an RAII
// ctor, or a one-off init function. None of them go through a wrapper
// that would force a NAME-vs-VALUE shape change.
//
// Call sites (refactored to use these macros):
//   src/integrator/hyper_int.cpp        : 7 sites
//   src/core/operator_memo.cpp           : 1 site
//   bridge/cli/gmp_mimalloc_init.cpp     : 2 sites
// Total: 10 std::getenv() invocations, 8 unique env-var names.
//
// Pattern (consistent with iter-62 Track-DAG-probe macro layer at
// `include/hyperflint/instrumentation/env_flags.hpp` and iter-63
// Track-diagnostic-dump at `include/hyperflint/diagnostics/env_flags.hpp`):
//   #define HF_FLAG_<NAME> std::getenv(<env var literal>)
// The string literals are preserved verbatim in the macro bodies so
// the ctest target `hf-env-flag-registry-coverage` parser-B (regex
// `"HF_[A-Z0-9_]+"` over *.cpp/*.hpp/*.cc/*.h under src/, bridge/,
// include/) continues to find them. Set S_src is unchanged before and
// after this refactor; only the file in which each literal appears
// changes (source/bridge file -> this header).
//
// IMPORTANT (iter-63 lesson): do NOT write any HF_ env-var name as a
// quoted literal in this file's comments, or parser-B will count it
// as an extra source-side entry. The prose above refers to names
// unquoted (HF_MI_STATS, etc.) precisely for that reason.
//
// Track-mimalloc rationale (allocator config, not instrumentation):
// The flags here gate optional mimalloc collect/heap-visit/stats hooks
// emitted around the integration step and the GMP/mimalloc bridge.
// Default-off semantics are preserved through the macro layer: each
// macro evaluates to `const char*`, NULL when the env var is unset,
// matching the call-site contract that pre-existed this refactor.
//
// Track-narrow-ctx rationale (iter-69 addition, runtime PolyCtx
// lifecycle controls; docs/env_flags.md §5.1 header-location rule
// case 1: domain-of-the-toggle-effect = runtime). The four toggles
// here are companions to `runtime/narrow_ctx_flag.hpp`; they gate
// narrow-context discovery (HF_NARROW_CTX), narrow multiplication
// opt-in (HF_MUL_NARROW), wide-ctx audit instrumentation
// (HF_WIDE_CTX_AUDIT), and reduce-narrow bypass
// (HF_NO_NARROW_REDUCE). Per-toggle call sites (iter-78 empirical
// re-grep; resolves iter-76 F6 advisory which flagged the prior
// "Call sites span bridge + core/rat + core/poly" wording as
// reading ambiguously, since HF_NO_NARROW_REDUCE in particular
// occurs only in core/rat):
//   HF_FLAG_NARROW_CTX        : src/bridge/handlers.cpp (1 site)
//   HF_FLAG_MUL_NARROW        : src/core/poly.cpp        (1 site)
//   HF_FLAG_WIDE_CTX_AUDIT    : src/core/rat.cpp         (1 site)
//   HF_FLAG_NO_NARROW_REDUCE  : src/core/rat.cpp         (2 sites)
// Per §5.1 rule case 1 every call site `#include`s this runtime
// header even though none of them lives in `src/runtime/`.
//
// Track-rat-canonical rationale (iter-83 addition, runtime
// rat-split-verify gate; docs/env_flags.md §5.1 header-location rule
// case 1: domain-of-the-toggle-effect = runtime). The single toggle
// (HF_RAT_SPLIT_VERIFY) is a companion to
// `runtime/rat_split_verify.hpp`; it gates the Phase-A rat-split
// regulator verifier exit-time emitter and per-call counters. Both
// the call site and the companion verifier header live in
// `runtime/`, so rule 1 single-effect-domain placement applies with
// no cross-domain question:
//   HF_FLAG_RAT_SPLIT_VERIFY  : src/runtime/rat_split_verify.cpp (1 site)
// VALUE-family (consistent with the existing 12 macros in this
// header; iter-83 Option ε keeps family-purity).
//
// Track-step-trace rationale (iter-85 addition; docs/env_flags.md §5.1
// header-location rule case 1: domain-of-the-toggle-effect = runtime).
// The single toggle (HF_STEP_TRACE) is the companion to
// `runtime/trace_gate.hpp`, the canonical inline `step_trace_enabled()`
// gate (cached via C++11 magic statics) that is consumed library-wide
// by core/poly.cpp, core/rat.cpp, integrator/primitive.cpp,
// integrator/integration_step.cpp, integrator/hyper_int.cpp, and
// algebra/linear_factors.cpp to gate per-thread chrono accumulators
// for HF_STEP_TRACE-emitted JSON telemetry. The CANONICAL gate site is
// the inline function in `runtime/trace_gate.hpp`; integrator-resident
// gate duplicates in src/integrator/primitive.cpp (cached) and
// src/integrator/hyper_int.cpp (uncached) were *eliminated* in the
// iter-85 LAND in favor of the canonical gate, leaving exactly one
// `std::getenv("HF_STEP_TRACE")` literal in the codebase (this header).
// Adversarial-reviewer (iter-85 Q-19 B1 substantive-pattern dispatch)
// ratified runtime/env_flags.hpp as the home, citing two binding
// constraints: (a) rule 1 effect-domain is runtime because the
// canonical gate lives in `runtime/trace_gate.hpp` (mirroring iter-83
// Track-rat-canonical's pairing with `runtime/rat_split_verify.hpp`);
// (b) options (a)/(c) of the dispatch question (placement in
// integrator/env_flags.hpp by majority-call-site-count or by §5.1
// rule-3 lower-level-domain tiebreaker) would require
// `runtime/trace_gate.hpp` to `#include` `integrator/env_flags.hpp`,
// which is a header-to-header upward (anti-layering) include
// (integrator ≺ runtime on the §5.1 partial order). The dispatch
// established the precedent that when a §T7 cluster's canonical gate
// lives in include/<domain>/, the macro home is that <domain>'s
// env_flags.hpp regardless of how many src/ call sites live elsewhere.
// VALUE-family (consistent with the existing 13 macros in this
// header). Macro consumed at exactly one site post-iter-85:
//   HF_FLAG_STEP_TRACE        : include/hyperflint/runtime/trace_gate.hpp
//                               (inside `inline bool step_trace_enabled()`,
//                                the canonical cached gate). The src/
//                                duplicates at primitive.cpp:97-104 and
//                                hyper_int.cpp:853-856 were deleted in
//                                iter-85 in favor of unqualified calls
//                                to `step_trace_enabled()` (resolved
//                                via Koenig lookup to
//                                `hyperflint::step_trace_enabled` from
//                                `runtime/trace_gate.hpp`).

#pragma once

#include <cstdlib>

// Track-mimalloc (8 flags; see docs/env_flags.md §2 "Track-mimalloc").
// All evaluate to `const char*` (the std::getenv return value), NULL when unset.
#define HF_FLAG_MI_COLLECT_AT_STEP             std::getenv("HF_MI_COLLECT_AT_STEP")
#define HF_FLAG_MI_COLLECT_OPTION_M_C          std::getenv("HF_MI_COLLECT_OPTION_M_C")
#define HF_FLAG_MI_HEAP_VISIT                  std::getenv("HF_MI_HEAP_VISIT")
#define HF_FLAG_MI_HEAP_VISIT_ABANDONED        std::getenv("HF_MI_HEAP_VISIT_ABANDONED")
#define HF_FLAG_MI_HEAP_VISIT_ALL              std::getenv("HF_MI_HEAP_VISIT_ALL")
#define HF_FLAG_MI_HEAP_VISIT_DISJOINT_AUDIT   std::getenv("HF_MI_HEAP_VISIT_DISJOINT_AUDIT")
#define HF_FLAG_MI_INIT_VERBOSE                std::getenv("HF_MI_INIT_VERBOSE")
#define HF_FLAG_MI_STATS                       std::getenv("HF_MI_STATS")

// Track-narrow-ctx (4 flags; see docs/env_flags.md §3 "Track-narrow-ctx"
// inventory and §5.1 header-location rule; iter-69 §T7 fifth chunk).
// All evaluate to `const char*` (the std::getenv return value), NULL when unset.
#define HF_FLAG_NARROW_CTX                     std::getenv("HF_NARROW_CTX")
#define HF_FLAG_MUL_NARROW                     std::getenv("HF_MUL_NARROW")
#define HF_FLAG_WIDE_CTX_AUDIT                 std::getenv("HF_WIDE_CTX_AUDIT")
#define HF_FLAG_NO_NARROW_REDUCE               std::getenv("HF_NO_NARROW_REDUCE")

// Track-rat-canonical (1 flag; see docs/env_flags.md §2 "Track-rat-canonical"
// inventory and §5.1 header-location rule; iter-83 §T7 thirteenth chunk).
// Evaluates to `const char*` (the std::getenv return value), NULL when unset.
#define HF_FLAG_RAT_SPLIT_VERIFY               std::getenv("HF_RAT_SPLIT_VERIFY")

// Track-step-trace (1 flag; see docs/env_flags.md §2 "Track-step-trace"
// inventory and §5.1 header-location rule; iter-85 §T7 fifteenth chunk).
// Evaluates to `const char*` (the std::getenv return value), NULL when unset.
// Companion to `runtime/trace_gate.hpp` (the inline canonical gate
// `step_trace_enabled()`); iter-85 dispatch ratified runtime/ as the home.
#define HF_FLAG_STEP_TRACE                     std::getenv("HF_STEP_TRACE")

// Track-cache-scalar-rep rationale (iter-91 addition, runtime
// representation-selector + persistent-allocation-require guard;
// docs/env_flags.md §5.1 header-location rule case 1: rule-1
// effect-domain = runtime). Two toggles (n=2) gating the v2
// `RegulatorSplit` MZV-as-scalar representation:
//
//   HF_FLAG_USE_SCALAR_REP                 : the representation knob.
//                                             Canonical accessor
//                                             `runtime::scalar_rep_enabled()`
//                                             in `runtime/scalar_rep.hpp`.
//                                             Called by integrator/transform
//                                             dispatch, reduce/break_up_contour,
//                                             algebra/partial_fractions and
//                                             algebra/linear_factors (read via
//                                             the canonical accessor, no
//                                             additional env-var literal).
//                                             A SECOND reader exists at
//                                             `src/core/operator_memo.cpp:313`
//                                             (the file-local
//                                             `scalar_rep_active()` predicate)
//                                             for the FOLD-ER3 forced-disable
//                                             of lf/pf/transform_shuffle when
//                                             SCALAR_REP=1; this is a
//                                             secondary dispatch on the same
//                                             runtime-layer effect, not an
//                                             independent toggle, so rule-1
//                                             effect-domain remains runtime.
//   HF_FLAG_SCALAR_REP_REQUIRE_PERSISTENT  : iter-44 debug-build assertion
//                                             gate (single-domain runtime).
//                                             Canonical accessor
//                                             `runtime::require_persistent_enabled()`
//                                             in `runtime/scalar_rep.hpp`.
//                                             Single std::getenv call site at
//                                             `src/runtime/scalar_rep.cpp:30`;
//                                             fatal-abort log lines at four
//                                             integrator/reduce/bridge sites
//                                             read the cached predicate but
//                                             do NOT re-call std::getenv on
//                                             the literal name (they emit
//                                             `"[HF_SCALAR_REP_REQUIRE_PERSISTENT=1]"`
//                                             as a stderr prefix, which
//                                             parser-B excludes because the
//                                             literal does not close with
//                                             `")` after the env-var name).
//
// Iter-91 adversarial-reviewer (Q-19 B1 substantive-pattern dispatch)
// ratified runtime/env_flags.hpp as the home, citing rule-1
// effect-domain as the binding rationale: the toggle effect of
// HF_USE_SCALAR_REP is in the runtime layer (the `RegulatorSplit` v2
// representation-selector coordinated via `runtime::scalar_rep_enabled()`);
// the operator_memo-side reader is a secondary dispatch on the same
// runtime effect, not an independent toggle. The iter-85 "canonical-
// gate-domain wins" precedent supports but does not strictly bind
// (no header-to-header anti-layering hazard applies here because no
// header consumes the macro; both consumers are TUs).
// HF_SCALAR_REP_REQUIRE_PERSISTENT is single-domain runtime (rule-1
// applies cleanly with no cross-domain question). Sibling co-location
// keeps both toggles in the same header.
//
// VALUE-family (consistent with the existing 14 macros in this
// header; iter-91 lands the 15th and 16th, taking the macro count to
// 16/20 against the §5.1 F7 growth-bound).
//
// Track-bridge rationale (iter-93 addition, runtime tolerant-parse
// gate; docs/env_flags.md §5.1 header-location rule case 1:
// effect-domain = runtime). The single toggle (the bridge tolerant
// parse mode flag) is consumed by the cached `tolerance_enabled()`
// accessor in `src/runtime/narrow_ctx_flag.cpp:19-28`, a
// magic-static once-init read by the bridge tolerant-parse paths in
// reduce/mzv_reduce.cpp + reduce/periods.cpp through that accessor
// (no `std::getenv` re-reads on the literal name; only the cached
// boolean is consulted). Although the table heading was retained as
// "Track-bridge" for historical continuity with docs/env_flags.md §2
// row 86 (vintage `pre-iter1 (HF Step 1 scaffolding)`), the actual
// effect-domain of the flag is runtime: it modulates a runtime
// once-init that is read by reduce/ TUs but is owned by
// `src/runtime/narrow_ctx_flag.cpp`. Rule 1 single-effect-domain
// placement applies cleanly with no cross-domain question; this is
// the 6th extension of the iter-64-vintage runtime/env_flags.hpp
// after iter-69 Track-narrow-ctx (1st), iter-83 Track-rat-canonical
// (2nd), iter-85 Track-step-trace (3rd), iter-91
// Track-cache-scalar-rep (4th and 5th, n=2), iter-93 Track-bridge
// (6th, n=1). Routine TIER 3 LAND: no reviewer dispatch (Q-19 B1
// substantive-pattern carve-out NOT triggered; no cross-domain
// question, no include/-resident gate, no growth-bound stress at
// 16/20 pre-iter-93 / 17/20 post-iter-93). Macro consumed at exactly
// one site post-iter-93:
//   HF_FLAG_PARSE_TOLERANT     : src/runtime/narrow_ctx_flag.cpp
//                                (inside `tolerance_enabled()`'s
//                                 magic-static initializer lambda).
// VALUE-family (consistent with the existing 16 macros in this
// header; iter-93 takes the macro count to 17/20 against the §5.1 F7
// growth-bound).
//
// Advisory follow-ups (out of scope for the iter-91 LAND, logged in
// the iter-91 attempt log for future-iter pickup):
//   (i)  The two HF_USE_SCALAR_REP predicates differ — the runtime
//        accessor at `scalar_rep.cpp:19` treats any non-empty,
//        non-`0`-leading value as ON, while the core/operator_memo
//        accessor at `operator_memo.cpp:314` strictly requires a
//        `1`-leading value. The macro layer preserves both call-site
//        predicates verbatim. Open issue: unify on the runtime
//        definition.
//   (ii) The "intentionally independent" justification at
//        `operator_memo.cpp:305-310` is contradicted by the
//        function's own commentary at lines 375-381 (the local
//        static is one-shot, identical-lifecycle to the canonical
//        accessor, and SCALAR_REP test-toggling requires subprocess
//        launch regardless). The local reader is a header-bloat
//        optimization, not a test-mode independence mechanism.
//        Either delete the local reader (consume the canonical
//        `runtime::scalar_rep_enabled()` directly — the
//        runtime/env_flags.hpp include is already present at
//        operator_memo.cpp:36) or rewrite the comment to reflect
//        actual semantics.
#define HF_FLAG_USE_SCALAR_REP                 std::getenv("HF_USE_SCALAR_REP")
#define HF_FLAG_SCALAR_REP_REQUIRE_PERSISTENT  std::getenv("HF_SCALAR_REP_REQUIRE_PERSISTENT")

// Track-bridge (1 flag; see docs/env_flags.md §2 "Track-bridge"
// inventory and §5.1 header-location rule; iter-93 §T7 twenty-third
// chunk). Evaluates to `const char*` (the std::getenv return value),
// NULL when unset. Companion to the cached `tolerance_enabled()`
// accessor in `src/runtime/narrow_ctx_flag.cpp` (magic-static
// once-init). Rule 1 effect-domain = runtime.
#define HF_FLAG_PARSE_TOLERANT                 std::getenv("HF_PARSE_TOLERANT")
