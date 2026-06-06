// HF FF Phase 5 §E Step E.2-impl-2 (iter-60-β.2).
//
// `OperatorMemo<KeyT, ValueT>` — sharded shared-mutex hash-map memoization
// template, the load-bearing data structure for Lever 5.4 operator-call
// memoization. Wraps five HF operator boundaries:
//   - Rat::add                              (rat.cpp)
//   - reduce_inplace                        (rat.cpp:1035)
//   - linear_factors                        (linear_factors.cpp:876)
//   - partial_fractions                     (partial_fractions.cpp:336)
//   - transform_shuffle                     (transform.cpp:1058)
//
// Per the §E implementation memo §2.1 + §iter-59 fold appendix the cache
// is a fixed-size array of shards (32 shards at OMP=13 default; per design
// memo §4.1 `max(32, OMP_NUM_THREADS × 2)`). Each shard owns its own
// `std::shared_mutex` and a `std::unordered_map<KeyHash, CacheEntry>`. The
// shard index is the low log2(shard_count_) bits of the 64-bit XXH3 hash;
// XXH3 produces well-distributed low bits so no extra mixing is needed.
//
// Cache HIT discipline (iter-70 REC-2: `std::shared_ptr<const ValueT>` COW):
//   1. Acquire shared_lock on the shard.
//   2. Look up by KeyHash (64-bit) in the shard's unordered_map.
//   3. If found, verify full-equality (KeyT::operator==) on the stored key
//      to defend against XXH3 64-bit collisions (FOLD-M3 BINDING).
//   4. If full-equality matches, COPY the `shared_ptr<const ValueT>`
//      held in `it->second.stored_value` WHILE the shared_lock is still
//      held; the ref-count increment is atomic and lock-free.
//      Return the copy; the lock releases at scope exit but the caller
//      holds an owning ref to the referent — safe against any future
//      LRU eviction or rehash on the cache side.
//   5. If full-equality fails, increment collision counter, return
//      `nullptr` (MISS, caller falls through to compute path).
//
// Iter-62-δ adversarial-reviewer (agentId a0dd0c1c009d169d4) verdict
// FUNDAMENTAL_FLAW: prior implementation returned raw `const ValueT*`
// to `it->second.stored_value`, dangling after the lock released —
// concurrent `insert()` (unique_lock) could rehash the unordered_map
// invalidating the pointer OR destructively overwrite stored_value
// mid-caller-copy. The iter-63 REQ-1 fix used `std::optional<ValueT>`
// (deep-copy under shared_lock); iter-70 REC-2 supersedes that with
// `std::shared_ptr<const ValueT>` (ref-counted handle, COW), which
// (a) eliminates the deep-copy on HIT, and (b) is safe-by-construction
// against the iter-62-δ S2 rehash + S4 destructive-overwrite hazards
// (the const-qualifier prevents writer-side mutation of an outstanding
// referent — a writer can only swap the cache entry's shared_ptr to
// point at a NEW const ValueT). See design.md §iter-69 fold appendix.
//
// Cache MISS / INSERT discipline:
//   1. Caller runs the impl body to produce ValueT.
//   2. Caller calls insert(key, hash, std::move(value)).
//   3. insert() acquires unique_lock on the shard, emplaces.
//
// SCALAR_REP=1 disposition (§iter-59-fold-REQ-3 + REQ-7; FOLD-ER3):
//   The per-boundary `*_enabled()` predicates in this file gate-off the
//   relevant operator boundary under HF_USE_SCALAR_REP=1:
//   - transform_shuffle_enabled() returns false   (primary FOLD-ER3 mandate)
//   - lf_enabled() returns false                  (REQ-3 option b)
//   - pf_enabled() returns false                  (REQ-3 option b)
//   The following REMAIN ENABLED under SCALAR_REP=1 (REQ-7 option b):
//   - rat_add_enabled() returns true              (no ZWTable embedding hazard)
//   - reduce_enabled() returns true               (no ZWTable embedding hazard)
//
// Env-gate convention (§5):
//   HF_OPERATOR_MEMO={0,1}                     — master switch (default 0)
//   HF_OPERATOR_MEMO_OFF_{RAT_ADD,REDUCE,LF,PF,TRANSFORM}={0,1}
//                                              — per-boundary disable
//   HF_OPERATOR_MEMO_COLLISION_LOG={0,1}       — collision-log JSON emit
//   HF_OPERATOR_MEMO_LRU_CAP_PER_OP=<int>      — LRU cap (default 1000;
//                                                 iter-70 REC-2 lever)
//
// References:
//   notes/hf_finite_field_program/phase5_three_paths/lever_5_4_op_call_memoization/design.md
//   notes/hf_finite_field_program/phase5_three_paths/lever_5_4_op_call_memoization/implementation/implementation_memo.md
//     §2.1 OperatorMemo template
//     §2.4 Shared-mutex sharding
//     §2.5 LRU intrusive list per shard (forward-scaffolded; design memo §4.2 Tier B)
//     §iter-59-fold-REQ-3, §iter-59-fold-REQ-6, §iter-59-fold-REQ-7

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>          // std::unique_lock (libstdc++ does not pull it in
                          // transitively via <shared_mutex> the way libc++ does)
