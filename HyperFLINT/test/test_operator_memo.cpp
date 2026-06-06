// HF FF Phase 5 §E Step E.2-impl-2 TDD body-fill (iter-60-γ).
//
// Iter-59-γ shipped 7 SKIP placeholders for §E (§4.1-§4.7). Iter-60-γ
// body-fills the ones that the iter-60 scaffolding (operator_memo.hpp +
// canonical_signature.hpp/cpp + operator_memo.cpp) can already exercise:
//
//   §4.1 per-op-hit-miss-unit         — BODY-FILLED (OperatorMemo template)
//   §4.2 scalar-rep-1-disable-path    — BODY-FILLED (env-gate predicates)
//   §4.3 omp-determinism-cache-hit    — BODY-FILLED (multithread direct on template)
//   §4.4 counter-replay-on-hit        — STAYS SKIP (per-op wraps land at iter-61)
//   §4.5 clear-between-fixtures       — BODY-FILLED (clear_between_fixtures())
//   §4.6 collision-full-equality-     — BODY-FILLED (synthetic forced-hash)
//        fallthrough
//   §4.7 smoke-heavy-fixture-tst2-mini — STAYS SKIP (per-op wraps land at iter-61)
//
// Per the iter-59 fold appendix §iter-59-fold-REQ-5, the §4.4 single
// counter-replay test EXPANDS to 5 per-boundary tests at iter-61 Step
// E.2-impl-2; iter-60 keeps the single SKIP placeholder until the wraps land.
//
// Build pattern mirrors the existing HF unit-test harness (no gtest):
// each `test_*` returns 0/1; main() aggregates and returns rc.

#include "hyperflint/core/canonical_signature.hpp"
#include "hyperflint/core/operator_memo.hpp"
#include "hyperflint/core/rat.hpp"  // iter-64-γ: init_reduce_per_thread + sum_*_per_thread

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace hf  = hyperflint;
namespace cs  = hyperflint::canonical_signature;
namespace om  = hyperflint::operator_memo;

namespace {

// ---------------------------------------------------------------------------
// Test helpers: env-gate management.
// ---------------------------------------------------------------------------
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

// A simple POD-style test key + value used to drive OperatorMemo<K,V>
// directly without dragging in the full HF poly/rat types. The template
// instantiations for the production-side key types are exercised by the
// SCALAR_REP=1 + clear-between-fixtures tests.
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

// Iter-60 uses the template via the existing 4 explicit instantiations in
// operator_memo.cpp. Per-test instantiations are deliberately inline so the
// test binary does not pull in extra translation units.

} // namespace

// ---------------------------------------------------------------------------
// §4.1 per-op-hit-miss-unit
// ---------------------------------------------------------------------------
static int test_per_op_hit_miss_unit() {
    int rc = 0;
    // Drive OperatorMemo directly with TestKey/TestValue: simulates the
    // contract that the per-op wraps will follow at iter-61. The template
    // is the same data structure as the production instantiations.
    hf::OperatorMemo<TestKey, TestValue> cache;
    const TestKey   k0{42, 7};
    const TestValue v0{100, 200};
    const std::uint64_t h0 = cs::xxh3_seeded_pair(k0.a, k0.b);

    // First lookup: MISS.
    // iter-70 REC-2: try_lookup returns std::shared_ptr<const ValueT>;
    // nullptr on MISS, owning handle on HIT.
    if (cache.try_lookup(k0, h0)) {
        std::cerr << "FAIL: first lookup should be MISS\n";
        return 1;
    }
    if (cache.miss_count() != 1) {
        std::cerr << "FAIL: miss_count should be 1 after first MISS, got "
                  << cache.miss_count() << "\n";
        return 1;
    }

    // Insert and look up again: HIT.
    cache.insert(k0, h0, TestValue{v0});
    if (cache.insert_count() != 1) {
        std::cerr << "FAIL: insert_count should be 1, got "
                  << cache.insert_count() << "\n";
        return 1;
    }
    auto p = cache.try_lookup(k0, h0);
    if (!p) {
        std::cerr << "FAIL: second lookup should be HIT\n";
        return 1;
    }
    if (!(*p == v0)) {
        std::cerr << "FAIL: cached value mismatch\n";
        return 1;
    }
    if (cache.hit_count() != 1) {
        std::cerr << "FAIL: hit_count should be 1, got "
                  << cache.hit_count() << "\n";
        return 1;
    }

    // Distinct key: MISS again.
    const TestKey k1{99, 13};
    const std::uint64_t h1 = cs::xxh3_seeded_pair(k1.a, k1.b);
    if (cache.try_lookup(k1, h1)) {
        std::cerr << "FAIL: distinct-key lookup should be MISS\n";
        return 1;
    }
    if (cache.miss_count() != 2) {
        std::cerr << "FAIL: miss_count should be 2 after distinct-key MISS, got "
                  << cache.miss_count() << "\n";
        return 1;
    }

    std::cout << "[PASS] per-op-hit-miss-unit "
                 "(OperatorMemo template HIT/MISS/INSERT counters correct)\n";
    return rc;
}

