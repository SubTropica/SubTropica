// bench_ff_ctx_init.cpp
//
// Tier B Pre-flight 1 (kickoff 2026-05-02): per-call FF (finite-field
// reconstruction) ctx-init micro-bench.
//
// Question: is the per-call FIXED overhead of FF reconstruction
//   nmod_mpoly_ctx_init × N_PRIMES + fmpz_multi_CRT_init+precompute+clear
// less than 20 % of the legacy fmpq_mpoly_gcd_cofactors wall on
// representative heavy operands?  If yes, FF is feasible at the call-site
// granularity proposed in `notes/branch_FF_scoping/scoping.md`.  If no,
// the per-call FIXED overhead alone consumes the win budget and the
// lever is fundamentally bottlenecked on allocation; we'd need a
// per-thread ctx pool (Avenue K1, currently in cold storage).
//
// Methods (per quad):
//
//   Method A:  fmpq_mpoly_gcd_cofactors(g, rn, rd, num, den) on the
//              quad's a_num / a_den (which are post-canonical operands;
//              gcd_cofactors is what reduce_inplace calls inside the
//              wide-ctx fast path).  Wide-ctx, no hoist.  This is the
//              call FF would replace.
//
//   Method B:  Just the FF FIXED overhead, on the same wide ctx:
//                for k = 0..N_PRIMES-1:
//                    nmod_mpoly_ctx_init(c_k, nvars, ORD_LEX, primes[k])
//                fmpz_multi_CRT_init(P)
//                fmpz_multi_CRT_precompute(P, moduli_fmpz, N_PRIMES)
//                fmpz_multi_CRT_clear(P)
//                for k: nmod_mpoly_ctx_clear(c_k)
//              No actual modular GCD, no CRT-run, no reconstruct.  This
//              measures the pure machinery cost that fires on EVERY FF
//              call regardless of operand size — the overhead that must
//              amortize.
//
// Default N_PRIMES = 10 (mid of scoping doc's 5-15 estimate).
//
// CLI: <quads.txt> [--reps=3] [--warmup=1] [--max-quads=N]
//                  [--n-primes=10] [--bits=63]
//
// Output (CSV to stdout, summary to stderr):
//   quad_idx, nvars, num_len, den_len,
//   gcd_us, ctxinit_us, ratio_BoverA
//
// Pass/fail gate (kickoff): mean ratio_BoverA < 0.20 over heavy quads.

#include <flint/fmpq_mpoly.h>
#include <flint/fmpz_mpoly.h>
#include <flint/nmod_mpoly.h>
#include <flint/fmpz.h>
#include <flint/fmpq.h>
#include <flint/ulong_extras.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
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