#include <shared_mutex>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hyperflint {

// Forward declarations needed by `operator_memo` namespace below.
// (Full forward declarations for the wrap accessors are repeated near
//  the per-op accessor block; the duplicates are harmless.)
class Poly;
namespace canonical_signature {
struct ReduceKey;
} // namespace canonical_signature

// ---------------------------------------------------------------------------
// OperatorMemo<KeyT, ValueT>
//
// KeyT  must be EqualityComparable (operator==), MoveConstructible, and
//       CopyConstructible. The cache stores keys by value for full-equality
//       verification on HIT.
// ValueT must be MoveConstructible. CopyConstructibility is NOT required at
//        the cache layer under iter-70 REC-2: the value is heap-allocated via
//        std::make_shared<const ValueT>(std::move(value)) at insert time and
//        readers receive a ref-counted handle.
// ---------------------------------------------------------------------------
template<typename KeyT, typename ValueT>
class OperatorMemo {
public:
    using KeyHash = std::uint64_t;

    struct CacheEntry {
        KeyT                              stored_key;     // full-equality verify on HIT
        std::shared_ptr<const ValueT>     stored_value;   // iter-70 REC-2 COW handle
        std::uint64_t                     insertion_seq;  // monotonically increasing
        std::uint32_t                     hit_count;      // local LRU surrogate
    };

    OperatorMemo() = default;

    // Non-copyable, non-movable (global singletons per-boundary).
    OperatorMemo(const OperatorMemo&)            = delete;
    OperatorMemo& operator=(const OperatorMemo&) = delete;
    OperatorMemo(OperatorMemo&&)                 = delete;
    OperatorMemo& operator=(OperatorMemo&&)      = delete;

    // Lookup: returns `std::shared_ptr<const ValueT>` on HIT (the
    // CacheEntry's owning handle, ref-count incremented under shared_lock),
    // `nullptr` on MISS or hash collision.
    //
    // iter-70 REC-2 (binding; reviewer agentId ac3f35a4db605c292 disposition
    // CONCERNS-FOLD with 4 REQ folds applied IN-PLACE, fold_then_proceed):
    // supersedes the iter-63 REQ-1 std::optional<ValueT> deep-copy form.
    // The shared_ptr<const ValueT> return:
    //   (a) eliminates the per-HIT deep-copy under shared_lock (the
    //       lock-held window shrinks from O(value-size) to O(1) +
    //       atomic ref-count increment);
    //   (b) is safe-by-construction against the iter-62-δ FUNDAMENTAL_FLAW
    //       hazards — the ref-counted ownership keeps the referent alive
    //       across any rehash on the cache's unordered_map (the referent
    //       is heap-allocated independently of the map's bucket array);
    //       the const-qualifier prevents writer-side mutation of an
    //       outstanding referent (a colliding insert can only swap the
    //       CacheEntry's shared_ptr to point at a NEW const ValueT, not
    //       mutate the existing one).
    //
    // The caller dereferences via `if (auto sp = ...; sp) { use(*sp); }`.
    // `*sp` yields a `const ValueT&`; the caller's use-site is responsible
    // for any deep-copy at the return boundary (typically a copy ctor at
    // `return *sp;`). The cache itself never re-copies the referent.
    //
    // See notes/.../lever_5_4_op_call_memoization/design.md §iter-69 fold
    // appendix for the full design (including the safety-by-construction
    // analysis vs iter-62-δ S2 rehash + S4 destructive-overwrite hazards).
    std::shared_ptr<const ValueT> try_lookup(const KeyT& key, KeyHash hash);

