// HF FF Phase 5 §E iter-65 Phase 63-γ-bis: TSan-instrumented stress test
// for OperatorMemo<KeyT,ValueT>. Targets the iter-62-δ S2 hazard
// (concurrent insert + try_lookup raw-pointer UAF), which iter-63 REQ-1
// resolved by returning std::optional<ValueT> deep-copied under
// shared_lock. The iter-62-δ adversarial reviewer (agentId
// `a0dd0c1c009d169d4`) explicitly flagged §4.3 omp-determinism-cache-hit
// as insufficient for active falsification of the post-REQ-1 fix
// (that test pre-populates serially and only exercises HIT-via-shared-lock
// readers, never concurrent INSERT under unique_lock interleaved with
// try_lookup under shared_lock on the same shard).
//
// Test design (per handoff iter-65 §63-γ-bis):
//   - 13 threads (matches OMP=13 production default).
//   - Each thread operates on a DISJOINT key range: thread `t` owns keys
//     `(t, i)` for `i ∈ [0, N)`. Concurrent HIT on the same stored_value
//     entry by multiple threads is therefore impossible by construction
//     (the only thread that ever looks up `(t, i)` is thread `t`), which
//     elides the deliberate non-atomic `hit_count++` race at
//     operator_memo.hpp:337 (a separate, documented design choice: best-
//     effort LRU surrogate per design memo §4.2 Tier B, intentionally
//     non-atomic for shared-path lock-freedom; out of scope for this
//     test).
//   - Hashes are crafted to land ALL keys on a SINGLE shard (shard 0) by
//     left-shifting the unique key id by 5 bits (shard_index_(h) = h & 31
//     for `shard_count_ = 32` in operator_memo.hpp:162-164). This forces
//     maximum reader/writer contention on a single shared_mutex.
//   - Per-thread workload: `N` iterations of `insert(k_i)` immediately
//     followed by `try_lookup(k_i)`. The lookup is for the thread's OWN
//     just-inserted key, so HIT is guaranteed (no MISS path expected).
//   - Across threads, however, the inserts and lookups are interleaved
//     freely: when thread A's `try_lookup(k_a)` takes shared_lock on
//     shard 0, thread B may be holding unique_lock for `insert(k_b)`;
//     the shared_mutex serialises them correctly. The CRITICAL path
//     exercised is: thread B's `insert` triggers an `unordered_map::
//     emplace` that may REHASH the map; under the pre-REQ-1 raw-pointer
//     return, thread A's just-returned pointer would have been dangling
//     after lock release. Under REQ-1, A deep-copies under the shared
//     lock, so the rehash is invisible.
//
// Assertions after all threads join:
//   * `cache.insert_count() == 13 * N`               (every insert took)
//   * `cache.entry_count()  == 13 * N`               (no key reuse;
//                                                     no XXH3 collision)
//   * `cache.hit_count()    == 13 * N`               (every lookup HIT)
//   * `cache.miss_count()   == 0`                    (no MISSes)
//   * `cache.collision_count() == 0`                 (no XXH3 collisions)
//   * `per_thread_errors == 0`                       (every lookup
//                                                     observed correct
//                                                     stored_value)
//
// TSan disposition:
//   The binary is built under HF_TSAN=ON (`-fsanitize=thread`) in
//   `HyperFLINT/build-tsan/`. Any reported data race aborts the test
//   with nonzero exit code (TSAN_OPTIONS="halt_on_error=1" recommended).
//   Race-freedom is the binding falsification gate; on PASS the iter-62-δ
//   S2 hazard is actively falsified post REQ-1.
//
// Build / run recipe:
//   cmake --build HyperFLINT/build-tsan \
//         --target hyperflint-test-operator-memo-tsan -j 8
//   ctest --test-dir HyperFLINT/build-tsan -R operator-memo-tsan --output-on-failure
// OR direct:
//   TSAN_OPTIONS="halt_on_error=1 abort_on_error=0" \
//     HyperFLINT/build-tsan/hyperflint-test-operator-memo-tsan [N_PER_THREAD]
//
// Default `N_PER_THREAD = 50000` gives ~650 k total ops, wall ~5-30 s
// under -O1 + TSan overhead on Apple M-series.

#include "hyperflint/core/operator_memo.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace hf = hyperflint;

