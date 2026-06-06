// HF FF Phase 5 §A.1 — Probe A1 implementation. Iter-49 MVP scope: value-layer
// (Poly / Rat / SymCoef ctor/dtor) + REQ-1 GMP-allocator wrap + REQ-2 sharded
// shared_mutex seen-set + FNV-1a 64 canonical-bits hash + ndjson emit. See the
// header (`include/hyperflint/instrumentation/dag_hashcons_probe.hpp`) for the
// public API contract and the design memo
// (`notes/hf_finite_field_program/phase5_three_paths/probe_a1_dag_hashcons/design.md`)
// for the load-bearing folds (FOLD-DC1..DC5 + FOLD-PR-REQ-1..3).

#include "hyperflint/instrumentation/dag_hashcons_probe.hpp"

// Track 7 first chunk (iter-62): env-flag macro layer. The four HF_FLAG_*
// macros below resolve to std::getenv("HF_DAG_HASHCONS_PROBE*") and keep the
// raw literal strings out of this translation unit; the literals are
// preserved (for the env-flag registry coverage gate) in env_flags.hpp.
#include "hyperflint/instrumentation/env_flags.hpp"

#include <flint/fmpq_mpoly.h>
#include <flint/fmpz.h>
#include <flint/mpoly.h>
#include <gmp.h>

#ifdef HF_HAVE_OPENMP
#include <omp.h>
#endif

#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace hyperflint {

// ============================================================================
// Process-global state. All file-local statics: not externally visible except
// via the explicit getters in the header.
// ============================================================================

bool hf_probe_active = false;

namespace {

// REQ-1 BINDING: saved Phase 0.5 retrofit pointers. Snapshotted at
// `hf_probe_init` BEFORE `mp_set_memory_functions` rewrites them with the probe
// wrappers. The probe wrappers MUST delegate through these to preserve the
// mimalloc-attribution lineage (§4.2).
void* (*g_prev_gmp_alloc)(size_t)                       = nullptr;
void* (*g_prev_gmp_realloc)(void*, size_t, size_t)      = nullptr;
void  (*g_prev_gmp_free)(void*, size_t)                 = nullptr;

// REQ-1: total GMP-labelled bytes in flight at any given moment. Incremented
// at alloc/realloc, decremented at free. Read at peak-RSS checkpoint to
// compute the M10 payload-vs-backing split column (§4.3).
std::atomic<int64_t> g_gmp_labelled_bytes_in_flight{0};

// Output directory for ndjson files (cwd by default). Set once at probe init
// from the HF_DAG_HASHCONS_PROBE_OUT_DIR env var.
//
// Iter-52 §3.3 FOLD-CLASS-C: marked `[[clang::no_destroy]]` so the static
// destructor never runs, sidestepping the destruction-order race between
// libomp worker threads still alive at process exit and libc++ static
// destruction (root cause iter52_root_cause.md §3.1).
[[clang::no_destroy]] std::string g_out_dir;

// File handles for ndjson output. Iter-52 §2.3 makes ndjson emission opt-in
// via HF_DAG_HASHCONS_PROBE_NDJSON (default OFF); when OFF, these streams
// are never opened and the per-emit `is_open()` guards short-circuit.
//
// FOLD-CLASS-C: `[[clang::no_destroy]]` for the same destruction-order
// reason as g_out_dir. `std::ofstream` holds heap-allocated streambuf state
// on libc++; suppressing the destructor leaks that state at process exit
// (reclaimed by the OS).
[[clang::no_destroy]] std::mutex     g_io_mu;
[[clang::no_destroy]] std::ofstream  g_ndjson_values;  // value_{create,destroy}
[[clang::no_destroy]] std::ofstream  g_ndjson_ops;     // op_call (iter-50+)

// Iter-52 §2.3: gate flag for the ndjson sub-feature. Set at hf_probe_init
// from HF_DAG_HASHCONS_PROBE_NDJSON env var (default OFF). When false, the
// emit helpers' existing `is_open()` checks short-circuit the file write.
// Counters + aggregate.json remain populated either way.
bool g_ndjson_enabled = false;

// Monotonic-clock baseline: subtract from every emitted ts_ns so they're
// process-relative and fit comfortably in int64.
std::chrono::steady_clock::time_point   g_t0;

inline int64_t hf_probe_now_ns() {
    auto delta = std::chrono::steady_clock::now() - g_t0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count();
}

inline int hf_probe_thread_id_compat() {
    // Lightweight thread id (cast to int) for the ndjson `thread_id` field.
    // Using `std::hash<std::thread::id>` would be more portable but is overkill
    // for a single-process ndjson stream consumed by the aggregator.
#ifdef HF_HAVE_OPENMP
    return omp_get_thread_num();
#else
    return 0;
#endif
}

// ============================================================================
// REQ-2 BINDING: ShardedSeenSet — process-global, thread-safe set tracking
// distinct (layer, canonical_bits_hash) pairs for the §5 construction-path
// dedup-rate column. 16 shards keyed on the top 4 bits of the hash; each shard
// holds an unordered_set guarded by a per-shard std::shared_mutex.
//
// REQ-2 CI test (DEFERRED to iter-50) asserts that OMP=1 and OMP=13 produce
// the same dedup-rate within ±0.5 pp on a fixed deterministic input.
// ============================================================================

struct ShardedSeenSet {
    static constexpr int kShards = 16;
    struct Shard {
        mutable std::shared_mutex mu;
        std::unordered_set<uint64_t> set;
    };
    std::array<Shard, kShards> shards;

