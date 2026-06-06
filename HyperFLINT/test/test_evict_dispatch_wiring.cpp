// HF FF Phase 6 REVISED §E iter-9 — REQ-11 dispatch-wiring per-cache test.
//
// Validates that `operator_memo::evict_lru_batch_all_caches(N)` dispatches
// to ALL FIVE production caches with bit-precise per-cache delta:
//   - g_rat_add_cache             (RatAddKey  / Rat)
//   - g_reduce_cache              (ReduceKey  / ReduceValue, via reduce_insert_with_kind)
//   - g_lf_cache_outer            (LfKey      / LinearFactorization)
//   - g_pf_cache_outer            (PfKey      / PartialFractionization)
//   - g_transform_shuffle_cache   (TransformShuffleKey / TransformResultSym)
//
// Iter-7 test_op_memo_evict.cpp §3b verified only that the hook does NOT
// crash with empty production caches. A broken dispatch line (e.g., a
// missing `total += g_X_cache().evict_lru_batch(...)` in
// operator_memo.cpp:486-495) would be undetectable by §3b under
// default-OFF byte-id smoke (REQ-12). REQ-11 closes that gap by populating
// each production cache with one entry, calling the dispatch, and
// asserting per-cache `eviction_count` deltas of exactly 1.
//
// Per runner_iter9_design.md §1.1.

#include "hyperflint/algebra/linear_factors.hpp"
#include "hyperflint/algebra/partial_fractions.hpp"
#include "hyperflint/core/canonical_signature.hpp"
#include "hyperflint/core/operator_memo.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/integrator/transform.hpp"

#include <cstdint>
#include <iostream>

namespace hf = hyperflint;
namespace cs = hyperflint::canonical_signature;
namespace om = hyperflint::operator_memo;

// Local forward-decl of g_transform_shuffle_cache (defined in
// operator_memo.cpp). operator_memo.hpp omits this accessor because
// TransformResultSym (= std::vector<TransformPairSym>) is awkward to
// forward-declare in the public header; transform.cpp uses the same
// pattern at transform.cpp:1069.
namespace hyperflint {
OperatorMemo<canonical_signature::TransformShuffleKey,
             TransformResultSym>&
g_transform_shuffle_cache();
} // namespace hyperflint

// ---------------------------------------------------------------------------
// Helper: report on PASS / FAIL with cache name.
// ---------------------------------------------------------------------------
#define ASSERT_DELTA_EQ(name, before, after, want)                  \
    do {                                                            \
        const std::uint64_t got = (after) - (before);               \
        if (got != static_cast<std::uint64_t>(want)) {              \
            std::cerr << "FAIL [" name "]: delta=" << got           \
                      << " expected=" << (want) << "\n";            \
            return 1;                                               \
        }                                                           \
    } while (0)

#define ASSERT_EQ(name, lhs, rhs)                                   \
    do {                                                            \
        if ((lhs) != (rhs)) {                                       \
            std::cerr << "FAIL [" name "]: lhs=" << (lhs)           \
                      << " rhs=" << (rhs) << "\n";                  \
            return 1;                                               \
        }                                                           \
    } while (0)

