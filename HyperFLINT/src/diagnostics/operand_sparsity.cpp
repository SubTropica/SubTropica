// operand_sparsity — implementation.  Header
// `hyperflint/diagnostics/operand_sparsity.hpp` documents the design
// rationale; this TU is the standard env-gate + atomic-counter +
// mutex-guarded-stderr-print pattern (mirrors HF_REGKEY_DUMP at
// integration_step.cpp ~2076 and HF_DUMP_RAT_QUADS at
// core/rat.cpp ~406).

#include "hyperflint/diagnostics/operand_sparsity.hpp"

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/diagnostics/env_flags.hpp"

#include <flint/fmpq_mpoly.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace hyperflint {

namespace {

// Process-global monotonic counter.  Incremented by every call to
// `sparsity_probe_should_emit()`, regardless of whether the gate is
// on.  Reading from `should_emit` only — no other call site reads
// or writes it.  Relaxed memory order is sufficient: the counter is
// observational; threads do not coordinate via its value.
std::atomic<uint64_t> g_sparsity_call_counter{0};

}  // namespace

bool sparsity_probe_enabled() {
    // Static-local once-init: cached at first call; thread-safe under
    // C++11 magic statics.  Default-off requires HF_DUMP_OPERAND_SPARSITY
    // to be set and != "0" (mirrors `tolerance_enabled()` in
    // narrow_ctx_flag.cpp).
    static const bool cached = []() {
        const char* v = HF_FLAG_DUMP_OPERAND_SPARSITY;
        return v && v[0] && v[0] != '0';
    }();
    return cached;
}

size_t sparsity_probe_sample_rate() {
    static const size_t cached = []() -> size_t {
        const char* v = HF_FLAG_OPERAND_SPARSITY_RATE;
        if (!v || !v[0]) return 1000;
        // strtoul: tolerate leading whitespace and a trailing
        // non-numeric suffix; clamp to >=1 so the modulus below
        // never divides by zero.
        char* end = nullptr;
        unsigned long parsed = std::strtoul(v, &end, 10);
        if (end == v || parsed == 0UL) return 1000;
        return static_cast<size_t>(parsed);
    }();
    return cached;
}

bool sparsity_probe_should_emit() {
    const uint64_t n = g_sparsity_call_counter.fetch_add(
        1, std::memory_order_relaxed) + 1;
    const size_t rate = sparsity_probe_sample_rate();
    return (n % rate) == 0;
}

PolyStats compute_sparsity(const Poly& p) {
    PolyStats out{0, 0, 0, 0.0};
    const fmpq_mpoly_struct* raw = p.raw();
    fmpq_mpoly_ctx_struct* ctx_raw = p.ctx().raw();
    const slong n = fmpq_mpoly_length(
        const_cast<fmpq_mpoly_struct*>(raw), ctx_raw);
    if (n <= 0) return out;
    const size_t n_vars = p.ctx().vars().size();
    out.n_terms = static_cast<size_t>(n);
    if (n_vars == 0) {
        // Degenerate ctx (no variables) — every term has k = 0.
        out.k_min = out.k_max = 0;
        out.k_avg = 0.0;
        return out;
    }
    std::vector<slong> exp_buf(n_vars);
    size_t k_min = SIZE_MAX;
    size_t k_max = 0;
    uint64_t k_sum = 0;
    for (slong i = 0; i < n; ++i) {
        fmpq_mpoly_get_term_exp_si(exp_buf.data(),
            const_cast<fmpq_mpoly_struct*>(raw),
            i, ctx_raw);
        size_t k = 0;
        for (size_t j = 0; j < n_vars; ++j) {
            if (exp_buf[j] != 0) ++k;
        }
        if (k < k_min) k_min = k;
        if (k > k_max) k_max = k;
        k_sum += k;
    }
    out.k_min = (k_min == SIZE_MAX) ? 0 : k_min;
    out.k_max = k_max;
    out.k_avg = static_cast<double>(k_sum) / static_cast<double>(n);
    return out;
}

void emit_sparsity_row(const Rat& a, const Rat& b) {
    // Re-read the post-increment counter (the value should_emit just
    // returned true for); use the current value as a representative
    // call ordinal.  Subject to a mild race when two threads emit
    // concurrently — the printed `call` value is approximate (within
    // (n_threads) of the actual emit ordinal) but the row itself is
    // serialized intact under the mutex below.
    const uint64_t call = g_sparsity_call_counter.load(
        std::memory_order_relaxed);
    const PolyStats a_n = compute_sparsity(a.num());
    const PolyStats a_d = compute_sparsity(a.den());
    const PolyStats b_n = compute_sparsity(b.num());
    const PolyStats b_d = compute_sparsity(b.den());
    const size_t n_vars = a.num().ctx().vars().size();

    // Format under a process-wide mutex so concurrent emits from the
    // OMP region don't interleave their bytes mid-line.  The mutex
    // is held only for the formatting + write — no FLINT ops inside.
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);

    // Build the JSONL row in a stack buffer to keep the mutex hold
    // time short.  Worst-case length is bounded: ~16 fields × ~30
    // chars + per-double formatting ~24 chars + envelope ~64 chars,
    // ~700 bytes, well under 4 KiB.
    char buf[2048];
    int len = std::snprintf(buf, sizeof(buf),
        "{\"hf_sparsity\":true"
        ",\"call\":%llu"
        ",\"n_vars\":%zu"
        ",\"a_n_terms\":%zu,\"a_n_k_min\":%zu,\"a_n_k_avg\":%.4f,\"a_n_k_max\":%zu"
        ",\"a_d_n_terms\":%zu,\"a_d_k_min\":%zu,\"a_d_k_avg\":%.4f,\"a_d_k_max\":%zu"
        ",\"b_n_terms\":%zu,\"b_n_k_min\":%zu,\"b_n_k_avg\":%.4f,\"b_n_k_max\":%zu"
        ",\"b_d_n_terms\":%zu,\"b_d_k_min\":%zu,\"b_d_k_avg\":%.4f,\"b_d_k_max\":%zu"
        "}\n",
        static_cast<unsigned long long>(call),
        n_vars,
        a_n.n_terms, a_n.k_min, a_n.k_avg, a_n.k_max,
        a_d.n_terms, a_d.k_min, a_d.k_avg, a_d.k_max,
        b_n.n_terms, b_n.k_min, b_n.k_avg, b_n.k_max,
        b_d.n_terms, b_d.k_min, b_d.k_avg, b_d.k_max);
    if (len > 0) {
        std::fwrite(buf, 1,
            (static_cast<size_t>(len) < sizeof(buf)) ? len : sizeof(buf) - 1,
            stderr);
    }
}

}  // namespace hyperflint
