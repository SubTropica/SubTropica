// bench_ff_factor.cpp
//
// C2 Pre-flight (kickoff 2026-05-02, post Tier B closure): per-call
// FF factor cost feasibility.
//
// Question: does single-prime `nmod_mpoly_factor` cost <= 1/10 of
// legacy `fmpq_mpoly_factor` on representative heavy operands?
// If yes, a full FF factor reconstruction (10 primes + CRT-lift +
// rational-reconstruct of factor coefficients) has *some* chance to
// beat legacy.  If no (single-prime cost is comparable or larger),
// FF factor is structurally dead at the per-call granularity, by the
// same logic that closed Tier B (C1, gcd_cofactors).
//
// This is a reduced pre-flight — it skips the full reconstruction
// pipeline because the gate is already failed if single-prime cost is
// not <= 0.10 of legacy.  Equivalent to "the lower bound of Method B
// already loses".
//
// Method A (legacy):
//   fmpq_mpoly_factor(F, a_den, ctx_q)
//   Best-of-3, 1 warmup.
//
// Method B (single-prime mod-p factor floor):
//   Lift fmpq → fmpz (a_den_q → a_den_z via fmpz_mpoly_q content
//   trick), then for one prime p:
//     nmod_mpoly_ctx_init(ctx_p)
//     fmpz_mpoly_interp_reduce_p(a_den_p, ctx_p, a_den_z, ctx_z)
//     nmod_mpoly_factor(F_p, a_den_p, ctx_p)
//   Best-of-3, 1 warmup.  This is the per-prime work that would
//   repeat 10× in a full FF factor pipeline.
//
// Aggregate gate (heavy subset, max(num_len,den_len) ≥ 50):
//   ratio_BoverA <= 0.10  → POTENTIAL GO (full FF factor merits
//                            building)
//   ratio_BoverA > 0.10   → NO-GO (10×B already exceeds A; no
//                            full-pipeline tuning closes it)
//
// CLI: <quads.txt> [--reps=3] [--warmup=1] [--max-quads=N]
//                  [--bits=63] [--min-mono=50]
//
// Bench-only; production untouched.

#include <flint/fmpq_mpoly.h>
#include <flint/fmpq_mpoly_factor.h>
#include <flint/fmpz_mpoly.h>
#include <flint/fmpz_mpoly_q.h>
#include <flint/fmpz_mpoly_factor.h>
#include <flint/nmod_mpoly.h>
#include <flint/nmod_mpoly_factor.h>
#include <flint/fmpz.h>
#include <flint/ulong_extras.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;

struct Quad {
    int idx = -1;
    std::vector<std::string> vars;
    std::string a_num_str;
    std::string a_den_str;
    std::string b_num_str;
    std::string b_den_str;
};

static std::vector<Quad> read_quads(const std::string& path,
                                     long max_quads) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "could not open %s: %s\n",
                     path.c_str(), std::strerror(errno));
        std::exit(2);
    }
    std::vector<Quad> out;
    std::string line;
    Quad cur;
    bool in_quad = false;
    while (std::getline(in, line)) {
        if (line.rfind("QUAD ", 0) == 0) {
            cur = Quad{};
            cur.idx = std::atoi(line.c_str() + 5);
            in_quad = true;
        } else if (in_quad && line.rfind("VARS: ", 0) == 0) {
            cur.vars.clear();
            std::string rest = line.substr(6);
            std::string tok;
            std::istringstream ss(rest);
            while (std::getline(ss, tok, ',')) {
                if (!tok.empty()) cur.vars.push_back(tok);
            }
        } else if (in_quad && line.rfind("A_NUM: ", 0) == 0) {
            cur.a_num_str = line.substr(7);
        } else if (in_quad && line.rfind("A_DEN: ", 0) == 0) {
            cur.a_den_str = line.substr(7);
        } else if (in_quad && line.rfind("B_NUM: ", 0) == 0) {
            cur.b_num_str = line.substr(7);
        } else if (in_quad && line.rfind("B_DEN: ", 0) == 0) {
            cur.b_den_str = line.substr(7);
        } else if (in_quad && line == "ENDQUAD") {
            out.push_back(std::move(cur));
            in_quad = false;
            if (max_quads > 0 &&
                static_cast<long>(out.size()) >= max_quads) break;
        }
    }
    return out;
}

static double time_us(std::function<void()> f, int reps, int warmup) {
    for (int i = 0; i < warmup; ++i) f();
    double best = 1e30;
    for (int i = 0; i < reps; ++i) {
        auto t0 = clk::now();
        f();
        auto t1 = clk::now();
        double us =
            std::chrono::duration<double, std::micro>(t1 - t0).count();
        if (us < best) best = us;
    }
    return best;
}