// ---------------------------------------------------------------------------
// run_pass — populate, dispatch, assert per-cache delta. Returns 0 on PASS.
// `pass_label` distinguishes the cold (first) and warm (second) invocations.
// ---------------------------------------------------------------------------
static int run_pass(const char* pass_label, bool expect_evicted)
{
    // Per-fixture clear: drop all entries from the 5 production caches.
    // Counters (insertion_seq, eviction_count, etc.) are NOT reset by
    // clear_between_fixtures (per operator_memo.hpp:543 docstring) — we
    // therefore measure deltas from a snapshot taken AFTER the clear.
    om::clear_between_fixtures();

    auto& cr = hf::g_rat_add_cache();
    auto& cd = hf::g_reduce_cache();
    auto& cl = hf::g_lf_cache_outer();
    auto& cp = hf::g_pf_cache_outer();
    auto& ct = hf::g_transform_shuffle_cache();

    // Sanity: post-clear, each cache should have entry_count() == 0.
    if (cr.entry_count() != 0 || cd.entry_count() != 0
        || cl.entry_count() != 0 || cp.entry_count() != 0
        || ct.entry_count() != 0) {
        std::cerr << "FAIL [" << pass_label << "]: post-clear entry counts not 0; "
                  << "cr=" << cr.entry_count()
                  << " cd=" << cd.entry_count()
                  << " cl=" << cl.entry_count()
                  << " cp=" << cp.entry_count()
                  << " ct=" << ct.entry_count() << "\n";
        return 1;
    }

    // Snapshot eviction_count deltas from this point.
    const auto e0_r = cr.eviction_count();
    const auto e0_d = cd.eviction_count();
    const auto e0_l = cl.eviction_count();
    const auto e0_p = cp.eviction_count();
    const auto e0_t = ct.eviction_count();

    if (expect_evicted) {
        // Build a minimal PolyCtx and use it to construct values for the
        // 5 caches. The cache stores values by std::shared_ptr<const T>;
        // we only need a movable instance per ValueT.
        hf::PolyCtx ctx({"x"});
        hf::Poly zero_poly = hf::Poly::zero_of(ctx);
        // Construct a minimal Rat (= 0). We pass through the implicit
        // Rat(Poly) ctor (rat.hpp:29). No FLINT computation beyond that.

        // ---- §1 rat_add cache: insert one entry with arbitrary key/hash.
        //
        // Hash is the unordered_map key inside the cache's shard; the
        // KeyT is verified on lookup (we never look up). Keys are
        // constructed by direct field-init to keep the test independent
        // of the make_*_key factory implementations.
        cr.insert(cs::RatAddKey{0xDEADBEEFULL, 0xCAFEBABEULL},
                  /*hash=*/0xAAAA000000000001ULL,
                  hf::Rat(zero_poly));

        // ---- §2 reduce cache: use reduce_insert_with_kind helper to
        //       avoid exposing the private ReduceValue type.
        om::reduce_insert_with_kind(
            cs::ReduceKey{0x0000DEADULL, 0x0000BEEFULL},
            /*hash=*/0xAAAA000000000002ULL,
            zero_poly, zero_poly, /*kind=*/0);

        // ---- §3 lf cache: default-construct LinearFactorization.
        cl.insert(cs::LfKey{0xDEADULL, /*var_idx=*/0,
                             /*zw_ptr=*/nullptr,
                             /*intro=*/false,
                             /*compute_constant=*/false},
                  /*hash=*/0xAAAA000000000003ULL,
                  hf::LinearFactorization{});

        // ---- §4 pf cache: PartialFractionization needs a Rat
        //       polynomial_part (no default ctor on Rat).
        cp.insert(cs::PfKey{0xDEADULL, /*var_idx=*/0,
                             /*zw_ptr=*/nullptr,
                             /*intro=*/false},
                  /*hash=*/0xAAAA000000000004ULL,
                  hf::PartialFractionization{hf::Rat(zero_poly), {}});

        // ---- §5 transform_shuffle cache: empty TransformResultSym.
        ct.insert(cs::TransformShuffleKey{0xDEADULL, /*var_idx=*/0,
                                           /*ctx_fp=*/0,
                                           /*zw_ptr=*/nullptr,
                                           /*intro=*/false,
                                           /*wordlist_size=*/0},
                  /*hash=*/0xAAAA000000000005ULL,
                  hf::TransformResultSym{});

        // Verify each cache picked up exactly one entry.
        ASSERT_EQ("rat_add insert", cr.entry_count(), 1u);
        ASSERT_EQ("reduce  insert", cd.entry_count(), 1u);
        ASSERT_EQ("lf      insert", cl.entry_count(), 1u);
        ASSERT_EQ("pf      insert", cp.entry_count(), 1u);
        ASSERT_EQ("xform   insert", ct.entry_count(), 1u);
    }

    // Call the dispatch. With `n_per_cache=1`, evict_lru_batch's
    // round-robin-per-shard rule (operator_memo.hpp:565-617) computes
    // n_per_shard = ceil(1/32) = 1; each non-empty shard evicts 1.
    // With one entry per cache and 32 shards, exactly one shard per
    // cache is non-empty → each cache evicts 1; total = 5.
    const std::size_t total = om::evict_lru_batch_all_caches(/*n_per_cache=*/1);

    if (expect_evicted) {
        if (total != 5) {
            std::cerr << "FAIL [" << pass_label
                      << "]: evict_lru_batch_all_caches(1) returned " << total
                      << ", expected 5 (1 per cache × 5 caches)\n";
            return 1;
        }
        // Per-cache delta: each was reached, each evicted 1.
        ASSERT_DELTA_EQ("rat_add", e0_r, cr.eviction_count(), 1u);
        ASSERT_DELTA_EQ("reduce",  e0_d, cd.eviction_count(), 1u);
        ASSERT_DELTA_EQ("lf",      e0_l, cl.eviction_count(), 1u);
        ASSERT_DELTA_EQ("pf",      e0_p, cp.eviction_count(), 1u);
        ASSERT_DELTA_EQ("xform",   e0_t, ct.eviction_count(), 1u);
        // Each cache is now empty.
        ASSERT_EQ("rat_add post-evict", cr.entry_count(), 0u);
        ASSERT_EQ("reduce  post-evict", cd.entry_count(), 0u);
        ASSERT_EQ("lf      post-evict", cl.entry_count(), 0u);
        ASSERT_EQ("pf      post-evict", cp.entry_count(), 0u);
        ASSERT_EQ("xform   post-evict", ct.entry_count(), 0u);
    } else {
        // Cold-start on empty caches: dispatch must return 0 and bump
        // no counters.
        if (total != 0) {
            std::cerr << "FAIL [" << pass_label
                      << "]: dispatch on empty caches returned " << total
                      << ", expected 0\n";
            return 1;
        }
        ASSERT_DELTA_EQ("rat_add empty", e0_r, cr.eviction_count(), 0u);
        ASSERT_DELTA_EQ("reduce  empty", e0_d, cd.eviction_count(), 0u);
        ASSERT_DELTA_EQ("lf      empty", e0_l, cl.eviction_count(), 0u);
        ASSERT_DELTA_EQ("pf      empty", e0_p, cp.eviction_count(), 0u);
        ASSERT_DELTA_EQ("xform   empty", e0_t, ct.eviction_count(), 0u);
    }

    std::cout << "[PASS] " << pass_label << "\n";
    return 0;
}

int main() {
    int rc = 0;

    // Pass 1: populated. All 5 caches reached; each evicts exactly 1.
    rc |= run_pass("evict_dispatch_wiring/populated", /*expect_evicted=*/true);

    // Pass 2: warm-but-empty. Caches have just been cleared & evicted;
    // a second dispatch should be a no-op (returns 0; no counter bump).
    rc |= run_pass("evict_dispatch_wiring/empty",     /*expect_evicted=*/false);

    if (rc == 0) {
        std::cout << "[OK] hyperflint-test-evict-dispatch-wiring "
                     "— all subtests PASS\n";
    } else {
        std::cerr << "[FAIL] hyperflint-test-evict-dispatch-wiring "
                     "— one or more subtests FAILED\n";
    }
    return rc;
}
