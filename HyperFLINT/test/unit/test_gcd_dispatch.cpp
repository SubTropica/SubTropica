// Phase 0.5 Item U Stage 2: unit tests for dispatch_parallel_for +
// slot resolver.

#include "hyperflint/integrator/gcd_dispatch.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    using namespace hyperflint::integrator;

    // -----------------------------------------------------------------------
    // Test 1: dispatch_parallel_for runs every iteration exactly once.
    // -----------------------------------------------------------------------
    {
        std::atomic<int> count{0};
        dispatch_parallel_for(0, 100, 16, [&](size_t /*i*/, int /*slot*/) {
            count.fetch_add(1, std::memory_order_relaxed);
        });
        assert(count.load() == 100);
        std::printf("Test 1 PASS: 100 iterations counted.\n");
    }

    // -----------------------------------------------------------------------
    // Test 2: slots are in [0, max_slots).
    // -----------------------------------------------------------------------
    {
        std::atomic<int> max_slot{-1};
        dispatch_parallel_for(0, 1000, 16, [&](size_t /*i*/, int slot) {
            assert(slot >= 0 && slot < 16);
            int prev = max_slot.load();
            while (slot > prev &&
                   !max_slot.compare_exchange_weak(prev, slot,
                       std::memory_order_relaxed, std::memory_order_relaxed)) {}
        });
        std::printf("Test 2 PASS: max_slot observed = %d (< 16).\n",
                    max_slot.load());
    }

    // -----------------------------------------------------------------------
    // Test 3: empty range is a no-op.
    // -----------------------------------------------------------------------
    {
        std::atomic<int> count{0};
        dispatch_parallel_for(5, 5, 4, [&](size_t /*i*/, int /*slot*/) {
            count.fetch_add(1, std::memory_order_relaxed);
        });
        assert(count.load() == 0);
        std::printf("Test 3 PASS: empty range is a no-op.\n");
    }

    // -----------------------------------------------------------------------
    // Test 4: begin-offset is respected (entry_i values span [begin, end)).
    // -----------------------------------------------------------------------
    {
        std::atomic<int> below_begin{0};
        std::atomic<int> above_end{0};
        constexpr size_t BEGIN = 10, END = 50;
        dispatch_parallel_for(BEGIN, END, 16, [&](size_t i, int /*slot*/) {
            if (i < BEGIN) below_begin.fetch_add(1, std::memory_order_relaxed);
            if (i >= END)  above_end.fetch_add(1, std::memory_order_relaxed);
        });
        assert(below_begin.load() == 0);
        assert(above_end.load() == 0);
        std::printf("Test 4 PASS: begin-offset respected.\n");
    }

    // -----------------------------------------------------------------------
    // Test 5: gcd_dispatch_available() returns a bool.
    // -----------------------------------------------------------------------
    {
        bool avail = gcd_dispatch_available();
#ifdef __APPLE__
        assert(avail == true);
        std::printf("Test 5 PASS: gcd_dispatch_available() = true (Apple).\n");
#else
        assert(avail == false);
        std::printf("Test 5 PASS: gcd_dispatch_available() = false (non-Apple).\n");
#endif
        (void)avail;
    }

    std::printf("test_gcd_dispatch: ALL PASS\n");
    return 0;
}
