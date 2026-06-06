// bench_ff_full.cpp
//
// Tier B Pre-flight 2 (kickoff 2026-05-02): full FF reconstruction
// vs legacy fmpq_mpoly_gcd_cofactors on the heavy subset of the 200
// canonical operand quads.
//
// Question (kickoff §2): does FF reconstruction on size-gated heavy
// operands (>= 50 monomials in num or den) beat
// `fmpq_mpoly_gcd_cofactors` by >= 20% aggregate?  If yes, Tier B
// production work is worth pursuing; if no, the modular GCD route is
// fundamentally not winning on this workload class and Tier B closes.
//
// Method A (legacy, baseline):
//   fmpq_mpoly_gcd_cofactors(g, abar, bbar, a_num, a_den, ctx_q)
//   Timed best-of-reps.  This is what Rat::reduce_inplace's wide-ctx
//   branch calls today.
//
// Method B (full FF reconstruction):
//   (1) Lift fmpq_mpoly pair (a_num, a_den) to fmpz_mpoly (clears
//       coefficient denominators via fmpz_mpoly_q's set_from_fmpq_pair
//       trick).  Outputs a_num_z, a_den_z.
//   (2) For k=0..n_primes-1:
//         nmod_mpoly_ctx_init(ctx_p_k, nvars, ORD_LEX, p_k)
//         fmpz_mpoly_interp_reduce_p(num_p_k, ctx_p_k, a_num_z, ctx_z)
//         fmpz_mpoly_interp_reduce_p(den_p_k, ctx_p_k, a_den_z, ctx_z)
//         nmod_mpoly_gcd_cofactors(g_p_k, abar_p_k, bbar_p_k,
//                                  num_p_k, den_p_k, ctx_p_k)
//   (3) CRT-lift each of {g, abar, bbar} via fmpz_mpoly_interp_mcrt_p
//       iteratively across all n_primes primes.  modulus = product.
//   (4) Rational reconstruct each coefficient of {g, abar, bbar} via
//       fmpq_reconstruct_fmpz(c_q, c_z, modulus).
//   Timed best-of-reps.
//
// Note: Method B does NOT verify byte-identity vs Method A.  That is
// pre-flight 3 (shadow-mode correctness).  This bench ONLY measures
// wall.  Reconstruction failures are reported but do not gate.
//
// CLI: <quads.txt> [--reps=3] [--warmup=1] [--max-quads=N]
//                  [--n-primes=10] [--bits=63] [--min-mono=50]
//
// Output: per-quad CSV to stdout, summary to stderr (heavy-subset
// aggregate).  Verdict: agg ratio_B/A < 0.80 (B is >= 20% faster) → GO.

#include <flint/fmpq_mpoly.h>
#include <flint/fmpz_mpoly.h>
#include <flint/fmpz_mpoly_q.h>
#include <flint/fmpz_mpoly_factor.h>
#include <flint/nmod_mpoly.h>
#include <flint/fmpq.h>
#include <flint/fmpz.h>
#include <flint/ulong_extras.h>

#include <algorithm>
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

// Same helper as v5 — lift fmpq_mpoly pair to fmpz_mpoly_q form.
//   q = (Nc * Nz) / (Dc * Dz)  where N = (Nc fmpq) * (Nz fmpz_mpoly).
static void fmpz_mpoly_q_set_from_fmpq_pair(
    fmpz_mpoly_q_t q,
    const fmpq_mpoly_t num_q, const fmpq_mpoly_t den_q,
    const fmpz_mpoly_ctx_t ctx_z)
{
    fmpz_t s_num, s_den;
    fmpz_init(s_num);
    fmpz_init(s_den);
    fmpz_mul(s_num, fmpq_numref(num_q->content), fmpq_denref(den_q->content));
    fmpz_mul(s_den, fmpq_denref(num_q->content), fmpq_numref(den_q->content));
    fmpz_mpoly_set(fmpz_mpoly_q_numref(q), num_q->zpoly, ctx_z);
    fmpz_mpoly_scalar_mul_fmpz(fmpz_mpoly_q_numref(q),
                               fmpz_mpoly_q_numref(q), s_num, ctx_z);
    fmpz_mpoly_set(fmpz_mpoly_q_denref(q), den_q->zpoly, ctx_z);
    fmpz_mpoly_scalar_mul_fmpz(fmpz_mpoly_q_denref(q),
                               fmpz_mpoly_q_denref(q), s_den, ctx_z);
    fmpz_clear(s_num);
    fmpz_clear(s_den);
}

