// HF_FLAG_* macros for integrator-domain env flags.
//
// Track scope: cache-toggle (docs/env_flags.md §T7 seventh chunk
// Track-cache-toggle, parallel-merge subset; n=1: the from_monomials
// parallel-merge kill switch) + probe-ctx (docs/env_flags.md §T7
// ninth chunk Track-probe-ctx, iter-77; n=4: the four wide-ctx
// instrumentation flags) + rat-counter (docs/env_flags.md §T7
// sixteenth chunk Track-rat-counter, iter-86; n=1: the Avenue-F
// PolesBucket flush ref-count bump gate at integration_step.cpp) +
// section-6D-DFS (docs/env_flags.md §T7 twentieth chunk
// Track-6D-DFS, iter-90; n=1: the REQ-21.4 BINDING placement
// invariant DFS thread-cap gate at section_6d_dfs_cap.cpp) +
// OMP-integrator (docs/env_flags.md §T7 twenty-fourth chunk
// Track-OMP, iter-94; n=2 of cluster=3: the macOS QoS promotion
// gate at hf_promote_qos_once() and the per-term closure parallel-
// for gate at the closure pass in integration_step.cpp; the third
// cluster member HF_FLAG_MAX_THREADS_PER_CALL lands in the new
// bridge/env_flags.hpp under iter-94 §5.1 rule-1 split-by-effect-
// domain placement — see iter-94 adversarial-reviewer Q-19 B1
// substantive-pattern BINDING verdict) + gcd-chunk
// (docs/env_flags.md §T7 twenty-fifth chunk Track-gcd-chunk,
// iter-95; n=4: the Phase 1 Task 1.E dispatch_apply gate and its
// verbose-mode diagnostic, plus the Phase 3 section B Lever U
// chunk-size knob and chunk-probe JSONL emitter, all in
// src/integrator/gcd_dispatch.cpp) + diagnostic-dump partial
// integrator portion (docs/env_flags.md §T7 twenty-sixth chunk
// Track-diagnostic-dump partial, iter-96; n=2 of cluster=4: the
// opt-in hashcons bucket-distribution probe inside the
// integration_step.cpp post-canonicalize aggregator (line ~2545)
// and the iter-83 single-iter relic input-wordlist diagnostic
// dump at the integrate_ii entry boundary in primitive.cpp
// (line ~211); the other two cluster members land in
// bridge/env_flags.hpp and a new core/env_flags_poly.hpp under
// iter-96 §5.1 rule-1 split-by-effect-domain placement — see
// iter-96 adversarial-reviewer Q-19 B1 substantive-pattern
// BINDING verdict).
//
// FIRST integrator-domain env_flags header (precedent: iter-71
// algebra/env_flags.hpp was the first algebra-domain env_flags
// header, created from scratch with 4 VALUE-family macros for
// Track-cache-pf; iter-74 follows the same shape here). Future
// integrator-domain clusters (Track-iter-relic + Track-debug-
// legacy retirements, etc.) should grow here in place per the
// §5.1 F7 intra-header soft growth bound (~20 macros / ~3
// tracks before subsystem-suffix split). Track-OMP per-term-
// closure + Track-OMP QoS are LANDED iter-94 as the fourth
// extension after iter-77 (Track-probe-ctx), iter-86 (Track-rat-
// counter), and iter-90 (Track-6D-DFS); macro count 7/20 -> 9/20
// post-iter-94. Track-gcd-chunk is LANDED iter-95 as the fifth
// extension; macro count 9/20 -> 13/20 post-iter-95.
// Track-diagnostic-dump partial integrator portion is LANDED
// iter-96 as the sixth extension (n=2 of the 4-member cluster
// resolved as a 3-way split: 2 macros here, 1 in bridge/, 1 in
// the new core/env_flags_poly.hpp); macro count 13/20 -> 15/20
// post-iter-96.
//
// CROSS-DOMAIN RULE-3 APPLICATION (iter-77 Track-probe-ctx).
// One of the four Track-probe-ctx call sites lives in the bridge
// domain (src/bridge/handlers.cpp, the wide-ctx campaign's R20
// step-2 per-face request-body dump at the hyperflint_sym entry),
// not in the integrator domain. Placing all four macros here
// invokes §5.1 rule-3 ("when call sites span multiple effect-
// domains, prefer the lower-level domain on the partial order
// core ≺ algebra ≈ integrator ≺ runtime ≺ bridge" — iter-75
// codification) for the first time. The three integrator sites
// (src/integrator/ctx_probe.cpp init_enabled() and the
// CTX_USAGE_PATH emit-time tail; src/integrator/hyper_int.cpp
// parity1_probe_enabled()) dominate the cluster by 3:1 call-site
// count, and the bridge site emits debug data shaped by integrator
// behavior. The lower-level domain (integrator) is therefore the
// natural home. Line numbers are intentionally omitted here in
// favor of symbol-name anchors that survive source-file edits;
// the Call-sites block below carries the same convention.
//
// DIAGNOSTICS-OFF-SPINE CAVEAT (iter-75 §5.1 rule-3 amendment).
// The iter-75 codification of rule-3 explicitly placed
// diagnostics/instrumentation flags OFF the partial order spine,
// reasoning that probe flags exist to observe rather than to gate
// production behavior, so cross-domain co-location decisions for
// probes should be made on naming and observability grounds rather
// than mechanically applying rule-3. The Track-probe-ctx cluster
// satisfies both readings: rule-3 mechanically picks integrator
// (lower-level than bridge), and the off-spine reading also picks
// integrator (three of four sites integrator-domain by name, the
// cluster name itself contains "ctx" which is integrator-scoped).
// This is the first iteration that explicitly applies rule-3 to a
// cross-domain cluster — earlier iterations had every cluster
// confined to a single effect-domain.
//
// Pattern: VALUE-family per docs/env_flags.md §5. Each macro
// expands to the full std::getenv(<literal>) expression returning
// const char* (NULL when unset). String literals are preserved
// verbatim so the ctest target hf-env-flag-registry-coverage
// parser-B regex continues to find them. Set S_src is unchanged
// before and after the iter-77 refactor in element count; the 4
// probe literals just relocate from src/ to this header.
//
// Call sites:
//   HF_FLAG_DISABLE_PARALLEL_MERGE (iter-74 Track-cache-toggle)
//     — src/integrator/integration_step.cpp from_monomials
//       parallel-merge dispatch (one site, inside the
//       kill_parallel_merge local that gates do_parallel_merge
//       under HF_HAVE_OPENMP).
//   HF_FLAG_PROBE_CTX_USAGE (iter-77 Track-probe-ctx)
//     — src/integrator/ctx_probe.cpp init_enabled() static
//       initializer; gates the per-process ctx-usage probe entirely.
//   HF_FLAG_PROBE_CTX_USAGE_PATH (iter-77 Track-probe-ctx)
//     — src/integrator/ctx_probe.cpp emit-time tail; when set to
//       a path, the probe also appends one line per call to the
//       file (used by SubTropica/Mma in-dylib callers whose stderr
//       is not captured).
//   HF_FLAG_PROBE_PARITY1 (iter-77 Track-probe-ctx)
//     — src/integrator/hyper_int.cpp parity1_probe_enabled() helper;
//       gates the persistent-state walk emitted immediately after
//       regulator_sym_as_shuffle_list_sym().
//   HF_FLAG_PROBE_DUMP_DIR (iter-77 Track-probe-ctx, bridge call site)
//     — src/bridge/handlers.cpp R20 step-2 instrumentation; when
//       set to a directory, every entry of hyperflint_sym writes
//       its JSON body to <dir>/face_<NNN>.json. Bridge-domain
//       call site placed here under §5.1 rule-3 cross-domain
//       application (see CROSS-DOMAIN RULE-3 APPLICATION above).
//   HF_FLAG_DEFER_BUMP (iter-86 Track-rat-counter, n=1)
//     — src/integrator/integration_step.cpp defer_bump_enabled()
//       (Avenue F gate). One-shot static-initializer probe inside
//       namespace detail; gates a PolesBucket flush optimization
//       that defers reference-count bumps. The call-site gate
//       semantics are default-ON ("ON unless explicitly set to
//       '0'"), in contrast to the docs/env_flags.md §2 table's
//       `unset⇒OFF` column for this row; the discrepancy is
//       call-site interpretation, not a macro-layer concern (the
//       macro here is a plain VALUE-family std::getenv wrapper).
//       Tenth precedent of "extend an existing same-domain
//       same-family env_flags*.hpp" pattern. Track-rat-counter is
//       a singleton (n=1) and lives in the rat-subsystem of the
//       integrator domain (the gate's effect-domain is integrator
//       even though the literal's mention-domain ("HF_DEFER_BUMP")
//       carries no rat-prefix; iter-71 PINNED lesson
//       `iter71_section5_1_rule_1_places_by_effect_domain_not_mention_domain`
//       applies).
//   HF_FLAG_SECTION_6D_DFS_THREAD_CAP (iter-90 Track-6D-DFS, n=1)
//     — src/integrator/section_6d_dfs_cap.cpp
//       read_dfs_cap_env_uncached() (anon-ns helper); gates the
//       REQ-21.4 BINDING placement-invariant DFS thread cap. The
//       outer parse_dfs_cap_env() caches the result in a function-
//       local static after the first call. Default-OFF (no cap):
//       returns 0 when the env var is null, empty, or parses to
//       a non-positive integer; returns the positive integer cap
//       otherwise. The call-site default-direction matches the
//       docs/env_flags.md §2 row 56 column `unset⇒0 (no cap)`
//       verbatim (no docs/code discrepancy). Fourteenth precedent
//       of "extend an existing same-domain same-family
//       env_flags*.hpp" pattern; *third* extension of this header
//       (after iter-77 Track-probe-ctx and iter-86 Track-rat-
//       counter). Single-effect-domain (integrator/) and single
//       TU; the symbol-name-anchor convention used by the iter-77
//       block is preserved here (no rat-subsystem suffix because
//       6D-DFS is an integrator-domain section-helper, not a
//       core/rat helper).
//   HF_FLAG_QOS_USER_INITIATED (iter-94 Track-OMP, integrator
//   portion, n=1 of cluster=3)
//     — src/integrator/integration_step.cpp hf_promote_qos_once()
//       (file-static helper guarded by __APPLE__). Per-thread
//       one-shot QoS-class promotion of integrator's OMP worker
//       threads to QOS_CLASS_USER_INITIATED on macOS via
//       pthread_set_qos_class_self_np. The thread_local one-shot
//       guard ensures each worker promotes itself exactly once
//       per thread-lifetime (across all parallel regions, not
//       per iteration). Call-site predicate-family: exact-match
//       "0" opt-out — promotion is ON when the env var is unset,
//       empty, or any value other than "0"; OFF only when value
//       is literally "0". Effect-domain is integrator (the
//       function is integrator-resident; the effect is on the
//       integrator's OMP workers' macOS QoS class). Sixth
//       precedent of "single-effect-domain integrator macro that
//       could have been misread as runtime-threading-effect via
//       the §5.1 rule-3 lower-level-domain tiebreaker"; the
//       iter-94 adversarial-reviewer ruled rule-1 disposes the
//       question because file-static state inside integration_
//       step.cpp is unambiguously integrator effect-domain.
//   HF_FLAG_OMP_PER_TERM_CLOSURE (iter-94 Track-OMP, integrator
//   portion, n=2 of cluster=3)
//     — src/integrator/integration_step.cpp magic-static
//       s_per_term_closure_disabled cached once-init lambda
//       inside the HF_HAVE_OPENMP block; gates per-term OMP
//       parallel-for at the closure pass (the per-bin closure
//       pass dominates Step 4 of tst2, the largest absolute
//       HF-vs-Maple gap per docs/hf_vs_hyperint_tst2_diagnostic.md).
//       Default-ON when n >= 8 (the existing team-overhead
//       amortization gate); OFF only when env var is literally
//       "0". Call-site predicate-family: exact-match "0" opt-out.
//       The call-site default-direction matches docs/env_flags.md
//       §2 row 9 column `unset⇒ON when n>=8` verbatim (no docs/
//       code discrepancy). Single-effect-domain (integrator/) and
//       single TU. Fifteenth precedent of "extend an existing
//       same-domain same-family env_flags*.hpp" pattern; *fourth*
//       extension of this header (after iter-77 Track-probe-ctx,
//       iter-86 Track-rat-counter, and iter-90 Track-6D-DFS).
//   HF_FLAG_USE_GCD (iter-95 Track-gcd-chunk, n=1 of cluster=4)
//     — src/integrator/gcd_dispatch.cpp gcd_dispatch_enabled()
//       at line ~181, thread_local-cached reader. Master gate for
//       Phase 1 Task 1.E dispatch_apply parallel-for path. Per
//       the 2026-05-09 Phase 1-Verdict comment block at line ~174,
//       the slot-collision fix (commit 81ef8a0df) restored
//       correctness and delivered tst2 -2.18% wall vs hyperflint_v1,
//       so GCD dispatch is the new default and the env var is now
//       an opt-back-to-OMP knob (used for bit-byte sha-id checks
//       against pre-flip binaries). Call-site predicate-family:
//       exact-match "0" opt-out — `cached = (v && std::strcmp(v,
//       "0") == 0) ? 0 : 1`. Default-when-unset = ON (cached==1).
//       Docs §2 row 177 says `unset⇒OFF` and retire=Y "RETIRED
//       iter-7"; both are STALE — the literal was un-retired by
//       the 2026-05-09 default-ON flip and is actively read each
//       dispatch. NEW EIGHTH-FORM docs/code discrepancy in two
//       senses (default-direction AND retire-status), logged as
//       advisory per iter-86 lesson `iter86_docs_code_default_
//       direction_discrepancy_is_advisory`; not LAND-blocking.
//       Single-effect-domain (integrator/) and single TU.
//   HF_FLAG_USE_GCD_VERBOSE (iter-95 Track-gcd-chunk, n=2 of
//   cluster=4)
//     — src/integrator/gcd_dispatch.cpp gcd_dispatch_verbose()
//       at line ~193, thread_local-cached reader (anon-ns static
//       helper). Diagnostic emitter for the dispatch_parallel_for
//       entry print at line ~243; gates whether each
//       dispatch_apply call emits a stderr line of the form
//       `GCD dispatch_apply n=%zu max_slots=%d`. Call-site
//       predicate-family: exact-match "1" opt-in — `cached = (v &&
//       std::strcmp(v, "1") == 0) ? 1 : 0`. Default-when-unset =
//       OFF. Docs §2 row 178 column `unset⇒OFF` MATCHES the
//       call-site behavior verbatim; the retire=Y "RETIRED" tag
//       in row 178 is STALE — the literal is still actively read
//       by `gcd_dispatch_verbose()` and called from
//       `dispatch_parallel_for()`. Retire-status-stale advisory
//       logged; default-direction OK. Single-effect-domain
//       (integrator/) and single TU.
//   HF_FLAG_GCD_CHUNK_SIZE (iter-95 Track-gcd-chunk, n=3 of
//   cluster=4)
//     — src/integrator/gcd_dispatch.cpp hf_gcd_chunk_size() at
//       line ~210, thread_local-cached reader. Phase 3 section B
//       Lever U knob for tuning the chunk size of the
//       dispatch_apply parallel-for body. Call-site predicate-
//       family: POSITIVE_INTEGER atoi-then-clamp-to-min-1 — when
//       env is unset, empty, or parses to a non-positive integer,
//       the cache stores 1; otherwise stores the positive integer.
//       Default-when-unset = 1, byte-identical to the Phase 1
//       Task 1.E implementation. Docs §2 row 176 column `unset⇒1`
//       MATCHES the call-site behavior verbatim (no docs/code
//       discrepancy). Retire=n in docs row 176. Single-effect-
//       domain (integrator/) and single TU.
//   HF_FLAG_GCD_CHUNK_PROBE (iter-95 Track-gcd-chunk, n=4 of
//   cluster=4)
//     — src/integrator/gcd_dispatch.cpp hf_gcd_chunk_probe_enabled()
//       at line ~232, thread_local-cached reader. Phase 3 section B
//       Lever U probe gate: when ON, every chunk emits a JSONL line
//       on stderr with per-chunk wall, sema-wait wall, slot-
//       acquisition wall, body-summed wall, chunk_idx, chunk_size,
//       slot, n_entries. Iter-9 chunk-size sweep aggregator consumes
//       these JSONLs. Call-site predicate-family: exact-match "1"
//       opt-in — `cached = (v && std::strcmp(v, "1") == 0) ? 1 : 0`.
//       Default-when-unset = OFF (no emission, no measurement). Docs
//       §2 row 175 column `unset⇒OFF` MATCHES the call-site behavior
//       verbatim (no docs/code discrepancy). Retire=n in docs row
//       175. Single-effect-domain (integrator/) and single TU.
//       Format mirrors HF_STEP_TRACE JSONL convention at
//       integration_step.cpp:~2034.
//   HF_FLAG_BUCKET_HASH_STATS (iter-96 Track-diagnostic-dump
//   partial, integrator portion, n=1 of cluster=4)
//     — src/integrator/integration_step.cpp line ~2545, inline
//       conditional inside the post-canonicalize merged-flat-map
//       sanity probe added 2026-04-26 (a-prime lever). When set
//       to the literal "1", the probe emits one stderr JSON line
//       summarizing the bucket distribution of the merged
//       flat-map: keys, buckets, nonempty_buckets, max_bucket_size,
//       load_factor. Reviewer threshold (per the 2026-04-26
//       comment) was max bucket size <= 4 (FNV-1a 128-bit at
//       ~10^5 keys -> expected <= 2). Call-site predicate-family:
//       exact-match "1" opt-in — `env && env[0] == '1'`.
//       Default-when-unset = OFF. Docs §2 row 71 column
//       `unset⇒OFF` matches verbatim (no docs/code discrepancy);
//       retire=n. Single-effect-domain (integrator/) and single
//       TU.
//   HF_FLAG_ITER83_DUMP (iter-96 Track-diagnostic-dump partial,
//   integrator portion, n=2 of cluster=4)
//     — src/integrator/primitive.cpp line ~211, inline
//       conditional at the integrate_ii entry boundary. When
//       set (TRUTHY: non-null pointer), the gate prints the
//       input wordlist (every (coef.num, coef.den, word) entry)
//       before queue construction so HJ-vs-HF diff at the
//       integrate_ii entry boundary is possible. Call-site
//       predicate-family: TRUTHY `if (std::getenv(...))`.
//       Default-when-unset = OFF. Docs §2 row 184 column
//       `unset⇒OFF` matches the call-site behavior verbatim
//       (no default-direction discrepancy). The docs §2 retire=Y
//       tag is STALE — the literal is still actively read at
//       the call site, contradicted directly by the live code
//       and by the docs §5.2 narrative line ~299 which claims
//       "no current callers" (also stale; primitive.cpp:211 IS
//       a current caller). Logged as advisory per the iter-86
//       lesson, dissolved per the iter-95 retire=Y dissolution
//       precedent (docs/§5.2 staleness, source canonical). The
//       iter-96 adversarial-reviewer recommendation specifically
//       notes that this iteration-tagged probe is a candidate
//       for §T8 retirement-sweep consideration: macro-layer
//       consolidation here does NOT assert continued relevance.
//       Single-effect-domain (integrator/) and single TU.
// Iter-95 §T7 twenty-fifth chunk Track-gcd-chunk: sixteenth precedent
// of "extend an existing same-domain same-family env_flags*.hpp"
// pattern; *fifth* extension of this header (after iter-77 Track-
// probe-ctx, iter-86 Track-rat-counter, iter-90 Track-6D-DFS, and
// iter-94 Track-OMP integrator portion). Single-effect-domain
// (integrator/) and single TU (all four call sites in src/integrator/
// gcd_dispatch.cpp). NO reviewer dispatch this Track: while the
// retire=Y question in docs §2 rows 177-178 raised a substantive-
// pattern question in the handoff prediction, empirical Step 7b
// dissolved it — both retire=Y literals are demonstrably active
// source-side (HF_USE_GCD un-retired by 2026-05-09 default-ON flip;
// HF_USE_GCD_VERBOSE still called from dispatch_parallel_for at
// line ~243). The retire= staleness is a docs/code discrepancy of
// the same shape covered by the iter-86 advisory-not-blocking
// lesson. Sixteenth and seventeenth advisory of the iter-86 form
// (HF_USE_GCD default-direction + retire-status; HF_USE_GCD_VERBOSE
// retire-status only).
//
// Iter-96 §T7 twenty-sixth chunk Track-diagnostic-dump partial
// (integrator portion): seventeenth precedent of "extend an
// existing same-domain same-family env_flags*.hpp" pattern;
// *sixth* extension of this header. The full cluster is genuinely
// cross-domain (integrator x2 + bridge x1 + core x1) and resolved
// as a 3-way split-placement per §5.1 rule-1 strict-priority,
// directly applying the iter-94 split-placement precedent. The
// integrator portion is single-effect-domain across the two
// included macros and across each of their single TUs. Reviewer
// dispatched this Track because of the genuine cross-domain
// question AND the iter-73 family-isolation collision in core/
// (the only NAME-pure header among the seven domain catch-alls);
// the BINDING verdict applied the iter-76 subsystem-suffix rule
// to core/ and created core/env_flags_poly.hpp as the
// iter-76-anticipated sibling of core/env_flags_rat.hpp and
// core/env_flags_reduce.hpp. Three retire=Y dissolutions logged
// (HF_DEBUG, HF_DEBUG_DISC_DUMP, HF_ITER83_DUMP) per the iter-95
// dissolution precedent; HF_ITER83_DUMP additionally flagged
// as iteration-tagged candidate for future §T8 retirement-sweep
// per the iter-96 reviewer Recommendation 3.
//
// Family-isolation note (per iter-73 reviewer concerns_binding,
// extension of iter-65 F2 from cluster-level to header-level): this
// header holds VALUE-family macros only. If the integrator domain
// ever needs NAME-family macros (e.g. for an operator_memo-style
// indirection), they MUST land in a sibling subsystem-suffix header
// (e.g. integrator/env_flags_<subsystem>.hpp) rather than co-reside
// with VALUE-family macros in this file. The iter-65 F2 hazard
// reader-confusion footgun applies symmetrically to either-direction
// family mixing.
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