    // Insert (acquires unique_lock on the target shard).
    // `value` is moved into a heap allocation via std::make_shared, and the
    // resulting shared_ptr<const ValueT> becomes the CacheEntry's
    // stored_value. iter-70 REC-2 REC-2 (DEFERRED → APPLIED): the
    // make_shared allocation happens OUTSIDE the unique_lock to keep the
    // lock-held window minimal.
    void insert(KeyT key, KeyHash hash, ValueT value);

    // Clear all cache contents (called from clear_between_fixtures()).
    void clear_all_shards();

    // §E iter-7 REQ-4 (round-robin-per-shard): evict approximately
    // `n_total` entries from this cache, distributed across all 32
    // shards as ceil(n_total / shard_count_) per shard (capped at the
    // shard's current entry count). Per-shard victim selection: the
    // entries with the smallest (hit_count, insertion_seq) tuple,
    // identical tie-breaking to the iter-75 insert-time LRU at
    // operator_memo.hpp::insert(). Returns the actual eviction count
    // (≤ n_total when shards are sparse).
    //
    // Concurrency contract: the per-shard unique_lock makes this safe to
    // call concurrently with try_lookup/insert, but in practice it is
    // intended to fire from the post-`integration_step()` serial scope
    // (hyper_int.cpp:1243 outer-loop boundary), AFTER OMP regions have
    // joined via implicit barrier. Determinism notes are in
    // notes/.../lever_e_op_memo_eviction_rss_pressure/determinism_audit.md.
    //
    // Algorithm complexity per shard: O(m) shard size scan + O(m log k)
    // partial_sort for k = n_per_shard victims. At cap=0 (LRU disabled)
    // m can be large; at cap=1000 (iter-75 default) m ≤ 31 → partial
    // sort is sub-microsecond. The eviction's primary cost at large m
    // is the linear scan, not the partial_sort.
    std::size_t evict_lru_batch(std::size_t n_total);

    // Diagnostic counters (atomic; safe to read under concurrent traffic).
    std::uint64_t collision_count() const { return collisions_.load(std::memory_order_relaxed); }
    std::uint64_t hit_count()       const { return hits_.load(std::memory_order_relaxed); }
    std::uint64_t miss_count()      const { return misses_.load(std::memory_order_relaxed); }
    std::uint64_t insert_count()    const { return inserts_.load(std::memory_order_relaxed); }
    std::uint64_t eviction_count()  const { return evictions_.load(std::memory_order_relaxed); }
    std::uint64_t entry_count()     const;

private:
    // shard_count_ is a power of 2 so `hash & (shard_count_-1)` works as the
    // shard selector without a modulo. 32 shards covers OMP=13 default with
    // ~2.5x oversubscription per design memo §4.1.
    static constexpr std::size_t shard_count_ = 32;
    static_assert((shard_count_ & (shard_count_ - 1)) == 0,
                  "shard_count_ must be a power of 2");

    struct Shard {
        mutable std::shared_mutex               mtx;
        std::unordered_map<KeyHash, CacheEntry> map;
    };
    std::array<Shard, shard_count_>               shards_;