int main(int argc, char** argv) {
    std::string path;
    int reps = 3;
    int warmup = 1;
    long max_quads = -1;
    int n_primes = 10;
    int prime_bits = 63;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a.rfind("--reps=",      0) == 0) reps     = std::atoi(a.c_str() +  7);
        else if (a.rfind("--warmup=",    0) == 0) warmup   = std::atoi(a.c_str() +  9);
        else if (a.rfind("--max-quads=", 0) == 0) max_quads= std::atol(a.c_str() + 12);
        else if (a.rfind("--n-primes=",  0) == 0) n_primes = std::atoi(a.c_str() + 11);
        else if (a.rfind("--bits=",      0) == 0) prime_bits = std::atoi(a.c_str() + 7);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: bench_ff_ctx_init <quads.txt> "
                        "[--reps=3] [--warmup=1] [--max-quads=N] "
                        "[--n-primes=10] [--bits=63]\n");
            return 0;
        } else if (path.empty()) path = a;
    }
    if (path.empty()) {
        std::fprintf(stderr, "missing quads.txt path\n");
        return 1;
    }

    auto quads = read_quads(path, max_quads);
    std::fprintf(stderr,
        "[bench_ff_ctx_init] loaded %zu quads from %s (max=%ld)\n",
        quads.size(), path.c_str(), max_quads);
    std::fprintf(stderr,
        "[bench_ff_ctx_init] reps=%d warmup=%d n_primes=%d bits=%d\n"
        "  Method A = fmpq_mpoly_gcd_cofactors on (a_num, a_den)\n"
        "  Method B = nmod_mpoly_ctx_init x %d + fmpz_multi_CRT init+precompute+clear\n",
        reps, warmup, n_primes, prime_bits, n_primes);

    // Pre-pick the primes once, deterministic, walking up from a small
    // base via n_nextprime.  For prime_bits=63 we want mp_limb-fitting
    // primes <= 2^63-1; n_nextprime starting from 2^(bits-1) walks
    // through nearby primes and stays well within the bit budget.
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
    std::fprintf(stderr, "[bench_ff_ctx_init] picked %d primes, bot=%lu top=%lu\n",
                 n_primes, primes[0], primes[n_primes - 1]);

    std::printf(
        "quad_idx,nvars,num_len,den_len,"
        "gcd_us,ctxinit_us,ratio_BoverA\n");

    double tot_A = 0.0, tot_B = 0.0;
    int n_done = 0;

    for (size_t qi = 0; qi < quads.size(); ++qi) {
        const Quad& q = quads[qi];
        const slong nvars = static_cast<slong>(q.vars.size());

        fmpq_mpoly_ctx_t ctx_q;
        fmpq_mpoly_ctx_init(ctx_q, nvars, ORD_LEX);

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
            continue;
        }

        const slong an_len = fmpq_mpoly_length(a_num_q, ctx_q);
        const slong ad_len = fmpq_mpoly_length(a_den_q, ctx_q);

        // ---- Method A: fmpq_mpoly_gcd_cofactors on (a_num, a_den) ----
        fmpq_mpoly_t g, rn, rd;
        fmpq_mpoly_init(g,  ctx_q);
        fmpq_mpoly_init(rn, ctx_q);
        fmpq_mpoly_init(rd, ctx_q);
        const double gcd_us = time_us([&]() {
            int ok = fmpq_mpoly_gcd_cofactors(
                g, rn, rd, a_num_q, a_den_q, ctx_q);
            if (!ok) std::fprintf(stderr,
                "    quad %d cofactors failed\n", q.idx);
        }, reps, warmup);

        // ---- Method B: per-call FF FIXED overhead ----
        // n_primes nmod_mpoly_ctx_init+clear + fmpz_multi_CRT init+
        // precompute+clear (the moduli must be fmpz_t).
        const double ctxinit_us = time_us([&]() {
            std::vector<nmod_mpoly_ctx_struct> ctxs(n_primes);
            for (int k = 0; k < n_primes; ++k) {
                nmod_mpoly_ctx_init(&ctxs[k], nvars, ORD_LEX, primes[k]);
            }
            fmpz_multi_CRT_t P;
            fmpz_multi_CRT_init(P);
            std::vector<fmpz> moduli_fmpz(n_primes);
            for (int k = 0; k < n_primes; ++k) {
                fmpz_init_set_ui(&moduli_fmpz[k], primes[k]);
            }
            int prec_ok = fmpz_multi_CRT_precompute(
                P, moduli_fmpz.data(), n_primes);
            if (!prec_ok) std::fprintf(stderr,
                "    quad %d CRT precompute failed\n", q.idx);
            for (int k = 0; k < n_primes; ++k) {
                fmpz_clear(&moduli_fmpz[k]);
            }
            fmpz_multi_CRT_clear(P);
            for (int k = 0; k < n_primes; ++k) {
                nmod_mpoly_ctx_clear(&ctxs[k]);
            }
        }, reps, warmup);

        const double ratio = (gcd_us > 0.0) ? ctxinit_us / gcd_us : 0.0;
        std::printf(
            "%d,%lld,%lld,%lld,"
            "%.2f,%.2f,%.4f\n",
            q.idx, static_cast<long long>(nvars),
            static_cast<long long>(an_len),
            static_cast<long long>(ad_len),
            gcd_us, ctxinit_us, ratio);
        std::fflush(stdout);

        tot_A += gcd_us;
        tot_B += ctxinit_us;
        ++n_done;

        fmpq_mpoly_clear(g,       ctx_q);
        fmpq_mpoly_clear(rn,      ctx_q);
        fmpq_mpoly_clear(rd,      ctx_q);
        fmpq_mpoly_clear(a_num_q, ctx_q);
        fmpq_mpoly_clear(a_den_q, ctx_q);
        fmpq_mpoly_ctx_clear(ctx_q);
    }

    const double agg_ratio = (tot_A > 0.0) ? tot_B / tot_A : 0.0;
    std::fprintf(stderr,
        "\n[bench_ff_ctx_init] summary:\n"
        "  quads=%d  total_gcd_us=%.0f  total_ctxinit_us=%.0f  "
        "agg_ratio_BoverA=%.4f  (gate: <0.20 → GO)\n",
        n_done, tot_A, tot_B, agg_ratio);
    if (agg_ratio < 0.20) {
        std::fprintf(stderr, "  VERDICT: GO  (overhead %.1f%% of legacy GCD)\n",
                     100.0 * agg_ratio);
    } else {
        std::fprintf(stderr, "  VERDICT: NO-GO  (overhead %.1f%% of legacy GCD)\n",
                     100.0 * agg_ratio);
    }
    return 0;
}
