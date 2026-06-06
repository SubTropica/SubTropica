// Phase 0 task 0-4 (2026-05-09): unit test for IntegrationNodeRssSampler.
//
// Test plan:
//   Test 1. Basic: 16 MiB allocation detected by single enter/exit.
//   Test 2. Nested enter/exit: regression for Bug C1 (exit_node was patching
//           records_.back() instead of the record at the stored index, so the
//           outer record's t_wall_s/rss_peak_kib_delta remained zero).
//   Test 3. Disabled state: HF_INTEG_NODE_RSS unset → no records created.
//   Test 4. Below-threshold: depth < threshold → entry is a no-op.
//
// Style: plain <cassert>, no external framework.  Consistent with other
// unit tests in this directory (test_step_trace_rss.cpp etc.).

#include "hyperflint/diagnostics/integration_node_rss.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>

using hyperflint::IntegrationNodeRssSampler;

int main() {
    // -------------------------------------------------------------------
    // Test 1: basic — 16 MiB allocation detected by single enter/exit.
    // -------------------------------------------------------------------
    {
        // Enable the sampler at depth threshold 1.
        // Use overwrite=1 in case the process environment already has
        // HF_INTEG_NODE_RSS set to something else.
        setenv("HF_INTEG_NODE_RSS", "1", /*overwrite=*/1);

        auto& sampler = IntegrationNodeRssSampler::instance();
        sampler.reset();  // picks up the new env var

        assert(sampler.enabled());
        assert(sampler.depth_threshold() == 1);

        // Enter an integration node at depth=2, letter=0.
        sampler.enter_node(/*depth=*/2, /*letter_id=*/0);

        // Allocate 16 MiB and touch every page so the OS maps it into
        // physical memory (page-fault it in).
        constexpr size_t kBytes = 16UL * 1024 * 1024;
        auto* big = new char[kBytes];
        for (size_t i = 0; i < kBytes; i += 4096) big[i] = 1;

        sampler.exit_node(/*depth=*/2);

        const auto& records = sampler.records();

        // Diagnostics: print before asserting so failures show actual values.
        printf("[Test 1] records.size()           = %zu\n", records.size());
        if (!records.empty()) {
            printf("[Test 1] records[0].depth         = %d\n",   records[0].depth);
            printf("[Test 1] records[0].letter_id     = %d\n",   records[0].letter_id);
            printf("[Test 1] records[0].t_wall_s      = %.6f\n", records[0].t_wall_s);
            printf("[Test 1] records[0].rss_cur_kib   = %lld\n",
                   static_cast<long long>(records[0].rss_current_kib));
            printf("[Test 1] records[0].rss_peak_del  = %lld\n",
                   static_cast<long long>(records[0].rss_peak_kib_delta));
        }

        assert(records.size() == 1);
        assert(records[0].depth == 2);
        assert(records[0].letter_id == 0);
        assert(records[0].rss_peak_kib_delta >= static_cast<int64_t>(16 * 1024));

        delete[] big;
        printf("[Test 1] PASS\n");
    }

    // -------------------------------------------------------------------
    // Test 2: nested enter/exit — regression for Bug C1.
    //
    // Before the fix, exit_node used records_.back() instead of the
    // record stored at enter time.  Under enter A → enter B → exit B →
    // exit A, exit-A patched B's record a second time and A's record
    // retained its zero sentinel values.
    // -------------------------------------------------------------------
    {
        setenv("HF_INTEG_NODE_RSS", "1", 1);
        auto& sampler = IntegrationNodeRssSampler::instance();
        sampler.reset();

        // Outer enter at depth 2
        sampler.enter_node(/*depth=*/2, /*letter_id=*/10);
        auto* outer_alloc = new char[8 * 1024 * 1024];  // 8 MiB
        for (size_t i = 0; i < 8 * 1024 * 1024; i += 4096) outer_alloc[i] = 1;

        // Inner enter at depth 3
        sampler.enter_node(/*depth=*/3, /*letter_id=*/20);
        auto* inner_alloc = new char[4 * 1024 * 1024];  // 4 MiB
        for (size_t i = 0; i < 4 * 1024 * 1024; i += 4096) inner_alloc[i] = 1;
        sampler.exit_node(/*depth=*/3);

        sampler.exit_node(/*depth=*/2);

        const auto records = sampler.records();

        printf("[Test 2] records.size()                  = %zu\n", records.size());
        if (records.size() >= 1) {
            printf("[Test 2] records[0].depth                = %d\n",   records[0].depth);
            printf("[Test 2] records[0].letter_id            = %d\n",   records[0].letter_id);
            printf("[Test 2] records[0].t_wall_s             = %.6f\n", records[0].t_wall_s);
            printf("[Test 2] records[0].rss_peak_kib_delta   = %lld\n",
                   static_cast<long long>(records[0].rss_peak_kib_delta));
        }
        if (records.size() >= 2) {
            printf("[Test 2] records[1].depth                = %d\n",   records[1].depth);
            printf("[Test 2] records[1].letter_id            = %d\n",   records[1].letter_id);
            printf("[Test 2] records[1].t_wall_s             = %.6f\n", records[1].t_wall_s);
            printf("[Test 2] records[1].rss_peak_kib_delta   = %lld\n",
                   static_cast<long long>(records[1].rss_peak_kib_delta));
        }

        assert(records.size() == 2);

        // Outer record (depth 2) pushed first → records[0]
        assert(records[0].depth == 2);
        assert(records[0].letter_id == 10);
        // Inner record (depth 3) → records[1]
        assert(records[1].depth == 3);
        assert(records[1].letter_id == 20);

        // CRITICAL regression check for Bug C1: outer record must be filled.
        // (Before the fix, t_wall_s stayed 0.0 and rss_peak_kib_delta stayed 0.)
        assert(records[0].t_wall_s > 0.0);
        assert(records[0].rss_peak_kib_delta >= static_cast<int64_t>(4 * 1024));

        // Inner record also correctly filled.
        assert(records[1].t_wall_s > 0.0);
        // rss_peak_kib_delta for the inner may be 0 if the peak was already
        // set during the outer allocation (monotone peak); >= 0 is the contract.
        assert(records[1].rss_peak_kib_delta >= 0);

        delete[] inner_alloc;
        delete[] outer_alloc;
        printf("[Test 2] PASS\n");
    }

    // -------------------------------------------------------------------
    // Test 3: disabled state — HF_INTEG_NODE_RSS unset → no records.
    // -------------------------------------------------------------------
    {
        unsetenv("HF_INTEG_NODE_RSS");
        auto& sampler = IntegrationNodeRssSampler::instance();
        sampler.reset();

        assert(!sampler.enabled());

        sampler.enter_node(/*depth=*/2, /*letter_id=*/0);
        sampler.exit_node(/*depth=*/2);

        assert(sampler.records().empty());
        printf("[Test 3] PASS\n");
    }

    // -------------------------------------------------------------------
    // Test 4: below-threshold — depth < threshold → entry is a no-op.
    // -------------------------------------------------------------------
    {
        setenv("HF_INTEG_NODE_RSS", "3", 1);  // threshold = 3
        auto& sampler = IntegrationNodeRssSampler::instance();
        sampler.reset();

        assert(sampler.enabled());
        assert(sampler.depth_threshold() == 3);

        sampler.enter_node(/*depth=*/2, /*letter_id=*/0);  // depth=2 < threshold=3 → skipped
        sampler.exit_node(/*depth=*/2);

        assert(sampler.records().empty());
        printf("[Test 4] PASS\n");
    }

    printf("All 4 tests PASSED.\n");
    return 0;
}
