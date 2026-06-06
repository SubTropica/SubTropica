// HF Phase 2 Track 7 (iter-62 / iter-79+): HF_FLAG_* macro layer for the
// instrumentation-domain env-var families. Wraps std::getenv() into
// single-token macros so the call sites under src/instrumentation/ reference
// one canonical name per flag rather than re-typing the raw string.
//
// Coverage-gate contract (test/unit/test_env_flag_registry.cpp): parser-B
// scans `*.cpp / *.hpp / *.cc / *.h` under `src/`, `bridge/`, `include/`
// for the regex `"HF_[A-Z0-9_]+"` (literal-string union). Each macro body
// below contains its corresponding env-var name as a quoted C string, so
// the registry scanner finds the same set of names whether the literals
// live at the call sites or here.
//
// Tracks resident in this header (call-sites per docs/env_flags.md §2):
//   - Track-DAG-probe (iter-62, n=4): src/instrumentation/dag_hashcons_probe.cpp
//   - Track-rec1 (iter-79, n=3): src/instrumentation/mpz_pool_probe.cpp
//
// Per docs/env_flags.md §5.1 rule 1 (single effect-domain) + iter-65 F2
// header-level family-isolation: both Tracks are VALUE-family and
// instrumentation-domain (probe/dump tooling that introspects but does not
// alter algebraic state). They cohabit cleanly inside this one header
// without crossing the family-mixing hazard (iter-65 F2 / iter-73 §T9 oracle).

#pragma once

#include <cstdlib>

// Track-DAG-probe (4 flags; see docs/env_flags.md §2 "Track-DAG-probe").
// All evaluate to `const char*` (the std::getenv return value), NULL when unset.
#define HF_FLAG_DAG_HASHCONS_PROBE              std::getenv("HF_DAG_HASHCONS_PROBE")
#define HF_FLAG_DAG_HASHCONS_PROBE_GMP_LABELLED std::getenv("HF_DAG_HASHCONS_PROBE_GMP_LABELLED")
#define HF_FLAG_DAG_HASHCONS_PROBE_OUT_DIR      std::getenv("HF_DAG_HASHCONS_PROBE_OUT_DIR")
#define HF_FLAG_DAG_HASHCONS_PROBE_NDJSON       std::getenv("HF_DAG_HASHCONS_PROBE_NDJSON")

// Track-rec1 (3 flags; see docs/env_flags.md §2 "Track-rec1 (n=3)").
// REC-1 mpz-pool tracker probe family. All 3 call sites in
// src/instrumentation/mpz_pool_probe.cpp:
//   HF_FLAG_REC1_TRACK_MPZ_POOL    -> hf_rec1_init() master gate
//   HF_FLAG_REC1_LABEL_HEADER_CHECK -> §3.2.1 page-header signature sub-flag
//   HF_FLAG_REC1_OUT_DIR           -> snapshot output directory
// All VALUE-family; evaluate to `const char*` (NULL when unset). Bind to a
// local once per resolution path per §5 VALUE-family convention.
#define HF_FLAG_REC1_TRACK_MPZ_POOL     std::getenv("HF_REC1_TRACK_MPZ_POOL")
#define HF_FLAG_REC1_LABEL_HEADER_CHECK std::getenv("HF_REC1_LABEL_HEADER_CHECK")
#define HF_FLAG_REC1_OUT_DIR            std::getenv("HF_REC1_OUT_DIR")
