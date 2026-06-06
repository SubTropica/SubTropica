// HF_FLAG_* macros for the core/rat-subsystem env flag(s).
//
// Track scope: cache-toggle (docs/env_flags.md §T7 seventh chunk
// Track-cache-toggle, core/rat subset; n=1: the from-canonical
// fast-path bypass toggle).
//
// SEPARATION FROM core/env_flags.hpp (iter-65 NAME-family
// Track-cache-op-memo home). The iter-65 F2 silent-default-off/-on
// hazard is a header-level hazard: a single header containing both
// NAME-family macros (which expand to bare string literals and MUST
// be wrapped in env_truthy/env_size/getenv) and VALUE-family macros
// (which expand to std::getenv calls and MUST be bound to const
// char* and dereferenced) creates a copy-paste hazard where a reader
// cannot tell at a glance which macro requires which use pattern.
// Per the iter-73 adversarial-reviewer concerns_binding verdict,
// family-mixing within a single <domain>/env_flags.hpp is prohibited
// (extension of iter-65 F2 from cluster-level to header-level).
// When core/ needs both families, the split goes by subsystem-suffix
// (this header, env_flags_rat.hpp) rather than family-suffix
// (env_flags_value.hpp). Subsystem-suffix matches existing
// convention of subsystem-named non-flag headers in core/ (poly.hpp,
// rat.hpp) and generalizes to future Track-rat-* / Track-cache-rat
// clusters; family-suffix would invent a new naming axis.
//
// SCOPE OF SUBSYSTEM-SUFFIX SPLITTING (iter-76 binding clarification,
// retroactive to this iter-74-vintage header). The subsystem-suffix
// rule applied here is `core/`-domain-specific. It is forced by the
// iter-65 NAME-family precedent at core/env_flags.hpp: because the
// catch-all `core/env_flags.hpp` is NAME-family, every new
// VALUE-family cluster placed in `core/` must split into its own
// sibling header to honor §5.1 family-isolation. Other domains
// (algebra/, integrator/, runtime/, bridge/, diagnostics/) DO NOT
// follow subsystem-suffix splitting; they continue to follow §5.1
// F7 same-domain co-location, accumulating VALUE-family tracks in
// a single per-domain env_flags.hpp until a family conflict arises.
// Sibling subsystem-suffix headers under core/ as of iter-76:
// env_flags_rat.hpp (this header) + env_flags_reduce.hpp (iter-76).
//
// Pattern: VALUE-family per docs/env_flags.md §5. The macro
// HF_FLAG_DISABLE_FROM_CANONICAL expands to the full
// std::getenv(<literal>) expression returning const char* (NULL when
// unset). String literal is preserved verbatim so the ctest target
// hf-env-flag-registry-coverage parser-B regex continues to find it.
// Set S_src is unchanged before and after the iter-74 refactor; only
// the file in which the literal appears changes (src/core/rat.cpp
// -> this header).
//
// Call site (refactored iter-74 to consume this macro):
//   src/core/rat.cpp:2335 (one site, inside the static initializer
//   for Rat::from_canonical's `disabled` flag).
//
// Future growth: additional core/rat-subsystem env flags grow here.
// Other core/ subsystems (e.g. Poly, ZWTable) needing their own env
// flags should add sibling headers env_flags_poly.hpp /
// env_flags_zwtable.hpp / etc., NOT extend this one beyond the rat
// subsystem.
//
// iter-84 (§T7 fourteenth chunk EXTENSION): two additional doc-§2
// Track families grow here under the same-domain (core/) +
// same-family (VALUE) + same-subsystem (rat) invariants. Eighth
// precedent of "extend an existing same-domain same-family
// env_flags*.hpp" pattern (after iter-69 runtime, iter-74 algebra,
// iter-77 integrator, iter-79 instrumentation, iter-80 algebra
// second-extension, iter-81 diagnostics, iter-83 runtime
// second-extension).
//
//   Track-rat-legacy (doc §2 row 244-248, n=1):
//     HF_FLAG_USE_LEGACY_RAT_ADD -> std::getenv("HF_USE_LEGACY_RAT_ADD")
//     Sole call site at src/core/rat.cpp:2060 (the lambda-initialized
//     static `e` flag inside use_legacy_rat_add(), gating the legacy
//     cross-mult+gcd_cofactors Rat::add path).
//
//   Track-rat-Q-vs-fmpq (doc §2 row 224-230, n=3):
//     HF_FLAG_USE_QUNDERSCORE_RAT_MUL -> std::getenv("HF_USE_QUNDERSCORE_RAT_MUL")
//     HF_FLAG_USE_QUNDERSCORE_RAT_SUB -> std::getenv("HF_USE_QUNDERSCORE_RAT_SUB")
//     HF_FLAG_USE_QUNDERSCORE_RAT_DIV -> std::getenv("HF_USE_QUNDERSCORE_RAT_DIV")
//     Three call sites at src/core/rat.cpp:2080,2087,2094 inside the
//     lambda-initialized static `e` flags of
//     use_qunderscore_rat_{mul,sub,div}().
//
// iter-87 (§T7 seventeenth chunk EXTENSION): one additional doc-§2
// Track family grows here under the same-domain (core/) +
// same-family (VALUE) + same-subsystem (rat) invariants. Eleventh
// precedent of "extend an existing same-domain same-family
// env_flags*.hpp" pattern (after iter-69 runtime, iter-74 algebra,
// iter-77 integrator, iter-79 instrumentation, iter-80 algebra
// second-extension, iter-81 diagnostics, iter-83 runtime
// second-extension, iter-84 core-rat first-extension, iter-85
// runtime third-extension via option (d) consolidation, iter-86
// integrator second-extension). Second extension of this header
// after iter-84.
//
//   Track-rat-profile (doc §2 row 254, n=1):
//     HF_FLAG_SPLIT_RAT_PROFILE -> std::getenv("HF_SPLIT_RAT_PROFILE")
//     Sole call site at src/core/rat_split.cpp:70 (the lambda-initialized
//     static `on` flag inside split_rat_profile_enabled(), gating the
//     iter-35 PIVOTED env-gated wall-share probe for
//     split_rat_by_w_monomial). Default-direction matches docs §2
//     row "unset⇒OFF" verbatim: call-site code reads
//     `if (s==nullptr || *s=='\0') return false;
//      if (s[0]=='0' && s[1]=='\0') return false; return true;`
//     (OFF unless explicitly set to a non-"0" non-empty value). No
//     doc/code discrepancy this Track (unlike iter-65 F8 and iter-86
//     Track-rat-counter, where the docs row and call-site default
//     disagreed).
//
// iter-88 (§T7 eighteenth chunk EXTENSION): one additional doc-§2
// Track family grows here under the same-domain (core/) +
// same-family (VALUE) + same-subsystem (rat) invariants. Twelfth
// precedent of "extend an existing same-domain same-family
// env_flags.hpp" pattern (after iter-69 runtime, iter-74 algebra,
// iter-77 integrator, iter-79 instrumentation, iter-80 algebra
// second-extension, iter-81 diagnostics, iter-83 runtime
// second-extension, iter-84 core-rat first-extension, iter-85
// runtime third-extension via option (d) consolidation, iter-86
// integrator second-extension, iter-87 core-rat second-extension).
// Third extension of this header after iter-84 + iter-87.
//
//   Track-recurrence-probe (doc §2 row 268, n=1):
//     HF_FLAG_TRANSPLANT_RECURRENCE_PROBE ->
//       env-var reference for "HF_TRANSPLANT_RECURRENCE_PROBE"
//     Sole call site at src/core/rat.cpp:522 (the
//     TransplantRecurrenceProbe ctor, gating the Phase-3 §A.3
//     transplant-recurrence audit + wide-ctx stability probe; ON only
//     when the env var begins with the literal character '1').
//     Default-direction matches docs §2 row 268 "unset⇒OFF" verbatim:
//     call-site reads `if (e && e[0] == '1') enabled = true;` (enabled
//     stays false when e is null or e[0] is not '1'). No doc/code
//     discrepancy this Track (matches the iter-87 Track-rat-profile
//     case; unlike iter-65 F8 and iter-86 Track-rat-counter where
//     docs and code defaults disagreed).
//
// iter-89 (§T7 nineteenth chunk EXTENSION): one additional doc-§2
// Track family grows here under the same-domain (core/) +
// same-family (VALUE) + same-subsystem (rat) invariants. Thirteenth
// precedent of "extend an existing same-domain same-family
// env_flags.hpp" pattern (after iter-69 runtime, iter-74 algebra,
// iter-77 integrator, iter-79 instrumentation, iter-80 algebra
// second-extension, iter-81 diagnostics, iter-83 runtime
// second-extension, iter-84 core-rat first-extension, iter-85
// runtime third-extension via option (d) consolidation, iter-86
// integrator second-extension, iter-87 core-rat second-extension,
// iter-88 core-rat third-extension). Fourth extension of this header
// after iter-84 + iter-87 + iter-88.
//
//   Track-lever-v (doc §2 rows 190-191, n=2):
//     HF_FLAG_LEVER_V_ENABLE -> env-var reference for "HF_LEVER_V_ENABLE"
//     HF_FLAG_LEVER_V_PROBE  -> env-var reference for "HF_LEVER_V_PROBE"
//
//     HF_FLAG_LEVER_V_ENABLE gates the iter-12 Lever V (Rat::repswap)
//     single-global + shared_mutex caching variant. Call site at
//     src/core/rat.cpp, inside hf_lever_v_enable(): the cached integer
//     is initialized to std::strtol(e, ...) clamped to [0, 3] when e is
//     non-null and non-empty, else 0. Active set is {1}; values 2/3 are
//     reserved for iter-13 §C.3 sweep variants and treated as disabled
//     in iter-12 source. Default-direction matches docs §2 row 190
//     "unset⇒OFF" verbatim: when e is null or e[0] is '\0', cached is
//     forced to 0 (disabled).
//
//     HF_FLAG_LEVER_V_PROBE gates the Lever V probe (instrumentation
//     side-channel). Call site at src/core/rat.cpp, inside
//     hf_lever_v_probe_enabled(): cached is 1 iff (e && e[0]=='1'),
//     else 0. Default-direction matches docs §2 row 191 "unset⇒OFF"
//     verbatim: when e is null, cached is 0 (disabled). Mirrors the
//     iter-88 Track-recurrence-probe stricter-than-typical
//     "only e[0]=='1' turns it on" idiom; the directional question for
//     the docs row is unambiguous (unset ⇒ OFF).
//
//     No doc/code discrepancy this Track (mirrors iter-87
//     Track-rat-profile and iter-88 Track-recurrence-probe; unlike
//     iter-65 F8 and iter-86 Track-rat-counter where docs and code
//     defaults disagreed).
//
// Header total after iter-89: 9 macros across 6 doc-§2 Tracks
// (Track-cache-toggle iter-74 + Track-rat-legacy iter-84 +
// Track-rat-Q-vs-fmpq iter-84 + Track-rat-profile iter-87 +
// Track-recurrence-probe iter-88 + Track-lever-v iter-89), all
// VALUE-family + core/rat-subsystem, well under the §5.1 F7 ~20-macro
// intra-header growth bound.

