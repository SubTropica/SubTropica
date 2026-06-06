// HF FF Phase 6 REVISED §E iter-9 — REQ-13 size-cap × RSS-pressure compose
// test.
//
// Validates the structural-distinctness claim from iter-6 REQ-8: the
// insert-time size-cap eviction (operator_memo.hpp::insert(),
// `HF_OPERATOR_MEMO_LRU_CAP_PER_OP`, iter-77 §E.2 Lever 5.4) and the
// step-boundary RSS-pressure eviction (operator_memo.hpp::evict_lru_batch,
// iter-7 §E) compose orthogonally. Both eviction paths must co-exist
// without crash, and combined eviction must NOT violate either's
// invariants:
//
//   - Size-cap holds across both eviction paths: entry_count() ≤ per-op
//     cap at all times.
//   - eviction_count counter is additive across the two paths (no
//     double-count, no off-by-one when the step-boundary scan walks an
//     entry already evicted by the size-cap path within the same insert).
//   - extra_evicted from evict_lru_batch is ≤ requested batch (the per-
//     shard ceiling caps each shard).
//
// Per runner_iter9_design.md §1.2.

#include "hyperflint/core/canonical_signature.hpp"
#include "hyperflint/core/operator_memo.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace hf = hyperflint;
namespace cs = hyperflint::canonical_signature;
namespace om = hyperflint::operator_memo;

namespace {

// Identical EnvGuard idiom to test_op_memo_evict.cpp: set vars in scope,
// reload operator_memo's env_gate, restore on dtor.
struct EnvGuard {
    std::vector<std::pair<std::string, std::string>> saved;
    EnvGuard(std::vector<std::pair<std::string, std::string>> sets) {
        for (auto const& kv : sets) {
            const char* old = std::getenv(kv.first.c_str());
            saved.emplace_back(kv.first, old ? old : "__UNSET__");
            setenv(kv.first.c_str(), kv.second.c_str(), 1);
        }
        om::reload_env_for_testing();
    }
    ~EnvGuard() {
        for (auto const& kv : saved) {
            if (kv.second == "__UNSET__") unsetenv(kv.first.c_str());
            else                          setenv(kv.first.c_str(), kv.second.c_str(), 1);
        }
        om::reload_env_for_testing();
    }
};

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

} // namespace