    std::atomic<std::uint64_t> collisions_   {0};
    std::atomic<std::uint64_t> hits_         {0};
    std::atomic<std::uint64_t> misses_       {0};
    std::atomic<std::uint64_t> inserts_      {0};
    std::atomic<std::uint64_t> evictions_    {0};  // §E iter-7: post-step LRU evictions
    std::atomic<std::uint64_t> insertion_seq_{0};

    static constexpr std::size_t shard_index_(KeyHash h) {
        return static_cast<std::size_t>(h) & (shard_count_ - 1);
    }
};

// ---------------------------------------------------------------------------
// Per-boundary enabled predicates (defined in operator_memo.cpp).
//
// These honour the master switch + per-boundary disable + SCALAR_REP=1
// forced-disable per §iter-59-fold-REQ-3 + REQ-7.
// ---------------------------------------------------------------------------
namespace operator_memo {

bool rat_add_enabled();
bool reduce_enabled();
bool lf_enabled();
bool pf_enabled();
bool transform_shuffle_enabled();

// Master predicate (false ⇒ all per-boundary predicates return false).
// Parsed once at startup; subsequent reads are atomic and lock-free.
bool master_enabled();

// Re-parse env vars (test harness; not called from production).
void reload_env_for_testing();

// LRU cap (per-shard derived from HF_OPERATOR_MEMO_LRU_CAP_PER_OP /
// shard_count_; design memo §4.2 Tier B forward-scaffolding).
std::size_t lru_cap_per_op();

// Collision-log toggle (HF_OPERATOR_MEMO_COLLISION_LOG; default ON when
// master enabled).
bool collision_log_enabled();

// Clear all five §E caches at fixture boundary. Wired from
// integration_step.cpp:fixture_boundary at iter-N+ Step E.2-impl-2 per-op
// wraps; the function is a no-op when called before the per-op wraps are
// landed (the caches are empty).
void clear_between_fixtures();

// iter-62-β.1: reduce-cache helpers that hide ReduceValue inside
// operator_memo.cpp. The wrap site at rat.cpp::reduce_inplace uses these
// to look up / insert without seeing ReduceValue's complete definition.
//
//   - reduce_try_lookup_and_apply: on HIT, deep-copies cached
//     (num_post, den_post, kind) into the by-reference slots, returns true.
//     On MISS (or hash collision), returns false; the by-reference slots
//     are unchanged.
//   - reduce_insert_with_kind: emplace the post-state into the cache with
//     the wrap-derived classifier kind. Polys are deep-copied (the wrap
//     site's local copies persist until insert returns).
bool reduce_try_lookup_and_apply(
    const canonical_signature::ReduceKey& key,
    std::uint64_t key_hash,
    Poly& num_out,
    Poly& den_out,
    int& kind_out);

void reduce_insert_with_kind(
    canonical_signature::ReduceKey key,
    std::uint64_t key_hash,
    const Poly& num_post,
    const Poly& den_post,
    int kind);

// ---------------------------------------------------------------------------
// §E iter-7: RSS-pressure-driven LRU eviction at integration-step boundary.
// See notes/.../lever_e_op_memo_eviction_rss_pressure/design.md (REQ-1..REQ-8
// folded) and determinism_audit.md.
//
// Env vars (all default OFF/0; opt-in semantics):
//   HF_OP_MEMO_EVICT_ON_RSS={0,1}           master switch (default 0)
//   HF_OP_MEMO_EVICT_RSS_THRESHOLD_MB=<int> trigger above this RSS (MiB);
//                                           default = 80% of hw.memsize
//                                           via sysctlbyname or sysconf
//                                           fallback (REQ-5).
//   HF_OP_MEMO_EVICT_LRU_BATCH=<int>        evictions per cache per trigger
//                                           (default 64)
//   HF_OP_MEMO_EVICT_STRATEGY=<lru|fifo>    eviction order (default lru;
//                                           fifo is min(insertion_seq) only)
//   HF_OP_MEMO_EVICT_TRACE={0,1}            stderr trace event per trigger
//
// Mutual exclusion (composability): HF_MI_COLLECT_OPTION_M_C=1 AND
// HF_OP_MEMO_EVICT_ON_RSS=1 is fatal — emit stderr message and abort at
// first env_gate access. iter-4 lessons_learned[11]: §6.M Option M.c is
// `mi_collect`-family pool-stickiness lever; §E is HF-internal-cache
// eviction. Running both simultaneously confounds memory-attribution.
// ---------------------------------------------------------------------------
bool evict_on_rss_enabled();
std::size_t evict_rss_threshold_bytes();
std::size_t evict_lru_batch_size();
bool evict_trace_enabled();
bool evict_strategy_is_fifo();  // false ⇒ default LRU

// Sum of evictions across all 5 caches (rat_add, reduce, lf, pf,
// transform_shuffle); per-cache eviction count = `n_per_cache`.
std::size_t evict_lru_batch_all_caches(std::size_t n_per_cache);

// Post-step hook called from hyper_int.cpp outer loop after `current` is
// updated (post-OMP-region serial scope at hyper_int.cpp:1243). Reads
// current peak RSS via getrusage(RUSAGE_SELF), compares to threshold,
// and fires evict_lru_batch_all_caches when threshold is exceeded.
// Cheap no-op when master switch is OFF (single env-gate load).
void evict_post_step_hook();

} // namespace operator_memo