#pragma once

#include <cstdlib>

#define HF_FLAG_DISABLE_FROM_CANONICAL          std::getenv("HF_DISABLE_FROM_CANONICAL")
#define HF_FLAG_USE_LEGACY_RAT_ADD              std::getenv("HF_USE_LEGACY_RAT_ADD")
#define HF_FLAG_USE_QUNDERSCORE_RAT_MUL         std::getenv("HF_USE_QUNDERSCORE_RAT_MUL")
#define HF_FLAG_USE_QUNDERSCORE_RAT_SUB         std::getenv("HF_USE_QUNDERSCORE_RAT_SUB")
#define HF_FLAG_USE_QUNDERSCORE_RAT_DIV         std::getenv("HF_USE_QUNDERSCORE_RAT_DIV")
#define HF_FLAG_SPLIT_RAT_PROFILE               std::getenv("HF_SPLIT_RAT_PROFILE")
#define HF_FLAG_TRANSPLANT_RECURRENCE_PROBE     std::getenv("HF_TRANSPLANT_RECURRENCE_PROBE")
#define HF_FLAG_LEVER_V_ENABLE                  std::getenv("HF_LEVER_V_ENABLE")
#define HF_FLAG_LEVER_V_PROBE                   std::getenv("HF_LEVER_V_PROBE")