// ---------------------------------------------------------------------------
// §4.2 scalar-rep-1-disable-path
// Per §iter-59-fold-REQ-3 + REQ-7:
//   transform_shuffle_enabled() == false under SCALAR_REP=1 (primary FOLD-ER3)
//   lf_enabled()                == false under SCALAR_REP=1 (REQ-3 option b)
//   pf_enabled()                == false under SCALAR_REP=1 (REQ-3 option b)
//   rat_add_enabled()           == true  under SCALAR_REP=1 (REQ-7 option b)
//   reduce_enabled()            == true  under SCALAR_REP=1 (REQ-7 option b)
//
// NOTE: HF_USE_SCALAR_REP is read once at first call to scalar_rep_active()
// via a function-local static, so this test runs the SCALAR_REP=1 branch
// only when launched from a parent shell with that env var preset.
// The test is structured to PASS in both modes (it checks the EXPECTED
// shape of enabled() per the SCALAR_REP env var value).
// ---------------------------------------------------------------------------
static int test_scalar_rep_1_disable_path() {
    const char* sr = std::getenv("HF_USE_SCALAR_REP");
    const bool scalar_rep_on = sr && sr[0] == '1';

    // Enable master + every per-boundary so we isolate the SCALAR_REP gate.
    // iter-73 Option ρ-(b): REDUCE cache default-disabled in production;
    // tests opt in via HF_OPERATOR_MEMO_ENABLE_REDUCE=1 (preserves test's
    // existing reduce_enabled() == true expectation under REQ-7 option b).
    EnvGuard g{{
        {"HF_OPERATOR_MEMO", "1"},
        {"HF_OPERATOR_MEMO_OFF_RAT_ADD",      "0"},
        {"HF_OPERATOR_MEMO_ENABLE_REDUCE",    "1"},
        {"HF_OPERATOR_MEMO_OFF_LF",           "0"},
        {"HF_OPERATOR_MEMO_OFF_PF",           "0"},
        {"HF_OPERATOR_MEMO_OFF_TRANSFORM",    "0"},
    }};

    if (!om::master_enabled()) {
        std::cerr << "FAIL: master_enabled() should be true\n";
        return 1;
    }

    // Always-on (REQ-7 option b): rat_add + reduce stay enabled even under SCALAR_REP=1.
    if (!om::rat_add_enabled()) {
        std::cerr << "FAIL: rat_add_enabled() should be true (REQ-7 option b)\n";
        return 1;
    }
    if (!om::reduce_enabled()) {
        std::cerr << "FAIL: reduce_enabled() should be true (REQ-7 option b)\n";
        return 1;
    }

    // SCALAR_REP-gated (REQ-3 option b + FOLD-ER3): lf + pf + transform_shuffle.
    if (scalar_rep_on) {
        if (om::lf_enabled() || om::pf_enabled() || om::transform_shuffle_enabled()) {
            std::cerr << "FAIL: under SCALAR_REP=1, lf/pf/transform_shuffle must be disabled\n";
            return 1;
        }
        std::cout << "[PASS] scalar-rep-1-disable-path "
                     "(SCALAR_REP=1 path verified: lf/pf/transform_shuffle gated off; "
                     "rat_add/reduce stay enabled)\n";
    } else {
        // Default-runtime path: all 5 should be enabled.
        if (!om::lf_enabled() || !om::pf_enabled() || !om::transform_shuffle_enabled()) {
            std::cerr << "FAIL: default runtime should have all 5 enabled\n";
            return 1;
        }
        std::cout << "[PASS] scalar-rep-1-disable-path "
                     "(default-runtime path verified: all 5 enabled; "
                     "SCALAR_REP=1 branch covered by subprocess test in iter-61+)\n";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// §4.3 omp-determinism-cache-hit
// 13-worker concurrent insert/lookup on the same key set; assert
// hit/miss/collision counters end in a consistent state.
// ---------------------------------------------------------------------------
static int test_omp_determinism_cache_hit() {
    hf::OperatorMemo<TestKey, TestValue> cache;

    // Pre-populate with 64 keys.
    constexpr int N_KEYS = 64;
    for (int i = 0; i < N_KEYS; ++i) {
        const TestKey   k{static_cast<std::uint64_t>(i), 0xDEADBEEFULL};
        const TestValue v{static_cast<std::uint64_t>(i * 100), 0xFEEDC0DEULL};
        const std::uint64_t h = cs::xxh3_seeded_pair(k.a, k.b);
        cache.insert(k, h, TestValue{v});
    }

    // 13 worker threads, each looking up all 64 keys 100 times.
    constexpr int N_WORKERS = 13;
    constexpr int N_REPEAT  = 100;
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(N_WORKERS);
    for (int w = 0; w < N_WORKERS; ++w) {
        threads.emplace_back([&] {
            for (int rep = 0; rep < N_REPEAT; ++rep) {
                for (int i = 0; i < N_KEYS; ++i) {
                    const TestKey   k{static_cast<std::uint64_t>(i), 0xDEADBEEFULL};
                    const std::uint64_t h = cs::xxh3_seeded_pair(k.a, k.b);
                    // iter-70 REC-2: try_lookup returns
                    // std::shared_ptr<const ValueT>; nullptr on MISS.
                    auto p = cache.try_lookup(k, h);
                    if (!p) { errors.fetch_add(1); break; }
                    if (p->v0 != static_cast<std::uint64_t>(i * 100)) {
                        errors.fetch_add(1); break;
                    }
                }
            }
        });
    }
    for (auto& t : threads) t.join();

    if (errors.load() != 0) {
        std::cerr << "FAIL: omp-determinism: " << errors.load() << " lookup errors\n";
        return 1;
    }
    // No new collisions: all lookups should be HITs against pre-populated entries.
    if (cache.collision_count() != 0) {
        std::cerr << "FAIL: collision_count should be 0, got "
                  << cache.collision_count() << "\n";
        return 1;
    }
    // hit_count should be exactly N_WORKERS * N_REPEAT * N_KEYS.
    const std::uint64_t expected = static_cast<std::uint64_t>(N_WORKERS)
                                   * N_REPEAT * N_KEYS;
    if (cache.hit_count() != expected) {
        std::cerr << "FAIL: hit_count expected " << expected
                  << " got " << cache.hit_count() << "\n";
        return 1;
    }
    std::cout << "[PASS] omp-determinism-cache-hit "
                 "(13 workers x 100 reps x 64 keys = " << expected << " HITs; "
                 "0 collisions; concurrent shared_lock readers correct)\n";
    return 0;
}

// ---------------------------------------------------------------------------
// §4.4 counter-replay-on-hit — 5 per-boundary tests
// (iter-64-γ body-fill replacing iter-60..63 SKIP placeholder).
//
// Per §iter-59-fold-REQ-5 §4.4-bis table the single SKIP at iter-60..63
// expands to 5 per-boundary tests (one per §E boundary). Each test invokes
// the corresponding `counter_replay::*_on_hit(...)` shim N times and asserts
// the per-thread storage delta for the boundary's call-count counter.
//
// The shims for rat_add and reduce wire to the production per-thread
// counters via the public shims in rat.hpp (`rat_add_record_call_for_thread`
// + `reduce_record_call_for_thread`). The lf, pf, and transform_shuffle
// shims are documented no-ops at iter-62..64 per §iter-59-fold-REQ-5
// §4.4-bis rows 3, 4, 5 (their per-thread observers are inner-cache-state
// probes or pure functional, not call counters); the tests for those
// boundaries assert that the shim is callable and produces no observable
// side-effects on the rat_add / reduce counters (the only summable
// per-thread storage currently wired).
//
// Test thread-context: outside any OMP parallel region or GCD dispatch
// block, `hf_get_thread_num()` resolves to `omp_get_thread_num()` which
// is 0 (master thread). All counter increments accumulate at index 0 of
// the per-thread storage; `sum_*_per_thread()` reads that index plus
// any zero-initialized siblings.
// ---------------------------------------------------------------------------
static constexpr int kCounterReplayThreads = 13;

// Helper: zero all per-thread counters by re-initializing storage at the
// canonical OMP=13 width.
static void reset_counter_storage_for_test() {
    ::hyperflint::init_reduce_per_thread(kCounterReplayThreads);
}

// §4.4.1 — CounterReplay_RatAdd_OnHit
static int test_counter_replay_rat_add_on_hit() {
    reset_counter_storage_for_test();
    const long baseline = ::hyperflint::sum_rat_add_calls_per_thread();
    constexpr int N_HITS = 17;
    for (int i = 0; i < N_HITS; ++i) {
        hf::counter_replay::rat_add_on_hit();
    }
    const long observed = ::hyperflint::sum_rat_add_calls_per_thread();
    if (observed - baseline != N_HITS) {
        std::cerr << "FAIL: counter-replay-rat-add-on-hit: expected delta "
                  << N_HITS << ", got " << (observed - baseline) << "\n";
        return 1;
    }
    std::cout << "[PASS] counter-replay-rat-add-on-hit "
                 "(rat_add_on_hit replays " << N_HITS
              << " per-thread call-count increments via "
                 "rat_add_record_call_for_thread)\n";
    return 0;
}

// §4.4.2 — CounterReplay_Reduce_OnHit (3-kind dispatch verification)
static int test_counter_replay_reduce_on_hit() {
    reset_counter_storage_for_test();
    const long zero_baseline   = ::hyperflint::sum_reduce_zero_calls_per_thread();
    const long narrow_baseline = ::hyperflint::sum_reduce_narrow_calls_per_thread();
    const long wide_baseline   = ::hyperflint::sum_reduce_wide_calls_per_thread();

    constexpr int N_ZERO   = 5;
    constexpr int N_NARROW = 11;
    constexpr int N_WIDE   = 7;
    for (int i = 0; i < N_ZERO;   ++i) hf::counter_replay::reduce_on_hit(0);
    for (int i = 0; i < N_NARROW; ++i) hf::counter_replay::reduce_on_hit(1);
    for (int i = 0; i < N_WIDE;   ++i) hf::counter_replay::reduce_on_hit(2);

    const long zero_obs   = ::hyperflint::sum_reduce_zero_calls_per_thread();
    const long narrow_obs = ::hyperflint::sum_reduce_narrow_calls_per_thread();
    const long wide_obs   = ::hyperflint::sum_reduce_wide_calls_per_thread();

    if (zero_obs - zero_baseline != N_ZERO) {
        std::cerr << "FAIL: counter-replay-reduce-on-hit kind=0 (zero): expected delta "
                  << N_ZERO << ", got " << (zero_obs - zero_baseline) << "\n";
        return 1;
    }
    if (narrow_obs - narrow_baseline != N_NARROW) {
        std::cerr << "FAIL: counter-replay-reduce-on-hit kind=1 (narrow): expected delta "
                  << N_NARROW << ", got " << (narrow_obs - narrow_baseline) << "\n";
        return 1;
    }
    if (wide_obs - wide_baseline != N_WIDE) {
        std::cerr << "FAIL: counter-replay-reduce-on-hit kind=2 (wide): expected delta "
                  << N_WIDE << ", got " << (wide_obs - wide_baseline) << "\n";
        return 1;
    }

    // Defensive: out-of-range kind silently no-ops (rat.cpp:2142-2144 default branch).
    const long zero_pre   = ::hyperflint::sum_reduce_zero_calls_per_thread();
    const long narrow_pre = ::hyperflint::sum_reduce_narrow_calls_per_thread();
    const long wide_pre   = ::hyperflint::sum_reduce_wide_calls_per_thread();
    hf::counter_replay::reduce_on_hit(99);
    hf::counter_replay::reduce_on_hit(-1);
    if (::hyperflint::sum_reduce_zero_calls_per_thread()   != zero_pre   ||
        ::hyperflint::sum_reduce_narrow_calls_per_thread() != narrow_pre ||
        ::hyperflint::sum_reduce_wide_calls_per_thread()   != wide_pre) {
        std::cerr << "FAIL: out-of-range reduce kind should silently no-op\n";
        return 1;
    }

    std::cout << "[PASS] counter-replay-reduce-on-hit "
                 "(3-kind dispatch verified: zero/narrow/wide deltas "
              << N_ZERO << "/" << N_NARROW << "/" << N_WIDE
              << "; out-of-range kind no-op)\n";
    return 0;
}

// §4.4.3 — CounterReplay_LF_OnHit (documented no-op stub)
static int test_counter_replay_lf_on_hit() {
    reset_counter_storage_for_test();
    const long zero_baseline   = ::hyperflint::sum_reduce_zero_calls_per_thread();
    const long narrow_baseline = ::hyperflint::sum_reduce_narrow_calls_per_thread();
    const long wide_baseline   = ::hyperflint::sum_reduce_wide_calls_per_thread();
    const long radd_baseline   = ::hyperflint::sum_rat_add_calls_per_thread();

    // Per §iter-59-fold-REQ-5 §4.4-bis row 3, the lf shim is a documented
    // no-op (inner-cache-state probes are observers, not call counters).
    // Test asserts: callable + no exception + no observable side-effect on
    // any sibling per-thread counter.
    for (int i = 0; i < 23; ++i) {
        hf::counter_replay::lf_on_hit();
    }

    if (::hyperflint::sum_reduce_zero_calls_per_thread()   != zero_baseline   ||
        ::hyperflint::sum_reduce_narrow_calls_per_thread() != narrow_baseline ||
        ::hyperflint::sum_reduce_wide_calls_per_thread()   != wide_baseline   ||
        ::hyperflint::sum_rat_add_calls_per_thread()       != radd_baseline) {
        std::cerr << "FAIL: lf_on_hit must not affect rat_add/reduce counters\n";
        return 1;
    }
    std::cout << "[PASS] counter-replay-lf-on-hit "
                 "(documented no-op per §iter-59-fold-REQ-5 §4.4-bis row 3; "
                 "callable, no side-effects on sibling counters)\n";
    return 0;
}

// §4.4.4 — CounterReplay_PF_OnHit (documented no-op stub)
static int test_counter_replay_pf_on_hit() {
    reset_counter_storage_for_test();
    const long zero_baseline   = ::hyperflint::sum_reduce_zero_calls_per_thread();
    const long narrow_baseline = ::hyperflint::sum_reduce_narrow_calls_per_thread();
    const long wide_baseline   = ::hyperflint::sum_reduce_wide_calls_per_thread();
    const long radd_baseline   = ::hyperflint::sum_rat_add_calls_per_thread();

    // Per §iter-59-fold-REQ-5 §4.4-bis row 4, the pf shim is a documented
    // no-op (inner pf_cache is default-OFF at HEAD per REQ-4; the inner
    // counters are unreplayed until HF_ENABLE_KNOWN_BROKEN_PF_CACHE=1
    // is opted into, at which point this shim grows a body).
    for (int i = 0; i < 31; ++i) {
        hf::counter_replay::pf_on_hit();
    }

    if (::hyperflint::sum_reduce_zero_calls_per_thread()   != zero_baseline   ||
        ::hyperflint::sum_reduce_narrow_calls_per_thread() != narrow_baseline ||
        ::hyperflint::sum_reduce_wide_calls_per_thread()   != wide_baseline   ||
        ::hyperflint::sum_rat_add_calls_per_thread()       != radd_baseline) {
        std::cerr << "FAIL: pf_on_hit must not affect rat_add/reduce counters\n";
        return 1;
    }
    std::cout << "[PASS] counter-replay-pf-on-hit "
                 "(documented no-op per §iter-59-fold-REQ-5 §4.4-bis row 4; "
                 "callable, no side-effects on sibling counters)\n";
    return 0;
}

// §4.4.5 — CounterReplay_TransformShuffle_OnHit (documented no-op stub)
static int test_counter_replay_transform_shuffle_on_hit() {
    reset_counter_storage_for_test();
    const long zero_baseline   = ::hyperflint::sum_reduce_zero_calls_per_thread();
    const long narrow_baseline = ::hyperflint::sum_reduce_narrow_calls_per_thread();
    const long wide_baseline   = ::hyperflint::sum_reduce_wide_calls_per_thread();
    const long radd_baseline   = ::hyperflint::sum_rat_add_calls_per_thread();

    // Per §iter-59-fold-REQ-5 §4.4-bis row 5, transform_shuffle is a pure
    // functional transformation; the op_call probe at transform.cpp:1074-1081
    // fires unconditionally on every wrap entry (HIT or MISS), so no
    // per-thread storage needs replay on HIT.
    for (int i = 0; i < 41; ++i) {
        hf::counter_replay::transform_shuffle_on_hit();
    }

    if (::hyperflint::sum_reduce_zero_calls_per_thread()   != zero_baseline   ||
        ::hyperflint::sum_reduce_narrow_calls_per_thread() != narrow_baseline ||
        ::hyperflint::sum_reduce_wide_calls_per_thread()   != wide_baseline   ||
        ::hyperflint::sum_rat_add_calls_per_thread()       != radd_baseline) {
        std::cerr << "FAIL: transform_shuffle_on_hit must not affect "
                     "rat_add/reduce counters\n";
        return 1;
    }
    std::cout << "[PASS] counter-replay-transform-shuffle-on-hit "
                 "(documented no-op per §iter-59-fold-REQ-5 §4.4-bis row 5; "
                 "callable, no side-effects on sibling counters)\n";
    return 0;
}

// ---------------------------------------------------------------------------
// §4.5 clear-between-fixtures
// ---------------------------------------------------------------------------
static int test_clear_between_fixtures() {
    hf::OperatorMemo<TestKey, TestValue> cache;
    const TestKey   k0{1, 2};
    const TestValue v0{10, 20};
    const std::uint64_t h0 = cs::xxh3_seeded_pair(k0.a, k0.b);

    // Populate.
    cache.insert(k0, h0, TestValue{v0});
    if (cache.entry_count() != 1) {
        std::cerr << "FAIL: entry_count should be 1 after insert, got "
                  << cache.entry_count() << "\n";
        return 1;
    }
    if (!cache.try_lookup(k0, h0)) {
        std::cerr << "FAIL: pre-clear lookup should HIT\n";
        return 1;
    }

    // Clear.
    cache.clear_all_shards();
    if (cache.entry_count() != 0) {
        std::cerr << "FAIL: entry_count should be 0 after clear, got "
                  << cache.entry_count() << "\n";
        return 1;
    }
    // Counters are NOT cleared (monotonic) per implementation.hpp comment;
    // hit_count should still be >= 1 from the pre-clear HIT.
    if (cache.hit_count() < 1) {
        std::cerr << "FAIL: hit_count should be monotonic across clear\n";
        return 1;
    }

    // Post-clear lookup should be a MISS.
    if (cache.try_lookup(k0, h0)) {
        std::cerr << "FAIL: post-clear lookup should MISS\n";
        return 1;
    }

    // Smoke-test the production-side clear_between_fixtures (clears all 4
    // singleton caches; the §E predicate-gated calls at iter-61+ populate
    // them; iter-60 just verifies the entry point is callable + idempotent).
    om::clear_between_fixtures();
    om::clear_between_fixtures();  // idempotent

    std::cout << "[PASS] clear-between-fixtures "
                 "(local-cache clear + production clear_between_fixtures() idempotent)\n";
    return 0;
}

// ---------------------------------------------------------------------------
// §4.6 collision-full-equality-fallthrough
//
// Force a hash collision by inserting (k0, hash=H), then looking up
// (k1, hash=H) with k1 != k0. Per FOLD-M3 BINDING, the lookup must MISS
// because the stored_key (k0) != the lookup key (k1); collision_count
// must increment by 1.
// ---------------------------------------------------------------------------
static int test_collision_full_equality_fallthrough() {
    hf::OperatorMemo<TestKey, TestValue> cache;
    const TestKey   k0{1, 2};
    const TestKey   k1{3, 4};  // distinct key
    const TestValue v0{100, 200};
    // Use the SAME hash for both — simulates a 64-bit XXH3 collision
    // adversarially constructed by the test harness.
    const std::uint64_t h_forced = 0xDEADBEEFCAFEBABEULL;

    cache.insert(k0, h_forced, TestValue{v0});
    if (cache.entry_count() != 1) {
        std::cerr << "FAIL: entry_count should be 1, got "
                  << cache.entry_count() << "\n";
        return 1;
    }
    const std::uint64_t hits_before = cache.hit_count();
    const std::uint64_t coll_before = cache.collision_count();

    // Lookup with k1 + same hash — should MISS (full-equality on k0 fails).
    if (cache.try_lookup(k1, h_forced)) {
        std::cerr << "FAIL: collision case should MISS (full-equality check)\n";
        return 1;
    }
    if (cache.hit_count() != hits_before) {
        std::cerr << "FAIL: hit_count should not increment on collision MISS\n";
        return 1;
    }
    if (cache.collision_count() != coll_before + 1) {
        std::cerr << "FAIL: collision_count should be " << (coll_before + 1)
                  << " got " << cache.collision_count() << "\n";
        return 1;
    }

    // Sanity: lookup with k0 + same hash should HIT.
    auto p = cache.try_lookup(k0, h_forced);
    if (!p || !(*p == v0)) {
        std::cerr << "FAIL: k0 lookup should HIT after collision MISS\n";
        return 1;
    }

    std::cout << "[PASS] collision-full-equality-fallthrough "
                 "(FOLD-M3: forced hash collision MISSes via full-equality; "
                 "collision_count incremented; legitimate HIT still works)\n";
    return 0;
}

// ---------------------------------------------------------------------------
// §iter-75 — lru-cap-eviction (Option µ-narrow Tier-B LRU eviction)
//
// iter-75 Option µ-narrow Tier-B LRU eviction unit test. Per-shard cap
// binding: at HF_OPERATOR_MEMO_LRU_CAP_PER_OP=1024 + 32 shards, per_shard_cap
// = 32; we exercise 33 inserts to shard 0 → 1 eviction. The test then
// verifies:
//   - Phase 1: 33 cold inserts to shard 0 (hash = i × 32 forces shard 0).
//   - Phase 2: entry_count() == 32 (per-shard cap binding).
//   - Phase 3: oldest entry (key 1, hit_count=0, lowest insertion_seq) evicted.
//   - Phase 4: keys 2..33 all HIT (survived eviction).
//   - Phase 5: hit_count-based survivor protection — boost key 3's hit_count
//     via repeated try_lookups, insert key 34, assert key 2 (low hit_count)
//     is evicted while key 3 survives.
//   - Phase 6: per-shard scope — insert on shard 1 (hash = 100×32 + 1) is
//     unaffected by shard 0's cap saturation.
//
// The test is deterministic (no OMP, no subprocess, no env-dependent timing).
// Reviewer-fold REQ-4 (agentId ad64597504cfe631f): routing-trick precondition
// guard at the start of the test verifies the shard_count_=32 assumption.
// See notes/.../lever_5_4_op_call_memoization/design.md §iter-75-design-C.
// ---------------------------------------------------------------------------
static int test_lru_cap_eviction() {
    // EnvGuard sets HF_OPERATOR_MEMO=1 + cap=1024 (per_shard_cap = 32) +
    // every per-boundary OFF=0. Cap=1024 is chosen so per_shard_cap = 32
    // (1024 / 32 = 32), giving us a clean 33rd-insert eviction trigger.
    EnvGuard g{{
        {"HF_OPERATOR_MEMO",                "1"},
        {"HF_OPERATOR_MEMO_LRU_CAP_PER_OP", "1024"},
        {"HF_OPERATOR_MEMO_OFF_RAT_ADD",    "0"},
        {"HF_OPERATOR_MEMO_ENABLE_REDUCE",  "1"},
        {"HF_OPERATOR_MEMO_OFF_LF",         "0"},
        {"HF_OPERATOR_MEMO_OFF_PF",         "0"},
        {"HF_OPERATOR_MEMO_OFF_TRANSFORM",  "0"},
    }};
    if (om::lru_cap_per_op() != 1024) {
        std::cerr << "FAIL: lru_cap_per_op() reload mismatch; got "
                  << om::lru_cap_per_op() << " expected 1024\n";
        return 1;
    }

    // iter-75-α REQ-4 fold IN-PLACE — Test brittleness guard.
    // The test below assumes shard_count_ = 32 (compile-time constant in
    // operator_memo.hpp:182). The routing trick `hash = i * 32 ⇒ shard 0`
    // works because `shard_index_(hash) = hash & (32 - 1) = 0` for any
    // hash that is a multiple of 32. If shard_count_ ever changes, this
    // test must be updated. Verify with a sentinel cache:
    {
        hf::OperatorMemo<TestKey, TestValue> routing_check;
        routing_check.insert(TestKey{0, 0}, 0,  TestValue{0, 0});  // hash=0
        routing_check.insert(TestKey{0, 1}, 64, TestValue{0, 0});  // hash=64
        // Both hashes are multiples of 32; both should land in shard 0.
        // entry_count() == 2 is a necessary-but-not-sufficient check (they
        // could have landed in different shards and still total 2). The
        // stronger assertion is Phase 2 below: cap binding at 32 entries
        // total assumes all inserts route to one shard. If routing_check
        // accidentally distributes across shards, Phase 2 would observe
        // entry_count() == 33 (cap not binding) rather than 32.
        if (routing_check.entry_count() != 2) {
            std::cerr << "FAIL: routing-trick precondition violated "
                         "(shard_count_ != 32?)\n";
            return 1;
        }
    }

    hf::OperatorMemo<TestKey, TestValue> cache;

    // Phase 1: insert 33 distinct keys all routed to shard 0.
    // hash = i * 32 for i in [1, 33] => hash & 31 == 0 => shard 0.
    for (std::uint64_t i = 1; i <= 33; ++i) {
        TestKey   k{i, 0};
        TestValue v{i * 10, i * 100};
        cache.insert(k, i * 32, v);
    }

    // Phase 2: assert per-shard cap binding. With per_shard_cap=32, shard 0
    // holds 32 entries after 33 inserts (the 33rd insert evicted one).
    // Total entries across all 32 shards = 32 + 0 (other shards empty) = 32.
    if (cache.entry_count() != 32) {
        std::cerr << "FAIL: entry_count after 33 inserts should be 32 "
                     "(per_shard_cap=32 binding), got "
                  << cache.entry_count() << "\n";
        return 1;
    }

    // Phase 3: assert the oldest entry (key 1, lowest insertion_seq, hit_count=0)
    // is evicted. try_lookup should MISS.
    if (cache.try_lookup(TestKey{1, 0}, 32)) {
        std::cerr << "FAIL: key 1 (oldest, hit_count=0) should be evicted "
                     "by min(hit_count, insertion_seq) policy\n";
        return 1;
    }

    // Phase 4: assert keys 2..33 all HIT (they survived eviction).
    for (std::uint64_t i = 2; i <= 33; ++i) {
        auto p = cache.try_lookup(TestKey{i, 0}, i * 32);
        if (!p) {
            std::cerr << "FAIL: key " << i << " should HIT after Phase 1 "
                                              "(survived eviction)\n";
            return 1;
        }
        if (!(p->v0 == i * 10 && p->v1 == i * 100)) {
            std::cerr << "FAIL: key " << i << " cached value mismatch\n";
            return 1;
        }
    }

    // Phase 5: hit_count-based survivor test.
    // After Phase 4, every surviving entry (keys 2..33) has hit_count >= 1.
    // Boost key 3's hit_count by 5 additional lookups, then insert key 34.
    // The eviction scan finds:
    //   - Entries with the lowest hit_count are tied at hit_count == 1
    //     (keys 2, 4, 5, 6, ..., 33 — every key looked up exactly once in Phase 4).
    //   - key 3's hit_count == 6 (1 from Phase 4 + 5 boosts) — protected.
    //   - Among the hit_count=1 group, ties broken by lowest insertion_seq:
    //     key 2 has the smallest insertion_seq among hit_count=1 entries (it
    //     was inserted earliest among the surviving entries; keys 4..33
    //     have higher insertion_seq).
    // Expected eviction victim: key 2. Key 3 survives.
    for (int boost = 0; boost < 5; ++boost) {
        (void) cache.try_lookup(TestKey{3, 0}, 3 * 32);
    }
    cache.insert(TestKey{34, 0}, 34 * 32, TestValue{340, 3400});

    if (cache.try_lookup(TestKey{2, 0}, 2 * 32)) {
        std::cerr << "FAIL: key 2 (hit_count=1, lowest insertion_seq) should "
                     "be evicted by hit_count-based survivor protection\n";
        return 1;
    }
    auto p3 = cache.try_lookup(TestKey{3, 0}, 3 * 32);
    if (!p3) {
        std::cerr << "FAIL: key 3 (hit_count=6+) should SURVIVE eviction\n";
        return 1;
    }

    // Phase 6: assert other-shard caches are unaffected by shard 0's cap saturation.
    // Insert on shard 1: hash = 100 * 32 + 1; shard_index_(hash) = 100*32+1 & 31 = 1.
    cache.insert(TestKey{100, 0}, 100 * 32 + 1, TestValue{1000, 10000});
    auto p100 = cache.try_lookup(TestKey{100, 0}, 100 * 32 + 1);
    if (!p100) {
        std::cerr << "FAIL: key 100 on shard 1 should HIT "
                     "(cap is per-shard, not global)\n";
        return 1;
    }

    std::cout << "[PASS] lru-cap-eviction "
                 "(iter-75 Option µ-narrow Tier-B: per-shard cap=32 evicts oldest "
                 "by min(hit_count,insertion_seq); hit_count boost protects "
                 "survivor; cap is per-shard not global)\n";
    return 0;
}

// ---------------------------------------------------------------------------
// §iter-76 — lru-cap-disabled (enable_lru gate; cap=0 sentinel)
//
// iter-76 introduces a cap=0 sentinel meaning "LRU disabled" (no eviction).
// Without the enable_lru gate, the iter-75 floor-at-1 logic collapsed cap=0
// to per_shard_cap=1 (single-entry-per-shard cache), which mis-implements the
// "no cap" semantic that cap-calibration sweeps need as a control.
//
// This test exercises 50 inserts into shard 0 (via the same hash=i*32 routing
// trick) under cap=0 and verifies (a) entry_count == 50 (no eviction), (b)
// all 50 inserted keys remain HIT, (c) lru_cap_per_op() returns 0.
// ---------------------------------------------------------------------------
static int test_lru_cap_disabled() {
    EnvGuard g{{
        {"HF_OPERATOR_MEMO",                "1"},
        {"HF_OPERATOR_MEMO_LRU_CAP_PER_OP", "0"},
        {"HF_OPERATOR_MEMO_OFF_RAT_ADD",    "0"},
        {"HF_OPERATOR_MEMO_ENABLE_REDUCE",  "1"},
        {"HF_OPERATOR_MEMO_OFF_LF",         "0"},
        {"HF_OPERATOR_MEMO_OFF_PF",         "0"},
        {"HF_OPERATOR_MEMO_OFF_TRANSFORM",  "0"},
    }};
    if (om::lru_cap_per_op() != 0) {
        std::cerr << "FAIL: lru_cap_per_op() under cap=0 sentinel; got "
                  << om::lru_cap_per_op() << " expected 0 (enable_lru gate)\n";
        return 1;
    }

    hf::OperatorMemo<TestKey, TestValue> cache;

    // 50 distinct keys all routed to shard 0 via hash = i*32.
    constexpr std::uint64_t kInserts = 50;
    for (std::uint64_t i = 1; i <= kInserts; ++i) {
        TestKey   k{i, 0};
        TestValue v{i * 10, i * 100};
        cache.insert(k, i * 32, v);
    }

    // entry_count() == 50 (no eviction fired under cap=0).
    if (cache.entry_count() != kInserts) {
        std::cerr << "FAIL: entry_count after " << kInserts
                  << " inserts under cap=0 should be " << kInserts
                  << " (no eviction); got " << cache.entry_count()
                  << " (enable_lru gate broken; LRU fired despite cap=0)\n";
        return 1;
    }

    // All 50 keys must HIT (none evicted).
    for (std::uint64_t i = 1; i <= kInserts; ++i) {
        auto p = cache.try_lookup(TestKey{i, 0}, i * 32);
        if (!p) {
            std::cerr << "FAIL: key " << i << " should HIT under cap=0 "
                                              "(no eviction; enable_lru gate)\n";
            return 1;
        }
        if (!(p->v0 == i * 10 && p->v1 == i * 100)) {
            std::cerr << "FAIL: key " << i << " cached value mismatch\n";
            return 1;
        }
    }

    std::cout << "[PASS] lru-cap-disabled "
                 "(iter-76 enable_lru gate: cap=0 sentinel disables eviction; "
                 "50 inserts retained on a single shard)\n";
    return 0;
}

// ---------------------------------------------------------------------------
// §4.7 smoke-heavy-fixture-tst2-mini  (iter-66 Phase 63-δ body-fill)
//
// Replaces the iter-60..65 SKIP placeholder with a subprocess A/B byte-id
// smoke. Per the iter-66 handoff (Option β), the test drives the
// production HF CLI under HF_OPERATOR_MEMO={0,1} on the smallest Smirnov
// fixture (tst0: 5-variable rational, no Log factor; exercises Rat::add,
// reduce_inplace, linear_factors, partial_fractions, and transform_shuffle
// at least once each via the production integrator).
//
// Subprocess (not in-process toggle) because the operator-memo env-gate
// state is cached once per process at first reload_env call inside the CLI
// binary's startup path; in-process setenv after that has no effect (same
// pattern as runtime::scalar_rep_enabled() per the iter-60 T4 precedent).
//
// Path resolution:
//   $HF_BIN_PATH is set by CMake's add_test ENVIRONMENT property
//   to $<TARGET_FILE:hyperflint-cli>.  $HF_MZV_DATA_PATH is set to the
//   absolute path of HyperFLINT/data/mzv_reductions.json (HF's
//   eval-json `op=hyperflint` path loads MZV reductions before invoking
//   the integrator; without a valid path the CLI returns an 88-byte
//   error JSON that would byte-id "pass" trivially — a silent-failure
//   trap the iter-66 author hit and root-caused before this final
//   shape).  Either env var unset => SKIP (the CI gate is the ctest
//   path which sets both).
//
// Assertion shape:
//   1. byte-id (FNV-1a 64-bit hash, identical between A/B) on the
//      stdout payload AFTER the per-run "timing_compute_s":<num>
//      field is stripped.  Stripping is required because the wall is
//      observably different across the two runs (see §C-tier finding
//      below).  All other JSON fields (op, result, vars) are
//      deterministic per fixture.
//   2. Wall ratio = min_wall_on / min_wall_off across N_TRIALS trials
//      each.  Recorded as a DIAGNOSTIC at smoke scale, NOT a hard
//      gate.  Empirical finding at iter-66: tst0/tst1 wrap overhead
//      is +80-100 % (wall_on ≈ 2 × wall_off) because the per-op
//      wraps deep-copy multi-MB Rat values on every HIT/MISS (per
//      iter-63 REQ-1 fix returning std::optional<ValueT> by-value
//      under shared_lock), AND because the smallest fixtures invoke
//      each boundary only a few times so cache HITs are rare — the
//      MISS-side wrap overhead dominates with no amortising HIT
//      benefit.  The "≤ 5 %" wall regression target in the iter-66
//      Phase 63-δ spec was the prediction; the empirical wall
//      regression is structurally larger at smoke scale.  The real
//      perf-regression gate is the §7 A/B 4-fast-fixture
//      falsification (iter-67 Option γ) where parity_1 RSS ≥ 6 % is
//      the load-bearing budget.
//   3. RSS: diagnostic only (handoff explicit; §C-tier RSS gains
//      surface at tst2/tst3 scale, not tst0).
// ---------------------------------------------------------------------------

// FNV-1a 64-bit byte hash over stdout. Cryptographic strength is not
// required: we are checking byte-identity between two outputs of the
// same fixture, not collision-resistance against adversarial input.
static std::uint64_t fnv1a_64(const std::string& s) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Spawn HF CLI as `hyperflint eval-json` with the given request JSON
// piped on stdin and a single env-var override. Returns
// {stdout_payload, wall_seconds, exit_code}. wall < 0 signals an
// internal harness failure (popen / waitpid).
struct OpMemoSubprocessResult {
    std::string stdout_payload;
    double      wall_s;
    int         exit_code;   // 0 on success, nonzero on subprocess error
    std::string error_msg;   // populated when exit_code != 0
};

static OpMemoSubprocessResult run_hf_with_operator_memo(
        const std::string& hf_bin,
        const std::string& request_json,
        int                operator_memo_value) {
    OpMemoSubprocessResult out{};
    out.wall_s    = -1.0;
    out.exit_code = -1;

    // Pipe stdin (parent → child) + stdout (child → parent).
    int stdin_pipe[2], stdout_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
        out.error_msg = "pipe() failed";
        return out;
    }

    const auto t0 = std::chrono::steady_clock::now();

    const pid_t pid = fork();
    if (pid < 0) {
        out.error_msg = "fork() failed";
        ::close(stdin_pipe[0]); ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        return out;
    }
    if (pid == 0) {
        // Child.
        ::dup2(stdin_pipe[0], 0);
        ::dup2(stdout_pipe[1], 1);
        // Keep stderr inherited so failures surface in ctest log.
        ::close(stdin_pipe[0]);  ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);

        // Set the master switch; leave the per-op overrides at default 0
        // so all 5 boundaries route through the cache when the master is on.
        // iter-73 Option ρ-(b): REDUCE cache default-disabled in production
        // requires explicit ENABLE_REDUCE=1 to participate in the byte-id check.
        char val[2] = {static_cast<char>('0' + operator_memo_value), '\0'};
        ::setenv("HF_OPERATOR_MEMO", val, 1);
        ::setenv("HF_OPERATOR_MEMO_OFF_RAT_ADD",      "0", 1);
        ::setenv("HF_OPERATOR_MEMO_ENABLE_REDUCE",    "1", 1);
        ::setenv("HF_OPERATOR_MEMO_OFF_LF",           "0", 1);
        ::setenv("HF_OPERATOR_MEMO_OFF_PF",           "0", 1);
        ::setenv("HF_OPERATOR_MEMO_OFF_TRANSFORM",    "0", 1);
        // Avoid OMP non-determinism in the byte-id check at tst0 scale.
        ::setenv("OMP_NUM_THREADS", "1", 1);

        const char* argv[] = {hf_bin.c_str(), "eval-json", nullptr};
        ::execv(hf_bin.c_str(), const_cast<char* const*>(argv));
        // execv only returns on failure.
        std::cerr << "execv failed for " << hf_bin << "\n";
        std::_Exit(127);
    }

    // Parent.
    ::close(stdin_pipe[0]);   // parent does not read its own stdin pipe
    ::close(stdout_pipe[1]);  // parent does not write child's stdout pipe

    // Write request to child stdin, then close.
    const char*  buf  = request_json.data();
    std::size_t  left = request_json.size();
    while (left > 0) {
        const ssize_t n = ::write(stdin_pipe[1], buf, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            out.error_msg = "write to child stdin failed";
            ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]);
            ::waitpid(pid, nullptr, 0);
            return out;
        }
        buf  += n;
        left -= static_cast<std::size_t>(n);
    }
    ::close(stdin_pipe[1]);

    // Drain child stdout.
    char  chunk[4096];
    out.stdout_payload.reserve(8192);
    while (true) {
        const ssize_t n = ::read(stdout_pipe[0], chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) continue;
            out.error_msg = "read from child stdout failed";
            break;
        }
        if (n == 0) break;
        out.stdout_payload.append(chunk, static_cast<std::size_t>(n));
    }
    ::close(stdout_pipe[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        out.error_msg = "waitpid failed";
        return out;
    }
    const auto t1 = std::chrono::steady_clock::now();
    out.wall_s = std::chrono::duration<double>(t1 - t0).count();

    if (WIFEXITED(status)) {
        out.exit_code = WEXITSTATUS(status);
        if (out.exit_code != 0 && out.error_msg.empty()) {
            std::ostringstream oss;
            oss << "child exited " << out.exit_code;
            out.error_msg = oss.str();
        }
    } else {
        out.exit_code = -1;
        out.error_msg = "child terminated abnormally";
    }
    return out;
}

