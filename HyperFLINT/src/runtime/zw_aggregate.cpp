// HF C-prep.1 (iter-22 amendment §3.2; iter-34 plumbing): process-
// global at-exit emitter for ZWTable accounting. See header for the
// full design rationale.

#include "hyperflint/runtime/zw_aggregate.hpp"

#include <atomic>
#include <cstdio>

namespace hyperflint {
namespace detail {

namespace {

// Process-global atomic accumulators. Updated on ZWTable destruction
// via zw_table_flush_to_aggregate; read at process exit by
// ~ZwAggregateAtExit.  Relaxed memory order is sufficient because
// every contributing thread must have joined before the static dtor
// runs (C++ exit-time destruction is single-threaded by spec).
std::atomic<std::uint64_t> g_zw_intern_regular{0};
std::atomic<std::uint64_t> g_zw_intern_opaque{0};
std::atomic<std::uint64_t> g_zw_would_have_been_opaque{0};
std::atomic<std::uint64_t> g_zw_distinct{0};

struct ZwAggregateAtExit {
    ~ZwAggregateAtExit() {
        std::fprintf(stderr,
            "hf_zw_aggregate: total_intern_regular=%llu "
            "total_intern_opaque=%llu "
            "total_would_have_been_opaque=%llu "
            "total_distinct=%llu\n",
            static_cast<unsigned long long>(
                g_zw_intern_regular.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_zw_intern_opaque.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_zw_would_have_been_opaque.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_zw_distinct.load(std::memory_order_relaxed)));
        std::fflush(stderr);
    }
};

// Static instance: its destructor fires at process exit.
ZwAggregateAtExit g_zw_atexit_emitter;

}  // namespace

void zw_table_flush_to_aggregate(
    std::size_t intern_regular,
    std::size_t intern_opaque,
    std::size_t would_have_been_opaque,
    std::size_t distinct_entries) {
    g_zw_intern_regular.fetch_add(
        static_cast<std::uint64_t>(intern_regular),
        std::memory_order_relaxed);
    g_zw_intern_opaque.fetch_add(
        static_cast<std::uint64_t>(intern_opaque),
        std::memory_order_relaxed);
    g_zw_would_have_been_opaque.fetch_add(
        static_cast<std::uint64_t>(would_have_been_opaque),
        std::memory_order_relaxed);
    g_zw_distinct.fetch_add(
        static_cast<std::uint64_t>(distinct_entries),
        std::memory_order_relaxed);
}

void zw_aggregate_peek(
    std::uint64_t* intern_regular,
    std::uint64_t* intern_opaque,
    std::uint64_t* would_have_been_opaque,
    std::uint64_t* distinct_entries) {
    if (intern_regular) {
        *intern_regular =
            g_zw_intern_regular.load(std::memory_order_relaxed);
    }
    if (intern_opaque) {
        *intern_opaque =
            g_zw_intern_opaque.load(std::memory_order_relaxed);
    }
    if (would_have_been_opaque) {
        *would_have_been_opaque =
            g_zw_would_have_been_opaque.load(std::memory_order_relaxed);
    }
    if (distinct_entries) {
        *distinct_entries =
            g_zw_distinct.load(std::memory_order_relaxed);
    }
}

}  // namespace detail
}  // namespace hyperflint
