// HF_FLAG_* macros for diagnostics-domain environment flags.
//
// Tracks resident in this header (post-iter-92):
//   * Track-diagnostic-dump (docs/env_flags.md §2 Track-diagnostic-dump,
//     17 entries total; 13 macro-wrapped here, 4 excluded -- see below).
//   * Track-structural-sharing-probe (docs/env_flags.md §2
//     Track-structural-sharing-probe, n=2; both macro-wrapped here).
//   * Track-RSS-probe (docs/env_flags.md §2 Track-RSS-probe, n=1; the
//     singleton macro-wrapped here at iter-92, third extension of this
//     header after the iter-63 Track-diagnostic-dump original LAND and
//     the iter-81 Track-structural-sharing-probe second extension).
//
// All three tracks are VALUE-family (every macro below expands to a
// `std::getenv("...")` call returning `const char*`) and all three
// control diagnostics-domain probes/dumpers. The header therefore hosts
// 13 + 2 + 1 = 16 macros across 3 tracks, under the §5.1 F7 ~20-macro
// intra-header growth bound, and respects §5.1 rule 1 (single
// effect-domain placement: diagnostics) plus the iter-65 F2
// family-purity invariant (no NAME-family macros mixed in).
//
// EXCLUDED from Track-diagnostic-dump (4 entries; deliberately NOT
// macro-wrapped here):
//   HF_DUMP_FACTOR_INPUTS, HF_DUMP_FACTOR_MAX,
//   HF_DUMP_GCD_INPUTS,    HF_DUMP_GCD_MAX.
// These live as string literals at the OpDumper factory call sites in
// `include/hyperflint/diagnostics/op_input_dumper.hpp` (lines 100, 156).
// The factory ctor takes the env-var NAME as a const char* parameter
// and calls std::getenv() internally. The literal name is therefore
// already at the high-level (single-line) factory call site, so the
// macro layer would require rearchitecting the OpDumper interface to
// take the FETCHED value rather than the name. That refactor is
// outside the §T7 macro-layer scope.
//
// Pattern (consistent with iter-62 Track-DAG-probe macro layer at
// `include/hyperflint/instrumentation/env_flags.hpp`):
//   #define HF_FLAG_<NAME> std::getenv(<env var literal>)
// The string literals are preserved verbatim in the macro bodies so
// the ctest target `hf-env-flag-registry-coverage` parser-B (regex
// `"HF_[A-Z0-9_]+"` over *.cpp/*.hpp/*.cc/*.h under src/, bridge/,
// include/) continues to find them. Set S_src is unchanged before and
// after this refactor; only the file in which each literal appears
// changes (source file -> this header).

#pragma once

#include <cstdlib>

// Operand-sparsity probe (src/diagnostics/operand_sparsity.cpp).
#define HF_FLAG_DUMP_OPERAND_SPARSITY std::getenv("HF_DUMP_OPERAND_SPARSITY")
#define HF_FLAG_OPERAND_SPARSITY_RATE std::getenv("HF_OPERAND_SPARSITY_RATE")

// Rat-add quadruple dumper (src/core/rat.cpp::RatAddQuadDumper).
#define HF_FLAG_DUMP_RAT_QUADS            std::getenv("HF_DUMP_RAT_QUADS")
#define HF_FLAG_DUMP_RAT_QUADS_PATH       std::getenv("HF_DUMP_RAT_QUADS_PATH")
#define HF_FLAG_DUMP_RAT_QUADS_MAX        std::getenv("HF_DUMP_RAT_QUADS_MAX")
#define HF_FLAG_DUMP_RAT_QUADS_STRIDE     std::getenv("HF_DUMP_RAT_QUADS_STRIDE")
#define HF_FLAG_DUMP_RAT_QUADS_MIN_NVARS  std::getenv("HF_DUMP_RAT_QUADS_MIN_NVARS")

// Mul-op dumper (src/core/poly.cpp::dump_mul_enabled).
#define HF_FLAG_DUMP_MUL        std::getenv("HF_DUMP_MUL")
#define HF_FLAG_DUMP_MUL_SKIP   std::getenv("HF_DUMP_MUL_SKIP")
#define HF_FLAG_DUMP_MUL_LIMIT  std::getenv("HF_DUMP_MUL_LIMIT")

// Poles-stream + regkey-dump (src/integrator/integration_step.cpp).
#define HF_FLAG_POLES_STREAM    std::getenv("HF_POLES_STREAM")
#define HF_FLAG_POLES_STREAM_N  std::getenv("HF_POLES_STREAM_N")
#define HF_FLAG_REGKEY_DUMP     std::getenv("HF_REGKEY_DUMP")

// Track-structural-sharing-probe (iter-81; n=2).
// Co-resident with Track-diagnostic-dump above under §5.1 rule 1
// (single effect-domain placement: diagnostics) and the iter-65 F2
// family-purity invariant (VALUE family throughout). Both call sites
// are in src/diagnostics/structural_sharing_probe.cpp::refresh_env_impl
// (single TU, single function), consumed as `const char*` for the
// master-enable gate and the per-thread sample-rate respectively.
#define HF_FLAG_STRUCTURAL_SHARING_PROBE              std::getenv("HF_STRUCTURAL_SHARING_PROBE")
#define HF_FLAG_STRUCTURAL_SHARING_PROBE_SAMPLE_RATE  std::getenv("HF_STRUCTURAL_SHARING_PROBE_SAMPLE_RATE")

// Track-RSS-probe (iter-92; n=1).
// Co-resident with Track-diagnostic-dump and Track-structural-sharing-probe
// above under §5.1 rule 1 (single effect-domain placement: diagnostics)
// and the iter-65 F2 family-purity invariant (VALUE family throughout).
// Sole call site is the per-thread RSS sampler's env refresh helper at
// src/diagnostics/integration_node_rss.cpp::refresh_env(), consumed as
// `const char*` and parsed by `std::atoi` for the depth threshold (the
// value-domain is a positive integer encoding depth, with unset/empty/
// "0" all decoded as OFF; see docs §2 Track-RSS-probe row and the
// iter-92 LAND entry under §5.1 for the unset-direction semantics).
#define HF_FLAG_INTEG_NODE_RSS  std::getenv("HF_INTEG_NODE_RSS")
