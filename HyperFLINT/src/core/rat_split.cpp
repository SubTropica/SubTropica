// rat_split: Phase-A commit (2) implementation.
//
// Splits a wide-ctx Rat into a list of (num_N, num_zw, den_zw) leaves
// and recombines back via the ZWTable. The round-trip is bit-identical
// to the source Rat under the design v2 §3.5 invariants.
//
// Folds adversarial-reviewer 2026-05-06 suggestions:
//   - explicit sign-fold convention applied here (denominator
//     pre-canonicalized via leading-coef sign check before intern),
//     not relegated to ZWTable::intern;
//   - free functions (kept here so SymCoef does not gain a
//     transitive include of <core/zw_table.hpp>);
//   - clear naming (`split_rat_by_w_monomial`).

#include "hyperflint/core/rat_split.hpp"

#include "hyperflint/core/env_flags_rat.hpp"

#include <flint/fmpq.h>
#include <flint/fmpq_mpoly.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace hyperflint {

namespace {

// Lex compare for ulong-vector exponent tuples; needed as the std::map
// key when binning numerator monomials by their W-exponent.
struct ULongVecLess {
    bool operator()(const std::vector<ulong>& a,
                    const std::vector<ulong>& b) const {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end());
    }
};

// Convenience: build a Poly over `ctx` from a single fmpq term and
// an exponent vector.
Poly poly_one_term(const PolyCtx& ctx, const fmpq_t coef,
                   const std::vector<ulong>& exps) {
    Poly p(ctx);
    fmpq_mpoly_push_term_fmpq_ui(p.raw(), coef, exps.data(), ctx.raw());
    return p;
}

// ----------------------------------------------------------------------
// Iter-35 PIVOTED probe (drift-check 35 advisory pivot; internal review):
// env-gated wall-share probe for
// `split_rat_by_w_monomial`. Decides whether the iter-22 amendment 6.3
// fallback predicate has a perf lever on tst2 (or is correctness-only
// for C0a). Mirrors iter-34 runtime/zw_aggregate.cpp at-exit pattern.
// Default-OFF; set HF_SPLIT_RAT_PROFILE=1 to enable. Build cost when
// off: one env-var read on first call (cached for process lifetime),
// one branch per call (predicted not-taken).
//
// iter-87 (§T7 17th chunk Track-rat-profile macro-layer LAND): the
// std::getenv literal is now consumed via HF_FLAG_SPLIT_RAT_PROFILE
// from hyperflint/core/env_flags_rat.hpp (the iter-74-vintage VALUE-
// family core/rat-subsystem env-flag header, 2nd extension after
// iter-84). Header total: 6 macros across 4 doc-§2 Tracks.
// Default-direction matches docs/env_flags.md §2 row 254 verbatim
// (unset/empty/"0" -> OFF; any other value -> ON); no doc/code
// discrepancy this Track.
// ----------------------------------------------------------------------

std::atomic<uint64_t> g_split_rat_ns_total{0};
std::atomic<uint64_t> g_split_rat_calls_total{0};

bool split_rat_profile_enabled() {
    static const bool on = []() {
        const char* s = HF_FLAG_SPLIT_RAT_PROFILE;
        if (s == nullptr || *s == '\0') return false;
        if (s[0] == '0' && s[1] == '\0') return false;
        return true;
    }();
    return on;
}

struct SplitRatProfileTimer {
    bool enabled;
    std::chrono::steady_clock::time_point t0;
    SplitRatProfileTimer() : enabled(split_rat_profile_enabled()) {
        if (enabled) t0 = std::chrono::steady_clock::now();
    }
    ~SplitRatProfileTimer() {
        if (!enabled) return;
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t dt_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count());
        g_split_rat_ns_total.fetch_add(dt_ns, std::memory_order_relaxed);
        g_split_rat_calls_total.fetch_add(1, std::memory_order_relaxed);
    }
};

