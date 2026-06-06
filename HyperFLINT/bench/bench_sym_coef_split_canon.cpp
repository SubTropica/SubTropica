// bench_sym_coef_split_canon.cpp
//
// HF MZV-rewrite C-prep.3 microbench (notes/hf_mzv_rewrite_design_2026-05-05/
// three_month_checkpoint/measurement_plan_2026-05-06.md §4 row C-prep.3,
// reviewer E2 advisory iter-22 amendment commit 2d76917b4).
//
// Times SymCoefSplit::add(a, b) -> canonicalize on synthetic non-W-empty
// inputs, sweeping N_terms (per-operand SymMonomial count) and a small
// set of structural parameters that control how many distinct SplitKeys
// the canonicalize pass actually produces:
//
//   N_terms         : per-operand SymMonomial count, controls operand size.
//   overlap_frac    : fraction of (a's keys) that are shared with (b's
//                     keys); 1.0 means every term in a has a like-key
//                     partner in b (canonicalize halves output size); 0.0
//                     means disjoint key sets (output size = 2 * N_terms).
//   K_w_distinct    : how many distinct W-monomial handles per operand
//                     (cycles through a small set of W exponents).
//   num_N_density   : how many narrow-ctx monomials per Rat numerator (
//                     1, 4, 16) — controls per-bin Poly::add cost.
//
// The bench is a pre-C0 sizing probe. It does NOT modify any production
// code path; it only exercises core/sym_coef_split.cpp primitives that
// are already unit-tested via test/unit/test_sym_coef_split_roundtrip.cpp.
//
// Key sensitivity question (reviewer E2): does canonicalize blow up
// O(N log N * log K) on non-trivial N? The std::map<SplitKey, Poly>
// implementation in src/core/sym_coef_split.cpp:265-290 is the reviewer's
// flagged hotspot. If add+canonicalize cost grows superlinearly in
// N_terms at modest num_N_density, that informs whether C0 needs a
// canonicalize-op rewrite (e.g. hash-based bins) BEFORE the per-B-commit
// caller-side refactors begin.
//
// CSV output to stdout:
//   N_terms,overlap_frac,K_w_distinct,num_N_density,
//   add_canon_us_per_call,canon_only_us_per_call,K_iters,a_terms,b_terms,
//   sum_terms_in,canon_terms_out
//
// Prefactor format: the wide ctx F has variables {x1, x2, eps, s12, s13,
// m1}; the narrow ctx N has {x1, x2, eps}. Every prefactor Rat splits
// cleanly into (num_N over N) / (W-only denom over F) by
// split_rat_by_w_monomial.
//
// Bench-only.

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/sym_coef_split.hpp"
#include "hyperflint/core/symcoef.hpp"
#include "hyperflint/core/zw_table.hpp"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace hf = hyperflint;
using clk = std::chrono::steady_clock;

