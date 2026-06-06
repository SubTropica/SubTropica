// HF FF Phase 5 §A.1 — Probe A1: DAG hash-cons / construction-path dedup-rate /
// canonical-equality / payload-vs-backing distinction instrumentation.
//
// Design contract:
//     notes/hf_finite_field_program/phase5_three_paths/probe_a1_dag_hashcons/design.md
//     (iter-48 ζ; 720 LOC; agentId abd5207a0ac38940b BINDING).
//
// Iter-49 (this file) scope: value-layer ctor/dtor for Poly / Rat / SymCoef
// (§2 instrumentation surface) + REQ-1 GMP-allocator wrap (§4.2) + REQ-2 sharded
// shared_mutex seen-set (§5.2) + REQ-3 OFF-path budget gate (§7.2-bis, verified
// externally). Operator-call layer (§3 linear_factors / partial_fractions /
// transform_shuffle) is DEFERRED to iter-50.
//
// Env-gate scheme (§7.1; iter-52 added NDJSON sub-flag):
//     HF_DAG_HASHCONS_PROBE=1                — master gate. When UNSET, every
//                                              emit site short-circuits on the
//                                              `if (!hf_probe_active) return;`
//                                              fast path (§7.1).
//     HF_DAG_HASHCONS_PROBE_GMP_LABELLED=1   — sub-flag for REQ-1 GMP wrap;
//                                              default-ON when master is set
//                                              (separable for diagnostics).
//     HF_DAG_HASHCONS_PROBE_OP_OUTPUT=1      — RESERVED (operator-output hash;
//                                              iter-50+ when operator-call
//                                              layer lands).
//     HF_DAG_HASHCONS_PROBE_NDJSON=1         — iter-52 §2.3: per-event ndjson
//                                              emission. Default OFF because
//                                              per-allocation ndjson overflows
//                                              disk on heavy fixtures (iter-51
//                                              tst2 saw 60.4 GB in <2 min).
//                                              When OFF, the aggregate.json
//                                              + per-layer cumulative counters
//                                              suffice for §6.5 steps 1, 2,
//                                              3b, 5 (and 4 deferred).
//                                              Step 3a (RSS-weighted
//                                              dedupable-share) needs the
//                                              sweep-line over (vc, vd)
//                                              events, so it is reported as
//                                              `step_3a_PENDING_NDJSON` when
//                                              the sub-flag is OFF.
//     HF_DAG_HASHCONS_PROBE_OUT_DIR=<dir>    — ndjson output directory; also
//                                              the directory the aggregate.json
//                                              snapshot is written to.
//                                              Defaults to cwd.
//
// REQ folds applied at iter-49 (per iter-48-γ pre-build reviewer
// abd5207a0ac38940b):
//   - REQ-1 (BINDING, correctness-cliff): wrap `mp_set_memory_functions` as a
//     delegating layer; chain through the Phase 0.5 retrofit registered at
//     `bridge/cli/gmp_mimalloc_init.cpp:145`. Direct replacement would destroy
//     the `mimalloc_heap_FLINT_GMP` attribution lineage (95.61 % bucket share)
//     and invalidate FOLD-M10 payload-vs-backing split. Unit test in
//     `test/unit/test_dag_hashcons_probe_init.cpp` exercises the init sequence.
//   - REQ-2 (BINDING, correctness-cliff under concurrency): sharded
//     `std::shared_mutex`-guarded seen-set for the §5 construction-path
//     dedup-rate column. 16 shards keyed on the top 4 bits of the 64-bit
//     canonical-bits hash. CI test (DEFERRED to iter-50) asserts OMP=1 vs
//     OMP=13 dedup-rate agreement within ±0.5 pp.
//   - REQ-3 (BINDING, baseline-soundness): OFF-path wall + RSS budget gate at
//     §7.2-bis (±5 % max_rss, ±10 % wall vs §0.3 baseline on 6 fixtures). This
//     is a build-time GATE, not a source constraint; verified externally at
//     Phase 49-δ.

#pragma once

#include <cstddef>
#include <cstdint>

// FLINT types appear in the public emit-helper signatures. We include the full
// `<flint/fmpq_mpoly.h>` rather than forward-declaring `struct
// fmpq_mpoly_struct` / `struct fmpq_mpoly_ctx_struct`, because FLINT 3 typedefs
// these via an unnamed-struct pattern (`typedef struct { ... } X;`) on some
// builds where a `struct X;` forward declaration would create an unrelated
// type. The header is already transitively included from every TU that uses
// FLINT, so the cost is zero. The probe header itself remains lightweight at
// the C++ standard-library level (just <cstddef> + <cstdint>).
#include <flint/fmpq_mpoly.h>

