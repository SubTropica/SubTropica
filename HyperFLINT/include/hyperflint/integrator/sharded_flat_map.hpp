// Phase 1 (poles streaming, 2026-05-03): ShardedFlatMap.
//
// Shared-target data structure for streaming per-thread PolesBucket
// drains out of the integration_step OMP region. Holds N shards of
// `unordered_map<pair<u64,u64>, MonomialAcc, PairU64Hash>` with a
// per-shard `std::mutex`, so worker threads can flush full buckets
// concurrently without serialising on a single map-wide lock.
//
// Reviewer R28 R2 default: N_SHARDS = 16 at the user's 13-thread
// default. Shards are indexed by `key.first & (N_SHARDS - 1)`, so
// N_SHARDS must be a power of two (static_assert).
//
// `MonomialAcc` is also defined here (promoted from a local struct
// inside the `collect_into_flat` lambda in integration_step.cpp:1985)
// so that both the existing in-OMP `flat` map and ShardedFlatMap
// share a single type. See notes/hf_memory_plan/2026-05-03-poles-
// streaming-and-rep-swap.md for the full Phase 1 plan.

#pragma once

#include "hyperflint/algebra/poly_struct_hash.hpp"  // PairU64Hash
#include "hyperflint/core/symcoef.hpp"              // SymCoef
#include "hyperflint/integrator/transform.hpp"      // RegKey

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hyperflint {

struct MonomialAcc {
    std::vector<SymCoef> chunks;
    RegKey key;
};

template <std::size_t N_SHARDS = 16>
class ShardedFlatMap {
public:
    // Power-of-two requirement: shard_idx uses `& (N_SHARDS - 1)`.
    static_assert(N_SHARDS > 0 && (N_SHARDS & (N_SHARDS - 1)) == 0,
                  "ShardedFlatMap N_SHARDS must be a power of two");

    using Key   = std::pair<uint64_t, uint64_t>;
    using Inner = std::unordered_map<Key, MonomialAcc, PairU64Hash>;

    ShardedFlatMap() = default;

    // Disable copy/move: each ShardedFlatMap owns N mutexes that are
    // not movable, and the intended lifetime is one-per-OMP-region.
    ShardedFlatMap(const ShardedFlatMap&)            = delete;
    ShardedFlatMap& operator=(const ShardedFlatMap&) = delete;
    ShardedFlatMap(ShardedFlatMap&&)                 = delete;
    ShardedFlatMap& operator=(ShardedFlatMap&&)      = delete;

    // Insert or merge a single (key, regkey, chunk). Used when the
    // caller already has a finalised SymCoef ready to land.
    void merge_one(const Key& key, const RegKey& regkey, SymCoef chunk);

    // Drain a vector of pending SymCoefs under a single shard lock —
    // amortises mutex acquisition when a per-thread bucket flush has
    // many entries for the same key.
    //
    // Caller contract (R28 C1, applies in Task 1.3 PolesBucket flush):
    // the per-thread bucket's `terms` / `pending` / `index` must be
    // cleared atomically AFTER this call returns.  Clearing mid-loop
    // can leave `index` referencing a freed slot.  See
    // notes/hf_memory_plan/phase1_streaming/r28_design.md.
    void merge_pending(const Key& key, const RegKey& regkey,
                       std::vector<SymCoef>& chunks);

    // Sum of entry counts across all shards. Concurrency note: locks
    // each shard in turn; not a snapshot under writer load.
    std::size_t size() const;

    // Drain into a single `unordered_map` keyed identically. Output
    // map is augmented in place (existing entries get their `chunks`
    // appended). After return, every shard is empty.
    void extract_into(Inner& output);

    // R28 C2 binding: same as `extract_into` but additionally sorts
    // each output `MonomialAcc.chunks` by a deterministic key
    // (`SymCoef::to_string()`) before emit. Required when shard
    // arrival order is non-deterministic (worker threads flushing
    // concurrently into the same key) — without sort, downstream
    // SymCoef::tree_merge consumes chunks in shard-arrival order
    // and the result_sha differs across runs even though the
    // mathematical answer is identical. With sort, chunks order
    // depends only on canonical content, so result_sha is stable.
    //
    // Cost: O(K log K) string compare per output key, where K =
    // number of contributing chunks (~ n_threads_outer per slot,
    // ~13 in production). Amortised across an entire OMP region.
    void extract_into_sorted(Inner& output);

private:
    static std::size_t shard_idx(const Key& k) noexcept {
        return k.first & (N_SHARDS - 1);
    }

    mutable std::array<std::mutex, N_SHARDS> mutexes_{};
    std::array<Inner, N_SHARDS>              shards_{};
};

extern template class ShardedFlatMap<16>;

}  // namespace hyperflint
