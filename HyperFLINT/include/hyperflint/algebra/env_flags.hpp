// HF_FLAG_* macros for algebra-domain environment flags.
//
// Track scope: cache-pf (docs/env_flags.md §T7 sixth chunk; 4 entries
// total; pre-iter1 vintage), Track-cache-toggle algebra subset
// (docs/env_flags.md §T7 seventh chunk; 4 of 6 entries co-located here
// per §5.1 rule 1 + iter-73 reviewer Option ζ), AND Track-cache-lf
// (docs/env_flags.md §T7 eleventh chunk; 3 entries; iter-37/pre-iter1
// vintage; iter-80). The 4 PF-cache + 3 PF-subset cache-toggle + 1
// LF-subset cache-toggle + 3 LF-cache flags are all consumed by
// partial-fraction or linear-factor cache resolvers in `src/algebra/`;
// their effect-domain is algebra (PF-cache and LF-cache respectively),
// and § 5.1 rule 1 (single effect-domain) places them in this header
// alongside the Track-cache-pf VALUE-family macros from iter-71. The
// iter-80 Track-cache-lf addition is a 4th precedent of the "extend an
// existing same-domain same-family env_flags.hpp" pattern (after
// iter-69 runtime, iter-71 algebra, iter-77 integrator extensions,
// and iter-79 instrumentation).
//
// Call sites (refactored to use these macros). Track-decomposed totals
// reflect the post-iter-74 multi-track state of this header (8 macros
// across 2 tracks, NOT the iter-71 single-track snapshot which is
// preserved in git history):
//
//   Track-cache-pf  (4 macros, 6 sites, 1 source file):
//     src/algebra/partial_fractions.cpp : 6 sites (4 unique names; two
//     names are consulted twice, once by the lazy resolver and once
//     by the static-init backstop `PfStorageStaticInit`).
//
//   Track-cache-toggle algebra subset (4 macros, 4 sites, 2 source
//   files; the 4-flag cluster IS split across two `src/algebra/` source
//   files, BUT the per-flag effect-domain remains "algebra (PF-cache or
//   LF-cache)" so § 5.1 rule 1 places all 4 here together):
//     src/algebra/partial_fractions.cpp : 3 sites at L239 (PF-cache
//       enable gate), L247-248 (paired-pointer silent-conjunction
//       opt-in pair). See the silent-conjunction comment block below.
//     src/algebra/linear_factors.cpp     : 1 site at L796 (LF-cache
//       enable gate).
//
//   Track-cache-lf (3 macros, 3 sites, 1 source file; iter-80 §T7
//   eleventh chunk):
//     src/algebra/linear_factors.cpp : 3 sites at L739 (LF-cache
//       shard count, value parsed via strtol with [1,1024] clamp),
//       L816 (LF squarefree-path enable; first-char non-'0' shape),
//       L829 (LF-cache lock-acquire wait-time profile enable;
//       first-char non-'0' shape).
//
//   Total: 13 std::getenv() invocations, 11 unique env-var names,
//   across 2 source files (both under src/algebra/).
//
// Outside src/algebra/, the cluster-collectively-multi-domain
// Track-cache-toggle has TWO sister macros at
// include/hyperflint/core/env_flags_rat.hpp (HF_FLAG_DISABLE_FROM_CANONICAL,
// consumed at src/core/rat.cpp:2336) and
// include/hyperflint/integrator/env_flags.hpp (HF_FLAG_DISABLE_PARALLEL_MERGE,
// consumed at src/integrator/integration_step.cpp:2601). Per the iter-73
// reviewer's binding Option ζ verdict, family-isolation is enforced
// via per-effect-domain subsystem-suffix headers, not by lumping the
// whole cluster into one home. The 4 algebra-domain Track-cache-toggle
// flags live here; the rat-domain and integrator-domain flags live in
// their respective subsystem headers.
//
// The previously-noted comment-only mention at
// src/integrator/hyper_int.cpp:1172 remains parser-B-invisible (iter-66
// strip_comments_cpp pre-pass) and does not enter the registry probe.
//
// Pattern (consistent with iter-62 Track-DAG-probe macro layer at
// `include/hyperflint/instrumentation/env_flags.hpp`, iter-63
// Track-diagnostic-dump at `include/hyperflint/diagnostics/env_flags.hpp`,
// and iter-64/iter-69 Track-mimalloc + Track-narrow-ctx at
// `include/hyperflint/runtime/env_flags.hpp`):
//   #define HF_FLAG_<NAME> std::getenv(<env var literal>)
// The string literals are preserved verbatim in the macro bodies so
// the ctest target `hf-env-flag-registry-coverage` parser-B (regex
// `"HF_[A-Z0-9_]+"` over *.cpp/*.hpp/*.cc/*.h under src/, bridge/,
// include/, comment-stripped per iter-66/67) continues to find them.
// Set S_src is unchanged before and after this refactor; only the
// file in which each literal appears changes (algebra/partial_fractions.cpp
// -> this header).
//
// IMPORTANT (iter-63 lesson): do NOT write any HF_ env-var name as a
// quoted literal in this file's comments, or parser-B will count it
// as an extra source-side entry. The prose above refers to names
// unquoted (HF_PF_STORAGE_STATS, etc.) precisely for that reason.
//
// Track-cache-pf rationale (algebra-domain PF (partial-fraction)
// cache controls; per docs/env_flags.md §5.1 rule 1 (single
// effect-domain "algebra")): the four toggles here are all consumed
// by the partial-fraction cache resolver and storage-stats backstop
// in `src/algebra/partial_fractions.cpp`. They gate:
//   * cache size cap (HF_PF_CACHE_MAX_ENTRIES, numeric value parsed
//     via strtoull; default 200000),
//   * storage-stats dump (HF_PF_STORAGE_STATS, presence + first-char
//     non-'0'),
//   * storage debug asserts (HF_PF_STORAGE_DEBUG_ASSERTS, same
//     presence + first-char shape), and
//   * stats-dump throttle period (HF_PF_STORAGE_STATS_THROTTLE,
//     numeric value parsed via atoi; default 1).
// All four are VALUE-family per docs/env_flags.md §5: each macro
// expands to the full std::getenv(<literal>) expression returning
// `const char*` (NULL when unset), preserving the existing
// call-site contract of strtoull/atoi/first-char inspection without
// shape change.

