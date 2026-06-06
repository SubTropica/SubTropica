// bench_polymul_v2.cpp — production-realistic Poly::mul wide-vs-narrow
//
// Round 1 microbench (bench_polymul.cpp) used exp ∈ [0,3], which makes
// FLINT pick bits/exp ≤ 2 and hides the bit-packing payoff that
// Avenue A actually exploits in 3l3pt step 3.  Reviewer (round 6)
// pinned this as the inversion source: at production bits=8, narrow
// saves words_per_exp by 25:1 not 10:1.
//
// This bench mirrors that production regime by:
//   - Sampling exponents from {[0,8] | [0,16] | [0,64]} (FLINT bits = 4, 5, 7).
//   - Sampling coefficient heights from {1, 10, 50, 100, 250} bits.
//   - Output schema matches HF_DUMP_MUL's CSV exactly so the same
//     analysis script consumes both.
//
// The HF_DUMP_MUL instrumentation is skipped here (the bench drives
// both paths explicitly).  CSV columns:
//   seq,la,lb,nvars_wide,used_count,worth_narrowing,
//   max_deg_a,max_deg_b,max_coef_bits,total_coef_bits,
//   narrow_us,wide_us

#include "hyperflint/core/poly.hpp"

#include <flint/fmpq_mpoly.h>
#include <flint/fmpz.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace hf = hyperflint;
using clk = std::chrono::steady_clock;

// Build a sparse poly with `len` requested monomials in vars 0..nvars_used-1.
// Exponents drawn uniformly from [0, exp_max].  Coefficient height
// (bits) drawn from `coef_bits` (one fixed value).  Uses signed
// numerators with random num/den for more realistic structure.
static hf::Poly make_random(const hf::PolyCtx& ctx,
                            std::size_t len,
                            std::size_t nvars_used,
                            int exp_max,
                            int coef_bits,
                            std::mt19937& rng) {
    std::uniform_int_distribution<int> exp_d(0, exp_max);
    std::uniform_int_distribution<int> sgn_d(0, 1);
    std::uniform_int_distribution<unsigned long> bit_word(0, ~0UL);

    std::ostringstream s;
    for (std::size_t i = 0; i < len; ++i) {
        // Build a positive integer with ~coef_bits significant bits.
        // Up to 60 bits fits a ulong; larger needs concatenation.
        std::string coef_str;
        if (coef_bits <= 60) {
            unsigned long w = bit_word(rng);
            unsigned long mask = (coef_bits >= 60) ? ~0UL : ((1UL << coef_bits) - 1UL);
            w &= mask;
            if (w == 0) w = 1;
            coef_str = std::to_string(w);
        } else {
            // Concatenate two 60-bit words to produce up to ~120 bits.
            unsigned long hi = bit_word(rng) & ((1UL << 60) - 1UL);
            unsigned long lo = bit_word(rng) & ((1UL << 60) - 1UL);
            if (hi == 0) hi = 1;
            // 2^60 = 1152921504606846976
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%lu", hi);
            std::string h(buf);
            std::snprintf(buf, sizeof(buf), "%020lu", lo);
            std::string l(buf);
            // Approximate concatenation via decimal: hi * 10^18 + lo;
            // This isn't exact ~bits but ballpark large.  Good enough.
            coef_str = h + l;
        }
        if (i > 0) s << "+";
        if (sgn_d(rng)) s << "-" << coef_str; else s << coef_str;
        bool any_var = false;
        for (std::size_t v = 0; v < nvars_used; ++v) {
            int e = exp_d(rng);
            if (v == (i % nvars_used) && e == 0) e = 1;
            if (e > 0) {
                s << "*" << ctx.vars()[v];
                if (e > 1) s << "^" << e;
                any_var = true;
            }
        }
        if (!any_var) s << "*" << ctx.vars()[0];
    }
    return hf::Poly(ctx, s.str());
}

// Compute the same features HF_DUMP_MUL records, so the CSV parses the
// same way.
struct Features {
    long max_deg_a, max_deg_b;
    long max_coef_bits, total_coef_bits;
};
static Features compute_features(const hf::Poly& a, const hf::Poly& b,
                                 const hf::PolyCtx& wide) {
    Features f{0, 0, 0, 0};
    f.max_deg_a = fmpq_mpoly_total_degree_si(a.raw(), wide.raw());
    f.max_deg_b = fmpq_mpoly_total_degree_si(b.raw(), wide.raw());

    auto coef_walk = [&](const hf::Poly& p) -> std::pair<long, long> {
        long mx = 0, tot = 0;
        const slong n = fmpq_mpoly_length(p.raw(), wide.raw());
        fmpq_t c; fmpq_init(c);
        for (slong i = 0; i < n; ++i) {
            fmpq_mpoly_get_term_coeff_fmpq(c, p.raw(), i, wide.raw());
            long nb = static_cast<long>(fmpz_bits(fmpq_numref(c)));
            long db = static_cast<long>(fmpz_bits(fmpq_denref(c)));
            long mb = nb > db ? nb : db;
            if (mb > mx) mx = mb;
            tot += mb;
        }
        fmpq_clear(c);
        return {mx, tot};
    };
    auto pa = coef_walk(a), pb = coef_walk(b);
    f.max_coef_bits = pa.first > pb.first ? pa.first : pb.first;
    f.total_coef_bits = pa.second + pb.second;
    return f;
}

