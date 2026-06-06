#include "hyperflint/integrator/sharded_flat_map.hpp"

#include <algorithm>
#include <mutex>      // std::lock_guard/std::mutex (not transitively included on libstdc++)
#include <utility>

namespace hyperflint {

template <std::size_t N_SHARDS>
void ShardedFlatMap<N_SHARDS>::merge_one(const Key& key,
                                         const RegKey& regkey,
                                         SymCoef chunk) {
    const std::size_t s = shard_idx(key);
    std::lock_guard<std::mutex> g(mutexes_[s]);
    auto& m  = shards_[s];
    auto  it = m.find(key);
    if (it == m.end()) {
        MonomialAcc acc;
        acc.key = regkey;
        acc.chunks.push_back(std::move(chunk));
        m.emplace(key, std::move(acc));
    } else {
        it->second.chunks.push_back(std::move(chunk));
    }
}

template <std::size_t N_SHARDS>
void ShardedFlatMap<N_SHARDS>::merge_pending(const Key& key,
                                             const RegKey& regkey,
                                             std::vector<SymCoef>& chunks) {
    if (chunks.empty()) return;
    const std::size_t s = shard_idx(key);
    std::lock_guard<std::mutex> g(mutexes_[s]);
    auto& m  = shards_[s];
    auto  it = m.find(key);
    if (it == m.end()) {
        MonomialAcc acc;
        acc.key = regkey;
        acc.chunks.reserve(chunks.size());
        for (auto& c : chunks) acc.chunks.push_back(std::move(c));
        m.emplace(key, std::move(acc));
    } else {
        auto& dst = it->second.chunks;
        dst.reserve(dst.size() + chunks.size());
        for (auto& c : chunks) dst.push_back(std::move(c));
    }
    chunks.clear();
}

template <std::size_t N_SHARDS>
std::size_t ShardedFlatMap<N_SHARDS>::size() const {
    std::size_t total = 0;
    for (std::size_t s = 0; s < N_SHARDS; ++s) {
        std::lock_guard<std::mutex> g(mutexes_[s]);
        total += shards_[s].size();
    }
    return total;
}

template <std::size_t N_SHARDS>
void ShardedFlatMap<N_SHARDS>::extract_into(Inner& output) {
    // R28 C2 note: this variant does NOT sort `chunks` per key.
    // It is byte-deterministic ONLY when the producer side appended
    // chunks to each key in a fixed order (e.g. the Phase 1.1/1.2
    // unit tests with a single-thread driver). Production OMP wiring
    // (Task 1.4) MUST use `extract_into_sorted` instead — different
    // worker-thread arrival orders interleave chunks differently
    // across runs and would break result_sha bit-identity.
    for (std::size_t s = 0; s < N_SHARDS; ++s) {
        std::lock_guard<std::mutex> g(mutexes_[s]);
        auto& m = shards_[s];
        for (auto& kv : m) {
            auto it = output.find(kv.first);
            if (it == output.end()) {
                output.emplace(kv.first, std::move(kv.second));
            } else {
                auto& dst = it->second.chunks;
                auto& src = kv.second.chunks;
                dst.reserve(dst.size() + src.size());
                for (auto& c : src) dst.push_back(std::move(c));
            }
        }
        m.clear();
    }
}

template <std::size_t N_SHARDS>
void ShardedFlatMap<N_SHARDS>::extract_into_sorted(Inner& output) {
    // R28 C2 binding fix. We drain each shard exactly as
    // `extract_into` does, then sort each output entry's `chunks`
    // vector by `SymCoef::to_string()` (memoized inside each Rat
    // prefactor; pure function of canonical content). The sort
    // comparator is a deterministic string less-than, so the final
    // chunks order depends only on canonical content, not on shard
    // arrival order.
    //
    // We track the set of Keys touched during drain, then post-sort
    // each one's chunks at the end (re-sorting after every shard
    // pop would be O(K^2) per key). Iterators into `output` are
    // unstable across rehash, so we collect Key copies and look up
    // again at sort time.
    std::vector<Key> touched;
    for (std::size_t s = 0; s < N_SHARDS; ++s) {
        std::lock_guard<std::mutex> g(mutexes_[s]);
        auto& m = shards_[s];
        touched.reserve(touched.size() + m.size());
        for (auto& kv : m) {
            auto it = output.find(kv.first);
            if (it == output.end()) {
                output.emplace(kv.first, std::move(kv.second));
            } else {
                auto& dst = it->second.chunks;
                auto& src = kv.second.chunks;
                dst.reserve(dst.size() + src.size());
                for (auto& c : src) dst.push_back(std::move(c));
            }
            touched.push_back(kv.first);
        }
        m.clear();
    }
    // Each (key.first & mask) maps a Key deterministically to one
    // shard, so a Key appears at most once across the loop above.
    // No dedup needed.
    //
    // Code-quality I2 fix (Task 1.4): SymCoef::to_string() returns by
    // value and is NOT memoised, so a comparator that calls
    // a.to_string() < b.to_string() per pair-compare rebuilds two
    // strings per compare × O(K log K) compares = up to +10% wall on
    // parity_1 OMP=13 stream-on. Pre-compute each chunk's
    // canonical-string ONCE, sort by index over the cached strings,
    // then permute chunks. Total cost: O(K) to_string + O(K log K)
    // string-compares on the cached strings.
    for (const Key& k : touched) {
        auto it = output.find(k);
        if (it == output.end()) continue;  // defensive
        auto& chunks = it->second.chunks;
        if (chunks.size() <= 1) continue;
        std::vector<std::string> keys;
        keys.reserve(chunks.size());
        for (const auto& c : chunks) keys.push_back(c.to_string());
        std::vector<std::size_t> idx(chunks.size());
        for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(),
                  [&keys](std::size_t a, std::size_t b) {
                      return keys[a] < keys[b];
                  });
        std::vector<SymCoef> sorted_chunks;
        sorted_chunks.reserve(chunks.size());
        for (std::size_t i : idx) {
            sorted_chunks.push_back(std::move(chunks[i]));
        }
        chunks = std::move(sorted_chunks);
    }
}

template class ShardedFlatMap<16>;

}  // namespace hyperflint
