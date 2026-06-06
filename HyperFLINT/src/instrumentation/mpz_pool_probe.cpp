// HF FF Phase 5 REC-1 — mpz-pool tracker / FLINT-pool partition attribution
// instrumentation implementation.
//
// Design contract:
//   notes/hf_finite_field_program/phase5_three_paths/rec1_mpz_pool_tracker/design.md
//   §6.1 file plan + §iter-79 fold appendix (5 REQ + 4 REC).
//
// Public API header:
//   include/hyperflint/instrumentation/mpz_pool_probe.hpp (iter-82 authoring,
//   218 LOC).
//
// Wrap layers (per design.md §3.2.1 + §iter-79 fold REQ-1):
//   - FLINT 6-tuple: alloc + free are labelled (block-size signature on
//     272-KiB on macOS / 17 * page_size in general); calloc + realloc +
//     aligned_alloc + aligned_free are pass-through. REQ-1 mandates the 6-arg
//     API surface so the Phase 0.5 retrofit's mimalloc-aligned binding
//     survives REC-1 install.
//   - GMP 3-tuple: alloc + realloc + free are size-classified. REQ-3 range
//     check `[MPZ_MIN_ALLOC * 8, FLINT_MPZ_MAX_CACHE_LIMBS * 8] = [16, 512]`
//     bytes AND multiple-of-8 partitions traffic into limb vs other-GMP.

#include "hyperflint/instrumentation/mpz_pool_probe.hpp"

#include "hyperflint/instrumentation/env_flags.hpp"

#include <gmp.h>

extern "C" {
#include <flint/flint.h>
}

#ifdef HF_HAVE_OPENMP
#include <omp.h>
#endif

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

// FLINT 6-arg memory function API (memory_manager.c:242-290). FLINT does not
// publish these in its installed headers as of 3.4.0, so we forward-declare
// them here; they are linked from the vendored libflint static archive.
extern "C" {
void __flint_get_all_memory_functions(
        void * (** alloc_func)(size_t),
        void * (** calloc_func)(size_t, size_t),
        void * (** realloc_func)(void *, size_t),
        void   (** free_func)(void *),
        void * (** aligned_alloc_func)(size_t, size_t),
        void   (** aligned_free_func)(void *));

void __flint_set_all_memory_functions(
        void * (* alloc_func)(size_t),
        void * (* calloc_func)(size_t, size_t),
        void * (* realloc_func)(void *, size_t),
        void   (* free_func)(void *),
        void * (* aligned_alloc_func)(size_t, size_t),
        void   (* aligned_free_func)(void *));
}

// Weak mimalloc residency probe symbol (REQ-4 fold). Resolves when the CLI
// binary links mimalloc statically via gmp_mimalloc_init.cpp; resolves to
// nullptr in test executables that do not link mimalloc.
extern "C" {
__attribute__((weak)) void mi_process_info(size_t*, size_t*, size_t*,
                                            size_t*, size_t*,
                                            size_t*, size_t*, size_t*);
}

// Phase 0.5 retrofit completion flag (REQ-1 defense-in-depth). Defined in
// bridge/cli/gmp_mimalloc_init.cpp and set true at the end of
// hf_init_mimalloc_for_gmp_flint(). Resolves to nullptr in test executables
// that do not link gmp_mimalloc_init.cpp, in which case REC-1 enters test
// mode (install proceeds with a clear stderr notice).
extern "C" {
__attribute__((weak)) extern bool hf_phase05_retrofit_done;
}