namespace hyperflint {

// Fast-path active flag. Read by every emit site as the first instruction.
// Set once at process start by `hf_probe_init()`. Externally visible so the
// ctor/dtor instrumentation in poly.cpp / rat.cpp / symcoef.cpp can early-out
// without a function call (§7.1 fast-path guarantee).
extern bool hf_probe_active;

// Three-stage initialisation (§4.2 REQ-1 ordering constraint):
//   1. `hf_init_mimalloc_for_gmp_flint()` — Phase 0.5 retrofit at
//      `bridge/cli/gmp_mimalloc_init.cpp:114-149`. Registers
//      `gmp_alloc`/`gmp_realloc`/`gmp_free` (delegating to mimalloc).
//   2. `hf_probe_init()` — THIS function. Snapshots the function pointers via
//      `mp_get_memory_functions`, saves into static globals, then re-registers
//      `hf_probe_gmp_alloc`/`hf_probe_gmp_realloc`/`hf_probe_gmp_free` (which
//      delegate through the saved pointers). Reads env flags; sets
//      `hf_probe_active`.
//   3. Application code (FLINT init, OMP setup, etc.).
//
// Calling `hf_probe_init()` BEFORE `hf_init_mimalloc_for_gmp_flint()` would
// snapshot null/libsystem-malloc pointers and break the M10 column. Calling it
// AFTER any FLINT/GMP allocation has already occurred would leave a portion of
// the M10 denominator unlabelled (the labelled-bytes counter would
// under-report). The init must therefore run as the second-line statement in
// main(), immediately after the Phase 0.5 retrofit. The order is asserted in
// `test/unit/test_dag_hashcons_probe_init.cpp`.
//
// If `HF_DAG_HASHCONS_PROBE` is unset, `hf_probe_init()` is a no-op: the
// `mp_set_memory_functions` wrap is skipped, `hf_probe_active` stays false, and
// the OFF-path REQ-3 budget gate (§7.2-bis) is structurally honoured.
void hf_probe_init();

// End-of-run aggregation: flush per-thread ndjson buffers to per-fixture
// `<layer>.ndjson.gz` files, write the aggregate snapshot (`aggregate.json`),
// then close. Idempotent: subsequent calls are no-ops. Registered via
// `std::atexit` from `hf_probe_init()` when the probe is active.
//
// Iter-49 MVP behaviour: writes raw `.ndjson` (no gzip) to the output dir; the
// iter-50+ aggregator (`aggregate.py`) handles compression + cross-fixture
// roll-up.
void hf_probe_finalize();

// Layer enum for the value-layer emit (§2.1). Used as a discriminant in the
// ndjson `"layer"` field. Operator-call layer (§3) is RESERVED iter-50+.
enum class HfProbeLayer : uint8_t {
    Poly    = 0,
    Rat     = 1,
    SymCoef = 2,
};

// Value-create emit for Poly layer (§2.1). Called from `Poly::Poly(...)` ctors
// (poly.cpp). The `instance_id` is the address of the Poly object (stable for
// the lifetime; `value_destroy` emit matches by same address). Reads the FLINT
// payload to compute `n_terms` / `bits_per_term` / `payload_bytes_est` /
// `canonical_bits_hash_u64` per §2.1 + §2.2.
//
// Fast path: `if (!hf_probe_active) return;` as the first statement (§7.1).
void hf_probe_emit_poly_create(uintptr_t instance_id,
                               const fmpq_mpoly_struct* poly,
                               const fmpq_mpoly_ctx_struct* ctx);

void hf_probe_emit_poly_destroy(uintptr_t instance_id);

// Value-create emit for Rat layer. Iter-49 MVP combines the num + den
// canonical-bits hashes via FNV-1a chaining; ndjson `n_terms`/`payload_bytes_est`
// are the sum of the two underlying Poly fields.
void hf_probe_emit_rat_create(uintptr_t instance_id,
                              const fmpq_mpoly_struct* num,
                              const fmpq_mpoly_struct* den,
                              const fmpq_mpoly_ctx_struct* ctx);

void hf_probe_emit_rat_destroy(uintptr_t instance_id);

// Value-create emit for SymCoef layer. Iter-49 MVP emits a count of monomials
// + aggregate hash over the canonical signatures of every prefactor's num+den.
// Per-monomial pi/i/log/delta exponents are folded into the hash mix.
//
// Hash is computed externally (in symcoef.cpp) because SymCoef holds private
// state we'd otherwise need to expose; the caller passes the precomputed hash
// + n_terms + payload_bytes_est. This keeps the probe header decoupled from
// the SymMonomial layout.
void hf_probe_emit_symcoef_create(uintptr_t instance_id,
                                  uint64_t  canonical_bits_hash,
                                  uint64_t  n_terms,
                                  uint64_t  payload_bytes_est);

void hf_probe_emit_symcoef_destroy(uintptr_t instance_id);

// FNV-1a 64-bit byte-stream helpers, exposed for SymCoef-side hash assembly
// and for the REC-2 micro-bench (iter-49+; iter-50 will run xxHash64
// comparison if FNV-1a collision rate exceeds 0.001 % on the §A.1 probe).
constexpr uint64_t kFnv1a64OffsetBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnv1a64Prime       = 0x100000001b3ULL;

inline uint64_t hf_probe_fnv1a64_step(uint64_t h, uint8_t b) {
    return (h ^ (uint64_t)b) * kFnv1a64Prime;
}

uint64_t hf_probe_fnv1a64_bytes(const void* data, size_t n);

// Mix-in helper used to combine sub-hashes into a parent canonical-bits hash
// without colliding with byte streams of the same length (Rat = num_hash mixed
// with den_hash; SymCoef = prefactor_hash mixed with power exponents).
inline uint64_t hf_probe_fnv1a64_mix_u64(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h = hf_probe_fnv1a64_step(h, (uint8_t)((v >> (i * 8)) & 0xFFu));
    }
    return h;
}