// ---------------------------------------------------------------------------
// counter_replay shims (defined in operator_memo.cpp).
//
// Per §iter-59-fold-REQ-5 §4.4-bis table: on cache HIT each per-op shim
// replays the boundary's diagnostic counters that would have been
// incremented by the cached body. Wall-time counters (e.g.
// `rat_add_legacy_wall_storage`) are NOT replayed — measuring zero wall
// under HIT is the correct semantics, not counter suppression.
//
// The op_call probe at the integrator step level fires unconditionally
// on every wrap entry (whether HIT or MISS); the wraps in §3 do not need
// to call op_call separately because the surrounding integrator step
// has already fired it.
// ---------------------------------------------------------------------------
namespace counter_replay {

void rat_add_on_hit();
// iter-62-β.3: `kind` selects which per-thread storage to replay:
//   0 = zero    (reduce_zero_calls_storage)
//   1 = narrow  (reduce_narrow_calls_storage)
//   2 = wide    (reduce_wide_calls_storage)
// The classifier is derived from the pre-impl input characterisation by
// the wrap site (rat.cpp::reduce_inplace) and stored in the cache entry's
// `ReduceValue.kind` field; on HIT, the cached `kind` is passed to this
// shim. See §iter-59-fold-REQ-5 §4.4-bis table row 2.
void reduce_on_hit(int kind);
void lf_on_hit();
void pf_on_hit();
void transform_shuffle_on_hit();

} // namespace counter_replay

// ---------------------------------------------------------------------------
// Global per-op cache accessors (defined in operator_memo.cpp; iter-61
// callers in rat.cpp / linear_factors.cpp / partial_fractions.cpp use these
// to reach the singleton caches). Forward-declared types so this header
// stays lightweight (the only HF type required to be complete at call sites
// is the one the caller already includes through its own production header).
// ---------------------------------------------------------------------------
class Rat;
class Poly;
class LinearFactorization;
class PartialFractionization;

namespace canonical_signature {
struct RatAddKey;
struct ReduceKey;
struct LfKey;
struct PfKey;
struct TransformShuffleKey;
} // namespace canonical_signature

// Forward-decl the ReduceValue type defined in operator_memo.cpp; it is a
// (Poly num_post, Poly den_post) pair stored as the §E cache value for
// reduce_inplace. The full definition is internal to operator_memo.cpp.
struct ReduceValue;