// Method B per-call body.  Performs full FF reconstruction of (g,
// abar, bbar) where (a_num_z, a_den_z) are the lifted fmpz_mpoly
// operands.  Returns recon_fail counter (0..3*nterms).  Nothing
// returned about the value — pre-flight 2 measures wall only.
//
// We pass scratch storage in via the args to allow reuse across reps
// without alloc churn corrupting timing.
struct FFScratch {
    std::vector<nmod_mpoly_ctx_struct> ctxs;
    std::vector<nmod_mpoly_struct>     num_p;
    std::vector<nmod_mpoly_struct>     den_p;
    std::vector<nmod_mpoly_struct>     g_p;
    std::vector<nmod_mpoly_struct>     abar_p;
    std::vector<nmod_mpoly_struct>     bbar_p;
};

static int run_ff_once(
    const fmpz_mpoly_t a_num_z, const fmpz_mpoly_t a_den_z,
    const fmpz_mpoly_ctx_t ctx_z,
    const std::vector<ulong>& primes,
    FFScratch& s)
{
    const int n_primes = static_cast<int>(primes.size());
    const slong nvars = fmpz_mpoly_ctx_nvars(ctx_z);

    // (2) Per-prime: ctx_init, project, modGCD.
    int bad_primes = 0;
    for (int k = 0; k < n_primes; ++k) {
        nmod_mpoly_ctx_init(&s.ctxs[k], nvars, ORD_LEX, primes[k]);
        nmod_mpoly_init(&s.num_p[k],  &s.ctxs[k]);
        nmod_mpoly_init(&s.den_p[k],  &s.ctxs[k]);
        nmod_mpoly_init(&s.g_p[k],    &s.ctxs[k]);
        nmod_mpoly_init(&s.abar_p[k], &s.ctxs[k]);
        nmod_mpoly_init(&s.bbar_p[k], &s.ctxs[k]);
        fmpz_mpoly_interp_reduce_p(&s.num_p[k], &s.ctxs[k], a_num_z, ctx_z);
        fmpz_mpoly_interp_reduce_p(&s.den_p[k], &s.ctxs[k], a_den_z, ctx_z);
        int ok = nmod_mpoly_gcd_cofactors(&s.g_p[k], &s.abar_p[k], &s.bbar_p[k],
                                          &s.num_p[k], &s.den_p[k], &s.ctxs[k]);
        if (!ok) ++bad_primes;
    }
    (void)bad_primes;

    // (3) CRT-lift g, abar, bbar.
    // Pattern: k=0 use interp_lift_p (H ← g_p[0] over fmpz with mod=p_0).
    //          k>=1 use interp_mcrt_p (incrementally update H using prime k).
    fmpz_mpoly_t g_lifted, abar_lifted, bbar_lifted;
    fmpz_mpoly_init(g_lifted,    ctx_z);
    fmpz_mpoly_init(abar_lifted, ctx_z);
    fmpz_mpoly_init(bbar_lifted, ctx_z);
    fmpz_t modulus;
    fmpz_init_set_ui(modulus, primes[0]);
    flint_bitcnt_t bits_g = 0, bits_a = 0, bits_b = 0;
    fmpz_mpoly_interp_lift_p(g_lifted,    ctx_z, &s.g_p[0],    &s.ctxs[0]);
    fmpz_mpoly_interp_lift_p(abar_lifted, ctx_z, &s.abar_p[0], &s.ctxs[0]);
    fmpz_mpoly_interp_lift_p(bbar_lifted, ctx_z, &s.bbar_p[0], &s.ctxs[0]);
    for (int k = 1; k < n_primes; ++k) {
        fmpz_mpoly_interp_mcrt_p(&bits_g, g_lifted,    ctx_z, modulus,
                                 &s.g_p[k],    &s.ctxs[k]);
        fmpz_mpoly_interp_mcrt_p(&bits_a, abar_lifted, ctx_z, modulus,
                                 &s.abar_p[k], &s.ctxs[k]);
        fmpz_mpoly_interp_mcrt_p(&bits_b, bbar_lifted, ctx_z, modulus,
                                 &s.bbar_p[k], &s.ctxs[k]);
        fmpz_mul_ui(modulus, modulus, primes[k]);
    }

    // (4) Rational reconstruct each coefficient.
    int recon_fail = 0;
    fmpq_t cq;
    fmpz_t cz;
    fmpq_init(cq);
    fmpz_init(cz);

    auto reconstruct_all = [&](fmpz_mpoly_t H) {
        slong n = fmpz_mpoly_length(H, ctx_z);
        for (slong j = 0; j < n; ++j) {
            fmpz_mpoly_get_term_coeff_fmpz(cz, H, j, ctx_z);
            int ok = fmpq_reconstruct_fmpz(cq, cz, modulus);
            if (!ok) ++recon_fail;
        }
    };
    reconstruct_all(g_lifted);
    reconstruct_all(abar_lifted);
    reconstruct_all(bbar_lifted);

    fmpq_clear(cq);
    fmpz_clear(cz);
    fmpz_clear(modulus);
    fmpz_mpoly_clear(g_lifted,    ctx_z);
    fmpz_mpoly_clear(abar_lifted, ctx_z);
    fmpz_mpoly_clear(bbar_lifted, ctx_z);

    // Per-prime cleanup.
    for (int k = 0; k < n_primes; ++k) {
        nmod_mpoly_clear(&s.bbar_p[k], &s.ctxs[k]);
        nmod_mpoly_clear(&s.abar_p[k], &s.ctxs[k]);
        nmod_mpoly_clear(&s.g_p[k],    &s.ctxs[k]);
        nmod_mpoly_clear(&s.den_p[k],  &s.ctxs[k]);
        nmod_mpoly_clear(&s.num_p[k],  &s.ctxs[k]);
        nmod_mpoly_ctx_clear(&s.ctxs[k]);
    }

    return recon_fail;
}