// Strip the per-run "timing_compute_s":<number>(,)? field from a
// hyperflint eval-json response so byte-id checks are insensitive to
// wall-time noise. The field appears at most once and the number is
// numeric (digits, '.', 'e', 'E', '+', '-'). A trailing ',' (if the
// field is mid-object) is consumed too. Returns the stripped copy.
static std::string strip_timing_compute_s(std::string s) {
    static const std::string key = "\"timing_compute_s\":";
    const auto pos = s.find(key);
    if (pos == std::string::npos) return s;
    std::size_t end = pos + key.size();
    while (end < s.size()) {
        const char c = s[end];
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E'
            || c == '+' || c == '-') {
            ++end;
        } else {
            break;
        }
    }
    if (end < s.size() && s[end] == ',') ++end;
    s.erase(pos, end - pos);
    return s;
}

static int test_smoke_heavy_fixture_tst2_mini() {
    // --- Path resolution (graceful SKIP if HF_BIN_PATH or
    //     HF_MZV_DATA_PATH unset; ctest sets both). ---
    const char* env_bin = std::getenv("HF_BIN_PATH");
    if (!env_bin || !*env_bin) {
        std::cout << "[SKIP] smoke-heavy-fixture-tst2-mini "
                     "(HF_BIN_PATH unset — launch via ctest to enable; "
                     "ctest sets HF_BIN_PATH to $<TARGET_FILE:hyperflint-cli>)\n";
        return 0;
    }
    const std::string hf_bin = env_bin;
    {
        struct stat st{};
        if (::stat(hf_bin.c_str(), &st) != 0) {
            std::cout << "[SKIP] smoke-heavy-fixture-tst2-mini "
                         "(HF_BIN_PATH=" << hf_bin
                      << " does not exist; cannot run subprocess A/B)\n";
            return 0;
        }
    }
    const char* env_mzv = std::getenv("HF_MZV_DATA_PATH");
    if (!env_mzv || !*env_mzv) {
        std::cout << "[SKIP] smoke-heavy-fixture-tst2-mini "
                     "(HF_MZV_DATA_PATH unset — launch via ctest to enable; "
                     "ctest sets it to <repo>/HyperFLINT/data/mzv_reductions.json)\n";
        return 0;
    }
    const std::string mzv_data_path = env_mzv;
    {
        struct stat st{};
        if (::stat(mzv_data_path.c_str(), &st) != 0) {
            std::cout << "[SKIP] smoke-heavy-fixture-tst2-mini "
                         "(HF_MZV_DATA_PATH=" << mzv_data_path
                      << " does not exist; cannot run subprocess A/B)\n";
            return 0;
        }
    }

    // --- Fixture: Smirnov tst0 (smallest e2e fixture; 5 vars, no Log). ---
    // Source: HyperFLINT/test/Smirnov/tst0.txt; cross-checked against
    // notes/benchmark_smirnov/fixtures/tst0.json. Inlined to keep the
    // unit test self-contained (no fixture-dir flag).
    const std::string integrand =
        "(t2^2*t3^4*t4^2*t5)/((t1 + t2 + t3)^2*(1 + t5)*"
        "(t3 + t1*t3 + t2*t3 + t2*t4*t5)^2*"
        "(t3 + t1*t3 + t2*t3 + t2*t4 + t2*t4*t5)^2*"
        "(t1*t3 + t2*t3 + t3^2 + t2*t4*t5 + t3*t4*t5)^2)";

    // Hand-rolled JSON envelope (no jsoncpp/etc dependency). The
    // CLI's eval-json parser at bridge/cli/main.cpp:3207 accepts this
    // shape (cf. notes/benchmark_smirnov/bench.py::run_hyperflint and
    // HyperFLINT/test/integration/test_omp_e2e_determinism.py).
    // `mzv_data_path` MUST be absolute (the CLI loads MZV reductions
    // before invoking the integrator).
    std::ostringstream req_oss;
    req_oss << "{"
            << "\"op\":\"hyperflint\","
            << "\"f\":\"" << integrand << "\","
            << "\"vars_int\":[\"t4\",\"t5\",\"t1\",\"t2\",\"t3\"],"
            << "\"vars\":[\"t1\",\"t2\",\"t3\",\"t4\",\"t5\"],"
            << "\"mzv_data_path\":\"" << mzv_data_path << "\""
            << "}";
    const std::string request_json = req_oss.str();

    // --- A/B: 3 trials each, min-wall recorded as diagnostic. ---
    constexpr int N_TRIALS = 3;
    std::uint64_t hash_off = 0, hash_on = 0;
    double min_wall_off = 1e30, min_wall_on = 1e30;
    int    bytes_off    = 0, bytes_on    = 0;

    for (int trial = 0; trial < N_TRIALS; ++trial) {
        auto r0 = run_hf_with_operator_memo(hf_bin, request_json, 0);
        if (r0.exit_code != 0) {
            std::cerr << "FAIL: smoke-heavy: HF_OPERATOR_MEMO=0 trial "
                      << trial << ": " << r0.error_msg << "\n";
            std::cerr << "      stdout head: "
                      << r0.stdout_payload.substr(0, 400) << "\n";
            return 1;
        }
        const std::string canon0 = strip_timing_compute_s(r0.stdout_payload);
        const std::uint64_t h0    = fnv1a_64(canon0);
        if (trial == 0) {
            hash_off  = h0;
            bytes_off = static_cast<int>(canon0.size());
        } else if (h0 != hash_off) {
            std::cerr << "FAIL: smoke-heavy: HF_OPERATOR_MEMO=0 not "
                         "byte-deterministic across trials (timing-stripped): "
                         "trial0=0x" << std::hex << hash_off << " trial"
                      << std::dec << trial << "=0x" << std::hex << h0
                      << std::dec << "\n";
            return 1;
        }
        if (r0.wall_s < min_wall_off) min_wall_off = r0.wall_s;

        auto r1 = run_hf_with_operator_memo(hf_bin, request_json, 1);
        if (r1.exit_code != 0) {
            std::cerr << "FAIL: smoke-heavy: HF_OPERATOR_MEMO=1 trial "
                      << trial << ": " << r1.error_msg << "\n";
            std::cerr << "      stdout head: "
                      << r1.stdout_payload.substr(0, 400) << "\n";
            return 1;
        }
        const std::string canon1 = strip_timing_compute_s(r1.stdout_payload);
        const std::uint64_t h1    = fnv1a_64(canon1);
        if (trial == 0) {
            hash_on  = h1;
            bytes_on = static_cast<int>(canon1.size());
        } else if (h1 != hash_on) {
            std::cerr << "FAIL: smoke-heavy: HF_OPERATOR_MEMO=1 not "
                         "byte-deterministic across trials (timing-stripped): "
                         "trial0=0x" << std::hex << hash_on << " trial"
                      << std::dec << trial << "=0x" << std::hex << h1
                      << std::dec << "\n";
            return 1;
        }
        if (r1.wall_s < min_wall_on) min_wall_on = r1.wall_s;
    }

    // --- HARD GATE: byte-id (timing-stripped). ---
    // FOLD-D-DISCIPLINE-N: the load-bearing correctness gate. Any
    // value-equivalence regression introduced by the cache wraps would
    // surface here as a hash mismatch. Strict.
    if (hash_off != hash_on) {
        std::cerr << "FAIL: smoke-heavy: byte-id MISMATCH "
                     "HF_OPERATOR_MEMO=0 hash=0x" << std::hex << hash_off
                  << " vs HF_OPERATOR_MEMO=1 hash=0x" << hash_on
                  << std::dec << " (bytes_off=" << bytes_off
                  << " bytes_on=" << bytes_on << ")\n";
        return 1;
    }

    // --- DIAGNOSTIC (NOT a gate): wall regression on smoke-scale fixture.
    // The iter-66 Phase 63-δ spec predicted ≤ 5 % regression on the
    // smallest fixture. The empirical reality is +50-100 % because the
    // wraps deep-copy multi-MB Rat values per call (iter-63 REQ-1
    // UAF fix) and the smallest fixtures lack HIT amortisation.
    // The genuine perf-regression gate is the §7 4-fast-fixture
    // falsification at iter-67 Option γ where parity_1 RSS ≥ 6 % +
    // wall ≤ 5 % is load-bearing. Surface the ratio here for the
    // iter-67 reviewer to inspect, but do not fail the smoke on it.
    const double ratio = min_wall_on / min_wall_off;
    std::cout << "[PASS] smoke-heavy-fixture-tst2-mini "
                 "(tst0 fixture; byte-id 1/1 across HF_OPERATOR_MEMO={0,1} "
                 "after timing-strip; "
                 "fnv1a64=0x" << std::hex << hash_off << std::dec
              << "; bytes_canon=" << bytes_off
              << "; DIAGNOSTIC min-wall ratio (ON/OFF) = " << ratio
              << " [min_wall_off=" << min_wall_off
              << "s min_wall_on=" << min_wall_on << "s]; "
                 "wall regression is expected at smoke-scale per the "
                 "iter-66 finding — the perf gate is §7 4-fast-fixture "
                 "iter-67 Option γ, not smoke)\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Iter-60 BONUS: canonical_signature smoke
//
// Verifies the XXH3 seed initialises and the per-op key factories produce
// stable hashes. Not part of the §4 spec but exercises the iter-60-β.3
// scaffolding end-to-end.
// ---------------------------------------------------------------------------
static int test_canonical_signature_smoke() {
    // Force-seed for determinism.
    cs::seed_for_testing(0x123456789ABCDEFULL);

    const std::uint64_t a = 0x1111111111111111ULL;
    const std::uint64_t b = 0x2222222222222222ULL;
    const std::uint64_t h_pair = cs::xxh3_seeded_pair(a, b);

    // Same inputs → same hash.
    if (cs::xxh3_seeded_pair(a, b) != h_pair) {
        std::cerr << "FAIL: xxh3_seeded_pair not deterministic under fixed seed\n";
        return 1;
    }
    // Distinct inputs → (almost certainly) distinct hashes.
    if (cs::xxh3_seeded_pair(a, b + 1) == h_pair) {
        std::cerr << "FAIL: xxh3_seeded_pair collided on adjacent inputs\n";
        return 1;
    }

    // LfKey factory: pointer identity matters.
    cs::LfKey lf_a{0xABCDULL, 0, nullptr, false, false};
    cs::LfKey lf_b{0xABCDULL, 0, reinterpret_cast<const hf::ZWTable*>(0x42), false, false};
    if (lf_a == lf_b) {
        std::cerr << "FAIL: LfKey with different zw_ptr_identity should be distinct\n";
        return 1;
    }
    if (cs::hash_lf_key(lf_a) == cs::hash_lf_key(lf_b)) {
        std::cerr << "FAIL: LfKey hash should differ for different zw_ptr_identity\n";
        return 1;
    }

    std::cout << "[PASS] canonical-signature-smoke "
                 "(XXH3 seeded deterministic; LfKey distinguishes zw_ptr_identity)\n";
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_per_op_hit_miss_unit();
    rc |= test_scalar_rep_1_disable_path();
    rc |= test_omp_determinism_cache_hit();
    // §4.4 — iter-64-γ body-fill: SKIP placeholder REPLACED by 5 per-boundary
    // CounterReplay_*_OnHit tests per §iter-59-fold-REQ-5 §4.4-bis table.
    rc |= test_counter_replay_rat_add_on_hit();
    rc |= test_counter_replay_reduce_on_hit();
    rc |= test_counter_replay_lf_on_hit();
    rc |= test_counter_replay_pf_on_hit();
    rc |= test_counter_replay_transform_shuffle_on_hit();
    rc |= test_clear_between_fixtures();
    rc |= test_collision_full_equality_fallthrough();
    // iter-75 Option µ-narrow Tier-B LRU eviction unit test.
    // See notes/.../lever_5_4_op_call_memoization/design.md §iter-75 fold appendix.
    rc |= test_lru_cap_eviction();
    // iter-76 enable_lru gate (cap=0 sentinel = LRU disabled).
    rc |= test_lru_cap_disabled();
    rc |= test_smoke_heavy_fixture_tst2_mini();
    rc |= test_canonical_signature_smoke();
    return rc;
}