// Diagnostic accessor for the REQ-1 unit test: returns the in-flight count of
// GMP-labelled bytes (incremented by `hf_probe_gmp_alloc`, decremented by
// `hf_probe_gmp_free`). Safe to call from any thread; relaxed atomic load.
int64_t hf_probe_gmp_labelled_bytes_in_flight();

// HF FF Phase 5 §A.1 iter-50 (operator-call layer; §3 of design.md):
// canonical-bits hash of a single fmpq_mpoly. Public wrapper around the
// internal `canonical_bits_hash_poly` so call sites in rat.cpp /
// linear_factors.cpp / partial_fractions.cpp / transform.cpp can compose
// input-tuple hashes for `hf_probe_emit_op_call` without needing access
// to the probe's internal namespace.
//
// Fast path: returns 0 immediately when `hf_probe_active` is false, so the
// OFF-path cost is one TLS-read + branch. ON-path cost is O(length * (bits/64))
// per poly — meaningful only when the probe is enabled.
uint64_t hf_probe_canonical_hash_poly(const fmpq_mpoly_struct* poly,
                                      const fmpq_mpoly_ctx_struct* ctx);

// Operator-call emit (§3.1). Records an op_call ndjson line with the op name,
// the combined canonical hash of the input tuple (caller-composed via
// `hf_probe_canonical_hash_poly` + `hf_probe_fnv1a64_mix_u64`), and the input
// arity (1 = unary like `Rat::reduce_inplace`, 2 = binary like `Rat::add`).
//
// Fast path: `if (!hf_probe_active) return;` as the first statement.
//
// The op_dup_rate column (design.md §3.3) is computed by `aggregate.py` from
// the ndjson stream by grouping on (op_name, input_combined_hash) and counting
// duplicates. The probe itself only emits raw events.
void hf_probe_emit_op_call(const char* op_name,
                           uint64_t   input_combined_hash,
                           uint8_t    input_arity);

// HF FF Phase 5 §A.1 iter-50: reset hook for the REQ-2 CI test.
// Clears the construction-path seen-set + hits/misses counters AND the
// op-call seen-set + hits/misses counters. Does NOT clear the labelled-bytes
// counter or the ndjson output stream. Used by
// `test_construction_path_dedup_rate_omp_invariance.cpp` between the OMP=1
// and OMP=13 measurement phases so the second phase sees a fresh seen-set
// against the same workload. Not intended for production code paths.
void hf_probe_reset_dedup_state();

// Diagnostic accessors for the REQ-2 unit test (replace internal-namespace
// peeks the test would otherwise need). Returns the current snapshot of the
// construction-path hit/miss counters as of the call. Safe to call from any
// thread; relaxed atomic loads.
struct HfProbeDedupSnapshot {
    uint64_t value_layer_hits   = 0;
    uint64_t value_layer_misses = 0;
    uint64_t op_layer_hits      = 0;
    uint64_t op_layer_misses    = 0;
};
HfProbeDedupSnapshot hf_probe_get_dedup_snapshot();

// Diagnostic accessor for the REQ-1 unit test: returns the function pointers
// that `mp_get_memory_functions` would emit after `hf_probe_init`. Used to
// assert that the probe wrappers are now installed at the GMP layer and that
// the Phase 0.5 retrofit pointers are saved (i.e., `g_prev_gmp_alloc != null`).
struct HfProbeGmpFunctionSnapshot {
    void* (*alloc)(size_t)                  = nullptr;
    void* (*realloc)(void*, size_t, size_t) = nullptr;
    void  (*free)(void*, size_t)            = nullptr;
    void* (*prev_alloc)(size_t)                  = nullptr;
    void* (*prev_realloc)(void*, size_t, size_t) = nullptr;
    void  (*prev_free)(void*, size_t)            = nullptr;
};
HfProbeGmpFunctionSnapshot hf_probe_get_gmp_function_snapshot();

}  // namespace hyperflint