namespace {

struct TestKey {
    std::uint64_t a;
    std::uint64_t b;
    bool operator==(const TestKey& o) const noexcept {
        return a == o.a && b == o.b;
    }
};

struct TestValue {
    std::uint64_t v0;
    std::uint64_t v1;
    bool operator==(const TestValue& o) const noexcept {
        return v0 == o.v0 && v1 == o.v1;
    }
};

// shard_index_ in operator_memo.hpp:178 is `h & (shard_count_-1)` with
// shard_count_ == 32, so the low 5 bits select the shard. Shifting a
// unique 59-bit id left by 5 places the shard index at 0 for every key
// while keeping all hashes distinct.
constexpr int kShardBits = 5;

inline std::uint64_t make_hash(int t, int i, int n_per_thread) {
    const std::uint64_t flat =
        static_cast<std::uint64_t>(t) *
            static_cast<std::uint64_t>(n_per_thread) +
        static_cast<std::uint64_t>(i) + 1ULL;
    return flat << kShardBits;
}

inline TestKey make_key(int t, int i) {
    return TestKey{
        (static_cast<std::uint64_t>(t) << 32) |
            static_cast<std::uint64_t>(i),
        0xDEADBEEFCAFEBABEULL};
}

inline TestValue make_value(int t, int i) {
    return TestValue{
        static_cast<std::uint64_t>(t) * 1000003ULL +
            static_cast<std::uint64_t>(i),
        0xFEEDC0DE12345678ULL};
}

} // namespace

int main(int argc, char** argv) {
    int n_per_thread = 50000;
    if (argc >= 2) n_per_thread = std::atoi(argv[1]);
    if (n_per_thread <= 0) n_per_thread = 50000;

    constexpr int N_WORKERS = 13;

    hf::OperatorMemo<TestKey, TestValue> cache;

    const auto t0 = std::chrono::steady_clock::now();

    std::atomic<int> per_thread_errors{0};
    std::vector<std::thread> workers;
    workers.reserve(N_WORKERS);

    for (int t = 0; t < N_WORKERS; ++t) {
        workers.emplace_back([t, n_per_thread, &cache, &per_thread_errors]() {
            for (int i = 0; i < n_per_thread; ++i) {
                const TestKey   k = make_key(t, i);
                const TestValue v = make_value(t, i);
                const std::uint64_t h = make_hash(t, i, n_per_thread);

                // Insert under unique_lock on shard 0.
                cache.insert(k, h, TestValue{v});

                // Lookup under shared_lock on the same shard. Concurrent
                // inserts by other threads may be triggering rehashes;
                // iter-70 REC-2 ensures the returned
                // shared_ptr<const ValueT> ref-counts the heap-allocated
                // referent so any concurrent rehash on the cache side
                // cannot invalidate the reader's handle.
                auto p = cache.try_lookup(k, h);
                if (!p) {
                    per_thread_errors.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (!(*p == v)) {
                    per_thread_errors.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    const auto t1 = std::chrono::steady_clock::now();
    const auto wall_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    const std::uint64_t inserts    = cache.insert_count();
    const std::uint64_t hits       = cache.hit_count();
    const std::uint64_t misses     = cache.miss_count();
    const std::uint64_t collisions = cache.collision_count();
    const std::uint64_t entries    = cache.entry_count();
    const int errs = per_thread_errors.load();

    const std::uint64_t expected =
        static_cast<std::uint64_t>(N_WORKERS) *
        static_cast<std::uint64_t>(n_per_thread);

    std::fprintf(stderr,
        "OperatorMemo_ConcurrentInsertLookup_S2_Stress:\n"
        "  workers=%d  N_per_thread=%d  total_ops=%llu  wall_ms=%lld\n"
        "  inserts=%llu hits=%llu misses=%llu collisions=%llu entries=%llu errors=%d\n",
        N_WORKERS, n_per_thread,
        (unsigned long long)expected, (long long)wall_ms,
        (unsigned long long)inserts,
        (unsigned long long)hits,
        (unsigned long long)misses,
        (unsigned long long)collisions,
        (unsigned long long)entries,
        errs);

    int rc = 0;
    if (errs != 0) {
        std::fprintf(stderr, "FAIL: per_thread_errors=%d (lookup miss or value mismatch)\n", errs);
        rc = 1;
    }
    if (inserts != expected) {
        std::fprintf(stderr, "FAIL: insert_count=%llu expected=%llu\n",
                     (unsigned long long)inserts,
                     (unsigned long long)expected);
        rc = 1;
    }
    if (entries != expected) {
        std::fprintf(stderr, "FAIL: entry_count=%llu expected=%llu (XXH3 collision suspected)\n",
                     (unsigned long long)entries,
                     (unsigned long long)expected);
        rc = 1;
    }
    if (hits != expected) {
        std::fprintf(stderr, "FAIL: hit_count=%llu expected=%llu\n",
                     (unsigned long long)hits,
                     (unsigned long long)expected);
        rc = 1;
    }
    if (misses != 0) {
        std::fprintf(stderr, "FAIL: miss_count=%llu expected=0\n",
                     (unsigned long long)misses);
        rc = 1;
    }
    if (collisions != 0) {
        std::fprintf(stderr, "FAIL: collision_count=%llu expected=0\n",
                     (unsigned long long)collisions);
        rc = 1;
    }
    if (rc == 0) {
        std::fprintf(stderr,
            "[PASS] ConcurrentInsertLookup_S2_Stress "
            "(13 workers × %d ops single-shard forced; iter-63 REQ-1 fix "
            "verified; counter invariants hold; TSan reports captured "
            "by the runtime — nonzero exit aborts on race)\n",
            n_per_thread);
    }
    return rc;
}
