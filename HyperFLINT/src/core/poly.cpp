// Poly implementation: arithmetic + factor.
//
// FLINT's fmpq_mpoly_add/sub/mul/neg/pow do the heavy lifting;
// we wrap them so the caller doesn't manage fmpq_mpoly_t lifetimes.

#include "hyperflint/core/poly.hpp"

#include "hyperflint/core/env_flags_poly.hpp"  // iter-96 §T7 26th chunk Track-diagnostic-dump partial core/poly portion (NEW header; iter-76-anticipated subsystem-suffix sibling of core/env_flags_rat.hpp and core/env_flags_reduce.hpp)
#include "hyperflint/diagnostics/env_flags.hpp"
#include "hyperflint/runtime/env_flags.hpp"  // HF_FLAG_MUL_NARROW (iter-69)
#include "hyperflint/runtime/hf_thread_num.hpp"
#include "hyperflint/runtime/trace_gate.hpp"

#include <flint/fmpq.h>
#include <flint/fmpz.h>
#include <flint/mpoly.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unistd.h>
#include <vector>

#ifdef HF_HAVE_OPENMP
#include <omp.h>
#endif

namespace hyperflint {

namespace {

std::string fmpq_to_string(const fmpq_t q) {
    char* s = fmpq_get_str(nullptr, 10, q);
    std::string out(s);
    flint_free(s);
    return out;
}

void assert_same_ctx(const Poly& a, const Poly& b) {
    assert(&a.ctx() == &b.ctx() &&
           "Poly: binary op across different PolyCtx is not supported");
}

// 2026-04-27 Avenue A (3l3pt step-3 polymul attack).  Mirrors the
// narrow-ctx hoist in Poly::gcd. Per-thread call counters and timers
// for the narrow-vs-wide branches inside Poly::mul.  Wired into
// integration_step's step harvest.  Hypothesis under test: 3l3pt
// step-3 has rat_add_polymul_s = 165 s on a 30-var wide ctx; bump-
// internal mults touch ~5-10 vars → narrow-ctx mult is ~2× faster
// at packed-exponent level (4 ulongs → 1-2 ulongs words_per_exp).
// Reviewer recalibrated estimate: 15-80 s saving (median 35 s).
std::vector<double>& mul_narrow_storage()
    { static std::vector<double> v; return v; }
std::vector<double>& mul_wide_storage()
    { static std::vector<double> v; return v; }
std::vector<long>&   mul_narrow_calls_storage()
    { static std::vector<long>   v; return v; }
std::vector<long>&   mul_wide_calls_storage()
    { static std::vector<long>   v; return v; }
// `mul_gated_calls` counts wide-path traversals taken because of the
// size_gate (len_total < kNarrowMulMinLen) — distinct from `mul_wide_calls`
// which counts wide-path traversals where worth_narrowing was false.
// Keeps the diagnostic crisp: gated_calls is "skipped narrow due to small";
// wide_calls is "skipped narrow due to dense / used_count too big".
std::vector<long>&   mul_gated_calls_storage()
    { static std::vector<long>   v; return v; }

// 2026-04-28 reviewer round 7: per-call distribution counters in the
// narrow path so we can disambiguate "tail-driven mean" vs "uniform
// mid-distribution".  Bins by la·lb (log scale, 6 bins) and by
// used_count (5 bins).  For each bin: per-thread counter + cumulative
// narrow_us + max narrow_us.  This is the decisive experiment to set
// the gate threshold.
//
// la*lb log bins:  <1k, <4k, <16k, <64k, <256k, ≥256k
// U bins:          1, 2, 3, 4-7, 8+
constexpr int kNarrowLaLbBins = 6;
constexpr int kNarrowUBins    = 5;
inline int la_lb_bin(slong la, slong lb) {
    const long p = static_cast<long>(la) * static_cast<long>(lb);
    if (p <  1000)   return 0;
    if (p <  4000)   return 1;
    if (p < 16000)   return 2;
    if (p < 64000)   return 3;
    if (p <256000)   return 4;
    return 5;
}
inline int u_bin(size_t U) {
    if (U <= 1) return 0;
    if (U == 2) return 1;
    if (U == 3) return 2;
    if (U <= 7) return 3;
    return 4;
}
// per-thread storage: outer vector indexed by thread, inner vector by bin
std::vector<std::vector<long>>&   nbin_lalb_count_storage()
    { static std::vector<std::vector<long>>   v; return v; }
std::vector<std::vector<double>>& nbin_lalb_us_storage()
    { static std::vector<std::vector<double>> v; return v; }
std::vector<std::vector<double>>& nbin_lalb_max_storage()
    { static std::vector<std::vector<double>> v; return v; }
std::vector<std::vector<long>>&   nbin_u_count_storage()
    { static std::vector<std::vector<long>>   v; return v; }
std::vector<std::vector<double>>& nbin_u_us_storage()
    { static std::vector<std::vector<double>> v; return v; }
}  // namespace

// -------- Basic props --------

bool Poly::is_zero() const {
    return fmpq_mpoly_is_zero(raw_, ctx_->raw()) != 0;
}

bool Poly::is_one() const {
    return fmpq_mpoly_is_one(raw_, ctx_->raw()) != 0;
}

bool Poly::equal(const Poly& other) const {
    if (ctx_ != other.ctx_) return false;
    return fmpq_mpoly_equal(raw_, other.raw_, ctx_->raw()) != 0;
}

bool Poly::leading_coef_is_negative() const {
    // term 0 under FLINT's default monomial order is the leading
    // monomial. Its fmpq coefficient has sign = fmpq_sgn.
    const slong n = fmpq_mpoly_length(raw_, ctx_->raw());
    if (n == 0) return false;   // zero poly -> non-negative
    fmpq_t c; fmpq_init(c);
    fmpq_mpoly_get_term_coeff_fmpq(c,
        const_cast<fmpq_mpoly_struct*>(raw_), 0, ctx_->raw());
    int sgn = fmpq_sgn(c);
    fmpq_clear(c);
    return sgn < 0;
}

Poly Poly::gen(const PolyCtx& ctx, size_t var_idx) {
    Poly r(ctx);
    fmpq_mpoly_gen(r.raw_, static_cast<slong>(var_idx), ctx.raw());
    return r;
}

std::vector<size_t> Poly::used_var_indices() const {
    const size_t nv = ctx_->vars().size();
    if (fmpq_mpoly_is_zero(raw_, ctx_->raw())) return {};
    // FLINT's fmpq_mpoly_used_vars scans the packed exponent words
    // once, O(n_terms · words_per_exp), no per-term unpack. That's
    // ~n_wide_bits/64 times faster than the previous hand-rolled
    // scan that called fmpq_mpoly_get_term_exp_si per term.
    std::vector<int> used(nv, 0);
    fmpq_mpoly_used_vars(used.data(),
        const_cast<fmpq_mpoly_struct*>(raw_), ctx_->raw());
    std::vector<size_t> out;
    out.reserve(nv);
    for (size_t j = 0; j < nv; ++j) if (used[j]) out.push_back(j);
    return out;
}

Poly Poly::transplant(const PolyCtx& dst_ctx,
                      const std::vector<size_t>& src_to_dst_idx,
                      bool skip_precheck) const {
    if (src_to_dst_idx.size() != ctx_->vars().size()) {
        throw std::invalid_argument(
            "Poly::transplant: src_to_dst_idx size does not match src ctx var count");
    }
    Poly dst(dst_ctx);
    const slong n  = fmpq_mpoly_length(raw_, ctx_->raw());
    if (n == 0) return dst;

    const size_t src_nv = ctx_->vars().size();
    const size_t dst_nv = dst_ctx.vars().size();

    // Build an inline list of (src_idx, dst_idx) pairs — only the
    // variables that actually participate in the transplant. Previously
    // this function called fmpq_mpoly_get_term_exp_si, which unpacks
    // all src_nv slots per term. At src_nv=711 (wide ctx) that was
    // ~16% of post-round-7 runtime inside `mpoly_unpack_vec_fmpz` and
    // `mpoly_get_monomial_ui_sp`. Iterating only the mapped positions
    // with fmpq_mpoly_get_term_var_exp_ui per used var is O(used_count)
    // per term instead of O(src_nv).
    // Precheck once per call: every wide var that actually appears in
    // *this must have a mapping, or the transplant loses monomials
    // silently. Done as a single O(n_wide_words) used_vars scan — cheap
    // *per call* but on tst2 the scan was paid 4 × 12.7 M = ~50 M
    // times, the in-side scans on the wide ctx accounting for ~330 s
    // of wall (sub-timer rn_setup_s). Trusted callers — Rat::
    // reduce_inplace, which built `src_to_dst_idx` from its own
    // fmpq_mpoly_used_vars scan moments earlier — pass
    // skip_precheck=true to bypass the redundant scan; that lever
    // shaves -12 % from tst2 wall. See
    // notes/hf_flint_pool_experiment/transplant_levers.md.
    if (!skip_precheck) {
        std::vector<int> used(src_nv, 0);
        fmpq_mpoly_used_vars(used.data(),
            const_cast<fmpq_mpoly_struct*>(raw_), ctx_->raw());
        for (size_t j = 0; j < src_nv; ++j) {
            if (used[j] && src_to_dst_idx[j] == SIZE_MAX) {
                throw std::runtime_error(
                    "Poly::transplant: polynomial uses a source variable "
                    "that has no mapping to the destination context");
            }
        }
    }

    // Note: an experimental lever 3 (HF_TRANSPLANT_USE_COMPOSE) tried
    // to replace the per-term loop below with a single
    // fmpq_mpoly_compose_fmpq_mpoly_gen call. Result: +3× rn_setup_s
    // wall on tst2 (333 s → 1052 s aggregate). FLINT's compose is a
    // general-purpose polynomial substitution primitive (Horner-style
    // internally); our transplant is the much simpler "rename
    // variables" special case, and the hand-tuned per-term loop here —
    // O(used_count) per term instead of O(src_nv) — beats compose by
    // ~3× on this workload shape. Don't switch.

    std::vector<std::pair<slong, size_t>> mapped_pairs;
    mapped_pairs.reserve(src_nv);
    for (size_t j = 0; j < src_nv; ++j) {
        if (src_to_dst_idx[j] != SIZE_MAX) {
            mapped_pairs.emplace_back(static_cast<slong>(j),
                                      src_to_dst_idx[j]);
        }
    }

    std::vector<ulong> dst_exps(dst_nv);
    fmpq_t c; fmpq_init(c);

    for (slong i = 0; i < n; ++i) {
        fmpq_mpoly_get_term_coeff_fmpq(c,
            const_cast<fmpq_mpoly_struct*>(raw_), i, ctx_->raw());

        std::fill(dst_exps.begin(), dst_exps.end(), 0UL);
        for (const auto& [src_j, dst_j] : mapped_pairs) {
            const ulong e = fmpq_mpoly_get_term_var_exp_ui(
                const_cast<fmpq_mpoly_struct*>(raw_), i, src_j,
                ctx_->raw());
            if (e != 0) dst_exps[dst_j] = e;
        }

        // Correctness guard: if an unmapped source variable has a
        // nonzero exponent we must refuse. Check by computing total
        // exponent on dst_exps and comparing to src total via a
        // single fmpq_mpoly_get_term_exp_si — deferred as a slow-path
        // validator, not paid in the fast loop.

        fmpq_mpoly_push_term_fmpq_ui(dst.raw_, c, dst_exps.data(),
                                      dst_ctx.raw());
    }
    fmpq_clear(c);
    fmpq_mpoly_sort_terms(dst.raw_, dst_ctx.raw());
    fmpq_mpoly_combine_like_terms(dst.raw_, dst_ctx.raw());
    return dst;
}

// -------- Arithmetic --------

Poly Poly::add(const Poly& b) const {
    assert_same_ctx(*this, b);
    Poly r(*ctx_);
    fmpq_mpoly_add(r.raw_, raw_, b.raw_, ctx_->raw());
    return r;
}

Poly Poly::sub(const Poly& b) const {
    assert_same_ctx(*this, b);
    Poly r(*ctx_);
    fmpq_mpoly_sub(r.raw_, raw_, b.raw_, ctx_->raw());
    return r;
}

// 2026-04-27 Avenue A: narrow-ctx hoist.  Mirrors Poly::gcd.
// fmpq_mpoly_mul cost depends on words_per_exp = ceil(nvars*bits/64).
// Wide ctx in 3l3pt step-3 has ~30 vars × 8 bits/exp = 240 bits =
// 4 ulongs; narrow ctx with ~5-10 used vars compresses to 1-2 ulongs
// → ~2× speedup on the mult itself, before counting transplant cost.
// Size_gate at len_total < kNarrowMulMinLen guards against transplant
// loss for tiny polys (mirroring the gcd gate).
//
// Default OFF.  Smoke benchmark on tst1 with the gate at 4 showed
// +27 % wall regression: tst1 polys are dominated by 4-10-monomial
// shuffle micro-products where the transplant round-trip outweighs
// the narrow-ctx mult savings even at 711-var wide ctx.  Env-gate
// `HF_MUL_NARROW=1` so we can opt in for 3l3pt-class fixtures (where
// the bump-internal accumulated polys are big enough to amortize)
// without regressing the Smirnov / Log-shuffle traffic on by default.
constexpr slong kNarrowMulMinLen = 4;

// 2026-04-28 reviewer round 8 gate refinement.  Production-call
// histogram on 3l3pt ord_2 step 3 (42,497 narrow calls captured via
// nbin_lalb_count) showed:
//   - 73% of calls at la·lb < 1k take only 6% of narrow time
//     (transplant overhead dominates per-call ~150us);
//   - 4% of calls at la·lb ≥ 256k take 62% of narrow time
//     (per-call ~26ms — pure tail);
//   - la·lb ≥ 16k captures 89% of narrow time with 17% of calls.
// Ship gate `la*lb ≥ 16000 AND min(la,lb) ≥ 4`: keeps the bulk of
// step-3 win, skips the small-call regime where v1 microbench
// confirmed narrow loses 4-6× to wide AND which is the source of
// tst1's prior +27% wall regression.  min(la,lb) ≥ 4 closes the
// pathological 1×N escape (round 4 concern) at zero cost.
constexpr long  kNarrowMulMinLaLb     = 16000;
constexpr slong kNarrowMulMinSideLen  = 4;

namespace {
inline bool mul_narrow_enabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* s = HF_FLAG_MUL_NARROW;
        cached = (s && s[0] && s[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

// HF_DUMP_MUL=path: one-shot diagnostic.  When set, Poly::mul calls in
// the window [HF_DUMP_MUL_SKIP, HF_DUMP_MUL_SKIP + HF_DUMP_MUL_LIMIT)
// run BOTH the wide and narrow paths on the same inputs, time each,
// and write a CSV row to `path.<pid>` (PID suffix lets multiple
// SubTropica parallel-kernel processes write without colliding).
// Diagnostic for the microbench-vs-trace inconsistency: the microbench
// predicts narrow always loses but 3l3pt step-3 trace shows narrow saves
// 103s, so we need real-input (la, lb, used_count, max_deg, coeff_bits)
// distributions, not synthetic ones.
//
// Defaults: SKIP=0, LIMIT=500.  For 3l3pt ord_2 step 3 specifically,
// SKIP ~ 6000 (skips ord_1 + ord_2 steps 0-2 cumulative mul calls).
struct DumpMulState {
    std::FILE* fp = nullptr;
    std::mutex mu;
    std::atomic<long> count{0};
    long skip = 0;
    long limit = 500;
    bool initialized = false;
    bool enabled = false;
};

DumpMulState& dump_mul_state() { static DumpMulState s; return s; }

inline bool dump_mul_enabled() {
    auto& s = dump_mul_state();
    if (!s.initialized) {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.initialized) {
            const char* path = HF_FLAG_DUMP_MUL;
            if (path && path[0]) {
                if (const char* sk = HF_FLAG_DUMP_MUL_SKIP)
                    s.skip = std::atol(sk);
                if (const char* lm = HF_FLAG_DUMP_MUL_LIMIT)
                    s.limit = std::atol(lm);
                char fullpath[1200];
                std::snprintf(fullpath, sizeof(fullpath), "%s.%d",
                              path, static_cast<int>(getpid()));
                s.fp = std::fopen(fullpath, "w");
                if (s.fp) {
                    std::fprintf(s.fp,
                        "seq,la,lb,nvars_wide,used_count,worth_narrowing,"
                        "max_deg_a,max_deg_b,max_coef_bits,total_coef_bits,"
                        "narrow_us,wide_us\n");
                    std::fflush(s.fp);
                    s.enabled = true;
                }
            }
            s.initialized = true;
        }
    }
    if (!s.enabled) return false;
    const long n = s.count.load(std::memory_order_relaxed);
    return n >= s.skip && n < s.skip + s.limit;
}

// Coefficient height: max(bits(num), bits(den)) summed and maxed over
// all terms.  Used to flag "deep rationals" — a candidate gate axis.
struct CoefStats { long max_bits; long total_bits; };
CoefStats coef_stats(const fmpq_mpoly_struct* p, fmpq_mpoly_ctx_struct* ctx) {
    CoefStats out{0, 0};
    const slong n = fmpq_mpoly_length(p, ctx);
    fmpq_t c; fmpq_init(c);
    for (slong i = 0; i < n; ++i) {
        fmpq_mpoly_get_term_coeff_fmpq(c, p, i, ctx);
        long nb = static_cast<long>(fmpz_bits(fmpq_numref(c)));
        long db = static_cast<long>(fmpz_bits(fmpq_denref(c)));
        long mb = nb > db ? nb : db;
        if (mb > out.max_bits) out.max_bits = mb;
        out.total_bits += mb;
    }
    fmpq_clear(c);
    return out;
}

slong max_total_degree(const fmpq_mpoly_struct* p, fmpq_mpoly_ctx_struct* ctx) {
    if (fmpq_mpoly_is_zero(p, ctx)) return 0;
    if (!fmpq_mpoly_total_degree_fits_si(p, ctx)) return -1;  // overflow flag
    return fmpq_mpoly_total_degree_si(p, ctx);
}
}  // namespace

Poly Poly::mul(const Poly& b) const {
    assert_same_ctx(*this, b);
    // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num()
    // in OMP-only mode; under HF_USE_GCD=1 returns the GCD slot index.
    const int _mul_tid = ::hyperflint::runtime::hf_get_thread_num();

    // HF_DUMP_MUL diagnostic.  Run BOTH paths, time each, write CSV.
    // Cost is roughly 2x normal mul; only enabled with explicit env var
    // set, and only for the call window [SKIP, SKIP+LIMIT).
    auto& _dms = dump_mul_state();
    const long _dms_seq = _dms.enabled
        ? _dms.count.fetch_add(1, std::memory_order_relaxed)
        : -1;
    if (dump_mul_enabled()) {
        auto& s = dump_mul_state();
        const long seq = _dms_seq;
        if (seq >= s.skip && seq < s.skip + s.limit) {
            const slong la = fmpq_mpoly_length(raw_, ctx_->raw());
            const slong lb = fmpq_mpoly_length(b.raw_, ctx_->raw());
            const size_t nvars_wide = ctx_->vars().size();

            std::vector<int> used(nvars_wide, 0);
            fmpq_mpoly_used_vars(used.data(),
                const_cast<fmpq_mpoly_struct*>(raw_), ctx_->raw());
            std::vector<int> used_b(nvars_wide, 0);
            fmpq_mpoly_used_vars(used_b.data(),
                const_cast<fmpq_mpoly_struct*>(b.raw_), ctx_->raw());
            for (size_t j = 0; j < nvars_wide; ++j)
                if (used_b[j]) used[j] = 1;
            std::vector<size_t> used_wide;
            used_wide.reserve(nvars_wide);
            for (size_t j = 0; j < nvars_wide; ++j)
                if (used[j]) used_wide.push_back(j);
            const bool worth = used_wide.size() * 4 < nvars_wide;

            const slong max_a = max_total_degree(raw_, ctx_->raw());
            const slong max_b = max_total_degree(b.raw_, ctx_->raw());
            const CoefStats csa = coef_stats(raw_, ctx_->raw());
            const CoefStats csb = coef_stats(b.raw_, ctx_->raw());
            const long max_coef_bits =
                csa.max_bits > csb.max_bits ? csa.max_bits : csb.max_bits;
            const long total_coef_bits = csa.total_bits + csb.total_bits;

            // Time wide path.
            const auto _w0 = std::chrono::steady_clock::now();
            Poly wide_r(*ctx_);
            fmpq_mpoly_mul(wide_r.raw_, raw_, b.raw_, ctx_->raw());
            const auto _w1 = std::chrono::steady_clock::now();
            const double wide_us =
                std::chrono::duration<double, std::micro>(_w1 - _w0).count();

            // Time narrow path (only if it would be exercised).
            double narrow_us = -1.0;
            if (worth && (la + lb) >= kNarrowMulMinLen) {
                std::vector<std::string> narrow_var_names;
                narrow_var_names.reserve(used_wide.size());
                std::vector<size_t> wide_to_narrow(nvars_wide, SIZE_MAX);
                for (size_t k = 0; k < used_wide.size(); ++k) {
                    narrow_var_names.push_back(ctx_->vars()[used_wide[k]]);
                    wide_to_narrow[used_wide[k]] = k;
                }
                const auto _n0 = std::chrono::steady_clock::now();
                const PolyCtx narrow(std::move(narrow_var_names));
                Poly a_n = this->transplant(narrow, wide_to_narrow);
                Poly b_n = b.transplant(narrow, wide_to_narrow);
                Poly r_n(narrow);
                fmpq_mpoly_mul(r_n.raw_, a_n.raw_, b_n.raw_, narrow.raw());
                std::vector<size_t> narrow_to_wide(used_wide);
                Poly result = r_n.transplant(*ctx_, narrow_to_wide);
                const auto _n1 = std::chrono::steady_clock::now();
                narrow_us =
                    std::chrono::duration<double, std::micro>(_n1 - _n0).count();
                (void)result;
            }

            {
                std::lock_guard<std::mutex> lk(s.mu);
                if (s.fp) {
                    std::fprintf(s.fp,
                        "%ld,%ld,%ld,%zu,%zu,%d,%ld,%ld,%ld,%ld,%.4f,%.4f\n",
                        seq,
                        (long)la, (long)lb, nvars_wide, used_wide.size(),
                        worth ? 1 : 0,
                        (long)max_a, (long)max_b,
                        max_coef_bits, total_coef_bits,
                        narrow_us, wide_us);
                    std::fflush(s.fp);
                }
            }

            return wide_r;  // semantically correct return
        }
    }

    if (!mul_narrow_enabled()) {
        // Default path: wide-ctx mult, no instrumentation overhead.
        Poly r(*ctx_);
        fmpq_mpoly_mul(r.raw_, raw_, b.raw_, ctx_->raw());
        return r;
    }

    const size_t nvars_wide = ctx_->vars().size();

    const slong la_len = fmpq_mpoly_length(raw_, ctx_->raw());
    const slong lb_len = fmpq_mpoly_length(b.raw_, ctx_->raw());
    const slong len_total = la_len + lb_len;
    const long  la_lb     = static_cast<long>(la_len) * static_cast<long>(lb_len);
    const slong min_side  = (la_len < lb_len) ? la_len : lb_len;
    // Reviewer round 8 gate: la·lb ≥ 16k AND min(la,lb) ≥ 4 AND
    // (legacy len_total ≥ kNarrowMulMinLen retained as outer guard).
    if (len_total < kNarrowMulMinLen ||
        la_lb     < kNarrowMulMinLaLb ||
        min_side  < kNarrowMulMinSideLen) {
        // Tiny / short-by-long mult — wide-ctx faster than transplant.
        auto& _gv = mul_gated_calls_storage();
        if (static_cast<size_t>(_mul_tid) < _gv.size()) _gv[_mul_tid] += 1;
        Poly r(*ctx_);
        fmpq_mpoly_mul(r.raw_, raw_, b.raw_, ctx_->raw());
        return r;
    }

    std::vector<int> used(nvars_wide, 0);
    fmpq_mpoly_used_vars(used.data(),
        const_cast<fmpq_mpoly_struct*>(raw_), ctx_->raw());
    {
        std::vector<int> used_b(nvars_wide, 0);
        fmpq_mpoly_used_vars(used_b.data(),
            const_cast<fmpq_mpoly_struct*>(b.raw_), ctx_->raw());
        for (size_t j = 0; j < nvars_wide; ++j)
            if (used_b[j]) used[j] = 1;
    }
    std::vector<size_t> used_wide;
    used_wide.reserve(nvars_wide);
    for (size_t j = 0; j < nvars_wide; ++j) if (used[j]) used_wide.push_back(j);

    const bool worth_narrowing = used_wide.size() * 4 < nvars_wide;

    if (worth_narrowing) {
        const bool _tg = step_trace_enabled();
        const slong la_for_bin = _tg ? fmpq_mpoly_length(raw_, ctx_->raw()) : 0;
        const slong lb_for_bin = _tg ? fmpq_mpoly_length(b.raw_, ctx_->raw()) : 0;
        const auto _t0 = _tg ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
        std::vector<std::string> narrow_var_names;
        narrow_var_names.reserve(used_wide.size());
        std::vector<size_t> wide_to_narrow(nvars_wide, SIZE_MAX);
        for (size_t k = 0; k < used_wide.size(); ++k) {
            narrow_var_names.push_back(ctx_->vars()[used_wide[k]]);
            wide_to_narrow[used_wide[k]] = k;
        }
        const PolyCtx narrow(std::move(narrow_var_names));

        Poly a_n = this->transplant(narrow, wide_to_narrow);
        Poly b_n = b.transplant(narrow, wide_to_narrow);
        Poly r_n(narrow);
        fmpq_mpoly_mul(r_n.raw_, a_n.raw_, b_n.raw_, narrow.raw());
        std::vector<size_t> narrow_to_wide(used_wide);
        Poly result = r_n.transplant(*ctx_, narrow_to_wide);
        if (_tg) {
            const auto _t1 = std::chrono::steady_clock::now();
            const double _dt = std::chrono::duration<double>(_t1 - _t0).count();
            auto& _nv = mul_narrow_storage();
            if (static_cast<size_t>(_mul_tid) < _nv.size())
                _nv[_mul_tid] += _dt;
            auto& _cv = mul_narrow_calls_storage();
            if (static_cast<size_t>(_mul_tid) < _cv.size()) _cv[_mul_tid] += 1;

            // Per-call distribution counters (reviewer round 7).
            const int lb_idx = la_lb_bin(la_for_bin, lb_for_bin);
            const int u_idx  = u_bin(used_wide.size());
            auto& nlc = nbin_lalb_count_storage();
            auto& nlt = nbin_lalb_us_storage();
            auto& nlm = nbin_lalb_max_storage();
            auto& nuc = nbin_u_count_storage();
            auto& nut = nbin_u_us_storage();
            if (static_cast<size_t>(_mul_tid) < nlc.size()) {
                nlc[_mul_tid][lb_idx] += 1;
                nlt[_mul_tid][lb_idx] += _dt * 1e6;  // store in us
                if (_dt * 1e6 > nlm[_mul_tid][lb_idx])
                    nlm[_mul_tid][lb_idx] = _dt * 1e6;
                nuc[_mul_tid][u_idx] += 1;
                nut[_mul_tid][u_idx] += _dt * 1e6;
            }
        }
        return result;
    }

    const bool _tg = step_trace_enabled();
    const auto _w0 = _tg ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{};
    Poly r(*ctx_);
    fmpq_mpoly_mul(r.raw_, raw_, b.raw_, ctx_->raw());
    if (_tg) {
        const auto _w1 = std::chrono::steady_clock::now();
        auto& _wv = mul_wide_storage();
        if (static_cast<size_t>(_mul_tid) < _wv.size())
            _wv[_mul_tid] += std::chrono::duration<double>(_w1 - _w0).count();
        auto& _cv = mul_wide_calls_storage();
        if (static_cast<size_t>(_mul_tid) < _cv.size()) _cv[_mul_tid] += 1;
    }
    return r;
}

// Reviewer round 7 distribution counter accessors.  Each returns a
// vector of per-bin totals summed across threads.
std::vector<long> sum_nbin_lalb_count_per_thread() {
    auto& v = nbin_lalb_count_storage();
    std::vector<long> out(kNarrowLaLbBins, 0);
    for (auto& tv : v)
        for (int i = 0; i < kNarrowLaLbBins; ++i)
            if (i < static_cast<int>(tv.size())) out[i] += tv[i];
    return out;
}
std::vector<double> sum_nbin_lalb_us_per_thread() {
    auto& v = nbin_lalb_us_storage();
    std::vector<double> out(kNarrowLaLbBins, 0.0);
    for (auto& tv : v)
        for (int i = 0; i < kNarrowLaLbBins; ++i)
            if (i < static_cast<int>(tv.size())) out[i] += tv[i];
    return out;
}
std::vector<double> sum_nbin_lalb_max_per_thread() {
    auto& v = nbin_lalb_max_storage();
    std::vector<double> out(kNarrowLaLbBins, 0.0);
    for (auto& tv : v)
        for (int i = 0; i < kNarrowLaLbBins; ++i)
            if (i < static_cast<int>(tv.size()) && tv[i] > out[i])
                out[i] = tv[i];
    return out;
}
std::vector<long> sum_nbin_u_count_per_thread() {
    auto& v = nbin_u_count_storage();
    std::vector<long> out(kNarrowUBins, 0);
    for (auto& tv : v)
        for (int i = 0; i < kNarrowUBins; ++i)
            if (i < static_cast<int>(tv.size())) out[i] += tv[i];
    return out;
}
std::vector<double> sum_nbin_u_us_per_thread() {
    auto& v = nbin_u_us_storage();
    std::vector<double> out(kNarrowUBins, 0.0);
    for (auto& tv : v)
        for (int i = 0; i < kNarrowUBins; ++i)
            if (i < static_cast<int>(tv.size())) out[i] += tv[i];
    return out;
}

void init_mul_per_thread(int n_threads) {
    const size_t n = static_cast<size_t>(n_threads > 0 ? n_threads : 1);
    mul_narrow_storage().assign(n, 0.0);
    mul_wide_storage().assign(n, 0.0);
    mul_narrow_calls_storage().assign(n, 0L);
    mul_wide_calls_storage().assign(n, 0L);
    mul_gated_calls_storage().assign(n, 0L);
    nbin_lalb_count_storage().assign(n, std::vector<long>(kNarrowLaLbBins, 0));
    nbin_lalb_us_storage().assign(n, std::vector<double>(kNarrowLaLbBins, 0.0));
    nbin_lalb_max_storage().assign(n, std::vector<double>(kNarrowLaLbBins, 0.0));
    nbin_u_count_storage().assign(n, std::vector<long>(kNarrowUBins, 0));
    nbin_u_us_storage().assign(n, std::vector<double>(kNarrowUBins, 0.0));
}
void reset_mul_per_thread() {
    for (auto& x : mul_narrow_storage())       x = 0.0;
    for (auto& x : mul_wide_storage())         x = 0.0;
    for (auto& x : mul_narrow_calls_storage()) x = 0L;
    for (auto& x : mul_wide_calls_storage())   x = 0L;
    for (auto& x : mul_gated_calls_storage())  x = 0L;
    for (auto& tv : nbin_lalb_count_storage()) std::fill(tv.begin(), tv.end(), 0L);
    for (auto& tv : nbin_lalb_us_storage())    std::fill(tv.begin(), tv.end(), 0.0);
    for (auto& tv : nbin_lalb_max_storage())   std::fill(tv.begin(), tv.end(), 0.0);
    for (auto& tv : nbin_u_count_storage())    std::fill(tv.begin(), tv.end(), 0L);
    for (auto& tv : nbin_u_us_storage())       std::fill(tv.begin(), tv.end(), 0.0);
}
double sum_mul_narrow_s_per_thread() {
    double s = 0; for (double x : mul_narrow_storage()) s += x; return s;
}
double sum_mul_wide_s_per_thread() {
    double s = 0; for (double x : mul_wide_storage()) s += x; return s;
}
long sum_mul_narrow_calls_per_thread() {
    long s = 0; for (long x : mul_narrow_calls_storage()) s += x; return s;
}
long sum_mul_wide_calls_per_thread() {
    long s = 0; for (long x : mul_wide_calls_storage()) s += x; return s;
}
long sum_mul_gated_calls_per_thread() {
    long s = 0; for (long x : mul_gated_calls_storage()) s += x; return s;
}

Poly Poly::neg() const {
    Poly r(*ctx_);
    fmpq_mpoly_neg(r.raw_, raw_, ctx_->raw());
    return r;
}

Poly Poly::pow(unsigned long n) const {
    Poly r(*ctx_);
    if (fmpq_mpoly_pow_ui(r.raw_, raw_, static_cast<ulong>(n),
                          ctx_->raw()) == 0) {
        throw std::runtime_error("Poly::pow: fmpq_mpoly_pow_ui failed "
                                 "(likely negative or huge exponent)");
    }
    return r;
}

// -------- Phase 1b calculus --------

Poly Poly::derivative(size_t var_idx) const {
    if (var_idx >= ctx_->vars().size()) {
        throw std::out_of_range("Poly::derivative: var_idx out of range");
    }
    Poly r(*ctx_);
    fmpq_mpoly_derivative(r.raw_, raw_, static_cast<slong>(var_idx),
                          ctx_->raw());
    return r;
}

std::string Poly::evaluate_all(const std::vector<std::string>& values) const {
    const size_t nv = ctx_->vars().size();
    if (values.size() != nv) {
        throw std::invalid_argument(
            "Poly::evaluate_all: need " + std::to_string(nv) + " values, got "
            + std::to_string(values.size()));
    }
    // Build an array of fmpq. fmpq_t is an array typedef, so
    // std::vector<fmpq_t> is ill-formed; use a manual heap array of
    // fmpq instead and pass (fmpq**)&ptr[i] = &arr[i].
    std::vector<fmpq> qs(nv);
    std::vector<fmpq*> ptrs(nv);
    for (size_t i = 0; i < nv; ++i) {
        fmpq_init(&qs[i]);
        ptrs[i] = &qs[i];
        if (fmpq_set_str(&qs[i], values[i].c_str(), 10) != 0) {
            for (size_t j = 0; j <= i; ++j) fmpq_clear(&qs[j]);
            throw std::runtime_error("Poly::evaluate_all: bad rational '"
                                     + values[i] + "'");
        }
    }

    fmpq_t result;
    fmpq_init(result);
    int ok = fmpq_mpoly_evaluate_all_fmpq(
        result, const_cast<fmpq_mpoly_struct*>(raw_),
        ptrs.data(), ctx_->raw());
    if (!ok) {
        fmpq_clear(result);
        for (size_t i = 0; i < nv; ++i) fmpq_clear(&qs[i]);
        throw std::runtime_error(
            "Poly::evaluate_all: fmpq_mpoly_evaluate_all_fmpq failed");
    }

    char* s = fmpq_get_str(nullptr, 10, result);
    std::string out(s);
    flint_free(s);
    fmpq_clear(result);
    for (size_t i = 0; i < nv; ++i) fmpq_clear(&qs[i]);
    return out;
}

// -------- Phase 1c division + GCD --------

Poly Poly::divexact(const Poly& b) const {
    assert_same_ctx(*this, b);
    Poly r(*ctx_);
    if (fmpq_mpoly_divides(r.raw_, raw_, b.raw_, ctx_->raw()) == 0) {
        throw std::runtime_error("Poly::divexact: non-exact division");
    }
    return r;
}

bool Poly::divides(const Poly& b) const {
    // Tests whether *this divides b (i.e. is there q such that b = q * this).
    assert_same_ctx(*this, b);
    fmpq_mpoly_t q;
    fmpq_mpoly_init(q, ctx_->raw());
    int ok = fmpq_mpoly_divides(q, b.raw_, raw_, ctx_->raw());
    fmpq_mpoly_clear(q, ctx_->raw());
    return ok != 0;
}

std::pair<Poly, Poly> Poly::divrem(const Poly& b) const {
    assert_same_ctx(*this, b);
    Poly q(*ctx_), r(*ctx_);
    fmpq_mpoly_divrem(q.raw_, r.raw_, raw_, b.raw_, ctx_->raw());
    return {std::move(q), std::move(r)};
}

// Minimum total term count before the narrow-ctx transplant pays
// for itself on gcd/reduce_inplace. Adversarial-review benchmarks
// showed narrow-ctx wins at sum-of-lengths >= 4 (e.g., a 2-term gcd
// 2-term costs ~9 transplant monomials to save ~4 Johnson ops —
// net loss; at sum >= 4 the gcd's Johnson/subresultant-content
// work starts to dominate the transplant overhead). tst1 / tst2
// traffic heavily in sum-of-length <= 3 polys (Log-expansion
// shuffle entries); gating those out reclaims the 33% transplant
// overhead observed in the tst1 profile.
constexpr slong kNarrowMinLen = 4;

Poly Poly::gcd(const Poly& b) const {
    assert_same_ctx(*this, b);
    // Narrow-ctx path: fmpq_mpoly_gcd pays O(n_wide_vars) in its
    // variable-compression pass per call. For the wide tst0 ctx
    // (~711 vars) but polys that typically touch <20 vars, this is
    // ~35× overhead. Transplant both polys into a narrow ctx
    // containing only the union of used variables, gcd there, then
    // transplant the result back. Mirrors the linear_factors narrow
    // path.
    //
    // Lifetime note: the narrow PolyCtx below is a local. All narrow
    // Polys created inside the if-branch are destructed BEFORE the
    // narrow ctx (LIFO scope order), which is what fmpq_mpoly_clear
    // requires. Do not hoist any narrow Poly out of this block.
    const size_t nvars_wide = ctx_->vars().size();

    // Size gate first (cheap): if both polys are trivially small,
    // wide-ctx gcd is already faster than the transplant round-trip.
    // Dodges the O(n_terms·words_per_exp) fmpq_mpoly_used_vars scans
    // below for all the tst1/tst2 Log-shuffle micro-polys.
    const slong len_total =
        fmpq_mpoly_length(raw_, ctx_->raw())
        + fmpq_mpoly_length(b.raw_, ctx_->raw());
    if (len_total < kNarrowMinLen) {
        Poly r(*ctx_);
        if (fmpq_mpoly_gcd(r.raw_, raw_, b.raw_, ctx_->raw()) == 0) {
            throw std::runtime_error("Poly::gcd: fmpq_mpoly_gcd failed");
        }
        return r;
    }

    std::vector<int> used(nvars_wide, 0);
    fmpq_mpoly_used_vars(used.data(),
        const_cast<fmpq_mpoly_struct*>(raw_), ctx_->raw());
    {
        std::vector<int> used_b(nvars_wide, 0);
        fmpq_mpoly_used_vars(used_b.data(),
            const_cast<fmpq_mpoly_struct*>(b.raw_), ctx_->raw());
        for (size_t j = 0; j < nvars_wide; ++j)
            if (used_b[j]) used[j] = 1;
    }
    std::vector<size_t> used_wide;
    used_wide.reserve(nvars_wide);
    for (size_t j = 0; j < nvars_wide; ++j) if (used[j]) used_wide.push_back(j);

    const bool worth_narrowing = used_wide.size() * 4 < nvars_wide;

    if (worth_narrowing) {
        std::vector<std::string> narrow_var_names;
        narrow_var_names.reserve(used_wide.size());
        std::vector<size_t> wide_to_narrow(nvars_wide, SIZE_MAX);
        for (size_t k = 0; k < used_wide.size(); ++k) {
            narrow_var_names.push_back(ctx_->vars()[used_wide[k]]);
            wide_to_narrow[used_wide[k]] = k;
        }
        const PolyCtx narrow(std::move(narrow_var_names));

        Poly a_n = this->transplant(narrow, wide_to_narrow);
        Poly b_n = b.transplant(narrow, wide_to_narrow);
        Poly g_n(narrow);
        if (fmpq_mpoly_gcd(g_n.raw_, a_n.raw_, b_n.raw_, narrow.raw()) == 0) {
            throw std::runtime_error("Poly::gcd: fmpq_mpoly_gcd failed (narrow)");
        }
        std::vector<size_t> narrow_to_wide(used_wide);
        return g_n.transplant(*ctx_, narrow_to_wide);
    }

    Poly r(*ctx_);
    if (fmpq_mpoly_gcd(r.raw_, raw_, b.raw_, ctx_->raw()) == 0) {
        throw std::runtime_error("Poly::gcd: fmpq_mpoly_gcd failed");
    }
    return r;
}

Poly Poly::resultant(const Poly& b, size_t var_idx) const {
    assert_same_ctx(*this, b);
    if (var_idx >= ctx_->vars().size()) {
        throw std::out_of_range("Poly::resultant: var_idx out of range");
    }
    Poly r(*ctx_);
    if (fmpq_mpoly_resultant(r.raw_, raw_, b.raw_,
                             static_cast<slong>(var_idx), ctx_->raw()) == 0) {
        throw std::runtime_error("Poly::resultant: fmpq_mpoly_resultant failed");
    }
    return r;
}

Poly Poly::canonical_prop_form() const {
    if (fmpq_mpoly_is_zero(raw_, ctx_->raw())) {
        return Poly(*ctx_);  // zero
    }
    if (fmpq_mpoly_is_fmpq(raw_, ctx_->raw())) {
        // All numeric — collapse to sign(lead) = ±1 so constants with
        // the same sign collide.
        fmpq_t c;
        fmpq_init(c);
        fmpq_mpoly_get_term_coeff_fmpq(c, raw_, 0, ctx_->raw());
        int s = fmpq_sgn(c);
        fmpq_clear(c);
        return Poly::from_int(*ctx_, s);
    }
    // Grab the leading-monomial coefficient (which under FLINT's ORD_LEX
    // is the term with the lexicographically greatest monomial).
    fmpq_t lead;
    fmpq_init(lead);
    fmpq_mpoly_get_term_coeff_fmpq(lead, raw_, 0, ctx_->raw());
    if (fmpq_is_zero(lead)) {
        fmpq_clear(lead);
        throw std::runtime_error(
            "Poly::canonical_prop_form: leading coefficient is zero");
    }
    Poly r(*ctx_);
    fmpq_mpoly_scalar_div_fmpq(r.raw(), raw_, lead, ctx_->raw());
    fmpq_clear(lead);
    return r;
}

// 2026-05-05 (path-A diagnostic — Probe 4 attribution machinery).
// Walks fmpq_mpoly_struct = { fmpq_t content; fmpz_mpoly_t zpoly; }.
// fmpz_mpoly_struct = { fmpz* coeffs; ulong* exps; slong alloc;
//                       slong length; flint_bitcnt_t bits; }.
// Per-monomial exponent footprint = words_per_exp_sp(bits, mctx) * 8.
// Reference scaffolding from stash@{0} ("today's HF probes + C2 (bisect)").
Poly::PolyByteBuckets Poly::total_bytes_buckets() const {
    PolyByteBuckets b;
    if (ctx_ == nullptr) return b;
    // Per-poly content fmpq_t.
    b.content_fmpq += fmpz_size(fmpq_numref(raw_->content)) * sizeof(ulong);
    b.content_fmpq += fmpz_size(fmpq_denref(raw_->content)) * sizeof(ulong);
    fmpz_mpoly_struct* zp = raw_->zpoly;
    if (zp->bits > 0) {
        const slong wpe = mpoly_words_per_exp_sp(zp->bits,
                                                  ctx_->raw()->zctx->minfo);
        const size_t exp_per_mono = static_cast<size_t>(wpe) * sizeof(ulong);
        b.exp_live  = static_cast<size_t>(zp->length) * exp_per_mono;
        b.exp_slack = static_cast<size_t>(zp->alloc - zp->length) * exp_per_mono;
    }
    b.handle_live  = static_cast<size_t>(zp->length) * sizeof(fmpz);
    b.handle_slack = static_cast<size_t>(zp->alloc - zp->length) * sizeof(fmpz);
    for (slong i = 0; i < zp->length; ++i) {
        b.coeff_intrinsic += fmpz_size(zp->coeffs + i) * sizeof(ulong);
    }
    return b;
}

size_t Poly::total_bytes() const {
    return total_bytes_buckets().total();
}

// HF_ADDPF_PROBE (additive-parfrac Phase 0, 2026-06-03): public
// term-count accessor; see poly.hpp.
size_t Poly::n_terms() const {
    return static_cast<size_t>(fmpq_mpoly_length(raw_, ctx_->raw()));
}

Poly Poly::discriminant_in_var(size_t var_idx) const {
    // Mathematica convention: Discriminant[p, x] = 1 when p is constant
    // in x (deg < 1).  Below that, the formula disc = Res(p, p', x)/lc
    // is ill-defined (p' = 0, lc = p itself).
    long n = degree_in_var(var_idx);
    if (n < 1) {
        return Poly::from_int(*ctx_, 1);
    }

    // disc = Resultant(p, dp/dvar, var) / lc(p, var).
    // Both numerator and denominator live in the same ctx; the division
    // is EXACT as a polynomial identity (Sylvester-matrix theory), so
    // fmpq_mpoly_divides must return nonzero.  If it doesn't, upstream
    // invariants have been violated and we surface that.
    Poly dp = derivative(var_idx);
    Poly num = resultant(dp, var_idx);
    Poly lc = coefficient_of_var(var_idx, n);

    if (fmpq_mpoly_is_zero(lc.raw(), ctx_->raw())) {
        // Defensive: lc should be nonzero when deg >= 1.
        throw std::runtime_error(
            "Poly::discriminant_in_var: leading coefficient is zero");
    }

    Poly disc(*ctx_);
    if (fmpq_mpoly_divides(disc.raw(), num.raw(), lc.raw(),
                           ctx_->raw()) == 0) {
        // Diagnostic dump: the divisibility a_n | Res(f,f') is a
        // polynomial identity in char 0 (Sylvester / GKZ Prop 12.1.6),
        // so a failure here is a real HF or FLINT bug. When the env
        // var (a filesystem path) is set, append (f, f', lc, Res, var)
        // to that path so we can cross-check against Mma. Capped at 10
        // dumps and serialized across threads to keep the output sane.
        // Iter-96 §T7 26th chunk Track-diagnostic-dump partial
        // core/poly portion: VALUE-family macro relocation to the new
        // core/env_flags_poly.hpp (iter-76-anticipated sibling of
        // core/env_flags_rat.hpp and core/env_flags_reduce.hpp; first
        // occupant of that header). Reviewer-binding rationale for the
        // NEW header creation rather than mixing into the NAME-pure
        // core/env_flags.hpp: §5.1 family-isolation per iter-73
        // BINDING (header-level family purity). The TRUTHY+use-as-path
        // predicate semantics are preserved verbatim; the path
        // pointer is consumed directly by std::ofstream below.
        if (const char* path = HF_FLAG_DEBUG_DISC_DUMP) {
            static std::atomic<int> dump_count{0};
            static std::mutex dump_mtx;
            if (dump_count.fetch_add(1) < 10) {
                std::lock_guard<std::mutex> lk(dump_mtx);
                std::ofstream dump(path, std::ios::app);
                if (dump) {
                    dump << "=== Poly::discriminant_in_var failure ===\n"
                         << "var_idx=" << var_idx
                         << "  var_name=" << ctx_->vars()[var_idx]
                         << "  deg_in_var=" << n << "\n"
                         << "ctx_vars: ";
                    for (const auto& v : ctx_->vars()) dump << v << " ";
                    dump << "\n"
                         << "f = " << to_string() << "\n"
                         << "f' = " << dp.to_string() << "\n"
                         << "lc = " << lc.to_string() << "\n"
                         << "Res(f,f') = " << num.to_string() << "\n"
                         << "=== end dump ===\n";
                }
            }
        }
        throw std::runtime_error(
            "Poly::discriminant_in_var: Res/lc division is not exact — "
            "Sylvester invariant violated");
    }
    return disc;
}

// -------- Phase 2 structural helpers --------

long Poly::degree_in_var(size_t var_idx) const {
    if (var_idx >= ctx_->vars().size()) {
        throw std::out_of_range("Poly::degree_in_var: var_idx out of range");
    }
    if (fmpq_mpoly_is_zero(raw_, ctx_->raw())) return -1;
    return static_cast<long>(
        fmpq_mpoly_degree_si(raw_, static_cast<slong>(var_idx), ctx_->raw()));
}

long Poly::min_exponent_in_var(size_t var_idx) const {
    if (var_idx >= ctx_->vars().size()) {
        throw std::out_of_range("Poly::min_exponent_in_var: var_idx out of range");
    }
    const fmpq_mpoly_struct* P = raw_;
    const slong nt = fmpq_mpoly_length(P, ctx_->raw());
    if (nt == 0) return LONG_MAX;
    const size_t nv = ctx_->vars().size();
    std::vector<slong> exps(nv);
    long lo = LONG_MAX;
    for (slong i = 0; i < nt; ++i) {
        fmpq_mpoly_get_term_exp_si(exps.data(),
                                   const_cast<fmpq_mpoly_struct*>(P), i,
                                   ctx_->raw());
        long e = static_cast<long>(exps[var_idx]);
        if (e < lo) lo = e;
    }
    return lo;
}

Poly Poly::coefficient_of_var(size_t var_idx, long exp) const {
    if (var_idx >= ctx_->vars().size()) {
        throw std::out_of_range("Poly::coefficient_of_var: var_idx out of range");
    }
    if (exp < 0) {
        throw std::invalid_argument("Poly::coefficient_of_var: negative exp");
    }
    Poly r(*ctx_);
    slong idx = static_cast<slong>(var_idx);
    ulong e  = static_cast<ulong>(exp);
    // fmpq_mpoly_get_coeff_vars_ui extracts the poly-coefficient of the
    // specified monomial in the indexed variables.  Here we fix only
    // `var_idx` to exponent `exp` and let the result live over the
    // remaining variables.
    fmpq_mpoly_get_coeff_vars_ui(
        r.raw_, const_cast<fmpq_mpoly_struct*>(raw_),
        &idx, &e, /*length=*/1, ctx_->raw());
    return r;
}

bool Poly::is_fmpq() const {
    return fmpq_mpoly_is_fmpq(raw_, ctx_->raw()) != 0;
}

void Poly::scalar_div_fmpq_const(const Poly& c) {
    if (!c.is_fmpq()) {
        throw std::invalid_argument(
            "Poly::scalar_div_fmpq_const: divisor is not a pure constant");
    }
    fmpq_t cv, cinv;
    fmpq_init(cv); fmpq_init(cinv);
    fmpq_mpoly_get_fmpq(cv, const_cast<fmpq_mpoly_struct*>(c.raw_),
                        c.ctx_->raw());
    if (fmpq_is_zero(cv)) {
        fmpq_clear(cv); fmpq_clear(cinv);
        throw std::runtime_error(
            "Poly::scalar_div_fmpq_const: divisor is zero");
    }
    fmpq_inv(cinv, cv);
    fmpq_mpoly_scalar_mul_fmpq(raw_, raw_, cinv, ctx_->raw());
    fmpq_clear(cv); fmpq_clear(cinv);
}

Poly Poly::substitute_one_rat(size_t var_idx,
                              const std::string& value) const {
    if (var_idx >= ctx_->vars().size()) {
        throw std::out_of_range("Poly::substitute_one_rat: var_idx out of range");
    }
    fmpq_t q;
    fmpq_init(q);
    if (fmpq_set_str(q, value.c_str(), 10) != 0) {
        fmpq_clear(q);
        throw std::runtime_error("Poly::substitute_one_rat: bad rational '"
                                 + value + "'");
    }
    Poly r(*ctx_);
    int ok = fmpq_mpoly_evaluate_one_fmpq(
        r.raw_, const_cast<fmpq_mpoly_struct*>(raw_),
        static_cast<slong>(var_idx), q, ctx_->raw());
    fmpq_clear(q);
    if (!ok) {
        throw std::runtime_error("Poly::substitute_one_rat: fmpq_mpoly_evaluate_one_fmpq failed");
    }
    return r;
}

Factored factor(const Poly& p) {
    const PolyCtx& ctx = p.ctx();
    fmpq_mpoly_factor_t f;
    fmpq_mpoly_factor_init(f, ctx.raw());
    if (fmpq_mpoly_factor(f, const_cast<fmpq_mpoly_struct*>(p.raw()),
                          ctx.raw()) == 0) {
        fmpq_mpoly_factor_clear(f, ctx.raw());
        throw std::runtime_error("fmpq_mpoly_factor failed");
    }

    Factored out;
    out.constant = fmpq_to_string(f->constant);

    const slong nfactors = f->num;
    out.factors.reserve(static_cast<size_t>(nfactors));
    for (slong i = 0; i < nfactors; ++i) {
        char* s = fmpq_mpoly_get_str_pretty(f->poly + i,
                                            ctx.cvars(), ctx.raw());
        long exp = fmpz_get_si(f->exp + i);
        out.factors.emplace_back(std::string(s), exp);
        flint_free(s);
    }

    fmpq_mpoly_factor_clear(f, ctx.raw());
    return out;
}

std::vector<Poly> factor_bases(const Poly& p) {
    const PolyCtx& ctx = p.ctx();
    std::vector<Poly> out;

    // Numeric / zero inputs produce no non-constant factors.
    if (fmpq_mpoly_is_zero(p.raw(), ctx.raw())) return out;
    if (fmpq_mpoly_is_fmpq(p.raw(), ctx.raw())) return out;

    fmpq_mpoly_factor_t f;
    fmpq_mpoly_factor_init(f, ctx.raw());
    if (fmpq_mpoly_factor(f, const_cast<fmpq_mpoly_struct*>(p.raw()),
                          ctx.raw()) == 0) {
        fmpq_mpoly_factor_clear(f, ctx.raw());
        throw std::runtime_error("factor_bases: fmpq_mpoly_factor failed");
    }

    const slong nfactors = f->num;
    out.reserve(static_cast<size_t>(nfactors));
    for (slong i = 0; i < nfactors; ++i) {
        // Skip factors that are themselves numeric (shouldn't happen
        // since FLINT separates the leading constant into f->constant,
        // but be defensive).
        if (fmpq_mpoly_is_fmpq(f->poly + i, ctx.raw())) continue;
        Poly factor_i(ctx);
        fmpq_mpoly_set(factor_i.raw(), f->poly + i, ctx.raw());
        out.push_back(std::move(factor_i));
    }

    fmpq_mpoly_factor_clear(f, ctx.raw());
    return out;
}

}  // namespace hyperflint