OperatorMemo<canonical_signature::RatAddKey, Rat>&            g_rat_add_cache();
OperatorMemo<canonical_signature::ReduceKey, ReduceValue>&    g_reduce_cache();
OperatorMemo<canonical_signature::LfKey, LinearFactorization>& g_lf_cache_outer();
OperatorMemo<canonical_signature::PfKey, PartialFractionization>& g_pf_cache_outer();

// ---------------------------------------------------------------------------
// Template-method definitions
// ---------------------------------------------------------------------------
template<typename KeyT, typename ValueT>
std::shared_ptr<const ValueT>
OperatorMemo<KeyT, ValueT>::try_lookup(const KeyT& key, KeyHash hash)
{
    Shard& s = shards_[shard_index_(hash)];
    std::shared_lock<std::shared_mutex> lk(s.mtx);
    auto it = s.map.find(hash);
    if (it == s.map.end()) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    // FOLD-M3 BINDING: full-equality verify to defend against 64-bit
    // XXH3 collisions. On collision, treat as MISS (caller computes,
    // and the colliding entry stays in cache; subsequent inserts with
    // the same hash but different key replace via unordered_map's
    // emplace+overwrite pattern — see insert()).
    if (!(it->second.stored_key == key)) {
        collisions_.fetch_add(1, std::memory_order_relaxed);
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    // HIT: increment hit_count (relaxed; LRU is best-effort).
    // NOTE: hit_count is in CacheEntry, mutated under shared_lock; the
    // increment is non-atomic to keep the shared-path lock-free. Best-
    // effort LRU surrogate, design memo §4.2 Tier B forward-scaffolding.
    // Sufficient for the diagnostic LRU heuristic; a precise count is
    // not required.
    const_cast<CacheEntry&>(it->second).hit_count++;
    hits_.fetch_add(1, std::memory_order_relaxed);
    // iter-70 REC-2: copy the stored shared_ptr<const ValueT> handle.
    // This is a single read of the pointer + atomic ref-count increment;
    // the shared_lock prevents a concurrent writer from running unique_lock
    // and swapping the CacheEntry, so the read is well-defined. After
    // the lock releases at scope exit, the caller owns a ref-counted
    // handle to a const referent that survives any subsequent cache-side
    // rehash or LRU eviction.
    return it->second.stored_value;
}

template<typename KeyT, typename ValueT>
void
OperatorMemo<KeyT, ValueT>::insert(KeyT key, KeyHash hash, ValueT value)
{
    // iter-70 REC-2 REC-2 (DEFERRED → APPLIED): allocate the const ValueT
    // BEFORE acquiring the unique_lock. The allocation does not need lock
    // protection (the value is the caller's local), and keeping it out of
    // the lock-held window reduces writer-vs-reader contention on this shard.
    auto sp = std::make_shared<const ValueT>(std::move(value));

    Shard& s = shards_[shard_index_(hash)];
    std::unique_lock<std::shared_mutex> lk(s.mtx);

    // iter-75 Option µ-narrow Tier-B LRU eviction (design.md §iter-75-design-A;
    // promotes §iter-71-design-A from DEFERRED scaffolding to production).
    //
    // Per-shard cap = max(1, lru_cap_per_op() / shard_count_). At
    // HF_OPERATOR_MEMO_LRU_CAP_PER_OP=1000 default + 32 shards, per_shard_cap = 31
    // (integer division floor; the ceiling-by-1 is intentional — at very small
    // total caps the per-shard cap floors to 0 absent the max(1, ...), which
    // would collapse the cache to a single-entry hash-rotate; we floor at 1 so
    // even total_cap = 1 retains one entry per shard).
    //
    // The cap check is BEFORE the emplace/overwrite below. The eviction policy
    // is min (hit_count, insertion_seq) — lowest hit_count first (LRU surrogate),
    // ties broken by lowest insertion_seq (oldest). Linear scan O(per_shard_cap)
    // under unique_lock; at per_shard_cap ≤ 312 (cap=10000 ceiling), the scan
    // is ≤ 312 comparisons, sub-microsecond, negligible vs unique_lock acquisition.
    //
    // BEST-EFFORT LRU CAVEATS (iter-75-α REQ-1 fold IN-PLACE):
    //   (a) hit_count is incremented non-atomically under shared_lock at
    //       try_lookup line ~357 (`const_cast<CacheEntry&>(it->second).hit_count++`).
    //       Under heavy OMP=13 concurrent readers, increment-load races may
    //       discard increments; the stored value is therefore a LOWER BOUND on
    //       true hit count. Precise count is NOT guaranteed.
    //   (b) Under cold-only insert workloads (no intervening lookups), the
    //       policy degrades to FIFO (all entries have hit_count=0; tie-broken
    //       by insertion_seq → oldest evicted). This is acceptable because the
    //       workload's structurally-canonical inputs ensure hot keys ARE
    //       looked up (Phase 5 §A.1 §5.2 GO_5.4_FINAL op_call_dup_rate > 0.91).
    //   (c) The eviction scan runs under unique_lock (single writer); the read
    //       of every entry's (hit_count, insertion_seq) is therefore a
    //       consistent snapshot at scan time. The non-atomic increment hazard
    //       affects WHICH entry has the lowest count, not the deterministic-
    //       at-scan invariant.
    //
    // SAFETY: the evicted entry's shared_ptr<const ValueT> ref-count drops on
    // s.map.erase; outstanding readers that already incremented the ref-count
    // via try_lookup retain the referent until their use-site dtor runs (the
    // iter-69-design-B safety-by-construction analysis applies recursively to
    // the LRU eviction path).
    //
    // SINGLE-FIND OPTIMISATION (iter-75-α REQ-2 fold IN-PLACE): we perform a
    // single `s.map.find(hash)` here whose iterator is reused at the
    // emplace-or-overwrite branch below. `s.map.end()` is a sentinel that is
    // stable across `erase()` calls on OTHER keys (per C++17 [unord.req]
    // §23.2.5/Table 91 — erase invalidates ONLY the iterator to the erased
    // element). The eviction's `s.map.erase(victim)` always targets a key
    // DIFFERENT from `hash` (the eviction branch only runs when `it_existing
    // == s.map.end()`), so `it_existing` remains a valid end-sentinel
    // post-erase and the emplace path is sound.
    const std::size_t cap_total      = operator_memo::lru_cap_per_op();
    const std::size_t per_shard_cap_ = cap_total / shard_count_;
    // iter-76 enable_lru gate (handoff iter-75 → iter-76 §"Next concrete step"):
    // cap_total == 0 is a sentinel meaning "LRU disabled". Without this gate,
    // the iter-75 `(per_shard_cap_ < 1) ? 1 : per_shard_cap_` floor collapsed
    // cap=0 to per_shard_cap=1 (single-entry-per-shard cache), NOT to "no cap".
    // Cap-calibration sweeps at iter-76 use cap=0 as the LRU-disabled control;
    // the floor-at-1 is preserved for cap=1..(shard_count_-1) so very small but
    // nonzero total caps still retain one entry per shard.
    const std::size_t per_shard_cap  = (cap_total == 0)
                                       ? 0
                                       : ((per_shard_cap_ < 1) ? 1 : per_shard_cap_);
    auto it_existing                 = s.map.find(hash);
    const bool is_overwrite          = (it_existing != s.map.end());
    if (!is_overwrite && per_shard_cap > 0 && s.map.size() >= per_shard_cap) {
        auto victim = s.map.begin();
        for (auto it = std::next(victim); it != s.map.end(); ++it) {
            if (std::tie(it->second.hit_count, it->second.insertion_seq)
                < std::tie(victim->second.hit_count, victim->second.insertion_seq)) {
                victim = it;
            }
        }
        s.map.erase(victim);
        // it_existing == s.map.end() before erase; remains end() after (erase
        // of a different key does not invalidate end()).
    }

    const std::uint64_t seq = insertion_seq_.fetch_add(1, std::memory_order_relaxed);
    if (is_overwrite) {
        it_existing->second = CacheEntry{std::move(key), std::move(sp), seq, 0u};
    } else {
        s.map.emplace(hash, CacheEntry{std::move(key), std::move(sp), seq, 0u});
    }
    inserts_.fetch_add(1, std::memory_order_relaxed);
}

template<typename KeyT, typename ValueT>
void
OperatorMemo<KeyT, ValueT>::clear_all_shards()
{
    for (auto& s : shards_) {
        std::unique_lock<std::shared_mutex> lk(s.mtx);
        s.map.clear();
    }
    // Counters are NOT cleared on fixture boundary — they remain monotonic
    // across the whole process lifetime so HF_STEP_TRACE diagnostics can
    // measure cumulative hit/miss/collision rates. Test scaffolding may
    // reset_counters() via test-only seam (not exposed in production).
}

template<typename KeyT, typename ValueT>
std::uint64_t
OperatorMemo<KeyT, ValueT>::entry_count() const
{
    std::uint64_t total = 0;
    for (auto const& s : shards_) {
        std::shared_lock<std::shared_mutex> lk(s.mtx);
        total += static_cast<std::uint64_t>(s.map.size());
    }
    return total;
}

// §E iter-7: RSS-pressure-driven LRU eviction batch. REQ-4 algorithm (a)
// round-robin-per-shard. Determinism: each shard acquires unique_lock,
// so the (hit_count, insertion_seq) snapshot used for victim selection
// is consistent within the shard. Cross-shard order is irrelevant for
// correctness (every shard prunes independently). The strategy gate
// (`evict_strategy_is_fifo()`) selects between LRU (lowest
// (hit_count, insertion_seq) lexicographic) and FIFO (lowest
// insertion_seq only); LRU is the design.md §6.2 default.
template<typename KeyT, typename ValueT>
std::size_t
OperatorMemo<KeyT, ValueT>::evict_lru_batch(std::size_t n_total)
{
    if (n_total == 0) return 0;
    const std::size_t n_per_shard =
        (n_total + shard_count_ - 1) / shard_count_;  // ceil
    const bool use_fifo = operator_memo::evict_strategy_is_fifo();

    std::size_t evicted_total = 0;
    for (auto& s : shards_) {
        std::unique_lock<std::shared_mutex> lk(s.mtx);
        if (s.map.empty()) continue;
        const std::size_t evict_in_shard =
            std::min(n_per_shard, s.map.size());
        if (evict_in_shard == 0) continue;

        using Iter = typename std::unordered_map<KeyHash, CacheEntry>::iterator;
        std::vector<Iter> iters;
        iters.reserve(s.map.size());
        for (auto it = s.map.begin(); it != s.map.end(); ++it) {
            iters.push_back(it);
        }
        // Partial sort: front `evict_in_shard` iterators become the
        // smallest by the chosen ordering. Lexicographic
        // (hit_count, insertion_seq) for LRU (default); insertion_seq
        // only for FIFO.
        if (use_fifo) {
            std::partial_sort(
                iters.begin(),
                iters.begin() + evict_in_shard,
                iters.end(),
                [](Iter a, Iter b) {
                    return a->second.insertion_seq < b->second.insertion_seq;
                });
        } else {
            std::partial_sort(
                iters.begin(),
                iters.begin() + evict_in_shard,
                iters.end(),
                [](Iter a, Iter b) {
                    return std::tie(a->second.hit_count, a->second.insertion_seq)
                         < std::tie(b->second.hit_count, b->second.insertion_seq);
                });
        }
        for (std::size_t i = 0; i < evict_in_shard; ++i) {
            s.map.erase(iters[i]);
        }
        evicted_total += evict_in_shard;
    }
    evictions_.fetch_add(static_cast<std::uint64_t>(evicted_total),
                         std::memory_order_relaxed);
    return evicted_total;
}

} // namespace hyperflint
