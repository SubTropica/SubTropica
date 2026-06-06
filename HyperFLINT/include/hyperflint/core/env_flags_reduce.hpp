// HF_FLAG_* macros for the core/reduce-subsystem env flags.
//
// Track scope: Track-reduce (docs/env_flags.md §T7 eighth chunk;
// docs/env_flags.md §2 Track-reduce, n=5).
//
// SEPARATION FROM core/env_flags.hpp AND core/env_flags_rat.hpp.
// Per §5.1 family-isolation (header-level, as codified iter-75 from
// the iter-73 adversarial-reviewer concerns_binding verdict), a
// single <domain>/env_flags*.hpp must not mix NAME-family and
// VALUE-family macros. core/env_flags.hpp is the NAME-family home
// (iter-65 Track-cache-op-memo). When core/ needs additional
// VALUE-family clusters, each cluster grows in a subsystem-suffix
// sibling header rather than mixing into the NAME-family parent.
// Precedent: core/env_flags_rat.hpp (iter-74 Track-cache-toggle
// core/rat subset). This header extends that precedent to the
// reduce subsystem; it is the second core/ subsystem-suffix
// VALUE-family header.
//
// SCOPE OF SUBSYSTEM-SUFFIX SPLITTING (iter-76 binding clarification).
// The subsystem-suffix rule applied in this header and in
// core/env_flags_rat.hpp is `core/`-domain-specific. It is forced
// by the iter-65 NAME-family precedent at core/env_flags.hpp:
// because the catch-all `core/env_flags.hpp` is NAME-family, every
// new VALUE-family cluster placed in `core/` must split into its
// own sibling header to honor §5.1 family-isolation. Other domains
// (algebra/, integrator/, runtime/, bridge/, diagnostics/) DO NOT
// follow subsystem-suffix splitting; they continue to follow §5.1
// F7 same-domain co-location, accumulating VALUE-family tracks in
// a single per-domain env_flags.hpp until a soft cap or until a
// family conflict arises. Concrete examples that intentionally
// honor F7: algebra/env_flags.hpp (Track-cache-pf + Track-cache-
// toggle + Track-cache-lf, 8 VALUE-family macros across 3 tracks);
// runtime/env_flags.hpp (Track-mimalloc + Track-narrow-ctx, 12
// VALUE-family macros across 2 tracks); integrator/env_flags.hpp
// (Track-cache-toggle + future intra-integrator tracks). Future
// Track-rec1 / Track-poly / Track-zwtable etc. landing in `core/`
// WILL grow as sibling subsystem-suffix headers
// (env_flags_rec1.hpp, env_flags_poly.hpp, ...); a future
// non-`core/` cluster (e.g., a hypothetical second algebra track
// requiring a different family) would re-litigate the rule rather
// than auto-applying subsystem-suffix.
//
// SUBSYSTEM-SUFFIX vs FAMILY-SUFFIX axis (per docs/env_flags.md
// §5.1 A1 codified iter-75). Within the `core/`-specific rule
// above, the split is named for the source subsystem (reduce)
// rather than the macro family (value), so that future
// Track-reduce growth aggregates here organically while other
// core/ subsystems keep their own sibling headers. Family-suffix
// would invent a new naming axis and force this header to be
// re-split later if a sixth Track-reduce-NAME-family flag ever
// arrived.
//
// FAMILY-ISOLATION enforcement. The §T9 oracle-1 (LANDED iter-75)
// asserts unique-home for every HF_FLAG_<NAME> #define across the
// repository, and oracle-2 asserts single-header for every
// "HF_<NAME>" literal. Together they catch the consequences of a
// family-mixing mistake (duplicate macros, multi-header literals)
// but do NOT directly assert per-header family purity (i.e.,
// nothing structurally prevents a future maintainer from adding a
// NAME-family macro to this header). The §5.1 family-isolation
// invariant is therefore enforced by convention at refactor time;
// a per-header family-purity ctest gate is deferred follow-up
// work (iter-74 F8b).
//
// CROSS-CLUSTER NOTE — HF_NO_NARROW_REDUCE. The runtime/env_flags.hpp
// macro HF_FLAG_NO_NARROW_REDUCE (iter-69 §T7 fifth chunk
// Track-narrow-ctx) is reduce-related by name but presently lives
// at runtime/env_flags.hpp because the iter-65 F5 header-location
// rationale identified the toggle TARGET (narrow-context
// PolyCtx) as the effect-domain rather than the toggle GATING
// CALLER (the reduce algorithm). The iter-69 docstring there
// claims call sites span bridge/ + core/rat + core/poly; an
// iter-76 grep finds the literal in only src/core/rat.cpp today,
// so the multi-domain rationale is empirically stale. The macro
// is NOT migrated here in iter-76 LAND because the migration is
// substantive (touches a second header) and properly belongs to
// the next Track-narrow-ctx-touching refactor or a dedicated
// header-location revisit. Future maintainers reading this header
// who want to consolidate all reduce-related flags should weigh
// the iter-65 F5 "toggle-target effect-domain" interpretation
// against a "toggle-gating-caller" reading; both have precedent.
//
// Pattern: VALUE-family per docs/env_flags.md §5. Each macro
// expands to the full std::getenv(<literal>) expression returning
// const char* (NULL when unset). Caller is responsible for
// NULL-check + dereference (typical idiom:
//   const char* e = HF_FLAG_REDUCE_NTERM_LOG;
//   if (e && e[0] == '1') ...
// or, for integer-parsing flags:
//   const char* e = HF_FLAG_REDUCE_SIZE_GATE_MIN;
//   if (!e || !*e) return slong{4};
//   long v = std::strtol(e, nullptr, 10);
//   return v > 0 ? static_cast<slong>(v) : slong{4};
// ). String literals are preserved verbatim so the ctest target
// hf-env-flag-registry-coverage parser-B regex continues to find
// them in this header (set S_src is unchanged in element count; the
// 5 reduce literals just relocate from src/core/rat.cpp to here).
//
// Call sites (all in src/core/rat.cpp, refactored to consume these
// macros; line numbers reference the post-LAND iter-76 commit and
// will drift as rat.cpp evolves — symbol-name anchors are stable):
//   HF_FLAG_REDUCE_NTERM_LOG       — reduce_nterm_log_enabled
//                                    Meyers-singleton; controls the
//                                    pre/post-reduce nterm-delta
//                                    diagnostic accumulators.
//   HF_FLAG_REDUCE_SUPPORT_PROBE   — ReduceSupportProbe ctor;
//                                    controls the support-key /
//                                    full-key bucket dump emitted
//                                    at dtor / process exit.
//   HF_FLAG_REDUCE_SIZE_GATE_MIN   — reduce_inplace size-gate-minimum
//                                    static lambda and the
//                                    narrow-decision size-gate-minimum
//                                    static lambda. Default 4 when
//                                    unset/0; positive int overrides.
//   HF_FLAG_REDUCE_SIZE_GATE_DIVISOR — reduce_inplace adaptive
//                                    per-call divisor static lambda
//                                    and the narrow-decision adaptive
//                                    divisor static lambda. Default 0
//                                    (disabled) when unset/empty;
//                                    positive int overrides _MIN.
//   HF_FLAG_REPSWAP_NVARS_MIN       — repswap_nvars_min()
//                                    Meyers-singleton; routes Rat::add
//                                    to add_legacy when nvars below
//                                    threshold; default 50 in source
//                                    when unset/empty (and now in the
//                                    registry: docs/env_flags.md §2
//                                    Track-reduce entry was updated
//                                    iter-78 to `unset⇒50 (no rep-swap
//                                    below 50 vars)`, resolving the
//                                    iter-76 F3 advisory; the prior
//                                    `unset⇒0 (no threshold)` registry
//                                    text was incorrect). 0 forces
//                                    rep-swap unconditional, very
//                                    large forces legacy unconditional.
//
// Future growth: additional core/reduce-subsystem env flags would
// grow here (e.g., a hypothetical HF_REDUCE_PARITY_GATE). Other
// core/ subsystems (rec1, poly, ZWTable, ...) needing their own
// VALUE-family env flags should add sibling headers (env_flags_rec1
// .hpp / env_flags_poly.hpp / ...) per the `core/`-domain
// subsystem-suffix rule above, NOT extend this one beyond the
// reduce subsystem.

#pragma once

#include <cstdlib>

#define HF_FLAG_REDUCE_NTERM_LOG        std::getenv("HF_REDUCE_NTERM_LOG")
#define HF_FLAG_REDUCE_SUPPORT_PROBE    std::getenv("HF_REDUCE_SUPPORT_PROBE")
#define HF_FLAG_REDUCE_SIZE_GATE_MIN    std::getenv("HF_REDUCE_SIZE_GATE_MIN")
#define HF_FLAG_REDUCE_SIZE_GATE_DIVISOR std::getenv("HF_REDUCE_SIZE_GATE_DIVISOR")
#define HF_FLAG_REPSWAP_NVARS_MIN       std::getenv("HF_REPSWAP_NVARS_MIN")
