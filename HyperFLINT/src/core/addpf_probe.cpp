// HF_ADDPF_PROBE implementation -- see include/hyperflint/core/addpf_probe.hpp.

#include "hyperflint/core/addpf_probe.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>

namespace hyperflint {
namespace addpf_probe {

namespace {

// Wall times accumulate as integer nanoseconds (atomic<double> has no
// fetch_add in C++17).
std::atomic<long long> g_same_den_n{0};
std::atomic<long long> g_same_den_ns{0};
std::atomic<long long> g_diff_den_n{0};
std::atomic<long long> g_diff_den_ns{0};
std::atomic<std::size_t> g_max_addend_den_terms{0};
std::atomic<std::size_t> g_max_fused_den_terms{0};

std::atomic<long long> g_keys_total{0};
std::atomic<long long> g_keys_multi{0};      // keys with >= 2 chunks
std::atomic<long long> g_chunks_sum{0};
std::atomic<std::size_t> g_chunks_max{0};
std::atomic<std::size_t> g_max_fused_num_terms{0};

std::atomic<long long> g_pf_calls{0};
std::atomic<long long> g_pf_nonzero_polypart{0};

// bump-layer extension (2026-06-04)
std::atomic<long long> g_bump_same_n{0};
std::atomic<long long> g_bump_same_ns{0};
std::atomic<long long> g_bump_diff_n{0};
std::atomic<long long> g_bump_diff_ns{0};
std::atomic<long long> g_rows_total{0};
std::atomic<long long> g_rows_multi_den{0};
std::atomic<long long> g_row_groups_sum{0};
std::atomic<std::size_t> g_row_groups_max{0};
std::atomic<long long> g_rows_capped{0};
std::atomic<std::size_t> g_user_var_count{0};
std::atomic<long long> g_mon_total{0};
std::atomic<long long> g_mon_with_period{0};

void atomic_max(std::atomic<std::size_t>& slot, std::size_t v) {
    std::size_t cur = slot.load(std::memory_order_relaxed);
    while (cur < v &&
           !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("HF_ADDPF_PROBE");
        return e && e[0] == '1';
    }();
    return on;
}

void record_merge_add(bool same_den, double seconds,
                      std::size_t den_a_terms, std::size_t den_b_terms,
                      std::size_t fused_den_terms) {
    const long long ns = static_cast<long long>(seconds * 1e9);
    if (same_den) {
        g_same_den_n.fetch_add(1, std::memory_order_relaxed);
        g_same_den_ns.fetch_add(ns, std::memory_order_relaxed);
    } else {
        g_diff_den_n.fetch_add(1, std::memory_order_relaxed);
        g_diff_den_ns.fetch_add(ns, std::memory_order_relaxed);
    }
    atomic_max(g_max_addend_den_terms,
               den_a_terms > den_b_terms ? den_a_terms : den_b_terms);
    atomic_max(g_max_fused_den_terms, fused_den_terms);
}

void record_drain_key(std::size_t n_chunks,
                      std::size_t max_fused_num_terms) {
    g_keys_total.fetch_add(1, std::memory_order_relaxed);
    if (n_chunks >= 2)
        g_keys_multi.fetch_add(1, std::memory_order_relaxed);
    g_chunks_sum.fetch_add(static_cast<long long>(n_chunks),
                           std::memory_order_relaxed);
    atomic_max(g_chunks_max, n_chunks);
    atomic_max(g_max_fused_num_terms, max_fused_num_terms);
}

void record_pf_call(bool nonzero_poly_part) {
    g_pf_calls.fetch_add(1, std::memory_order_relaxed);
    if (nonzero_poly_part)
        g_pf_nonzero_polypart.fetch_add(1, std::memory_order_relaxed);
}

void record_bump_add(bool same_den, double seconds) {
    const long long ns = static_cast<long long>(seconds * 1e9);
    if (same_den) {
        g_bump_same_n.fetch_add(1, std::memory_order_relaxed);
        g_bump_same_ns.fetch_add(ns, std::memory_order_relaxed);
    } else {
        g_bump_diff_n.fetch_add(1, std::memory_order_relaxed);
        g_bump_diff_ns.fetch_add(ns, std::memory_order_relaxed);
    }
}

void record_bump_row(std::size_t n_groups, bool capped) {
    g_rows_total.fetch_add(1, std::memory_order_relaxed);
    if (n_groups >= 2)
        g_rows_multi_den.fetch_add(1, std::memory_order_relaxed);
    g_row_groups_sum.fetch_add(static_cast<long long>(n_groups),
                               std::memory_order_relaxed);
    atomic_max(g_row_groups_max, n_groups);
    if (capped)
        g_rows_capped.fetch_add(1, std::memory_order_relaxed);
}

void set_user_var_count(std::size_t n) {
    g_user_var_count.store(n, std::memory_order_relaxed);
}
std::size_t user_var_count() {
    return g_user_var_count.load(std::memory_order_relaxed);
}
void record_monomial(bool uses_period_var) {
    g_mon_total.fetch_add(1, std::memory_order_relaxed);
    if (uses_period_var)
        g_mon_with_period.fetch_add(1, std::memory_order_relaxed);
}

void emit_and_reset(const char* var_name) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    std::cerr << "{\"hf_addpf_probe\":true"
              << ",\"var\":\"" << (var_name ? var_name : "?") << "\""
              << ",\"same_den_n\":" << g_same_den_n.exchange(0)
              << ",\"same_den_s\":"
              << static_cast<double>(g_same_den_ns.exchange(0)) / 1e9
              << ",\"diff_den_n\":" << g_diff_den_n.exchange(0)
              << ",\"diff_den_s\":"
              << static_cast<double>(g_diff_den_ns.exchange(0)) / 1e9
              << ",\"max_addend_den_terms\":"
              << g_max_addend_den_terms.exchange(0)
              << ",\"max_fused_den_terms\":"
              << g_max_fused_den_terms.exchange(0)
              << ",\"keys_total\":" << g_keys_total.exchange(0)
              << ",\"keys_multi\":" << g_keys_multi.exchange(0)
              << ",\"chunks_sum\":" << g_chunks_sum.exchange(0)
              << ",\"chunks_max\":" << g_chunks_max.exchange(0)
              << ",\"max_fused_num_terms\":"
              << g_max_fused_num_terms.exchange(0)
              << ",\"pf_calls\":" << g_pf_calls.exchange(0)
              << ",\"pf_nonzero_polypart\":"
              << g_pf_nonzero_polypart.exchange(0)
              << ",\"bump_same_n\":" << g_bump_same_n.exchange(0)
              << ",\"bump_same_s\":"
              << static_cast<double>(g_bump_same_ns.exchange(0)) / 1e9
              << ",\"bump_diff_n\":" << g_bump_diff_n.exchange(0)
              << ",\"bump_diff_s\":"
              << static_cast<double>(g_bump_diff_ns.exchange(0)) / 1e9
              << ",\"rows_total\":" << g_rows_total.exchange(0)
              << ",\"rows_multi_den\":" << g_rows_multi_den.exchange(0)
              << ",\"row_groups_sum\":" << g_row_groups_sum.exchange(0)
              << ",\"row_groups_max\":" << g_row_groups_max.exchange(0)
              << ",\"rows_capped\":" << g_rows_capped.exchange(0)
              << ",\"mon_total\":" << g_mon_total.exchange(0)
              << ",\"mon_with_period\":" << g_mon_with_period.exchange(0)
              << "}\n";
}

}  // namespace addpf_probe
}  // namespace hyperflint