    // Combines layer + hash into a single 64-bit key. Layer occupies the top
    // 3 bits (max 8 layers); hash occupies the bottom 61. Shard selection
    // uses the next-after-layer 4 bits so the layer bits don't dominate the
    // shard distribution.
    static uint64_t make_key(HfProbeLayer layer, uint64_t hash) {
        return ((uint64_t)layer << 61) | (hash & 0x1FFFFFFFFFFFFFFFULL);
    }

    // Returns true if newly inserted (MISS); false if already present (HIT).
    // Fast path: shared_lock on shared_mutex for the membership test; only
    // upgrades to exclusive on actual insert.
    bool insert_if_absent(uint64_t key) {
        const int s = (int)((key >> 57) & 0xF);  // 4 bits below layer field
        Shard& sh = shards[s];
        {
            std::shared_lock<std::shared_mutex> rl(sh.mu);
            if (sh.set.count(key)) return false;
        }
        {
            std::unique_lock<std::shared_mutex> wl(sh.mu);
            return sh.set.insert(key).second;
        }
    }
};

// FOLD-CLASS-C (iter-52 §3.3): ShardedSeenSet holds 16 `std::shared_mutex`
// instances per shard. Their pthread_rwlock_destroy() racing with still-live
// libomp worker threads at static-destruction time is the root cause of the
// libc++abi "mutex lock failed: Invalid argument" abort observed on iter-51
// tst0 + findroots21_a (root cause memo §3.1). `[[clang::no_destroy]]`
// suppresses the destructor emission; the heap state leaks at process exit
// and is reclaimed by the OS.
[[clang::no_destroy]] ShardedSeenSet g_seen_set;
std::atomic<uint64_t> g_construction_path_hits{0};
std::atomic<uint64_t> g_construction_path_misses{0};

// Total emit-event counters; informational, surfaced in the finalize snapshot.
std::atomic<uint64_t> g_n_value_create{0};
std::atomic<uint64_t> g_n_value_destroy{0};

// Iter-50 (§3 operator-call layer): op_call event counter + op-layer seen-set.
// Op-layer seen-set is independent of the value-layer set so the §3.3 op_dup_rate
// column is algebraically separable from the §5 construction-path dedup-rate.
std::atomic<uint64_t> g_n_op_call{0};
[[clang::no_destroy]] ShardedSeenSet g_op_seen_set;  // FOLD-CLASS-C
std::atomic<uint64_t> g_op_call_hits{0};
std::atomic<uint64_t> g_op_call_misses{0};

// Iter-52 §2.3 + §3.3: per-layer cumulative counters for ndjson-free §6.5
// step-1 / payload-share derivation. Indexed by HfProbeLayer enum
// (Poly=0, Rat=1, SymCoef=2). Incremented in emit helpers; surfaced in the
// finalize snapshot so aggregate.py can compute per-layer counts and total
// payload bytes without scanning ndjson.
std::atomic<uint64_t> g_n_create_by_layer[3]{0, 0, 0};
std::atomic<uint64_t> g_n_destroy_by_layer[3]{0, 0, 0};
std::atomic<uint64_t> g_payload_bytes_total_by_layer[3]{0, 0, 0};

// FOLD-CLASS-C: replace std::mutex + bool finalize sentinel with a
// destructor-free atomic-flag compare-exchange. Sidesteps the mutex-destructor
// race entirely for the most common atexit path. The lock-guard pattern is
// only needed for mutual exclusion if `hf_probe_finalize` is called
// concurrently from multiple atexit handlers (it isn't), so a CAS is
// strictly stronger here than the std::mutex was.
std::atomic<bool> g_finalize_done{false};

}  // namespace

// ============================================================================
// FNV-1a 64-bit hashing helpers
// ============================================================================

uint64_t hf_probe_fnv1a64_bytes(const void* data, size_t n) {
    uint64_t h = kFnv1a64OffsetBasis;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = hf_probe_fnv1a64_step(h, p[i]);
    return h;
}