#pragma once

#include <cstdlib>

// Track-cache-pf (4 flags; see docs/env_flags.md §T7 sixth chunk
// Track-cache-pf inventory and §5.1 rule 1 single-effect-domain;
// iter-71 §T7 sixth chunk).
// All evaluate to `const char*` (the std::getenv return value), NULL when unset.
#define HF_FLAG_PF_CACHE_MAX_ENTRIES           std::getenv("HF_PF_CACHE_MAX_ENTRIES")
#define HF_FLAG_PF_STORAGE_STATS               std::getenv("HF_PF_STORAGE_STATS")
#define HF_FLAG_PF_STORAGE_DEBUG_ASSERTS       std::getenv("HF_PF_STORAGE_DEBUG_ASSERTS")
#define HF_FLAG_PF_STORAGE_STATS_THROTTLE      std::getenv("HF_PF_STORAGE_STATS_THROTTLE")

// Track-cache-toggle PF subset (3 flags; iter-74 §T7 seventh chunk).
// All consumed in `src/algebra/partial_fractions.cpp` (§5.1 rule 1:
// effect-domain is PF-cache, in src/algebra/).
//
// The pair HF_FLAG_ENABLE_KNOWN_BROKEN_PF_CACHE +
// HF_FLAG_I_KNOW_THIS_IS_BROKEN at `partial_fractions.cpp:247-249`
// implements a runtime SILENT CONJUNCTION (NOT a fail-fast abort,
// correcting the iter-65 F6 mis-statement per iter-73 reviewer): the
// lambda at `pf_cache_enabled()` is
//     return (e1 && e1[0] == '1') && (e2 && e2[0] == '1');
// so if exactly one of the two env vars is set to "1", the lambda
// silently returns false and the cache stays disabled. No warning, no
// diagnostic, no abort. The first flag (HF_DISABLE_PF_CACHE) is the
// retired-name warning toggle, also evaluated in the same lambda.
// Future maintainers: do NOT change the conjunction shape and do NOT
// rename one half of the pair without re-reading
// `pf_cache_enabled()` at `partial_fractions.cpp:232-252`.
#define HF_FLAG_DISABLE_PF_CACHE               std::getenv("HF_DISABLE_PF_CACHE")
#define HF_FLAG_ENABLE_KNOWN_BROKEN_PF_CACHE   std::getenv("HF_ENABLE_KNOWN_BROKEN_PF_CACHE")
#define HF_FLAG_I_KNOW_THIS_IS_BROKEN          std::getenv("HF_I_KNOW_THIS_IS_BROKEN")

// Track-cache-toggle LF subset (1 flag; iter-74 §T7 seventh chunk).
// Consumed in `src/algebra/linear_factors.cpp:795`. Per §5.1 rule 1
// the LF-cache effect-domain is also in src/algebra/, so the macro
// co-locates with the PF subset here; both subsets share the same
// effect-domain (algebra) but address distinct caches (PF cache vs
// LF cache).
#define HF_FLAG_DISABLE_LF_CACHE               std::getenv("HF_DISABLE_LF_CACHE")

// Track-cache-lf (3 flags; iter-80 §T7 eleventh chunk). All consumed
// in `src/algebra/linear_factors.cpp` at L739/L816/L829. Per §5.1
// rule 1 these are LF-cache effect-domain (algebra), co-locating
// with the PF and cache-toggle subsets above. Header now holds 4 PF
// + 4 cache-toggle + 3 cache-lf = 11 macros across 3 tracks,
// comfortably under the §5.1 F7 intra-header ~20-macro growth bound.
// Family-isolation note: all 11 macros in this header are
// VALUE-family (expand to a `const char*` from std::getenv); no
// NAME-family macros co-reside, so the iter-65 F2 family-purity
// hazard does not apply.
#define HF_FLAG_LF_CACHE_SHARDS                std::getenv("HF_LF_CACHE_SHARDS")
#define HF_FLAG_LF_SQF                         std::getenv("HF_LF_SQF")
#define HF_FLAG_LF_LOCK_WAIT_PROFILE           std::getenv("HF_LF_LOCK_WAIT_PROFILE")