namespace {

// Cycle of W-only monomials (numerator side). Produces K_w_distinct
// distinct strings.
std::string w_monomial(std::size_t idx) {
    static const char* w_atoms[] = {
        "1",
        "s12",
        "s13",
        "m1",
        "s12*s13",
        "s12*m1",
        "s13*m1",
        "s12*s13*m1",
    };
    return w_atoms[idx % (sizeof(w_atoms) / sizeof(w_atoms[0]))];
}

// Narrow-ctx numerator polynomial of size `density`. Every monomial uses
// only x1, x2, eps so split_rat_by_w_monomial routes them all into the
// same num_N (W-exponent zero on the narrow side).
std::string narrow_num(std::size_t density, std::size_t seed) {
    std::ostringstream s;
    for (std::size_t k = 0; k < density; ++k) {
        if (k > 0) s << "+";
        long c = static_cast<long>((seed * 7 + k * 11) % 19) + 1;
        s << c;
        std::size_t e1 = (seed + k) % 4;
        std::size_t e2 = (seed * 3 + k * 5) % 3;
        std::size_t ee = (seed + 2 * k) % 4;
        if (e1 > 0) { s << "*x1"; if (e1 > 1) s << "^" << e1; }
        if (e2 > 0) { s << "*x2"; if (e2 > 1) s << "^" << e2; }
        if (ee > 0) { s << "*eps"; if (ee > 1) s << "^" << ee; }
    }
    if (density == 0) s << "1";
    return s.str();
}

// Build a SymMonomial over the wide ctx F whose prefactor = (narrow_num)
// * w_monomial(w_idx) / (1 + s12). The transcendental flags are set
// from `key_idx` so different `key_idx` values produce different
// SplitKeys.
//
// `key_diversity` (D) caps the SplitKey count by hashing key_idx into
// the range [0, D); the bit allocation below spans 3 * 2^8 = 768
// distinct (pi, i, 4×log, 3×delta) tuples, so D ≤ 768 yields exactly
// min(N_terms, D) distinct SplitKeys. D > 768 saturates at 768.
//
// Caveat-1 follow-up addition (iter-24): the original cycling used
// only 5 axes spanning 3*2^4 = 48 unique tuples, capping the bench
// at SplitKey diversity ~60 which left the > 60 regime unmeasured
// (memo SUMMARY caveat 1). The 9-axis allocation here lets the bench
// probe up to ~768 distinct SplitKeys per operand, addressing that
// gap while keeping the same `make_term` call shape.
hf::SymMonomial make_term(const hf::PolyCtx& F,
                          std::size_t key_idx,
                          std::size_t w_idx,
                          std::size_t num_N_density,
                          std::size_t seed,
                          std::size_t key_diversity) {
    std::string nw = narrow_num(num_N_density, seed);
    std::string w  = w_monomial(w_idx);
    std::ostringstream r;
    r << "(" << nw << ")*(" << w << ") / (1 + s12)";
    hf::SymMonomial m(hf::Rat::parse(F, r.str()));
    std::size_t D = (key_diversity == 0) ? 1 : key_diversity;
    std::size_t k = key_idx % D;
    // 9-axis bit allocation, total span = 3 * 2^8 = 768 unique tuples.
    m.pi_power           = static_cast<int>(k % 3);
    m.i_power            = static_cast<int>((k /   3) % 2);
    m.log_powers[2]      = static_cast<int>((k /   6) % 2);
    m.log_powers[3]      = static_cast<int>((k /  12) % 2);
    m.log_powers[5]      = static_cast<int>((k /  24) % 2);
    m.log_powers[7]      = static_cast<int>((k /  48) % 2);
    m.delta_powers["x1"] = static_cast<int>((k /  96) % 2);
    m.delta_powers["x2"] = static_cast<int>((k / 192) % 2);
    m.delta_powers["eps"] = static_cast<int>((k / 384) % 2);
    return m;
}

hf::SymCoefSplit build_operand(const hf::PolyCtx& F,
                               const hf::PolyCtx& N,
                               std::shared_ptr<hf::ZWTable> tab,
                               std::size_t N_terms,
                               std::size_t K_w_distinct,
                               std::size_t num_N_density,
                               std::size_t key_offset,
                               std::size_t key_diversity) {
    std::vector<hf::SymMonomial> mons;
    mons.reserve(N_terms);
    for (std::size_t i = 0; i < N_terms; ++i) {
        std::size_t key_idx = key_offset + i;
        std::size_t w_idx   = i % K_w_distinct;
        std::size_t seed    = i * 13 + 1;
        mons.push_back(make_term(F, key_idx, w_idx, num_N_density,
                                 seed, key_diversity));
    }
    hf::SymCoef sc = hf::SymCoef::from_monomials(F, std::move(mons));
    return hf::SymCoefSplit::from_rat(sc, N, tab);
}

struct BinResult {
    double add_canon_us;
    double canon_only_us;
    int    K;
    std::size_t a_terms;
    std::size_t b_terms;
    std::size_t canon_out_terms;
};

BinResult bench_bin(const hf::SymCoefSplit& a,
                    const hf::SymCoefSplit& b) {
    // Adaptive K (target ~10 ms per arm; min 3, max 1000).
    int K = 100;
    {
        auto t0 = clk::now();
        for (int i = 0; i < 3; ++i) {
            hf::SymCoefSplit r = a.add(b);
            (void)r;
        }
        auto t1 = clk::now();
        double per_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / 3.0;
        if (per_us > 0) K = static_cast<int>(10000.0 / per_us);
        if (K < 3)    K = 3;
        if (K > 1000) K = 1000;
    }

    // add+canonicalize arm (the cumulative cost; add() ends with
    // canonicalize() per src/core/sym_coef_split.cpp:135).
    auto t0 = clk::now();
    hf::SymCoefSplit last = a.add(b);
    for (int i = 1; i < K; ++i) {
        hf::SymCoefSplit r = a.add(b);
        last = std::move(r);
    }
    auto t1 = clk::now();
    double add_canon_us =
        std::chrono::duration<double, std::micro>(t1 - t0).count() / K;

    // canonicalize-only arm — feed the post-add (already-canonical)
    // SymCoefSplit through canonicalize() again to isolate the second-
    // pass cost (the typical Phase-C call site is canonicalize-on-
    // already-canonical input, e.g. the `out.canonicalize()` line at
    // end of add()).
    auto t2 = clk::now();
    for (int i = 0; i < K; ++i) {
        hf::SymCoefSplit r = last.canonicalize();
        (void)r;
    }
    auto t3 = clk::now();
    double canon_only_us =
        std::chrono::duration<double, std::micro>(t3 - t2).count() / K;

    return {add_canon_us, canon_only_us, K,
            a.terms().size(), b.terms().size(), last.terms().size()};
}

}  // namespace