namespace {

// §2.2 canonical-bits hash for a single fmpq_mpoly. NOT thread-modifying:
// `mpoly_fix_bits` and `fmpq_mpoly_canonicalise` may mutate the in-memory
// payload; we do NOT call them from the probe path (the design memo §2.2
// specifies these as preconditions on the canonical signature, but applying
// them mid-ctor on a not-yet-fully-constructed Poly would be unsafe). Iter-49
// MVP hashes the raw byte stream as-presented; the §A.1 verdict (iter-50+)
// will document the choice.
//
// FLINT 3 layout (fmpq_mpoly_struct):
//   fmpq_t        content;   // fmpq = fmpz num + fmpz den
//   fmpz_mpoly_t  zpoly;     // zpoly->coeffs[length] = fmpz array
//                            // zpoly->exps[length * (bits/64)] = uint64 array
//                            // zpoly->length, zpoly->bits, zpoly->alloc
// We hash the structural fields in a deterministic order. The choice of which
// fields constitute the canonical signature is per §2.2 of the design memo;
// for iter-49 MVP we hash (nvars, ord, length, bits, exps[], content + coeffs
// as canonical-magnitude fmpz tuples). fmpz hashing folds the inline-vs-limb
// distinction so that an inline fmpz (≤ 1 limb) and a heap fmpz that happens
// to fit in 1 limb produce the same hash.
uint64_t hash_fmpz_canonical(uint64_t h, const fmpz_t z) {
    // fmpz canonical encoding:
    //   sign (-1 / 0 / +1) packed into int8, then n_limbs (uint32), then the
    //   limbs themselves. fmpz_sgn handles both inline + heap.
    const int sgn = fmpz_sgn(z);
    h = hf_probe_fnv1a64_step(h, (uint8_t)(int8_t)sgn);
    if (sgn == 0) return h;
    // Use fmpz_size to get limb count then write each limb.
    const slong n_limbs = fmpz_size(z);
    const uint32_t nl = (uint32_t)n_limbs;
    h = hf_probe_fnv1a64_mix_u64(h, (uint64_t)nl);
    // Read limbs through fmpz_get_ui_array (safe for both inline + heap fmpz).
    std::vector<mp_limb_t> limbs((size_t)n_limbs);
    fmpz_get_ui_array(limbs.data(), n_limbs, z);
    for (slong i = 0; i < n_limbs; ++i) {
        h = hf_probe_fnv1a64_mix_u64(h, (uint64_t)limbs[(size_t)i]);
    }
    return h;
}

uint64_t hash_fmpq_canonical(uint64_t h, const fmpq_t q) {
    // fmpq = fmpz num + fmpz den (with den > 0 in canonical form).
    h = hash_fmpz_canonical(h, fmpq_numref(q));
    h = hash_fmpz_canonical(h, fmpq_denref(q));
    return h;
}

uint64_t canonical_bits_hash_poly(const fmpq_mpoly_struct* poly,
                                  const fmpq_mpoly_ctx_struct* ctx) {
    if (poly == nullptr || ctx == nullptr) return 0;

    uint64_t h = kFnv1a64OffsetBasis;

    // §2.2 prefix: (nvars, ord). FOLD-M3 BINDING. ord==ORD_DEGREVLEX is the
    // FLINT default; we hash the actual value rather than asserting because
    // the design memo's degrevlex-assertion sub-flag is optional at iter-49.
    // fmpq_mpoly_ctx_struct stores its mpoly_ctx via the underlying
    // fmpz_mpoly_ctx (`ctx->zctx->minfo`) — same access pattern used in
    // poly.cpp:925-926 (PolyByteBuckets).
    const slong nvars = ctx->zctx->minfo->nvars;
    const ordering_t ord = ctx->zctx->minfo->ord;
    h = hf_probe_fnv1a64_mix_u64(h, (uint64_t)nvars);
    h = hf_probe_fnv1a64_mix_u64(h, (uint64_t)(int32_t)ord);

    // (length, bits) prefix for the structural body.
    const slong length = poly->zpoly->length;
    const slong bits   = poly->zpoly->bits;
    h = hf_probe_fnv1a64_mix_u64(h, (uint64_t)length);
    h = hf_probe_fnv1a64_mix_u64(h, (uint64_t)bits);

    // Exponent array: length * (bits/FLINT_BITS) ulong slots. The fmpz_mpoly
    // exp layout packs nvars exponents into `bits` bits each, then stripes
    // them as ulong slots in row-major order. We hash the raw ulong stream.
    // `_sp` (small-bits) suffix matches the FLINT 3 API and the existing
    // poly.cpp:925 usage; the heavy `_mp` path is rare for fmpq_mpoly inputs
    // produced by the HF pipeline (typical `bits` ≤ 64).
    if (length > 0 && bits > 0) {
        const slong words_per_exp =
            mpoly_words_per_exp_sp(bits, ctx->zctx->minfo);
        const ulong* exps = poly->zpoly->exps;
        const size_t exp_words = (size_t)length * (size_t)words_per_exp;
        for (size_t i = 0; i < exp_words; ++i) {
            h = hf_probe_fnv1a64_mix_u64(h, (uint64_t)exps[i]);
        }
    }

    // Coefficient array (length fmpz entries).
    if (length > 0) {
        for (slong i = 0; i < length; ++i) {
            h = hash_fmpz_canonical(h, poly->zpoly->coeffs + i);
        }
    }

    // Content (fmpq scalar: num + den).
    h = hash_fmpq_canonical(h, poly->content);

    return h;
}

// §2.1 payload-bytes estimate. Lightweight: (length * (bits/8 + per-coeff)).
// The per-coeff term is a conservative tally of fmpz overhead (one ulong slot
// per coeff for the handle, plus ~limb-count bytes for heap-backed fmpz).
uint64_t payload_bytes_est_poly(const fmpq_mpoly_struct* poly,
                                const fmpq_mpoly_ctx_struct* ctx) {
    if (poly == nullptr || ctx == nullptr) return 0;
    const slong length = poly->zpoly->length;
    const slong bits   = poly->zpoly->bits;
    if (length <= 0 || bits <= 0) return 0;
    // FLINT 3: fmpq_mpoly_ctx is { fmpz_mpoly_ctx zctx; }; the mpoly_ctx_t is
    // reached via ctx->zctx->minfo (matches poly.cpp:925 PolyByteBuckets).
    const slong words_per_exp = mpoly_words_per_exp_sp(bits, ctx->zctx->minfo);
    uint64_t exp_bytes  = (uint64_t)length * (uint64_t)words_per_exp * 8u;
    uint64_t coef_bytes = (uint64_t)length * 24u;  // ~3 limbs avg for fmpq num+den
    return exp_bytes + coef_bytes;
}

inline void seen_set_update(uint64_t key) {
    if (g_seen_set.insert_if_absent(key)) {
        g_construction_path_misses.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_construction_path_hits.fetch_add(1, std::memory_order_relaxed);
    }
}

void emit_value_create_line(HfProbeLayer layer,
                            uintptr_t instance_id,
                            const fmpq_mpoly_struct* poly_or_null,
                            const fmpq_mpoly_ctx_struct* ctx_or_null,
                            uint64_t canonical_hash,
                            uint64_t n_terms,
                            uint64_t payload_bytes_est) {
    const char* layer_str = "?";
    switch (layer) {
        case HfProbeLayer::Poly:    layer_str = "Poly";    break;
        case HfProbeLayer::Rat:     layer_str = "Rat";     break;
        case HfProbeLayer::SymCoef: layer_str = "SymCoef"; break;
    }
    const int    tid       = hf_probe_thread_id_compat();
    const int64_t ts_ns    = hf_probe_now_ns();
    // ctx access path: fmpq_mpoly_ctx_struct -> zctx -> minfo (see above).
    const slong   nvars    = ctx_or_null ? ctx_or_null->zctx->minfo->nvars : 0;
    const int32_t ord      = ctx_or_null
                                 ? (int32_t)ctx_or_null->zctx->minfo->ord : 0;
    // `zpoly` is an array typedef of `fmpz_mpoly_struct[1]`, so the address is
    // never null when `poly_or_null` is non-null; the `bits` field is the
    // structural payload's exponent-packing width.
    const slong   bits     = poly_or_null ? poly_or_null->zpoly->bits : 0;

    // Iter-52 §2.3: per-layer ndjson-free counters (always live; cheap atomics).
    const int layer_idx = (int)layer;
    if (layer_idx >= 0 && layer_idx < 3) {
        g_n_create_by_layer[layer_idx].fetch_add(1, std::memory_order_relaxed);
        g_payload_bytes_total_by_layer[layer_idx].fetch_add(
            payload_bytes_est, std::memory_order_relaxed);
    }

    // Iter-52 §2.3: ndjson emit is opt-in (HF_DAG_HASHCONS_PROBE_NDJSON=1).
    // When OFF, skip the snprintf + write entirely. The seen-set update and
    // total counter still fire so the aggregate.json is fully populated.
    if (g_ndjson_enabled && g_ndjson_values.is_open()) {
        // Buffer the line locally to keep the I/O lock as short as possible.
        char buf[512];
        int n = std::snprintf(
            buf, sizeof(buf),
            "{\"k\":\"vc\",\"l\":\"%s\",\"id\":%llu,\"tid\":%d,\"ts\":%lld,"
            "\"nv\":%lld,\"ord\":%d,\"nt\":%llu,\"bpt\":%lld,"
            "\"pb\":%llu,\"h\":%llu}\n",
            layer_str,
            (unsigned long long)instance_id,
            tid,
            (long long)ts_ns,
            (long long)nvars,
            (int)ord,
            (unsigned long long)n_terms,
            (long long)bits,
            (unsigned long long)payload_bytes_est,
            (unsigned long long)canonical_hash);
        if (n > 0) {
            std::lock_guard<std::mutex> lk(g_io_mu);
            if (g_ndjson_values.is_open()) g_ndjson_values.write(buf, n);
        }
    }
    g_n_value_create.fetch_add(1, std::memory_order_relaxed);
    seen_set_update(ShardedSeenSet::make_key(layer, canonical_hash));
}

void emit_value_destroy_line(HfProbeLayer layer,
                             uintptr_t instance_id) {
    const char* layer_str = "?";
    switch (layer) {
        case HfProbeLayer::Poly:    layer_str = "Poly";    break;
        case HfProbeLayer::Rat:     layer_str = "Rat";     break;
        case HfProbeLayer::SymCoef: layer_str = "SymCoef"; break;
    }
    // Iter-52 §2.3: per-layer destroy counter; always live regardless of
    // ndjson gate so aggregate.py can match create/destroy without ndjson.
    const int layer_idx = (int)layer;
    if (layer_idx >= 0 && layer_idx < 3) {
        g_n_destroy_by_layer[layer_idx].fetch_add(1, std::memory_order_relaxed);
    }

    if (g_ndjson_enabled && g_ndjson_values.is_open()) {
        const int    tid    = hf_probe_thread_id_compat();
        const int64_t ts_ns = hf_probe_now_ns();

        char buf[160];
        int n = std::snprintf(
            buf, sizeof(buf),
            "{\"k\":\"vd\",\"l\":\"%s\",\"id\":%llu,\"tid\":%d,\"ts\":%lld}\n",
            layer_str,
            (unsigned long long)instance_id,
            tid,
            (long long)ts_ns);
        if (n > 0) {
            std::lock_guard<std::mutex> lk(g_io_mu);
            if (g_ndjson_values.is_open()) g_ndjson_values.write(buf, n);
        }
    }
    g_n_value_destroy.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

// ============================================================================
// REQ-1 BINDING: GMP allocator wrap.
//
// The Phase 0.5 retrofit (`bridge/cli/gmp_mimalloc_init.cpp:145`) registered
// `gmp_alloc`/`gmp_realloc`/`gmp_free` which delegate to `mi_malloc` family.
// The probe MUST chain through those (not call `mi_malloc` directly) to keep
// the mimalloc-attribution lineage intact. Direct replacement would break
// the M10 numerator/denominator consistency (§4.2).
// ============================================================================

extern "C" {
void* hf_probe_gmp_alloc(size_t size) {
    void* p = g_prev_gmp_alloc ? g_prev_gmp_alloc(size) : std::malloc(size);
    g_gmp_labelled_bytes_in_flight.fetch_add((int64_t)size,
                                             std::memory_order_relaxed);
    return p;
}

void* hf_probe_gmp_realloc(void* old_ptr, size_t old_size, size_t new_size) {
    void* p = g_prev_gmp_realloc
                  ? g_prev_gmp_realloc(old_ptr, old_size, new_size)
                  : std::realloc(old_ptr, new_size);
    g_gmp_labelled_bytes_in_flight.fetch_add(
        (int64_t)new_size - (int64_t)old_size, std::memory_order_relaxed);
    return p;
}

void hf_probe_gmp_free(void* ptr, size_t size) {
    if (g_prev_gmp_free) g_prev_gmp_free(ptr, size);
    else std::free(ptr);
    g_gmp_labelled_bytes_in_flight.fetch_sub((int64_t)size,
                                             std::memory_order_relaxed);
}
}  // extern "C"

int64_t hf_probe_gmp_labelled_bytes_in_flight() {
    return g_gmp_labelled_bytes_in_flight.load(std::memory_order_relaxed);
}

HfProbeGmpFunctionSnapshot hf_probe_get_gmp_function_snapshot() {
    HfProbeGmpFunctionSnapshot s;
    mp_get_memory_functions(&s.alloc, &s.realloc, &s.free);
    s.prev_alloc   = g_prev_gmp_alloc;
    s.prev_realloc = g_prev_gmp_realloc;
    s.prev_free    = g_prev_gmp_free;
    return s;
}

// ============================================================================
// Init / finalize
// ============================================================================

void hf_probe_init() {
    const char* master = HF_FLAG_DAG_HASHCONS_PROBE;
    if (master == nullptr || master[0] == '\0' || master[0] == '0') {
        // OFF path. Fast path branch in every emit site short-circuits via
        // `hf_probe_active = false`. The Phase 0.5 retrofit is untouched.
        return;
    }

    g_t0 = std::chrono::steady_clock::now();

    // REQ-1 §4.2: snapshot Phase 0.5 retrofit pointers BEFORE rewriting.
    const char* gmp_lab = HF_FLAG_DAG_HASHCONS_PROBE_GMP_LABELLED;
    const bool gmp_lab_on =
        (gmp_lab == nullptr) || (gmp_lab[0] != '0');  // default-ON when master is set
    if (gmp_lab_on) {
        mp_get_memory_functions(&g_prev_gmp_alloc,
                                &g_prev_gmp_realloc,
                                &g_prev_gmp_free);
        mp_set_memory_functions(&hf_probe_gmp_alloc,
                                &hf_probe_gmp_realloc,
                                &hf_probe_gmp_free);
    }

    // Output directory (cwd if unset).
    const char* outdir = HF_FLAG_DAG_HASHCONS_PROBE_OUT_DIR;
    g_out_dir = (outdir && outdir[0]) ? outdir : ".";

    // Iter-52 §2.3: ndjson is opt-in. Default OFF because per-event ndjson
    // overflows disk on heavy fixtures (tst2 hit 60.4 GB in <2 min on iter-51).
    // When ON, the emit helpers write one line per ctor/dtor + op_call. When
    // OFF, only the aggregate.json + per-layer counters survive.
    const char* ndjson_env = HF_FLAG_DAG_HASHCONS_PROBE_NDJSON;
    g_ndjson_enabled = (ndjson_env != nullptr) && (ndjson_env[0] != '\0')
                                              && (ndjson_env[0] != '0');

    // Open ndjson sinks only when the sub-flag is set. The aggregator
    // (iter-50+) reads either ndjson or ndjson.gz; iter-49 writes raw ndjson
    // for simplicity.
    if (g_ndjson_enabled) {
        std::string values_path = g_out_dir + "/probe_a1_values.ndjson";
        std::string ops_path    = g_out_dir + "/probe_a1_ops.ndjson";
        g_ndjson_values.open(values_path, std::ios::out | std::ios::trunc);
        g_ndjson_ops.open(ops_path,    std::ios::out | std::ios::trunc);
        if (!g_ndjson_values.is_open()) {
            std::fprintf(stderr,
                         "[hf_probe_init] WARNING: could not open %s; "
                         "value-layer ndjson disabled.\n", values_path.c_str());
        }

        // Header line capturing the run-wide context (parsed by aggregate.py).
        if (g_ndjson_values.is_open()) {
            char hdr[256];
            int n = std::snprintf(
                hdr, sizeof(hdr),
                "{\"k\":\"hdr\",\"v\":1,\"ts0_unix_ns\":%lld,"
                "\"gmp_labelled\":%s,\"hash\":\"fnv1a64\"}\n",
                (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count(),
                gmp_lab_on ? "true" : "false");
            if (n > 0) g_ndjson_values.write(hdr, n);
        }
    }

    hf_probe_active = true;
    std::atexit(&hf_probe_finalize);
}

void hf_probe_finalize() {
    // FOLD-CLASS-C (iter-52 §3.3): atomic CAS replaces the std::mutex+bool
    // sentinel; sidesteps the mutex-destructor race that crashed iter-51.
    bool expected = false;
    if (!g_finalize_done.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel)) {
        return;  // already finalized once; idempotent
    }
    if (!hf_probe_active) return;

    // Aggregate snapshot (per-thread aggregates omitted at iter-49 MVP; the
    // aggregator computes per-thread breakdowns by streaming the ndjson).
    const uint64_t h = g_construction_path_hits.load(std::memory_order_relaxed);
    const uint64_t m = g_construction_path_misses.load(std::memory_order_relaxed);
    const uint64_t denom = h + m;
    const double dedup_rate = (denom == 0) ? 0.0 : (double)h / (double)denom;
    const int64_t labelled_bytes =
        g_gmp_labelled_bytes_in_flight.load(std::memory_order_relaxed);

    // Iter-50 (§3.3 op_dup_rate column): op-layer hit/miss counters.
    const uint64_t oh = g_op_call_hits.load(std::memory_order_relaxed);
    const uint64_t om = g_op_call_misses.load(std::memory_order_relaxed);
    const uint64_t op_denom = oh + om;
    const double op_dup_rate = (op_denom == 0) ? 0.0
                                : (double)oh / (double)op_denom;

    std::string snap_path = g_out_dir + "/probe_a1_aggregate.json";
    std::ofstream snap(snap_path, std::ios::out | std::ios::trunc);
    if (snap.is_open()) {
        snap << "{\n";
        snap << "  \"probe\": \"a1_dag_hashcons\",\n";
        snap << "  \"schema\": 1,\n";
        snap << "  \"hash\": \"fnv1a64\",\n";
        snap << "  \"n_value_create\":  "
             << g_n_value_create.load(std::memory_order_relaxed) << ",\n";
        snap << "  \"n_value_destroy\": "
             << g_n_value_destroy.load(std::memory_order_relaxed) << ",\n";
        snap << "  \"n_op_call\": "
             << g_n_op_call.load(std::memory_order_relaxed) << ",\n";
        snap << "  \"construction_path_hits\":   " << h << ",\n";
        snap << "  \"construction_path_misses\": " << m << ",\n";
        snap << "  \"construction_path_dedup_rate\": " << dedup_rate << ",\n";
        snap << "  \"op_call_hits\":   " << oh << ",\n";
        snap << "  \"op_call_misses\": " << om << ",\n";
        snap << "  \"op_call_dup_rate\": " << op_dup_rate << ",\n";
        snap << "  \"gmp_labelled_bytes_in_flight_at_finalize\": "
             << labelled_bytes << ",\n";

        // Iter-52 §2.3: per-layer cumulative counters for ndjson-free
        // §6.5 step-1 / payload-share derivation. Indexed Poly=0, Rat=1,
        // SymCoef=2 (matches HfProbeLayer enum). When NDJSON is OFF on a
        // heavy fixture, aggregate.py reads these fields and reports
        // `step_3a_PENDING_NDJSON` instead of synthesizing 0.0.
        snap << "  \"ndjson_enabled\": "
             << (g_ndjson_enabled ? "true" : "false") << ",\n";
        snap << "  \"per_layer_counters\": {\n";
        snap << "    \"Poly\":    {\"n_create\": "
             << g_n_create_by_layer[0].load(std::memory_order_relaxed)
             << ", \"n_destroy\": "
             << g_n_destroy_by_layer[0].load(std::memory_order_relaxed)
             << ", \"payload_bytes_total\": "
             << g_payload_bytes_total_by_layer[0].load(std::memory_order_relaxed)
             << "},\n";
        snap << "    \"Rat\":     {\"n_create\": "
             << g_n_create_by_layer[1].load(std::memory_order_relaxed)
             << ", \"n_destroy\": "
             << g_n_destroy_by_layer[1].load(std::memory_order_relaxed)
             << ", \"payload_bytes_total\": "
             << g_payload_bytes_total_by_layer[1].load(std::memory_order_relaxed)
             << "},\n";
        snap << "    \"SymCoef\": {\"n_create\": "
             << g_n_create_by_layer[2].load(std::memory_order_relaxed)
             << ", \"n_destroy\": "
             << g_n_destroy_by_layer[2].load(std::memory_order_relaxed)
             << ", \"payload_bytes_total\": "
             << g_payload_bytes_total_by_layer[2].load(std::memory_order_relaxed)
             << "}\n";
        snap << "  }\n";
        snap << "}\n";
    }

    if (g_ndjson_values.is_open()) {
        g_ndjson_values.flush();
        g_ndjson_values.close();
    }
    if (g_ndjson_ops.is_open()) {
        g_ndjson_ops.flush();
        g_ndjson_ops.close();
    }
}

// ============================================================================
// Public emit entry points
// ============================================================================

void hf_probe_emit_poly_create(uintptr_t instance_id,
                               const fmpq_mpoly_struct* poly,
                               const fmpq_mpoly_ctx_struct* ctx) {
    if (!hf_probe_active) return;
    const uint64_t hash    = canonical_bits_hash_poly(poly, ctx);
    // `poly->zpoly` is an `fmpz_mpoly_struct[1]` array typedef, so its address
    // is never null when `poly` is non-null; check `poly` only.
    const uint64_t n_terms = poly ? (uint64_t)poly->zpoly->length : 0;
    const uint64_t pb      = payload_bytes_est_poly(poly, ctx);
    emit_value_create_line(HfProbeLayer::Poly, instance_id, poly, ctx,
                           hash, n_terms, pb);
}

void hf_probe_emit_poly_destroy(uintptr_t instance_id) {
    if (!hf_probe_active) return;
    emit_value_destroy_line(HfProbeLayer::Poly, instance_id);
}

void hf_probe_emit_rat_create(uintptr_t instance_id,
                              const fmpq_mpoly_struct* num,
                              const fmpq_mpoly_struct* den,
                              const fmpq_mpoly_ctx_struct* ctx) {
    if (!hf_probe_active) return;
    const uint64_t hn = canonical_bits_hash_poly(num, ctx);
    const uint64_t hd = canonical_bits_hash_poly(den, ctx);
    // Chained FNV-1a; not commutative, which is correct for (num, den) ordering.
    uint64_t hash = kFnv1a64OffsetBasis;
    hash = hf_probe_fnv1a64_mix_u64(hash, hn);
    hash = hf_probe_fnv1a64_mix_u64(hash, hd);
    const uint64_t n_terms = (num ? (uint64_t)num->zpoly->length : 0)
                           + (den ? (uint64_t)den->zpoly->length : 0);
    const uint64_t pb = payload_bytes_est_poly(num, ctx)
                      + payload_bytes_est_poly(den, ctx);
    emit_value_create_line(HfProbeLayer::Rat, instance_id, num, ctx,
                           hash, n_terms, pb);
}

void hf_probe_emit_rat_destroy(uintptr_t instance_id) {
    if (!hf_probe_active) return;
    emit_value_destroy_line(HfProbeLayer::Rat, instance_id);
}

void hf_probe_emit_symcoef_create(uintptr_t instance_id,
                                  uint64_t  canonical_bits_hash,
                                  uint64_t  n_terms,
                                  uint64_t  payload_bytes_est) {
    if (!hf_probe_active) return;
    emit_value_create_line(HfProbeLayer::SymCoef, instance_id,
                           nullptr, nullptr,
                           canonical_bits_hash, n_terms, payload_bytes_est);
}

void hf_probe_emit_symcoef_destroy(uintptr_t instance_id) {
    if (!hf_probe_active) return;
    emit_value_destroy_line(HfProbeLayer::SymCoef, instance_id);
}

// ============================================================================
// Iter-50: operator-call layer (§3 of design.md)
// ============================================================================

namespace {

// Op-name FNV-1a hash, used as the upper 32 bits of the shard key so distinct
// ops can never collide on the seen-set even if two callers happen to compute
// identical input_combined_hash. Counters + seen-set themselves are declared
// in the early anonymous namespace alongside the value-layer counters so they
// are visible to `hf_probe_finalize` (which references them when authoring the
// aggregate.json snapshot).
inline uint64_t op_call_key(const char* op_name, uint64_t input_hash) {
    uint64_t name_h = kFnv1a64OffsetBasis;
    if (op_name) {
        for (const char* p = op_name; *p; ++p)
            name_h = hf_probe_fnv1a64_step(name_h, (uint8_t)*p);
    }
    return hf_probe_fnv1a64_mix_u64(name_h, input_hash);
}

}  // namespace

uint64_t hf_probe_canonical_hash_poly(const fmpq_mpoly_struct* poly,
                                      const fmpq_mpoly_ctx_struct* ctx) {
    if (!hf_probe_active) return 0;
    return canonical_bits_hash_poly(poly, ctx);
}

void hf_probe_emit_op_call(const char* op_name,
                           uint64_t   input_combined_hash,
                           uint8_t    input_arity) {
    if (!hf_probe_active) return;

    if (g_ndjson_enabled && g_ndjson_ops.is_open()) {
        const int     tid    = hf_probe_thread_id_compat();
        const int64_t ts_ns  = hf_probe_now_ns();

        char buf[256];
        int n = std::snprintf(
            buf, sizeof(buf),
            "{\"k\":\"oc\",\"op\":\"%s\",\"tid\":%d,\"ts\":%lld,"
            "\"ih\":%llu,\"a\":%u}\n",
            op_name ? op_name : "?",
            tid,
            (long long)ts_ns,
            (unsigned long long)input_combined_hash,
            (unsigned)input_arity);
        if (n > 0) {
            std::lock_guard<std::mutex> lk(g_io_mu);
            if (g_ndjson_ops.is_open()) g_ndjson_ops.write(buf, n);
        }
    }
    g_n_op_call.fetch_add(1, std::memory_order_relaxed);

    // Op-layer dedup tracking: same (op_name, input_combined_hash) → HIT.
    const uint64_t key = op_call_key(op_name, input_combined_hash);
    if (g_op_seen_set.insert_if_absent(key)) {
        g_op_call_misses.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_op_call_hits.fetch_add(1, std::memory_order_relaxed);
    }
}

void hf_probe_reset_dedup_state() {
    // Clear both layer seen-sets + their hit/miss counters. Each shard is
    // locked exclusively for the clear so concurrent emits from other threads
    // serialize cleanly. The intended use case is single-threaded (between
    // the OMP=1 and OMP=13 phases of the REQ-2 test); we still take locks
    // defensively. Counters are atomic stores so no lock needed there.
    for (auto& sh : g_seen_set.shards) {
        std::unique_lock<std::shared_mutex> wl(sh.mu);
        sh.set.clear();
    }
    for (auto& sh : g_op_seen_set.shards) {
        std::unique_lock<std::shared_mutex> wl(sh.mu);
        sh.set.clear();
    }
    g_construction_path_hits.store(0,   std::memory_order_relaxed);
    g_construction_path_misses.store(0, std::memory_order_relaxed);
    g_op_call_hits.store(0,   std::memory_order_relaxed);
    g_op_call_misses.store(0, std::memory_order_relaxed);
}

HfProbeDedupSnapshot hf_probe_get_dedup_snapshot() {
    HfProbeDedupSnapshot s;
    s.value_layer_hits =
        g_construction_path_hits.load(std::memory_order_relaxed);
    s.value_layer_misses =
        g_construction_path_misses.load(std::memory_order_relaxed);
    s.op_layer_hits =
        g_op_call_hits.load(std::memory_order_relaxed);
    s.op_layer_misses =
        g_op_call_misses.load(std::memory_order_relaxed);
    return s;
}

}  // namespace hyperflint