struct SplitRatProfileAtExit {
    ~SplitRatProfileAtExit() {
        if (!split_rat_profile_enabled()) return;
        const uint64_t total_ns =
            g_split_rat_ns_total.load(std::memory_order_relaxed);
        const uint64_t calls =
            g_split_rat_calls_total.load(std::memory_order_relaxed);
        std::fprintf(stderr,
            "hf_split_rat_profile: total_ns=%llu calls=%llu wall_s=%.6f "
            "mean_us=%.3f\n",
            static_cast<unsigned long long>(total_ns),
            static_cast<unsigned long long>(calls),
            static_cast<double>(total_ns) * 1e-9,
            calls == 0
                ? 0.0
                : (static_cast<double>(total_ns) * 1e-3 /
                   static_cast<double>(calls)));
    }
};

SplitRatProfileAtExit g_split_rat_atexit_emitter;

}  // namespace

FNIndexMaps build_fn_index_maps(const PolyCtx& F, const PolyCtx& N) {
    FNIndexMaps m;
    m.F_to_N_idx.assign(F.vars().size(), SIZE_MAX);
    m.N_to_F_idx.assign(N.vars().size(), SIZE_MAX);
    for (size_t j = 0; j < N.vars().size(); ++j) {
        const std::string& nv = N.vars()[j];
        size_t f_idx = F.index_of(nv);
        if (f_idx == SIZE_MAX) {
            throw std::runtime_error(
                "build_fn_index_maps: narrow-ctx variable '" + nv +
                "' not present in wide ctx");
        }
        m.N_to_F_idx[j] = f_idx;
        m.F_to_N_idx[f_idx] = j;
    }
    return m;
}

std::vector<SymMonomialSplit>
split_rat_by_w_monomial(const Rat& r_F,
                        const PolyCtx& N,
                        ZWTable& tab,
                        const std::vector<size_t>& F_to_N_idx) {
    // Iter-35 PIVOTED probe: env-gated wall-share timer. Default-OFF;
    // RAII timer cost when off is one bool-init + one early-return on
    // dtor. See file-level probe block above.
    SplitRatProfileTimer _split_rat_profile_timer;

    const PolyCtx& F = r_F.ctx();
    if (F_to_N_idx.size() != F.vars().size()) {
        throw std::runtime_error(
            "split_rat_by_w_monomial: F_to_N_idx size mismatch");
    }
    if (r_F.den().is_zero()) {
        throw std::runtime_error(
            "split_rat_by_w_monomial: source Rat has zero denominator");
    }

    // Sign-canonicalize the denominator. Rat's invariants normally
    // guarantee positive leading coef, but defense-in-depth: if the
    // denominator is negative-leading, negate both num and den
    // before splitting (the convention is documented in design v2
    // §3.4 sign-fold invariant).
    Poly den_F = r_F.den();
    Poly num_F = r_F.num();
    if (den_F.leading_coef_is_negative()) {
        den_F = den_F.neg();
        num_F = num_F.neg();
    }

    // Intern the denominator whole as a single wide-ctx ZWTable entry.
    ZWHandle den_zw = tab.intern(std::move(den_F), ZWTable::Intent::DenIntent);

    if (num_F.is_zero()) {
        // Trivial: zero numerator → empty split list.
        return {};
    }

    // Walk numerator monomials. For each monomial, split its exponent
    // vector by F_to_N_idx into (e_N over N) + (e_W over W). Bin by
    // e_W; build a per-bin narrow-ctx polynomial (sum of coef * x^e_N).
    fmpq_mpoly_struct* raw_num = num_F.raw();
    const fmpq_mpoly_ctx_struct* F_ctx_raw = F.raw();
    const slong nterms = raw_num->zpoly->length;
    const size_t F_nv = F.vars().size();
    const size_t N_nv = N.vars().size();

    // Use a std::map keyed by the W-exponent tuple to keep deterministic
    // (lex) ordering on output for sha256 stability.
    std::map<std::vector<ulong>, Poly, ULongVecLess> bins;

    std::vector<ulong> term_exps(F_nv);
    std::vector<ulong> e_N(N_nv);
    std::vector<ulong> e_W(F_nv);  // dense over F's nvars; W-positions only nonzero
    fmpq_t c;
    fmpq_init(c);
    for (slong i = 0; i < nterms; ++i) {
        fmpq_mpoly_get_term_exp_ui(term_exps.data(), raw_num, i, F_ctx_raw);
        fmpq_mpoly_get_term_coeff_fmpq(c, raw_num, i, F_ctx_raw);

        std::fill(e_N.begin(), e_N.end(), 0UL);
        std::fill(e_W.begin(), e_W.end(), 0UL);
        for (size_t j = 0; j < F_nv; ++j) {
            if (term_exps[j] == 0) continue;
            const size_t n_idx = F_to_N_idx[j];
            if (n_idx == SIZE_MAX) {
                e_W[j] = term_exps[j];
            } else {
                e_N[n_idx] = term_exps[j];
            }
        }

        // Build (or extend) the bin keyed by `e_W`. The key vector
        // is the dense F-length exponent with N-positions zeroed —
        // this preserves the W-monomial's position in F's vars()
        // (so when we later intern it, the wide-ctx layout matches).
        auto it = bins.find(e_W);
        if (it == bins.end()) {
            it = bins.emplace(e_W, Poly(N)).first;
        }
        // Push the (c, e_N) term into the bin's narrow-ctx poly.
        fmpq_mpoly_push_term_fmpq_ui(it->second.raw(), c, e_N.data(),
                                     N.raw());
    }
    fmpq_clear(c);

    // Finalize each bin's poly (sort + combine).
    for (auto& kv : bins) {
        fmpq_mpoly_sort_terms(kv.second.raw(), N.raw());
        fmpq_mpoly_combine_like_terms(kv.second.raw(), N.raw());
    }

    // Emit one SymMonomialSplit per bin. Intern each W-monomial as a
    // single-term wide-ctx Poly via NumIntent.
    std::vector<SymMonomialSplit> out;
    out.reserve(bins.size());

    fmpq_t one;
    fmpq_init(one);
    fmpq_set_si(one, 1, 1);
    for (auto& kv : bins) {
        const std::vector<ulong>& e_W_dense = kv.first;
        // Build the W-only monomial as a wide-ctx Poly with coef 1.
        Poly w_mono = poly_one_term(F, one, e_W_dense);
        fmpq_mpoly_sort_terms(w_mono.raw(), F.raw());
        fmpq_mpoly_combine_like_terms(w_mono.raw(), F.raw());

        ZWHandle num_zw = tab.intern(std::move(w_mono),
                                     ZWTable::Intent::NumIntent);
        out.push_back(SymMonomialSplit{
            std::move(kv.second), num_zw, den_zw});
    }
    fmpq_clear(one);
    return out;
}