int main(int argc, char** argv) {
    // Wide ctx: x1, x2, eps narrow; s12, s13, m1 W-side.
    hf::PolyCtx F({"x1", "x2", "eps", "s12", "s13", "m1"});
    hf::PolyCtx N({"x1", "x2", "eps"});

    // Default outer sweep over SplitKey diversity caps. Caveat-1
    // follow-up: D=768 saturates the 9-axis bit allocation in
    // make_term; D ∈ {12, 60, 300, 768} probes the >60 regime
    // that the iter-23 bench (D effectively ~48) did not reach.
    // CLI override: a single positional integer arg overrides the
    // sweep with a single-element D = that value (clamped to
    // [1, 768]). This is a sweep tool, not a microbenchmark
    // harness; argv parsing is intentionally minimal.
    std::vector<std::size_t> D_sweep = {12, 60, 300, 768};
    if (argc >= 2) {
        long v = std::stol(argv[1]);
        if (v < 1)   v = 1;
        if (v > 768) v = 768;
        D_sweep = {static_cast<std::size_t>(v)};
    }

    std::cout << "key_diversity,N_terms,overlap_frac,K_w_distinct,num_N_density,"
              << "add_canon_us,canon_only_us,K_iters,"
              << "a_terms,b_terms,canon_out_terms\n";

    const std::vector<std::size_t> N_sweep =
        {1, 5, 10, 50, 100, 500, 1000};
    const std::vector<double>      overlap_sweep =
        {0.0, 0.5, 1.0};
    const std::vector<std::size_t> K_w_sweep =
        {1, 5};
    const std::vector<std::size_t> dens_sweep =
        {1, 4, 16};

    for (std::size_t D : D_sweep) {
        for (std::size_t N_terms : N_sweep) {
            for (double overlap : overlap_sweep) {
                // overlap_frac=1.0  -> b's key_offset = 0  (full overlap with a)
                // overlap_frac=0.5  -> b's key_offset = N_terms/2
                // overlap_frac=0.0  -> b's key_offset = N_terms (disjoint)
                std::size_t b_offset =
                    static_cast<std::size_t>((1.0 - overlap) * N_terms);
                for (std::size_t K_w : K_w_sweep) {
                    for (std::size_t dens : dens_sweep) {
                        // Fresh ZWTable per (cell) for clean per-cell
                        // accounting; the table is reused across the K
                        // adaptive iterations within a bin (multiply()
                        // memoizes, so post-warmup the per-call cost is
                        // representative of steady-state Phase-C calls).
                        auto tab = std::make_shared<hf::ZWTable>(F);
                        hf::SymCoefSplit a = build_operand(
                            F, N, tab, N_terms, K_w, dens,
                            /*key_offset=*/0, D);
                        hf::SymCoefSplit b = build_operand(
                            F, N, tab, N_terms, K_w, dens,
                            b_offset, D);

                        BinResult br = bench_bin(a, b);

                        std::cout << D << "," << N_terms << "," << overlap << ","
                                  << K_w << "," << dens << ","
                                  << br.add_canon_us << ","
                                  << br.canon_only_us << ","
                                  << br.K << ","
                                  << br.a_terms << "," << br.b_terms
                                  << "," << br.canon_out_terms << "\n";
                        std::cout.flush();
                    }
                }
            }
        }
    }
    return 0;
}