namespace hyperflint {

// ============================================================================
// Public-API externals (declared in mpz_pool_probe.hpp).
// ============================================================================

// iter-84 REC-3 fold: promoted from `bool` to `std::atomic<bool>`. Default
// initialiser is required because std::atomic<bool> is not implicitly zero-
// initialised; we explicitly construct `false` to match the prior semantics.
std::atomic<bool> hf_rec1_active{false};
std::atomic<bool> hf_rec1_label_header_check_active{false};

namespace {

// ============================================================================
// Saved pointers from the chain-predecessor (REQ-2 install order).
// ============================================================================

// FLINT 6-tuple snapshot from __flint_get_all_memory_functions before
// REC-1 installs its own wraps. REC-1 chains through these so the Phase 0.5
// retrofit's mimalloc bindings remain in effect for every code path.
void * (*g_prev_flint_alloc)(size_t)                  = nullptr;
void * (*g_prev_flint_calloc)(size_t, size_t)         = nullptr;
void * (*g_prev_flint_realloc)(void *, size_t)        = nullptr;
void   (*g_prev_flint_free)(void *)                   = nullptr;
void * (*g_prev_flint_aligned_alloc)(size_t, size_t)  = nullptr;
void   (*g_prev_flint_aligned_free)(void *)           = nullptr;

// GMP 3-tuple snapshot from mp_get_memory_functions. REC-1 init runs AFTER
// hf_probe_init() per REQ-2 chain-ordering contract, so this snapshot
// captures the dag_hashcons_probe wraps when active, else the Phase 0.5
// retrofit's gmp_* adapters, else the GMP libsystem default in test mode.
void * (*g_prev_gmp_alloc)(size_t)                    = nullptr;
void * (*g_prev_gmp_realloc)(void *, size_t, size_t)  = nullptr;
void   (*g_prev_gmp_free)(void *, size_t)             = nullptr;

// ============================================================================
// Partition counters. See HfRec1PartitionSnapshot in the header.
// ============================================================================

// FLINT-side: block-storage and total traffic.
std::atomic<int64_t> g_mpz_block_bytes_in_flight{0};
std::atomic<int64_t> g_labelled_block_count{0};
std::atomic<int64_t> g_flint_malloc_total_bytes_cumulative{0};

// GMP-side: limb-classified and other.
std::atomic<int64_t> g_mpz_limb_bytes_in_flight{0};
std::atomic<int64_t> g_labelled_limb_alloc_count{0};
std::atomic<int64_t> g_gmp_other_bytes_in_flight{0};
std::atomic<int64_t> g_gmp_total_bytes_in_flight{0};

// iter-86 Option (a) peak-tracking atomic globals (schema 2 → 3). See
// HfRec1PartitionSnapshot *_peak comments + update_peak_to_max() below.
// Updated via CAS loop on each fetch_add at the wrap callsite. Free-path
// fetch_sub leaves these untouched (peak is monotonically non-decreasing).
std::atomic<int64_t> g_mpz_block_bytes_in_flight_peak{0};
std::atomic<int64_t> g_labelled_block_count_peak{0};
std::atomic<int64_t> g_mpz_limb_bytes_in_flight_peak{0};
std::atomic<int64_t> g_gmp_total_bytes_in_flight_peak{0};

// Per-OMP-slot block-count counters (REC-1 design §3.3). 16 slots matches the
// §C.a slab's N_SLOTS = max(OMP_NUM_THREADS, 16) convention.
constexpr int kSlots = 16;
[[clang::no_destroy]] std::array<std::atomic<int64_t>, kSlots>
    g_block_count_per_slot{};

// iter-84 §3.2.1 OPTION (b) structural-match counters. See HfRec1PartitionSnapshot
// struct_match_pass_count / struct_match_fail_count comments.
std::atomic<int64_t> g_struct_match_pass_count{0};
std::atomic<int64_t> g_struct_match_fail_count{0};

// Block-size signature (computed at init from sysconf(_SC_PAGESIZE)).
// On macOS 16K pages this is 17 * 16384 = 272 KiB.
// On Linux 4K pages this is 17 * 4096 = 68 KiB.
size_t g_expected_block_size = 0;
slong  g_flint_page_size     = 0;

// iter-84 §3.2.1 OPTION (b): expected `flint_mpz_structs_per_block` value
// (computed at init exactly the way FLINT computes it at fmpz_single.c:108-113;
// PAGES_PER_BLOCK * (page_size / sizeof(__mpz_struct) - skip)). At full-block
// free time, the block-base `fmpz_block_header_s::count` field equals this
// value, by FLINT invariant (line 167-172 of fmpz_single.c). On macOS 16K
// pages with sizeof(__mpz_struct) = 16 and sizeof(fmpz_block_header_s) ≈ 24
// (count + thread + address; struct alignment to 8 → 24), skip = 2 and the
// expected value is 16 * (1024 - 2) = 16352.
slong g_expected_struct_count_at_full_free = 0;

// iter-84 §3.2.1 layout offsets of `fmpz_block_header_s` from
// `fmpz/link/fmpz_single.c:29-35` under `FLINT_USES_PTHREAD`:
//     _Atomic(int) count;   // offset 0, 4 bytes
//     <padding>             // 4 bytes (struct alignment to 8 for pthread_t)
//     pthread_t  thread;    // offset 8, 8 bytes (opaque ptr on macOS, ulong on glibc)
//     void *     address;   // offset 16, 8 bytes
//
// Reading the count and address fields at well-defined offsets avoids
// declaring a C++ mirror of `_Atomic(int)` (which is C-only syntax) and lets
// us use plain memcpy/load semantics. The count value is established before
// `flint_free` is called on the block, so a relaxed load is sufficient.
constexpr size_t kHdrCountOffset   = 0;
constexpr size_t kHdrAddressOffset = 16;

// Tag map: every labelled block-sized FLINT alloc is recorded here so the
// matching free can be detected (FLINT's free has signature void(void*) — no
// size — so without a tag map we cannot decrement on free). Block allocations
// are rare (O(10^2) per fixture per design §3.4) so a mutex-guarded
// std::unordered_map is acceptable.
//
// FOLD-CLASS-C (iter-52 §3.3): [[clang::no_destroy]] suppresses the static
// destructor so its libc++ heap state doesn't race with still-live libomp
// workers at process exit.
[[clang::no_destroy]] std::mutex g_tag_mu;
[[clang::no_destroy]] std::unordered_map<void *, int /* slot */>
    g_block_ptr_to_slot;

// Output directory for snapshot JSON files. Set at init from
// HF_REC1_OUT_DIR; defaults to ".".
[[clang::no_destroy]] std::string g_out_dir;

// Idempotence flag for hf_rec1_finalize().
std::atomic<bool> g_finalize_done{false};

inline int current_omp_slot() {
#ifdef HF_HAVE_OPENMP
    const int t = omp_get_thread_num();
    if (t < 0)        return 0;
    if (t >= kSlots)  return kSlots - 1;
    return t;
#else
    return 0;
#endif
}

// REQ-G iter-83-δ fold (agentId `aca17701072051b8f`): defensive JSON-escape
// for caller-supplied fixture_id / checkpoint_tag strings. Production
// callsites pass hardcoded strings (e.g. "tst2", "peak_rss"), but defending
// against a caller that passes a string containing `"` or `\` or a control
// character prevents the snapshot.json output from being malformed.
inline std::string json_escape(const char * s) {
    if (s == nullptr) return std::string("");
    std::string out;
    out.reserve(std::strlen(s) + 2);
    for (const char * p = s; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// iter-86 Option (a) peak-tracking CAS-loop helper. Updates `peak_atomic` to
// `new_value` iff `new_value > current_peak`. Uses compare_exchange_weak with
// relaxed memory ordering (diagnostic counters; no synchronization with other
// memory operations). Correctness sketch: fetch_add returns the prior value
// before the increment, so the caller computes `new_v = old + delta` which is
// our local-observable value of the counter immediately after our fetch_add.
// Concurrent threads may bump the counter higher in between; the CAS loop
// retries until either (a) we successfully install new_v as peak, or
// (b) prev_peak ≥ new_v (some other thread already installed an equal-or-
// higher peak). In case (b) the invariant peak ≥ all-historic-in-flight is
// preserved without our update.
//
// REC-1 iter-86 BINDING reviewer (agentId a6e7c42a949778ffa) snapshot-race
// note: under concurrent fetch_add + snapshot, a reader can observe
// `current = v` (after the writer's fetch_add) before the writer's CAS
// retires, leaving `peak < v` momentarily. The `peak ≥ current` invariant
// therefore holds eventually but not instantaneously. Production snapshots
// are taken from the serial driver (eval-end, atexit) AFTER OMP regions
// have completed, so this race window is structurally absent on the read
// path. The unit test test_peak_tracking is serial-only by construction.
inline void update_peak_to_max(std::atomic<int64_t>& peak_atomic,
                                int64_t new_value) {
    int64_t prev_peak = peak_atomic.load(std::memory_order_relaxed);
    while (new_value > prev_peak) {
        if (peak_atomic.compare_exchange_weak(
                prev_peak, new_value,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return;
        }
        // CAS failure: prev_peak is updated to peak_atomic's current value;
        // loop continues until we either install new_value or observe a
        // peer-installed peak that already covers new_value.
    }
}

inline bool size_is_limb(size_t n) {
    // REQ-3 range check (design memo §4.5.1 + §iter-79 fold REQ-3):
    //   B * MPZ_MIN_ALLOC * 8 <= mpz_limb_bytes <= B * FLINT_MPZ_MAX_CACHE_LIMBS * 8
    // per cell. Cells are in the [MPZ_MIN_ALLOC, FLINT_MPZ_MAX_CACHE_LIMBS]
    // limb range (= [2, 64] limbs = [16, 512] bytes on FLINT_BITS=64). Add a
    // multiple-of-8 requirement to exclude raw byte allocations.
    return n >= 16u && n <= 512u && (n & 7u) == 0u;
}

// iter-84 §3.2.1 OPTION (b) structural-match check. Called at free time on
// blocks that were tagged at alloc time with the size signature. Returns true
// if the page-header layout at `p` (block base) matches FLINT's
// `fmpz_block_header_s` invariants:
//
//   - count at block base must equal `flint_mpz_structs_per_block` (the
//     block is being freed exactly when count saturates per fmpz_single.c:
//     167-173 and :210-211).
//   - aligned_ptr[0] must contain `address == p` (every aligned page within
//     the block has its address field set to the block base ptr at line 120
//     of fmpz_single.c).
//
// Returns false otherwise (size-pattern false-positive: a non-mpz-pool
// allocation that happened to hit the 272 KiB signature, or a mid-life
// freed-but-not-saturated block — the latter does not actually happen because
// FLINT only calls flint_free on saturated blocks).
//
// SAFETY: reads memory at `p` (the block base, 24 bytes spanning the header)
// and at `aligned_ptr` (first aligned page within the block). Both are valid
// mapped memory at this call site because the caller's `flint_free(p)` will
// dereference them moments later; if `p` were unmapped, the free itself
// would segfault. The reads are non-atomic but use std::memcpy for strict
// aliasing safety.
inline bool page_header_structural_match(const void * p) {
    if (p == nullptr || g_flint_page_size <= 0 ||
        g_expected_struct_count_at_full_free <= 0) {
        return false;
    }
    // Read count at the block base (offset 0).
    int count = 0;
    std::memcpy(&count,
                static_cast<const char *>(p) + kHdrCountOffset,
                sizeof(count));
    if (count != static_cast<int>(g_expected_struct_count_at_full_free)) {
        return false;
    }
    // Compute the first aligned page and read its `address` field
    // (offset 16). This is the canonical block-base back-pointer set by
    // FLINT at fmpz_single.c:120 on every page within the block.
    // flint_align_ptr(ptr, page_size) returns ptr rounded UP to the next
    // page boundary (strictly greater than ptr unless ptr was already
    // aligned, in which case it returns ptr + page_size).
    const slong mask = ~(static_cast<slong>(g_flint_page_size) - 1);
    const slong p_raw = reinterpret_cast<slong>(p);
    const slong aligned_ptr_raw =
        (p_raw & mask) + static_cast<slong>(g_flint_page_size);
    const void * aligned_ptr =
        reinterpret_cast<const void *>(aligned_ptr_raw);
    void * stored_addr = nullptr;
    std::memcpy(&stored_addr,
                static_cast<const char *>(aligned_ptr) + kHdrAddressOffset,
                sizeof(stored_addr));
    return stored_addr == p;
}

}  // namespace

// ============================================================================
// FLINT wrap layer (6 functions; REQ-1 6-arg API surface).
// ============================================================================

extern "C" {

void * hf_rec1_flint_alloc(size_t size) {
    void * p = g_prev_flint_alloc ? g_prev_flint_alloc(size)
                                  : std::malloc(size);
    g_flint_malloc_total_bytes_cumulative.fetch_add(
        static_cast<int64_t>(size), std::memory_order_relaxed);
    if (p != nullptr && size == g_expected_block_size) {
        const int slot = current_omp_slot();
        // iter-86 Option (a): capture (old + delta) as the post-fetch_add
        // local-observable value; CAS-update peak via update_peak_to_max.
        const int64_t prev_bytes = g_mpz_block_bytes_in_flight.fetch_add(
            static_cast<int64_t>(size), std::memory_order_relaxed);
        update_peak_to_max(g_mpz_block_bytes_in_flight_peak,
                           prev_bytes + static_cast<int64_t>(size));
        const int64_t prev_count = g_labelled_block_count.fetch_add(
            1, std::memory_order_relaxed);
        update_peak_to_max(g_labelled_block_count_peak, prev_count + 1);
        g_block_count_per_slot[slot].fetch_add(
            1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(g_tag_mu);
            g_block_ptr_to_slot.emplace(p, slot);
        }
    }
    return p;
}

void * hf_rec1_flint_calloc(size_t count, size_t size) {
    void * p = g_prev_flint_calloc ? g_prev_flint_calloc(count, size)
                                   : std::calloc(count, size);
    if (p != nullptr) {
        g_flint_malloc_total_bytes_cumulative.fetch_add(
            static_cast<int64_t>(count) * static_cast<int64_t>(size),
            std::memory_order_relaxed);
    }
    return p;
}

void * hf_rec1_flint_realloc(void * old_ptr, size_t size) {
    // Not labelled: FLINT calls realloc on growing internal arrays (e.g.
    // mpz_free_arr in fmpz_single.c:133), never on the 272-KiB block buffer
    // itself. Pass through.
    return g_prev_flint_realloc ? g_prev_flint_realloc(old_ptr, size)
                                : std::realloc(old_ptr, size);
}

void hf_rec1_flint_free(void * p) {
    if (p != nullptr) {
        bool tagged = false;
        int slot    = 0;
        {
            std::lock_guard<std::mutex> lk(g_tag_mu);
            auto it = g_block_ptr_to_slot.find(p);
            if (it != g_block_ptr_to_slot.end()) {
                slot = it->second;
                g_block_ptr_to_slot.erase(it);
                tagged = true;
            }
        }
        if (tagged) {
            // iter-84 §3.2.1 OPTION (b) structural-match check. The check
            // runs BEFORE the chained free (so the bytes are still mapped
            // and FLINT's header content is still intact). The in-flight
            // counters are decremented unconditionally (to balance the
            // alloc-time increment); the structural-match outcome is
            // recorded separately as a diagnostic.
            if (hf_rec1_label_header_check_active.load(
                    std::memory_order_relaxed)) {
                if (page_header_structural_match(p)) {
                    g_struct_match_pass_count.fetch_add(
                        1, std::memory_order_relaxed);
                } else {
                    g_struct_match_fail_count.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            g_mpz_block_bytes_in_flight.fetch_sub(
                static_cast<int64_t>(g_expected_block_size),
                std::memory_order_relaxed);
            // labelled_block_count is per-block in-flight count (per header
            // spec: "per-block, the labelling wrap increments at flint_malloc
            // and decrements at flint_free matched by stored tag"). Decrement
            // to mirror mpz_block_bytes_in_flight's net-in-flight semantic.
            g_labelled_block_count.fetch_sub(1, std::memory_order_relaxed);
            g_block_count_per_slot[slot].fetch_sub(
                1, std::memory_order_relaxed);
        }
    }
    if (g_prev_flint_free) g_prev_flint_free(p);
    else                   std::free(p);
}

void * hf_rec1_flint_aligned_alloc(size_t alignment, size_t size) {
    return g_prev_flint_aligned_alloc
               ? g_prev_flint_aligned_alloc(alignment, size)
               : std::aligned_alloc(alignment, size);
}

void hf_rec1_flint_aligned_free(void * p) {
    if (g_prev_flint_aligned_free) g_prev_flint_aligned_free(p);
    else                           std::free(p);
}

// ============================================================================
// GMP wrap layer (3 functions; REQ-2 chain through dag_hashcons_probe).
// ============================================================================

void * hf_rec1_gmp_alloc(size_t size) {
    void * p = g_prev_gmp_alloc ? g_prev_gmp_alloc(size) : std::malloc(size);
    // iter-86 Option (a): CAS-update peak for gmp_total alongside fetch_add.
    const int64_t prev_total = g_gmp_total_bytes_in_flight.fetch_add(
        static_cast<int64_t>(size), std::memory_order_relaxed);
    update_peak_to_max(g_gmp_total_bytes_in_flight_peak,
                       prev_total + static_cast<int64_t>(size));
    if (size_is_limb(size)) {
        const int64_t prev_limb = g_mpz_limb_bytes_in_flight.fetch_add(
            static_cast<int64_t>(size), std::memory_order_relaxed);
        update_peak_to_max(g_mpz_limb_bytes_in_flight_peak,
                           prev_limb + static_cast<int64_t>(size));
        g_labelled_limb_alloc_count.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        g_gmp_other_bytes_in_flight.fetch_add(
            static_cast<int64_t>(size), std::memory_order_relaxed);
    }
    return p;
}

void * hf_rec1_gmp_realloc(void * old_ptr, size_t old_size, size_t new_size) {
    void * p = g_prev_gmp_realloc
                   ? g_prev_gmp_realloc(old_ptr, old_size, new_size)
                   : std::realloc(old_ptr, new_size);
    const int64_t delta =
        static_cast<int64_t>(new_size) - static_cast<int64_t>(old_size);
    // iter-86 Option (a): peak update after net delta application; CAS no-op
    // when delta < 0 (shrink) since new_v ≤ prev_peak.
    const int64_t prev_total_r = g_gmp_total_bytes_in_flight.fetch_add(
        delta, std::memory_order_relaxed);
    update_peak_to_max(g_gmp_total_bytes_in_flight_peak, prev_total_r + delta);
    if (size_is_limb(old_size)) {
        g_mpz_limb_bytes_in_flight.fetch_sub(
            static_cast<int64_t>(old_size), std::memory_order_relaxed);
    } else {
        g_gmp_other_bytes_in_flight.fetch_sub(
            static_cast<int64_t>(old_size), std::memory_order_relaxed);
    }
    if (size_is_limb(new_size)) {
        const int64_t prev_limb_r = g_mpz_limb_bytes_in_flight.fetch_add(
            static_cast<int64_t>(new_size), std::memory_order_relaxed);
        update_peak_to_max(g_mpz_limb_bytes_in_flight_peak,
                           prev_limb_r + static_cast<int64_t>(new_size));
    } else {
        g_gmp_other_bytes_in_flight.fetch_add(
            static_cast<int64_t>(new_size), std::memory_order_relaxed);
    }
    return p;
}

void hf_rec1_gmp_free(void * ptr, size_t size) {
    if (g_prev_gmp_free) g_prev_gmp_free(ptr, size);
    else                 std::free(ptr);
    g_gmp_total_bytes_in_flight.fetch_sub(
        static_cast<int64_t>(size), std::memory_order_relaxed);
    if (size_is_limb(size)) {
        g_mpz_limb_bytes_in_flight.fetch_sub(
            static_cast<int64_t>(size), std::memory_order_relaxed);
    } else {
        g_gmp_other_bytes_in_flight.fetch_sub(
            static_cast<int64_t>(size), std::memory_order_relaxed);
    }
}

}  // extern "C"

// ============================================================================
// Init / snapshot / finalize.
// ============================================================================

void hf_rec1_init() {
    const char * master = HF_FLAG_REC1_TRACK_MPZ_POOL;
    if (master == nullptr || master[0] == '\0' || master[0] == '0') {
        // OFF path: REC-1 stays inactive, wrap functions are never installed,
        // OFF-path budget (design memo §3.5) is structurally honoured.
        return;
    }

    // iter-84 REC-1 idempotence guard (BINDING reviewer aca17701072051b8f).
    // A second call to `hf_rec1_init()` while already-active is a no-op.
    // Rationale: protect against (a) test driver re-invocations between
    // cases, (b) future call-sites that might re-enter the init path after
    // the Phase 0.5 retrofit was reloaded, and (c) accidental double-init
    // in long-lived CLI worker processes. Without the guard, a second init
    // would call `__flint_set_all_memory_functions` and chain REC-1 wraps
    // on top of REC-1 wraps, doubling the in-flight counter increments per
    // alloc and corrupting the partition snapshot.
    if (hf_rec1_active.load(std::memory_order_acquire)) {
        return;
    }

    // Read sub-flag for the §3.2.1 page-header signature check.
    const char * hdr_env = HF_FLAG_REC1_LABEL_HEADER_CHECK;
    hf_rec1_label_header_check_active.store(
        (hdr_env == nullptr) || (hdr_env[0] != '0'),
        std::memory_order_relaxed);

    // Compute expected block-size signature. Mirrors fmpz_single.c:51 +
    // flint_get_page_size() (line 64-75).
    g_flint_page_size = sysconf(_SC_PAGESIZE);
    if (g_flint_page_size <= 0) g_flint_page_size = 4096;
    constexpr int kPagesPerBlock = 16;  // fmpz_single.c:51
    g_expected_block_size =
        static_cast<size_t>(kPagesPerBlock) *
            static_cast<size_t>(g_flint_page_size) +
        static_cast<size_t>(g_flint_page_size);
    // = 17 * page_size = 272 KiB on macOS 16K, 68 KiB on Linux 4K.

    // iter-84 §3.2.1: compute `flint_mpz_structs_per_block` exactly the way
    // FLINT does at fmpz_single.c:108-113. `__mpz_struct` is the canonical
    // GMP mpz representation: { int alloc, int size, mp_limb_t * d } — 4 + 4
    // + 8 = 16 bytes on 64-bit platforms. The header skip is computed in
    // units of `sizeof(__mpz_struct)`, rounded up. With our header layout
    // approximating 24 bytes (count + padding + thread + address), skip = 2
    // on macOS 16K and Linux 4K. The block holds PAGES_PER_BLOCK * (num -
    // skip) mpz structs total. At full free, count == this value.
    constexpr size_t kMpzStructSize = 16;  // __mpz_struct size on 64-bit
    constexpr size_t kHeaderSize = 24;     // count + padding + thread + address
    const size_t skip = (kHeaderSize - 1) / kMpzStructSize + 1;
    const size_t num = static_cast<size_t>(g_flint_page_size) / kMpzStructSize;
    g_expected_struct_count_at_full_free =
        static_cast<slong>(kPagesPerBlock) *
        (static_cast<slong>(num) - static_cast<slong>(skip));
    // = 16 * (1024 - 2) = 16352 on macOS 16K, 16 * (256 - 2) = 4064 on Linux 4K.

    // REQ-1 defense-in-depth: detect whether the Phase 0.5 retrofit has run.
    // The retrofit defines `hf_phase05_retrofit_done` and sets it true at the
    // end of hf_init_mimalloc_for_gmp_flint(). In the test executable (which
    // does not link gmp_mimalloc_init.cpp), the weak extern resolves to
    // nullptr — we treat that as "test mode" and proceed with a notice.
    const bool retrofit_done =
        (&hf_phase05_retrofit_done != nullptr) ? hf_phase05_retrofit_done
                                               : false;
    const bool weak_symbol_absent = (&hf_phase05_retrofit_done == nullptr);
    if (!weak_symbol_absent && !retrofit_done) {
        // Production binary linked against gmp_mimalloc_init.cpp but the
        // retrofit was not invoked before REC-1 init. REFUSE install per
        // design memo §iter-79 fold REQ-1 defense-in-depth assertion.
        std::fprintf(stderr,
            "[hf_rec1_init] REFUSES install: Phase 0.5 mimalloc retrofit "
            "(hf_init_mimalloc_for_gmp_flint) did not run before REC-1 init. "
            "REC-1 chains through the retrofit's allocator bindings; without "
            "the retrofit, the FLINT aligned-alloc would still be at FLINT "
            "default _flint_aligned_alloc2 (memory_manager.c:40), which "
            "would mean REC-1 would silently route aligned allocations to "
            "libsystem-malloc instead of mimalloc. REC-1 stays inactive.\n");
        return;
    }
    if (weak_symbol_absent) {
        std::fprintf(stderr,
            "[hf_rec1_init] test mode: hf_phase05_retrofit_done weak extern "
            "is absent (gmp_mimalloc_init.cpp not linked). Installing REC-1 "
            "wraps against the current FLINT/GMP defaults. NOT a production "
            "binary path.\n");
    }

    // Snapshot the FLINT 6-tuple BEFORE installing REC-1 wraps. REQ-1: use
    // the 6-arg API so we capture (and preserve) the aligned bindings.
    __flint_get_all_memory_functions(
        &g_prev_flint_alloc,
        &g_prev_flint_calloc,
        &g_prev_flint_realloc,
        &g_prev_flint_free,
        &g_prev_flint_aligned_alloc,
        &g_prev_flint_aligned_free);

    if (g_prev_flint_alloc == nullptr || g_prev_flint_free == nullptr) {
        std::fprintf(stderr,
            "[hf_rec1_init] REFUSES install: snapshotted FLINT alloc/free "
            "pointer is null. REC-1 stays inactive.\n");
        return;
    }

    __flint_set_all_memory_functions(
        &hf_rec1_flint_alloc,
        &hf_rec1_flint_calloc,
        &hf_rec1_flint_realloc,
        &hf_rec1_flint_free,
        &hf_rec1_flint_aligned_alloc,
        &hf_rec1_flint_aligned_free);

    // Snapshot the GMP 3-tuple (REQ-2: this captures dag_hashcons_probe's
    // wraps if hf_probe_init() ran earlier with HF_DAG_HASHCONS_PROBE=1, else
    // the Phase 0.5 retrofit gmp_* adapters, else GMP libsystem default in
    // test mode).
    mp_get_memory_functions(&g_prev_gmp_alloc,
                            &g_prev_gmp_realloc,
                            &g_prev_gmp_free);
    mp_set_memory_functions(&hf_rec1_gmp_alloc,
                            &hf_rec1_gmp_realloc,
                            &hf_rec1_gmp_free);

    // Output directory.
    const char * outdir = HF_FLAG_REC1_OUT_DIR;
    g_out_dir = (outdir && outdir[0]) ? outdir : ".";

    // REQ-E iter-83-δ fold (agentId `aca17701072051b8f`): mkdir the out_dir if
    // it does not exist. Otherwise snapshots silently fail (line 498 logs to
    // stderr but the ctest harness sets HF_REC1_OUT_DIR to a build-tree
    // subdirectory that CMake does NOT auto-create). Single-level mkdir is
    // sufficient because ctest's path is one level under the build dir.
    // ENOENT (parent missing) or EEXIST (already exists) are both acceptable;
    // any other errno is reported but non-fatal.
    if (outdir && outdir[0]) {
        if (mkdir(outdir, 0755) != 0 && errno != EEXIST) {
            std::fprintf(stderr,
                "[hf_rec1_init] WARNING: could not create HF_REC1_OUT_DIR=%s "
                "(errno=%d). Snapshots may fail to write.\n", outdir, errno);
        }
    }

    // iter-84 impl-2 update: the §3.2.1 OPTION (b) page-header structural-
    // match check is now LIVE. When HF_REC1_LABEL_HEADER_CHECK is unset or
    // != "0", the check runs at every tagged-block free and updates
    // `struct_match_pass_count` / `struct_match_fail_count`. When set to
    // "0", the check is suppressed (size-pattern-only labelling). Emit a
    // one-time stderr notice indicating which mode is active.
    if (hf_rec1_label_header_check_active.load(std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[hf_rec1_init] page-header structural-match check ACTIVE "
            "(HF_REC1_LABEL_HEADER_CHECK!=0). Expected count-at-full-free = "
            "%ld; expected aligned_ptr.address = block-base ptr. "
            "Counters reported as struct_match_pass_count / "
            "struct_match_fail_count in the snapshot JSON.\n",
            static_cast<long>(g_expected_struct_count_at_full_free));
    } else {
        std::fprintf(stderr,
            "[hf_rec1_init] page-header structural-match check SUPPRESSED "
            "(HF_REC1_LABEL_HEADER_CHECK=0). Labelling is size-pattern-only; "
            "false-positive rate diagnostic is not collected.\n");
    }

    hf_rec1_active.store(true, std::memory_order_release);
    std::atexit(&hf_rec1_finalize);
}

HfRec1PartitionSnapshot hf_rec1_get_partition_snapshot() {
    HfRec1PartitionSnapshot s;
    s.mpz_block_bytes_in_flight =
        g_mpz_block_bytes_in_flight.load(std::memory_order_relaxed);
    s.labelled_block_count =
        g_labelled_block_count.load(std::memory_order_relaxed);
    s.flint_malloc_total_bytes_cumulative =
        g_flint_malloc_total_bytes_cumulative.load(std::memory_order_relaxed);
    s.mpz_limb_bytes_in_flight =
        g_mpz_limb_bytes_in_flight.load(std::memory_order_relaxed);
    s.labelled_limb_alloc_count =
        g_labelled_limb_alloc_count.load(std::memory_order_relaxed);
    s.gmp_other_bytes_in_flight =
        g_gmp_other_bytes_in_flight.load(std::memory_order_relaxed);
    s.gmp_total_bytes_in_flight =
        g_gmp_total_bytes_in_flight.load(std::memory_order_relaxed);
    s.struct_match_pass_count =
        g_struct_match_pass_count.load(std::memory_order_relaxed);
    s.struct_match_fail_count =
        g_struct_match_fail_count.load(std::memory_order_relaxed);

    // iter-86 Option (a) peak-tracking snapshot fields.
    s.mpz_block_bytes_in_flight_peak =
        g_mpz_block_bytes_in_flight_peak.load(std::memory_order_relaxed);
    s.labelled_block_count_peak =
        g_labelled_block_count_peak.load(std::memory_order_relaxed);
    s.mpz_limb_bytes_in_flight_peak =
        g_mpz_limb_bytes_in_flight_peak.load(std::memory_order_relaxed);
    s.gmp_total_bytes_in_flight_peak =
        g_gmp_total_bytes_in_flight_peak.load(std::memory_order_relaxed);

    // REQ-4 fold: mimalloc committed-bytes from mi_process_info.peak_commit.
    // mi_process_info signature (from third_party/mimalloc/mimalloc.h):
    //   void mi_process_info(size_t* elapsed_msecs,
    //                        size_t* user_msecs,
    //                        size_t* system_msecs,
    //                        size_t* current_rss,
    //                        size_t* peak_rss,
    //                        size_t* current_commit,
    //                        size_t* peak_commit,
    //                        size_t* page_faults);
    if (mi_process_info != nullptr) {
        size_t elapsed = 0, user_ms = 0, sys_ms = 0;
        size_t cur_rss = 0,  peak_rss = 0;
        size_t cur_com = 0,  peak_com = 0;
        size_t page_faults = 0;
        mi_process_info(&elapsed, &user_ms, &sys_ms,
                        &cur_rss, &peak_rss,
                        &cur_com, &peak_com, &page_faults);
        // REQ-D iter-83-δ fold (agentId `aca17701072051b8f`): mimalloc reports
        // SIZE_MAX-ish sentinels on some platforms when the value is unknown.
        // Mirror the `sane_mb` filter pattern from hyper_int.cpp:198 — clamp
        // implausibly-large values to -1 to match the "absent" sentinel.
        // 1 PB threshold matches hyper_int.cpp.
        //
        // iter-84 REC-6 fold (BINDING reviewer aca17701072051b8f): renamed
        // field from `mi_resident_bytes_peak` to `mi_committed_bytes_peak`
        // to match mimalloc's `peak_commit` source-name (peak_com here).
        constexpr size_t kMaxSane = static_cast<size_t>(1) << 50;  // 1 PB
        s.mi_committed_bytes_peak = (peak_com > kMaxSane)
                                        ? int64_t{-1}
                                        : static_cast<int64_t>(peak_com);
        s.process_peak_rss_bytes  = (peak_rss > kMaxSane)
                                        ? int64_t{-1}
                                        : static_cast<int64_t>(peak_rss);
    } else {
        s.mi_committed_bytes_peak = -1;
        s.process_peak_rss_bytes  = -1;
    }
    return s;
}

int64_t hf_rec1_get_slot_block_count(int omp_slot) {
    if (omp_slot < 0) {
        int64_t total = 0;
        for (int i = 0; i < kSlots; ++i) {
            total += g_block_count_per_slot[i].load(
                std::memory_order_relaxed);
        }
        return total;
    }
    if (omp_slot >= kSlots) return 0;
    return g_block_count_per_slot[omp_slot].load(std::memory_order_relaxed);
}

// iter-84 REC-4 fold helper: format `bytes` as a double-precision MiB value
// inline into an output stream, with 3 decimal digits. Standalone so it does
// not pull <iomanip> into the .cpp top.
//
// Sentinel passthrough: `-1` bytes (mimalloc-absent marker, see
// `mi_committed_bytes_peak` / `process_peak_rss_bytes`) emits `-1` literal
// rather than a meaningless `-0.000` MiB. Negative values from real underflow
// would emit a real negative MiB (still informative as a diagnostic).
inline void emit_mib(std::ofstream & f, int64_t bytes) {
    if (bytes == -1) {
        f << "-1";
        return;
    }
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", mib);
    f << buf;
}

void hf_rec1_snapshot(const char * fixture_id, const char * checkpoint_tag) {
    if (!hf_rec1_active.load(std::memory_order_acquire)) return;

    const HfRec1PartitionSnapshot s = hf_rec1_get_partition_snapshot();
    // REQ-G iter-83-δ fold: JSON-escape caller-supplied strings before embed.
    // Note: also use the escaped form in the filename path (rare but defensive
    // against unsafe filesystem chars; mkdir/open both tolerate the result).
    const std::string fix = json_escape(fixture_id     ? fixture_id     : "unk");
    const std::string tag = json_escape(checkpoint_tag ? checkpoint_tag : "unk");
    const std::string path =
        g_out_dir + "/rec1_mpz_pool_" + fix + "_" + tag + ".json";

    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f.is_open()) {
        std::fprintf(stderr,
            "[hf_rec1_snapshot] could not open %s for write; snapshot "
            "skipped.\n", path.c_str());
        return;
    }

    // iter-84 REC-4 fold (BINDING reviewer aca17701072051b8f): every byte-
    // valued field is emitted alongside a `_MiB` sibling with 3-decimal-digit
    // double-precision so the JSON is directly human-readable without a
    // separate post-processing step. Sentinel -1 passes through.
    f << "{\n";
    f << "  \"probe\":               \"rec1_mpz_pool\",\n";
    f << "  \"schema\":              3,\n";   // iter-86 bumped: Option (a) peak-tracking fields added
    f << "  \"fixture_id\":          \"" << fix << "\",\n";
    f << "  \"checkpoint_tag\":      \"" << tag << "\",\n";
    f << "  \"expected_block_size\": " << g_expected_block_size << ",\n";
    f << "  \"flint_page_size\":     " << g_flint_page_size << ",\n";
    f << "  \"expected_struct_count_at_full_free\": "
      << g_expected_struct_count_at_full_free << ",\n";
    f << "  \"label_header_check_active\": "
      << (hf_rec1_label_header_check_active.load(std::memory_order_relaxed)
              ? "true" : "false") << ",\n";

    f << "  \"mpz_block_bytes_in_flight\":          "
      << s.mpz_block_bytes_in_flight << ",\n";
    f << "  \"mpz_block_bytes_in_flight_MiB\":      ";
    emit_mib(f, s.mpz_block_bytes_in_flight);
    f << ",\n";
    // iter-86 Option (a) peak-tracking fields.
    f << "  \"mpz_block_bytes_in_flight_peak\":     "
      << s.mpz_block_bytes_in_flight_peak << ",\n";
    f << "  \"mpz_block_bytes_in_flight_peak_MiB\": ";
    emit_mib(f, s.mpz_block_bytes_in_flight_peak);
    f << ",\n";
    f << "  \"labelled_block_count\":               "
      << s.labelled_block_count << ",\n";
    f << "  \"labelled_block_count_peak\":          "
      << s.labelled_block_count_peak << ",\n";

    // REQ-A/REQ-C iter-83-δ fold (agentId `aca17701072051b8f`): JSON field
    // name carries the CUMULATIVE-NOT-IN-FLIGHT semantic clearly.
    f << "  \"flint_malloc_total_bytes_cumulative\":     "
      << s.flint_malloc_total_bytes_cumulative << ",\n";
    f << "  \"flint_malloc_total_bytes_cumulative_MiB\": ";
    emit_mib(f, s.flint_malloc_total_bytes_cumulative);
    f << ",\n";

    f << "  \"mpz_limb_bytes_in_flight\":           "
      << s.mpz_limb_bytes_in_flight << ",\n";
    f << "  \"mpz_limb_bytes_in_flight_MiB\":       ";
    emit_mib(f, s.mpz_limb_bytes_in_flight);
    f << ",\n";
    // iter-86 Option (a) peak-tracking field.
    f << "  \"mpz_limb_bytes_in_flight_peak\":      "
      << s.mpz_limb_bytes_in_flight_peak << ",\n";
    f << "  \"mpz_limb_bytes_in_flight_peak_MiB\":  ";
    emit_mib(f, s.mpz_limb_bytes_in_flight_peak);
    f << ",\n";
    f << "  \"labelled_limb_alloc_count\":          "
      << s.labelled_limb_alloc_count << ",\n";

    f << "  \"gmp_other_bytes_in_flight\":          "
      << s.gmp_other_bytes_in_flight << ",\n";
    f << "  \"gmp_other_bytes_in_flight_MiB\":      ";
    emit_mib(f, s.gmp_other_bytes_in_flight);
    f << ",\n";

    f << "  \"gmp_total_bytes_in_flight\":          "
      << s.gmp_total_bytes_in_flight << ",\n";
    f << "  \"gmp_total_bytes_in_flight_MiB\":      ";
    emit_mib(f, s.gmp_total_bytes_in_flight);
    f << ",\n";
    // iter-86 Option (a) peak-tracking field.
    f << "  \"gmp_total_bytes_in_flight_peak\":     "
      << s.gmp_total_bytes_in_flight_peak << ",\n";
    f << "  \"gmp_total_bytes_in_flight_peak_MiB\": ";
    emit_mib(f, s.gmp_total_bytes_in_flight_peak);
    f << ",\n";

    // iter-84 REC-6 fold: renamed from `mi_resident_bytes_peak` to
    // `mi_committed_bytes_peak`.
    f << "  \"mi_committed_bytes_peak\":            "
      << s.mi_committed_bytes_peak << ",\n";
    f << "  \"mi_committed_bytes_peak_MiB\":        ";
    emit_mib(f, s.mi_committed_bytes_peak);
    f << ",\n";

    f << "  \"process_peak_rss_bytes\":             "
      << s.process_peak_rss_bytes << ",\n";
    f << "  \"process_peak_rss_bytes_MiB\":         ";
    emit_mib(f, s.process_peak_rss_bytes);
    f << ",\n";

    // iter-84 §3.2.1 OPTION (b) structural-match counters.
    f << "  \"struct_match_pass_count\":            "
      << s.struct_match_pass_count << ",\n";
    f << "  \"struct_match_fail_count\":            "
      << s.struct_match_fail_count << ",\n";

    f << "  \"slot_block_counts\": [";
    for (int i = 0; i < kSlots; ++i) {
        if (i > 0) f << ", ";
        f << g_block_count_per_slot[i].load(std::memory_order_relaxed);
    }
    f << "]\n";
    f << "}\n";
}

void hf_rec1_finalize() {
    bool expected = false;
    if (!g_finalize_done.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;  // already finalized once; idempotent.
    }
    if (!hf_rec1_active.load(std::memory_order_acquire)) return;

    hf_rec1_snapshot("aggregate", "atexit");
}

}  // namespace hyperflint