// Lift fmpq_mpoly to fmpz_mpoly via fmpz_mpoly_q's content trick.
// Input: P_q (fmpq_mpoly).  Output: P_z (fmpz_mpoly), populated by
// scaling P_q->zpoly by the numerator of the rational content.
static void fmpz_mpoly_set_from_fmpq_mpoly(
    fmpz_mpoly_t P_z,
    const fmpq_mpoly_t P_q,
    const fmpz_mpoly_ctx_t ctx_z)
{
    // P_q = (content_num/content_den) * zpoly.  For factoring we drop
    // the rational multiplier (factoring is up to a unit) and just
    // factor the integer-coefficient zpoly scaled by content_num
    // (so the result has integer coefficients matching P_q's
    // content_num × zpoly representation).
    fmpz_mpoly_set(P_z, P_q->zpoly, ctx_z);
    fmpz_mpoly_scalar_mul_fmpz(P_z, P_z,
                               fmpq_numref(P_q->content), ctx_z);
    // Note: content_den is dropped (factor up to a unit).  This is
    // sufficient for measuring factor wall.
}

int main(int argc, char** argv) {
    std::string path;
    int reps = 3;
    int warmup = 1;
    long max_quads = -1;
    int prime_bits = 63;
    int min_mono = 50;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a.rfind("--reps=",      0) == 0) reps     = std::atoi(a.c_str() +  7);
        else if (a.rfind("--warmup=",    0) == 0) warmup   = std::atoi(a.c_str() +  9);
        else if (a.rfind("--max-quads=", 0) == 0) max_quads= std::atol(a.c_str() + 12);
        else if (a.rfind("--bits=",      0) == 0) prime_bits = std::atoi(a.c_str() + 7);
        else if (a.rfind("--min-mono=",  0) == 0) min_mono = std::atoi(a.c_str() + 11);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: bench_ff_factor <quads.txt> "
                        "[--reps=3] [--warmup=1] [--max-quads=N] "
                        "[--bits=63] [--min-mono=50]\n");
            return 0;
        } else if (path.empty()) path = a;
    }
    if (path.empty()) {
        std::fprintf(stderr, "missing quads.txt path\n");
        return 1;
    }

    auto quads = read_quads(path, max_quads);
    std::fprintf(stderr,
        "[bench_ff_factor] loaded %zu quads from %s (max=%ld)\n",
        quads.size(), path.c_str(), max_quads);
    std::fprintf(stderr,
        "[bench_ff_factor] reps=%d warmup=%d bits=%d min_mono=%d\n"
        "  Method A = fmpq_mpoly_factor(F, a_den, ctx_q)\n"
        "  Method B = lift + fmpz_mpoly_interp_reduce_p + "
        "nmod_mpoly_factor (single prime, the per-prime floor)\n"
        "  Gate (heavy subset): agg ratio_B/A <= 0.10 → potential GO\n",
        reps, warmup, prime_bits, min_mono);

    ulong p_chosen;
    {
        ulong p = (prime_bits >= 64)
            ? ((ulong)1 << 62)
            : ((ulong)1 << (prime_bits - 1));
        p_chosen = n_nextprime(p, /*proved=*/1);
    }
    std::fprintf(stderr,
        "[bench_ff_factor] chosen prime: %lu\n", p_chosen);

    std::printf(
        "quad_idx,nvars,den_len,heavy,"
        "factor_A_us,factor_B_us,ratio_BoverA,n_factors_A,n_factors_B\n");

    double tot_A_heavy = 0.0, tot_B_heavy = 0.0;
    int n_heavy = 0;
    int n_done = 0;

    for (size_t qi = 0; qi < quads.size(); ++qi) {
        const Quad& q = quads[qi];
        const slong nvars = static_cast<slong>(q.vars.size());

        fmpq_mpoly_ctx_t ctx_q;
        fmpq_mpoly_ctx_init(ctx_q, nvars, ORD_LEX);
        fmpz_mpoly_ctx_t ctx_z;
        fmpz_mpoly_ctx_init(ctx_z, nvars, ORD_LEX);
        nmod_mpoly_ctx_t ctx_p;
        nmod_mpoly_ctx_init(ctx_p, nvars, ORD_LEX, p_chosen);

        std::vector<const char*> vptrs;
        vptrs.reserve(q.vars.size());
        for (const auto& v : q.vars) vptrs.push_back(v.c_str());

        fmpq_mpoly_t a_den_q;
        fmpq_mpoly_init(a_den_q, ctx_q);

        bool parse_ok = true;
        if (fmpq_mpoly_set_str_pretty(a_den_q, q.a_den_str.c_str(),
                                       vptrs.data(), ctx_q) != 0) parse_ok = false;
        if (!parse_ok) {
            std::fprintf(stderr, "  quad %d parse failed\n", q.idx);
            fmpq_mpoly_clear(a_den_q, ctx_q);
            fmpq_mpoly_ctx_clear(ctx_q);
            fmpz_mpoly_ctx_clear(ctx_z);
            nmod_mpoly_ctx_clear(ctx_p);
            continue;
        }

        const slong ad_len = fmpq_mpoly_length(a_den_q, ctx_q);
        const bool heavy = (ad_len >= min_mono);

        // ---- Method A: legacy fmpq_mpoly_factor ----
        fmpq_mpoly_factor_t F_q;
        fmpq_mpoly_factor_init(F_q, ctx_q);
        const double factor_A_us = time_us([&]() {
            int ok = fmpq_mpoly_factor(F_q, a_den_q, ctx_q);
            if (!ok) std::fprintf(stderr,
                "    quad %d fmpq factor failed\n", q.idx);
        }, reps, warmup);
        const slong n_factors_A = fmpq_mpoly_factor_length(F_q, ctx_q);

        // ---- Method B: single-prime mod-p factor (the floor) ----
        // Lift outside timed loop?  No — production FF would lift per
        // call.  Time the full per-call work INCLUDING lift + project.
        nmod_mpoly_factor_t F_p;
        nmod_mpoly_factor_init(F_p, ctx_p);
        slong n_factors_B = 0;
        const double factor_B_us = time_us([&]() {
            // Lift in-loop
            fmpz_mpoly_t a_den_z;
            fmpz_mpoly_init(a_den_z, ctx_z);
            fmpz_mpoly_set_from_fmpq_mpoly(a_den_z, a_den_q, ctx_z);
            // Project
            nmod_mpoly_t a_den_p;
            nmod_mpoly_init(a_den_p, ctx_p);
            fmpz_mpoly_interp_reduce_p(a_den_p, ctx_p, a_den_z, ctx_z);
            // Factor mod p
            int ok = nmod_mpoly_factor(F_p, a_den_p, ctx_p);
            if (!ok) std::fprintf(stderr,
                "    quad %d nmod factor failed\n", q.idx);
            n_factors_B = nmod_mpoly_factor_length(F_p, ctx_p);
            nmod_mpoly_clear(a_den_p, ctx_p);
            fmpz_mpoly_clear(a_den_z, ctx_z);
        }, reps, warmup);

        const double ratio = (factor_A_us > 0.0)
            ? factor_B_us / factor_A_us : 0.0;
        std::printf(
            "%d,%lld,%lld,%d,"
            "%.2f,%.2f,%.4f,%lld,%lld\n",
            q.idx, static_cast<long long>(nvars),
            static_cast<long long>(ad_len),
            heavy ? 1 : 0,
            factor_A_us, factor_B_us, ratio,
            static_cast<long long>(n_factors_A),
            static_cast<long long>(n_factors_B));
        std::fflush(stdout);

        if (heavy) {
            tot_A_heavy += factor_A_us;
            tot_B_heavy += factor_B_us;
            ++n_heavy;
        }
        ++n_done;

        fmpq_mpoly_factor_clear(F_q, ctx_q);
        nmod_mpoly_factor_clear(F_p, ctx_p);
        fmpq_mpoly_clear(a_den_q, ctx_q);
        nmod_mpoly_ctx_clear(ctx_p);
        fmpz_mpoly_ctx_clear(ctx_z);
        fmpq_mpoly_ctx_clear(ctx_q);
    }

    const double agg_ratio =
        (tot_A_heavy > 0.0) ? tot_B_heavy / tot_A_heavy : 0.0;
    std::fprintf(stderr,
        "\n[bench_ff_factor] heavy-subset summary (min_mono=%d):\n"
        "  quads_total=%d  quads_heavy=%d\n"
        "  total_A_us=%.0f  total_B_us=%.0f  agg_ratio_BoverA=%.4f\n"
        "  Gate threshold: <= 0.10  →  10*B <= A  →  POTENTIAL GO\n"
        "  Observed:        %.4f\n",
        min_mono, n_done, n_heavy,
        tot_A_heavy, tot_B_heavy, agg_ratio,
        agg_ratio);
    if (agg_ratio <= 0.10) {
        std::fprintf(stderr,
            "  VERDICT: POTENTIAL-GO  (single-prime floor leaves room)\n");
    } else {
        std::fprintf(stderr,
            "  VERDICT: NO-GO  (single-prime cost already %.1fx of legacy "
            "→ 10*N+CRT cannot recover)\n",
            agg_ratio * 10.0);
    }
    return 0;
}