int main(int argc, char** argv) {
    std::string path;
    int reps = 3;
    int warmup = 1;
    long max_quads = -1;
    int n_primes = 10;
    int prime_bits = 63;
    int min_mono = 50;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a.rfind("--reps=",      0) == 0) reps     = std::atoi(a.c_str() +  7);
        else if (a.rfind("--warmup=",    0) == 0) warmup   = std::atoi(a.c_str() +  9);
        else if (a.rfind("--max-quads=", 0) == 0) max_quads= std::atol(a.c_str() + 12);
        else if (a.rfind("--n-primes=",  0) == 0) n_primes = std::atoi(a.c_str() + 11);
        else if (a.rfind("--bits=",      0) == 0) prime_bits = std::atoi(a.c_str() + 7);
        else if (a.rfind("--min-mono=",  0) == 0) min_mono = std::atoi(a.c_str() + 11);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: bench_ff_full <quads.txt> "
                        "[--reps=3] [--warmup=1] [--max-quads=N] "
                        "[--n-primes=10] [--bits=63] [--min-mono=50]\n");
            return 0;
        } else if (path.empty()) path = a;
    }
    if (path.empty()) {
        std::fprintf(stderr, "missing quads.txt path\n");
        return 1;
    }

    auto quads = read_quads(path, max_quads);
    std::fprintf(stderr,
        "[bench_ff_full] loaded %zu quads from %s (max=%ld)\n",
        quads.size(), path.c_str(), max_quads);
    std::fprintf(stderr,
        "[bench_ff_full] reps=%d warmup=%d n_primes=%d bits=%d "
        "min_mono=%d\n"
        "  Method A = fmpq_mpoly_gcd_cofactors\n"
        "  Method B = full FF reconstruction (lift + project + nmod_gcd "
        "x N + CRT + reconstruct)\n",
        reps, warmup, n_primes, prime_bits, min_mono);

    std::vector<ulong> primes(n_primes);
    {
        ulong p = (prime_bits >= 64)
            ? ((ulong)1 << 62)
            : ((ulong)1 << (prime_bits - 1));
        for (int k = 0; k < n_primes; ++k) {
            p = n_nextprime(p, /*proved=*/1);
            primes[k] = p;
        }
    }
    std::fprintf(stderr,
        "[bench_ff_full] picked %d primes, bot=%lu top=%lu\n",
        n_primes, primes[0], primes[n_primes - 1]);

    std::printf(
        "quad_idx,nvars,num_len,den_len,heavy,"
        "gcd_us,ff_us,ratio_BoverA,recon_fail\n");

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

        std::vector<const char*> vptrs;
        vptrs.reserve(q.vars.size());
        for (const auto& v : q.vars) vptrs.push_back(v.c_str());

        fmpq_mpoly_t a_num_q, a_den_q;
        fmpq_mpoly_init(a_num_q, ctx_q);
        fmpq_mpoly_init(a_den_q, ctx_q);

        bool parse_ok = true;
        if (fmpq_mpoly_set_str_pretty(a_num_q, q.a_num_str.c_str(),
                                       vptrs.data(), ctx_q) != 0) parse_ok = false;
        if (parse_ok && fmpq_mpoly_set_str_pretty(a_den_q, q.a_den_str.c_str(),
                                       vptrs.data(), ctx_q) != 0) parse_ok = false;
        if (!parse_ok) {
            std::fprintf(stderr, "  quad %d parse failed\n", q.idx);
            fmpq_mpoly_clear(a_num_q, ctx_q);
            fmpq_mpoly_clear(a_den_q, ctx_q);
            fmpq_mpoly_ctx_clear(ctx_q);
            fmpz_mpoly_ctx_clear(ctx_z);
            continue;
        }

        const slong an_len = fmpq_mpoly_length(a_num_q, ctx_q);
        const slong ad_len = fmpq_mpoly_length(a_den_q, ctx_q);
        const bool heavy =
            (an_len >= min_mono) || (ad_len >= min_mono);

        // ---- Method A: legacy ----
        fmpq_mpoly_t g, rn, rd;
        fmpq_mpoly_init(g,  ctx_q);
        fmpq_mpoly_init(rn, ctx_q);
        fmpq_mpoly_init(rd, ctx_q);
        const double gcd_us = time_us([&]() {
            int ok = fmpq_mpoly_gcd_cofactors(g, rn, rd, a_num_q, a_den_q, ctx_q);
            if (!ok) std::fprintf(stderr, "    quad %d cofactors failed\n", q.idx);
        }, reps, warmup);

        // ---- Method B: full FF ----
        // Lift once outside the timed loop?  No — production would lift
        // per-call.  Time the full per-call cost INCLUDING lift.
        // But re-init scratch outside the timed loop: scratch reuse mirrors
        // a per-thread pool which would be the production path anyway.
        FFScratch scratch;
        scratch.ctxs.resize(n_primes);
        scratch.num_p.resize(n_primes);
        scratch.den_p.resize(n_primes);
        scratch.g_p.resize(n_primes);
        scratch.abar_p.resize(n_primes);
        scratch.bbar_p.resize(n_primes);

        int recon_fail = 0;
        const double ff_us = time_us([&]() {
            // Lift in-loop (production-faithful: each call lifts).
            fmpz_mpoly_q_t qa;
            fmpz_mpoly_q_init(qa, ctx_z);
            fmpz_mpoly_q_set_from_fmpq_pair(qa, a_num_q, a_den_q, ctx_z);
            // Note: do NOT canonicalise — that would short-circuit the GCD
            // we're about to compute, defeating the point.  Keep the
            // operands in their lifted but un-canonicalised form.
            fmpz_mpoly_t a_num_z_local, a_den_z_local;
            fmpz_mpoly_init(a_num_z_local, ctx_z);
            fmpz_mpoly_init(a_den_z_local, ctx_z);
            fmpz_mpoly_set(a_num_z_local, fmpz_mpoly_q_numref(qa), ctx_z);
            fmpz_mpoly_set(a_den_z_local, fmpz_mpoly_q_denref(qa), ctx_z);
            recon_fail = run_ff_once(a_num_z_local, a_den_z_local, ctx_z,
                                     primes, scratch);
            fmpz_mpoly_clear(a_num_z_local, ctx_z);
            fmpz_mpoly_clear(a_den_z_local, ctx_z);
            fmpz_mpoly_q_clear(qa, ctx_z);
        }, reps, warmup);

        const double ratio = (gcd_us > 0.0) ? ff_us / gcd_us : 0.0;
        std::printf(
            "%d,%lld,%lld,%lld,%d,"
            "%.2f,%.2f,%.4f,%d\n",
            q.idx, static_cast<long long>(nvars),
            static_cast<long long>(an_len),
            static_cast<long long>(ad_len),
            heavy ? 1 : 0,
            gcd_us, ff_us, ratio, recon_fail);
        std::fflush(stdout);

        if (heavy) {
            tot_A_heavy += gcd_us;
            tot_B_heavy += ff_us;
            ++n_heavy;
        }
        ++n_done;

        fmpq_mpoly_clear(g,       ctx_q);
        fmpq_mpoly_clear(rn,      ctx_q);
        fmpq_mpoly_clear(rd,      ctx_q);
        fmpq_mpoly_clear(a_num_q, ctx_q);
        fmpq_mpoly_clear(a_den_q, ctx_q);
        fmpq_mpoly_ctx_clear(ctx_q);
        fmpz_mpoly_ctx_clear(ctx_z);
    }

    const double agg_ratio_heavy =
        (tot_A_heavy > 0.0) ? tot_B_heavy / tot_A_heavy : 0.0;
    std::fprintf(stderr,
        "\n[bench_ff_full] heavy-subset summary (min_mono=%d):\n"
        "  quads_total=%d  quads_heavy=%d\n"
        "  total_gcd_us=%.0f  total_ff_us=%.0f  agg_ratio_BoverA=%.4f\n"
        "  Δ = %+.1f%%  (%s)\n",
        min_mono, n_done, n_heavy,
        tot_A_heavy, tot_B_heavy, agg_ratio_heavy,
        100.0 * (tot_A_heavy - tot_B_heavy) / std::max(tot_A_heavy, 1.0),
        (tot_B_heavy < tot_A_heavy) ? "B faster" : "A faster");
    if (agg_ratio_heavy < 0.80) {
        std::fprintf(stderr,
            "  VERDICT: GO  (FF >= 20%% B-faster on heavy subset)\n");
    } else {
        std::fprintf(stderr,
            "  VERDICT: NO-GO  (FF gain < 20%% on heavy subset)\n");
    }
    return 0;
}