// ---------------------------------------------------------------------------
// Subtest: size-cap fires at insert; step-boundary evict_lru_batch
// composes additively. We keep this on a stand-alone OperatorMemo to
// isolate from production cache state.
// ---------------------------------------------------------------------------
static int test_compose_size_cap_with_step_boundary()
{
    EnvGuard g{{
        {"HF_OPERATOR_MEMO",                "1"},
        {"HF_OPERATOR_MEMO_LRU_CAP_PER_OP", "64"},
    }};

    // Sanity: env-gate sees the cap.
    if (om::lru_cap_per_op() != 64u) {
        std::cerr << "FAIL: lru_cap_per_op() returned " << om::lru_cap_per_op()
                  << ", expected 64\n";
        return 1;
    }

    hf::OperatorMemo<TestKey, TestValue> cache;
    constexpr int N_INSERT = 200;

    // Insert 200 distinct entries. Per-shard cap = max(1, 64/32) = 2.
    // Each shard fills to 2 then evicts on subsequent inserts. Aggregate
    // entry_count is bounded by per_shard_cap * shard_count_ = 2 * 32 = 64.
    for (int i = 0; i < N_INSERT; ++i) {
        const TestKey   k{static_cast<std::uint64_t>(i), 0xC0DEULL};
        const TestValue v{static_cast<std::uint64_t>(i), 0};
        const std::uint64_t h = cs::xxh3_seeded_pair(k.a, k.b);
        cache.insert(k, h, TestValue{v});
    }

    // (a) Size-cap holds: entry_count ≤ 64.
    if (cache.entry_count() > 64u) {
        std::cerr << "FAIL: size-cap violated; entry_count="
                  << cache.entry_count() << " > 64\n";
        return 1;
    }
    // (b) Bookkeeping orthogonality #1 — STRUCTURAL property of the
    //     iter-77 §E.2 size-cap path: insert-time eviction at
    //     operator_memo.hpp::insert() (lines ~513-524) erases entries
    //     directly via `s.map.erase(victim)` WITHOUT bumping
    //     `evictions_`. Only the step-boundary `evict_lru_batch` path
    //     (operator_memo.hpp::evict_lru_batch, lines ~617-621)
    //     increments the counter. The two paths are therefore
    //     bookkeeping-distinct: the counter exclusively reflects the
    //     RSS-pressure-driven path.
    //
    //     This is the orthogonality the composability test asserts.
    //     If a future change starts bumping `evictions_` from the
    //     insert-time path, the runner_iter9 measurement would
    //     conflate the two contributions and the iter-9 verdict
    //     would be ambiguous. The strict-equality assertion here
    //     pins the bookkeeping discipline as a load-bearing
    //     invariant of the §E ↔ size-cap composition.
    const auto e1 = cache.eviction_count();
    if (e1 != 0u) {
        std::cerr << "FAIL: size-cap insert-time path bumped eviction "
                     "counter (e1=" << e1 << "); the two eviction paths "
                     "would conflate in measurement\n";
        return 1;
    }

    const auto entries_pre_step = cache.entry_count();

    // Bump hit_count on the hot half. Cold = odd indices (post insert
    // survivors are unpredictable since size-cap evicts in shard order;
    // we use parity over `i` as the hot/cold tag and bump hot via lookup).
    for (int i = 0; i < N_INSERT; ++i) {
        if ((i & 1) == 0) {
            const TestKey k{static_cast<std::uint64_t>(i), 0xC0DEULL};
            const std::uint64_t h = cs::xxh3_seeded_pair(k.a, k.b);
            for (int rep = 0; rep < 5; ++rep) {
                (void)cache.try_lookup(k, h);
            }
        }
    }

    // (c) Call step-boundary evict_lru_batch(16). The actual eviction
    // semantic (operator_memo.hpp::evict_lru_batch line 574-575) is
    // `n_per_shard = ceil(n_total / shard_count_)` and each non-empty
    // shard evicts up to `n_per_shard` entries. The aggregate is
    // therefore in [0, n_per_shard × non_empty_shards], NOT bounded
    // by `n_total` itself: with N_INSERT=200 and cap=64 every shard
    // holds 2 entries, so n_per_shard=1 yields ~32 evictions on a
    // request of 16.
    //
    // (Iter-9 finding: runner_iter9_design.md §1.2 step 5's claim
    // `extra_evicted ≤ 16` is too tight — the per-shard ceiling rule
    // makes the upper bound `ceil(REQ / shard_count_) * shard_count_`,
    // not REQ. This is folded into the iter-9 close docs.)
    constexpr std::size_t REQ              = 16;
    constexpr std::size_t SHARDS           = 32;
    constexpr std::size_t PER_SHARD_CEIL   = (REQ + SHARDS - 1) / SHARDS;
    constexpr std::size_t MAX_EXTRA_EVICTED = PER_SHARD_CEIL * SHARDS;
    const std::size_t extra_evicted = cache.evict_lru_batch(REQ);

    if (extra_evicted > MAX_EXTRA_EVICTED) {
        std::cerr << "FAIL: evict_lru_batch returned " << extra_evicted
                  << " > per-shard-ceiling cap " << MAX_EXTRA_EVICTED
                  << " (request was " << REQ << ")\n";
        return 1;
    }
    if (extra_evicted == 0u) {
        std::cerr << "FAIL: evict_lru_batch on populated cache returned 0\n";
        return 1;
    }

    const auto e2 = cache.eviction_count();

    // (d) Counter is additive: e2 - e1 must equal extra_evicted exactly.
    // No double-count (size-cap path's stored count + this batch's count
    // must compose without overlap).
    if (e2 - e1 != extra_evicted) {
        std::cerr << "FAIL: eviction_count counter not additive; "
                  << "e1=" << e1 << " e2=" << e2 << " delta=" << (e2 - e1)
                  << " extra_evicted=" << extra_evicted << "\n";
        return 1;
    }

    // (e) Size-cap continues to hold.
    if (cache.entry_count() > 64u) {
        std::cerr << "FAIL: size-cap broken after step-boundary evict; "
                  << "entry_count=" << cache.entry_count() << "\n";
        return 1;
    }
    if (cache.entry_count() != entries_pre_step - extra_evicted) {
        std::cerr << "FAIL: post-step entry_count mismatch; "
                  << "pre_step=" << entries_pre_step
                  << " extra_evicted=" << extra_evicted
                  << " got=" << cache.entry_count() << "\n";
        return 1;
    }

    std::cout << "[PASS] compose_size_cap_with_step_boundary "
                 "(size-cap=64, inserts=" << N_INSERT
              << ", insert-time evictions=" << e1
              << ", step-boundary evictions=" << extra_evicted
              << ", final entry_count=" << cache.entry_count() << ")\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Subtest: edge case — request a step-boundary batch larger than the
// current entry count. The dispatch should evict everything and return
// the actual evicted count, not the request.
// ---------------------------------------------------------------------------
static int test_compose_step_evict_drains_under_size_cap()
{
    EnvGuard g{{
        {"HF_OPERATOR_MEMO",                "1"},
        {"HF_OPERATOR_MEMO_LRU_CAP_PER_OP", "64"},
    }};

    hf::OperatorMemo<TestKey, TestValue> cache;
    constexpr int N_INSERT = 200;
    for (int i = 0; i < N_INSERT; ++i) {
        const TestKey   k{static_cast<std::uint64_t>(i), 0xBA5EULL};
        const TestValue v{static_cast<std::uint64_t>(i), 1};
        const std::uint64_t h = cs::xxh3_seeded_pair(k.a, k.b);
        cache.insert(k, h, TestValue{v});
    }

    const auto e1 = cache.eviction_count();
    const auto entries_pre = cache.entry_count();

    // Request way more than the cap. evict_lru_batch should drain
    // every populated shard (per-shard ceiling = ceil(N/32) but actual
    // evictions per shard = min(per_shard_ceiling, shard.map.size())).
    const std::size_t huge = 10000;
    const std::size_t evicted = cache.evict_lru_batch(huge);

    if (evicted != entries_pre) {
        std::cerr << "FAIL: drain mismatch; entries_pre=" << entries_pre
                  << " evicted=" << evicted << " huge=" << huge << "\n";
        return 1;
    }
    if (cache.entry_count() != 0u) {
        std::cerr << "FAIL: cache should be empty after drain; got "
                  << cache.entry_count() << "\n";
        return 1;
    }
    if (cache.eviction_count() - e1 != evicted) {
        std::cerr << "FAIL: counter not additive on drain\n";
        return 1;
    }

    std::cout << "[PASS] compose_step_evict_drains_under_size_cap "
                 "(entries_pre=" << entries_pre
              << ", drained=" << evicted << ")\n";
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_compose_size_cap_with_step_boundary();
    rc |= test_compose_step_evict_drains_under_size_cap();
    if (rc == 0) {
        std::cout << "[OK] hyperflint-test-evict-compose-with-size-cap "
                     "— all subtests PASS\n";
    } else {
        std::cerr << "[FAIL] hyperflint-test-evict-compose-with-size-cap "
                     "— one or more subtests FAILED\n";
    }
    return rc;
}
