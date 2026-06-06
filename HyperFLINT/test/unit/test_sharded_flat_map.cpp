#include "hyperflint/integrator/sharded_flat_map.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/symcoef.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <utility>

using namespace hyperflint;

using OutMap = std::unordered_map<std::pair<uint64_t, uint64_t>,
                                  MonomialAcc, PairU64Hash>;

static int test_empty_map() {
    ShardedFlatMap<16> m;
    if (m.size() != 0) {
        std::cout << "[FAIL] empty-map invariant: size != 0\n";
        return 1;
    }
    OutMap out;
    m.extract_into(out);
    if (!out.empty()) {
        std::cout << "[FAIL] empty-map invariant: extract non-empty\n";
        return 1;
    }
    std::cout << "[PASS] empty-map invariant\n";
    return 0;
}

static int test_single_key_merge() {
    PolyCtx ctx({"x"});
    ShardedFlatMap<16> m;

    const std::pair<uint64_t, uint64_t> key{42, 0};
    const RegKey rk;
    SymCoef chunk(ctx);
    m.merge_one(key, rk, std::move(chunk));

    if (m.size() != 1) {
        std::cout << "[FAIL] single-key merge: size=" << m.size() << " != 1\n";
        return 1;
    }
    OutMap out;
    m.extract_into(out);
    if (out.size() != 1) {
        std::cout << "[FAIL] single-key merge: extract size=" << out.size()
                  << " != 1\n";
        return 1;
    }
    auto it = out.find(key);
    if (it == out.end()) {
        std::cout << "[FAIL] single-key merge: key (42,0) missing\n";
        return 1;
    }
    if (it->second.chunks.size() != 1) {
        std::cout << "[FAIL] single-key merge: chunks="
                  << it->second.chunks.size() << " != 1\n";
        return 1;
    }
    if (m.size() != 0) {
        std::cout << "[FAIL] single-key merge: size after extract != 0\n";
        return 1;
    }
    std::cout << "[PASS] single-key merge\n";
    return 0;
}

static int test_collision_two_threads() {
    PolyCtx ctx({"x"});
    ShardedFlatMap<16> m;

    const std::pair<uint64_t, uint64_t> key{7, 0};
    const RegKey rk;

    // Spin barrier (C++17 has no std::barrier): both threads land at
    // merge_one within a few cycles of each other to maximise the chance
    // of contention on the same shard mutex.
    std::atomic<int> arrived{0};
    std::atomic<bool> go{false};

    auto worker = [&]() {
        SymCoef chunk(ctx);
        arrived.fetch_add(1, std::memory_order_acq_rel);
        while (!go.load(std::memory_order_acquire)) { /* spin */ }
        m.merge_one(key, rk, std::move(chunk));
    };

    std::thread t1(worker);
    std::thread t2(worker);
    while (arrived.load(std::memory_order_acquire) < 2) { /* spin */ }
    go.store(true, std::memory_order_release);
    t1.join();
    t2.join();

    if (m.size() != 1) {
        std::cout << "[FAIL] collision two threads: size=" << m.size()
                  << " != 1\n";
        return 1;
    }
    OutMap out;
    m.extract_into(out);
    auto it = out.find(key);
    if (it == out.end()) {
        std::cout << "[FAIL] collision two threads: key (7,0) missing\n";
        return 1;
    }
    if (it->second.chunks.size() != 2) {
        std::cout << "[FAIL] collision two threads: chunks="
                  << it->second.chunks.size() << " != 2\n";
        return 1;
    }
    std::cout << "[PASS] collision two threads\n";
    return 0;
}

static int test_cross_shard_distribution() {
    PolyCtx ctx({"x"});
    ShardedFlatMap<16> m;
    const RegKey rk;

    constexpr std::size_t N = 1024;
    constexpr std::size_t N_SHARDS = 16;
    constexpr std::size_t EXPECTED_PER_SHARD = N / N_SHARDS;  // 64

    for (std::size_t i = 0; i < N; ++i) {
        std::pair<uint64_t, uint64_t> key{static_cast<uint64_t>(i), 0};
        m.merge_one(key, rk, SymCoef(ctx));
    }
    if (m.size() != N) {
        std::cout << "[FAIL] cross-shard: total size=" << m.size()
                  << " != " << N << "\n";
        return 1;
    }

    OutMap out;
    m.extract_into(out);
    if (out.size() != N) {
        std::cout << "[FAIL] cross-shard: extracted size=" << out.size()
                  << " != " << N << "\n";
        return 1;
    }

    // Recompute the shard index for each output key (it must agree with
    // ShardedFlatMap's internal mask: key.first & (N_SHARDS - 1)).
    std::array<std::size_t, N_SHARDS> per_shard{};
    for (auto& kv : out) {
        const std::size_t s = kv.first.first & (N_SHARDS - 1);
        ++per_shard[s];
    }

    // Round-robin keys (i, 0) for i in [0, 1024) are exactly even.
    for (std::size_t s = 0; s < N_SHARDS; ++s) {
        if (per_shard[s] != EXPECTED_PER_SHARD) {
            std::cout << "[FAIL] cross-shard: shard " << s
                      << " count=" << per_shard[s]
                      << " != " << EXPECTED_PER_SHARD << "\n";
            return 1;
        }
    }

    // R28 FM5 falsifier gate: min/max ratio <= 1.5. Trivially satisfied
    // here (ratio = 1.0) but assert it explicitly.
    std::size_t lo = per_shard[0], hi = per_shard[0];
    for (std::size_t s = 1; s < N_SHARDS; ++s) {
        if (per_shard[s] < lo) lo = per_shard[s];
        if (per_shard[s] > hi) hi = per_shard[s];
    }
    if (lo == 0 || hi * 2 > lo * 3) {
        std::cout << "[FAIL] cross-shard: hi/lo ratio out of bound (lo="
                  << lo << " hi=" << hi << ")\n";
        return 1;
    }

    std::cout << "[PASS] cross-shard distribution (64/shard x 16)\n";
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_empty_map();
    rc |= test_single_key_merge();
    rc |= test_collision_two_threads();
    rc |= test_cross_shard_distribution();
    return rc;
}
