// bench_polymul.cpp
//
// Microbenchmark: Poly::mul wide path vs Avenue A narrow-ctx hoist path,
// at varied (nvars_wide, nvars_used, la, lb).  Diagnostic for the
// 3l3pt step-4 +240 us/call regression with HF_MUL_NARROW=1.
//
// Measures two paths for each grid point:
//   wide_us   - direct fmpq_mpoly_mul on wide-ctx polys.
//   narrow_us - replicate Avenue A's hot path manually:
//                 PolyCtx narrow(used_var_names);
//                 a_n = a.transplant(narrow, wide_to_narrow);
//                 b_n = b.transplant(narrow, wide_to_narrow);
//                 fmpq_mpoly_mul(r_n, a_n, b_n, narrow);
//                 result = r_n.transplant(wide, narrow_to_wide);
//               (matches src/core/poly.cpp:276-302 exactly, including
//                per-call PolyCtx construction.)
//
// CSV output to stdout: nvars_wide,nvars_used,la,lb,wide_us,narrow_us,ratio,K

#include "hyperflint/core/poly.hpp"

#include <flint/fmpq_mpoly.h>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace hf = hyperflint;
using clk = std::chrono::steady_clock;

// Build a sparse poly with `len` distinct monomials in vars 0..nvars_used-1
// of the wide ctx.  Coefficients in [-9,9]\{0}, exponents in [0,3].
// Uses fmpq_mpoly_set_str_pretty rather than coeff-by-coeff because
// Avenue A operates on parsed inputs in practice; the cost is
// construction-only, not measured.
static hf::Poly make_random(const hf::PolyCtx& ctx,
                            std::size_t len,
                            std::size_t nvars_used,
                            std::mt19937& rng) {
    std::uniform_int_distribution<int> coef_d(1, 9);
    std::uniform_int_distribution<int> sgn_d(0, 1);
    std::uniform_int_distribution<int> exp_d(0, 3);
    std::ostringstream s;
    for (std::size_t i = 0; i < len; ++i) {
        if (i > 0) s << "+";
        int c = coef_d(rng);
        if (sgn_d(rng)) s << "-" << c; else s << c;
        // Force at least one variable per monomial so they are distinct
        // enough to survive fmpq_mpoly's canonical merge.
        bool any_var = false;
        for (std::size_t v = 0; v < nvars_used; ++v) {
            int e = exp_d(rng);
            // Bias slot (i mod nvars_used) to e>=1 so we get `len` terms.
            if (v == (i % nvars_used) && e == 0) e = 1;
            if (e > 0) {
                s << "*" << ctx.vars()[v];
                if (e > 1) s << "^" << e;
                any_var = true;
            }
        }
        // Constant terms collapse — skip if first; if mid-string we just
        // accept a coefficient bias on slot-0.
        if (!any_var) s << "*" << ctx.vars()[0];
    }
    return hf::Poly(ctx, s.str());
}

// Time one bin.  Uses an inner repeat count K chosen so the bin runs at
// least ~1ms total (so timer noise is negligible).
struct BinResult { double wide_us, narrow_us; int K; };
static BinResult bench_bin(const hf::Poly& a,
                           const hf::Poly& b,
                           const hf::PolyCtx& wide,
                           std::size_t nvars_used) {
    // Pre-build the narrow var name list.  But Avenue A reconstructs
    // PolyCtx per call — we mirror that, so build outside is for the
    // wide_to_narrow mapping only.
    std::vector<std::string> narrow_var_names;
    narrow_var_names.reserve(nvars_used);
    for (std::size_t i = 0; i < nvars_used; ++i)
        narrow_var_names.push_back(wide.vars()[i]);
    std::vector<std::size_t> wide_to_narrow(wide.vars().size(), SIZE_MAX);
    for (std::size_t i = 0; i < nvars_used; ++i) wide_to_narrow[i] = i;
    std::vector<std::size_t> narrow_to_wide(nvars_used);
    for (std::size_t i = 0; i < nvars_used; ++i) narrow_to_wide[i] = i;

    // Adaptive K: target ~5 ms per arm, min 5 iters, max 5000.
    int K = 100;
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
        if (K > 5000) K = 5000;
    }

    // Wide arm.
    auto t0 = clk::now();
    for (int i = 0; i < K; ++i) {
        hf::Poly r(wide);
        fmpq_mpoly_mul(r.raw(), a.raw(), b.raw(), wide.raw());
    }
    auto t1 = clk::now();
    double wide_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / K;

    // Narrow arm — full Avenue A round-trip per call.
    auto t2 = clk::now();
    for (int i = 0; i < K; ++i) {
        std::vector<std::string> nv = narrow_var_names;
        const hf::PolyCtx narrow(std::move(nv));
        hf::Poly a_n = a.transplant(narrow, wide_to_narrow);
        hf::Poly b_n = b.transplant(narrow, wide_to_narrow);
        hf::Poly r_n(narrow);
        fmpq_mpoly_mul(r_n.raw(), a_n.raw(), b_n.raw(), narrow.raw());
        hf::Poly result = r_n.transplant(wide, narrow_to_wide);
    }
    auto t3 = clk::now();
    double narrow_us = std::chrono::duration<double, std::micro>(t3 - t2).count() / K;

    return {wide_us, narrow_us, K};
}

int main() {
    std::cout << "nvars_wide,nvars_used,la,lb,wide_us,narrow_us,ratio,K\n";

    std::mt19937 rng(42);

    // Cover step-3-style (large nvars_wide, low nvars_used, la,lb large)
    // and step-4-style (large nvars_wide, low nvars_used, asymmetric la<<lb).
    std::vector<std::size_t> wide_sizes  = {20, 50, 100, 200};
    std::vector<std::size_t> used_sizes  = {1, 2, 3, 4, 6};
    std::vector<std::size_t> mono_sizes  = {1, 2, 4, 8, 16, 32, 64, 128};

    for (std::size_t W : wide_sizes) {
        std::vector<std::string> wvars;
        wvars.reserve(W);
        for (std::size_t i = 0; i < W; ++i) wvars.push_back("x" + std::to_string(i));
        const hf::PolyCtx wide(std::move(wvars));

        for (std::size_t U : used_sizes) {
            if (U > W) continue;
            for (std::size_t la : mono_sizes) {
                for (std::size_t lb : mono_sizes) {
                    // Build operands once per bin; sizes la,lb in U vars.
                    hf::Poly a = make_random(wide, la, U, rng);
                    hf::Poly b = make_random(wide, lb, U, rng);
                    BinResult br = bench_bin(a, b, wide, U);
                    double ratio = br.wide_us > 0 ? br.narrow_us / br.wide_us : 0.0;
                    std::cout << W << "," << U << "," << la << "," << lb
                              << "," << br.wide_us << "," << br.narrow_us
                              << "," << ratio << "," << br.K << "\n";
                    std::cout.flush();
                }
            }
        }
    }
    return 0;
}