Rat recombine_rat_split(const std::vector<SymMonomialSplit>& parts,
                        const PolyCtx& F,
                        ZWTable& tab,
                        const std::vector<size_t>& N_to_F_idx) {
    if (parts.empty()) {
        // Convention: empty split list represents the zero Rat over F.
        return Rat::zero_of(F);
    }
    // All splits share the same `den_zw` by construction. Cross-merge
    // (commit 4) may produce SymMonomialSplits with different den_zw
    // handles; for the Phase-A round trip those are not exercised.
    const ZWHandle den_zw = parts.front().den_zw;
    for (const auto& s : parts) {
        if (s.den_zw != den_zw) {
            throw std::runtime_error(
                "recombine_rat_split: split list mixes denominator "
                "handles; not supported in commit (2) scope");
        }
    }

    // Accumulate the wide-ctx numerator: sum over splits of
    //   transplant(num_N -> F) * tab.get(num_zw).
    Poly num_F = Poly::zero_of(F);
    for (const auto& s : parts) {
        Poly num_N_in_F = s.num_N.transplant(F, N_to_F_idx);
        const Poly& w_mono = tab.get(s.num_zw);
        Poly term = num_N_in_F.mul(w_mono);
        num_F = num_F.add(term);
    }

    // Compose num_F with den_F via Rat::from_canonical (no further
    // reduce) — the Rat ctor would re-run reduce_inplace, which
    // would undo any structural choice the source rep had made.
    // For the round-trip, we explicitly use the (num, den) ctor that
    // does run reduce_inplace, since the source Rat is already
    // canonical and reduce on a canonical Rat is a no-op (gcd = 1).
    const Poly& den_F = tab.get(den_zw);
    return Rat(std::move(num_F), den_F);
}

}  // namespace hyperflint