struct BinResult { double wide_us, narrow_us; int K; bool worth; std::size_t used_count; };
static BinResult bench_bin(const hf::Poly& a,
                           const hf::Poly& b,
                           const hf::PolyCtx& wide,
                           std::size_t nvars_used) {
    // Determine actual used_count by intersecting both operands' supports.
    const std::size_t W = wide.vars().size();
    std::vector<int> used(W, 0);
    fmpq_mpoly_used_vars(used.data(),
        const_cast<fmpq_mpoly_struct*>(a.raw()), wide.raw());
    {
        std::vector<int> usedb(W, 0);
        fmpq_mpoly_used_vars(usedb.data(),
            const_cast<fmpq_mpoly_struct*>(b.raw()), wide.raw());
        for (std::size_t j = 0; j < W; ++j) if (usedb[j]) used[j] = 1;
    }
    std::vector<std::size_t> used_wide;
    for (std::size_t j = 0; j < W; ++j) if (used[j]) used_wide.push_back(j);

    const bool worth = used_wide.size() * 4 < W;

    std::vector<std::string> narrow_var_names;
    narrow_var_names.reserve(used_wide.size());
    std::vector<std::size_t> wide_to_narrow(W, SIZE_MAX);
    for (std::size_t k = 0; k < used_wide.size(); ++k) {
        narrow_var_names.push_back(wide.vars()[used_wide[k]]);
        wide_to_narrow[used_wide[k]] = k;
    }

    int K = 50;
    {
        auto t0 = clk::now();
        for (int i = 0; i < 5; ++i) {
            hf::Poly r(wide);
            fmpq_mpoly_mul(r.raw(), a.raw(), b.raw(), wide.raw());
        }
        auto t1 = clk::now();
        double per_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / 5.0;
        if (per_us > 0) K = static_cast<int>(5000.0 / per_us);
        if (K < 5) K = 5;
        if (K > 2000) K = 2000;
    }

    auto t0 = clk::now();
    for (int i = 0; i < K; ++i) {
        hf::Poly r(wide);
        fmpq_mpoly_mul(r.raw(), a.raw(), b.raw(), wide.raw());
    }
    auto t1 = clk::now();
    double wide_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / K;

    double narrow_us = -1.0;
    if (worth) {
        auto t2 = clk::now();
        for (int i = 0; i < K; ++i) {
            std::vector<std::string> nv = narrow_var_names;
            const hf::PolyCtx narrow(std::move(nv));
            hf::Poly a_n = a.transplant(narrow, wide_to_narrow);
            hf::Poly b_n = b.transplant(narrow, wide_to_narrow);
            hf::Poly r_n(narrow);
            fmpq_mpoly_mul(r_n.raw(), a_n.raw(), b_n.raw(), narrow.raw());
            std::vector<std::size_t> narrow_to_wide(used_wide);
            hf::Poly result = r_n.transplant(wide, narrow_to_wide);
            (void)result;
        }
        auto t3 = clk::now();
        narrow_us = std::chrono::duration<double, std::micro>(t3 - t2).count() / K;
    }

    return {wide_us, narrow_us, K, worth, used_wide.size()};
}

int main(int argc, char** argv) {
    std::printf("seq,la,lb,nvars_wide,used_count,worth_narrowing,"
                "max_deg_a,max_deg_b,max_coef_bits,total_coef_bits,"
                "narrow_us,wide_us\n");
    std::fflush(stdout);

    std::mt19937 rng(13);

    std::vector<std::size_t> wide_sizes  = {30, 80, 200};
    std::vector<std::size_t> used_sizes  = {2, 3, 4, 6};
    std::vector<std::size_t> mono_sizes  = {2, 4, 16, 64, 256};
    std::vector<int>         exp_maxes   = {3, 8, 16};      // forces bits=2,4,5
    std::vector<int>         coef_bitset = {4, 30, 100};    // small / mid / large

    long seq = 0;
    for (std::size_t W : wide_sizes) {
        std::vector<std::string> wvars;
        wvars.reserve(W);
        for (std::size_t i = 0; i < W; ++i) wvars.push_back("x" + std::to_string(i));
        const hf::PolyCtx wide(std::move(wvars));

        for (std::size_t U : used_sizes) {
            if (U > W) continue;
            for (int em : exp_maxes) {
                for (int cb : coef_bitset) {
                    for (std::size_t la : mono_sizes) {
                        for (std::size_t lb : mono_sizes) {
                            hf::Poly a = make_random(wide, la, U, em, cb, rng);
                            hf::Poly b = make_random(wide, lb, U, em, cb, rng);
                            Features f = compute_features(a, b, wide);
                            BinResult br = bench_bin(a, b, wide, U);
                            std::printf("%ld,%zu,%zu,%zu,%zu,%d,"
                                        "%ld,%ld,%ld,%ld,%.3f,%.3f\n",
                                        seq++,
                                        la, lb, W, br.used_count,
                                        br.worth ? 1 : 0,
                                        f.max_deg_a, f.max_deg_b,
                                        f.max_coef_bits, f.total_coef_bits,
                                        br.narrow_us, br.wide_us);
                            std::fflush(stdout);
                        }
                    }
                }
            }
        }
    }
    return 0;
}