#define HF_FLAG_DISABLE_PARALLEL_MERGE       std::getenv("HF_DISABLE_PARALLEL_MERGE")
#define HF_FLAG_PROBE_CTX_USAGE              std::getenv("HF_PROBE_CTX_USAGE")
#define HF_FLAG_PROBE_CTX_USAGE_PATH         std::getenv("HF_PROBE_CTX_USAGE_PATH")
#define HF_FLAG_PROBE_PARITY1                std::getenv("HF_PROBE_PARITY1")
#define HF_FLAG_PROBE_DUMP_DIR               std::getenv("HF_PROBE_DUMP_DIR")
#define HF_FLAG_DEFER_BUMP                   std::getenv("HF_DEFER_BUMP")
#define HF_FLAG_SECTION_6D_DFS_THREAD_CAP    std::getenv("HF_SECTION_6D_DFS_THREAD_CAP")
#define HF_FLAG_QOS_USER_INITIATED           std::getenv("HF_QOS_USER_INITIATED")
#define HF_FLAG_OMP_PER_TERM_CLOSURE         std::getenv("HF_OMP_PER_TERM_CLOSURE")
#define HF_FLAG_USE_GCD                      std::getenv("HF_USE_GCD")
#define HF_FLAG_USE_GCD_VERBOSE              std::getenv("HF_USE_GCD_VERBOSE")
#define HF_FLAG_GCD_CHUNK_SIZE               std::getenv("HF_GCD_CHUNK_SIZE")
#define HF_FLAG_GCD_CHUNK_PROBE              std::getenv("HF_GCD_CHUNK_PROBE")
#define HF_FLAG_BUCKET_HASH_STATS            std::getenv("HF_BUCKET_HASH_STATS")
#define HF_FLAG_ITER83_DUMP                  std::getenv("HF_ITER83_DUMP")
