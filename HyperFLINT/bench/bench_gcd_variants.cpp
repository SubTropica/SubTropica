// bench_gcd_variants.cpp
//
// Re-bench GCD strategies after research finding (2026-05-01):
// fmpq_mpoly_gcd_cofactors is a dispatcher (BMA → Brown → Zippel
// internally), so "Brown was 62% slower" was Brown-standalone vs the
// dispatcher's choice — not Brown vs Zippel.  Hu/Monagan published
// that Zippel beats subresultant by 100-1000× on truly sparse
// multivariate.  This bench tests cofactors / brown / zippel /
// zippel2 / hensel / subresultant on the same Avenue-H pair file.
//
// Output (stdout): CSV with columns:
//   idx,nvars,num_len,den_len,
//   cof_us,brown_us,zip_us,zip2_us,hen_us,sub_us,
//   match_zip,match_zip2,match_hen,match_sub
//
// Match=1 means the variant returned the same G as cofactors.
// (We compare G itself; the cofactors variants would also need
// divexact, identical to bench_brown_gcd's pattern.)

#include <flint/fmpq_mpoly.h>
#include <flint/flint.h>

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

struct Pair {
    int idx = -1;
    std::vector<std::string> vars;
    std::string num_str;
    std::string den_str;
};

static std::vector<Pair> read_pairs(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "could not open %s: %s\n",
                     path.c_str(), std::strerror(errno));
        std::exit(2);
    }
    std::vector<Pair> out;
    std::string line;
    Pair cur;
    bool in_pair = false;
    while (std::getline(in, line)) {
        if (line.rfind("PAIR ", 0) == 0) {
            cur = Pair{};
            cur.idx = std::atoi(line.c_str() + 5);
            in_pair = true;
        } else if (in_pair && line.rfind("VARS: ", 0) == 0) {
            cur.vars.clear();
            std::string rest = line.substr(6);
            std::string tok;
            std::istringstream ss(rest);
            while (std::getline(ss, tok, ',')) {
                if (!tok.empty()) cur.vars.push_back(tok);
            }
        } else if (in_pair && line.rfind("NUM: ", 0) == 0) {
            cur.num_str = line.substr(5);
        } else if (in_pair && line.rfind("DEN: ", 0) == 0) {
            cur.den_str = line.substr(5);
        } else if (in_pair && line == "ENDPAIR") {
            out.push_back(std::move(cur));
            in_pair = false;
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
    int reps = 3, warmup = 1;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--reps=", 0) == 0) reps = std::atoi(a.c_str() + 7);
        else if (a.rfind("--warmup=", 0) == 0) warmup = std::atoi(a.c_str() + 9);
        else if (path.empty()) path = a;
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: %s pairs.txt [--reps=N] [--warmup=N]\n",
                     argv[0]);
        return 1;
    }

    auto pairs = read_pairs(path);
    std::fprintf(stderr,
                 "[bench_gcd_variants] %zu pairs, reps=%d, warmup=%d\n",
                 pairs.size(), reps, warmup);

    std::printf("idx,nvars,num_len,den_len,"
                "cof_us,brown_us,zip_us,zip2_us,hen_us,sub_us,"
                "match_brown,match_zip,match_zip2,match_hen,match_sub\n");

    double tot_cof = 0, tot_brown = 0, tot_zip = 0, tot_zip2 = 0, tot_hen = 0, tot_sub = 0;
    int n_brown = 0, n_zip = 0, n_zip2 = 0, n_hen = 0, n_sub = 0;
    int n_brown_match = 0, n_zip_match = 0, n_zip2_match = 0, n_hen_match = 0, n_sub_match = 0;

    for (const auto& p : pairs) {
        std::vector<const char*> var_cstrs;
        for (auto& s : p.vars) var_cstrs.push_back(s.c_str());

        fmpq_mpoly_ctx_t ctx;
        fmpq_mpoly_ctx_init(ctx, p.vars.size(), ORD_LEX);

        fmpq_mpoly_t num, den;
        fmpq_mpoly_init(num, ctx);
        fmpq_mpoly_init(den, ctx);
        if (fmpq_mpoly_set_str_pretty(num, p.num_str.c_str(),
                                       var_cstrs.data(), ctx) != 0 ||
            fmpq_mpoly_set_str_pretty(den, p.den_str.c_str(),
                                       var_cstrs.data(), ctx) != 0) {
            std::fprintf(stderr, "parse failed pair %d\n", p.idx);
            continue;
        }
        slong num_len = fmpq_mpoly_length(num, ctx);
        slong den_len = fmpq_mpoly_length(den, ctx);
        slong nvars = p.vars.size();

        fmpq_mpoly_t g_cof, abar, bbar;
        fmpq_mpoly_init(g_cof, ctx); fmpq_mpoly_init(abar, ctx); fmpq_mpoly_init(bbar, ctx);
        double cof_us = time_us([&]() {
            fmpq_mpoly_zero(g_cof, ctx); fmpq_mpoly_zero(abar, ctx); fmpq_mpoly_zero(bbar, ctx);
            fmpq_mpoly_gcd_cofactors(g_cof, abar, bbar, num, den, ctx);
        }, reps, warmup);

        // GCD-only variants: just G, no cofactors.  This is the apples-to-apples
        // test of the GCD algorithm (cofactors variant = GCD + 2 divides built-in).
        fmpq_mpoly_t g_brown, g_zip, g_zip2, g_hen, g_sub;
        fmpq_mpoly_init(g_brown, ctx); fmpq_mpoly_init(g_zip, ctx);
        fmpq_mpoly_init(g_zip2, ctx); fmpq_mpoly_init(g_hen, ctx); fmpq_mpoly_init(g_sub, ctx);

        int brown_ok = 1, zip_ok = 1, zip2_ok = 1, hen_ok = 1, sub_ok = 1;
        double brown_us = time_us([&]() {
            fmpq_mpoly_zero(g_brown, ctx);
            if (!fmpq_mpoly_gcd_brown(g_brown, num, den, ctx)) brown_ok = 0;
        }, reps, warmup);
        double zip_us = time_us([&]() {
            fmpq_mpoly_zero(g_zip, ctx);
            if (!fmpq_mpoly_gcd_zippel(g_zip, num, den, ctx)) zip_ok = 0;
        }, reps, warmup);
        double zip2_us = time_us([&]() {
            fmpq_mpoly_zero(g_zip2, ctx);
            if (!fmpq_mpoly_gcd_zippel2(g_zip2, num, den, ctx)) zip2_ok = 0;
        }, reps, warmup);
        double hen_us = time_us([&]() {
            fmpq_mpoly_zero(g_hen, ctx);
            if (!fmpq_mpoly_gcd_hensel(g_hen, num, den, ctx)) hen_ok = 0;
        }, reps, warmup);
        double sub_us = time_us([&]() {
            fmpq_mpoly_zero(g_sub, ctx);
            if (!fmpq_mpoly_gcd_subresultant(g_sub, num, den, ctx)) sub_ok = 0;
        }, reps, warmup);

        int m_brown = brown_ok && fmpq_mpoly_equal(g_brown, g_cof, ctx);
        int m_zip   = zip_ok   && fmpq_mpoly_equal(g_zip,   g_cof, ctx);
        int m_zip2  = zip2_ok  && fmpq_mpoly_equal(g_zip2,  g_cof, ctx);
        int m_hen   = hen_ok   && fmpq_mpoly_equal(g_hen,   g_cof, ctx);
        int m_sub   = sub_ok   && fmpq_mpoly_equal(g_sub,   g_cof, ctx);

        std::printf("%d,%lld,%lld,%lld,"
                    "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
                    "%d,%d,%d,%d,%d\n",
                    p.idx,
                    (long long)nvars, (long long)num_len, (long long)den_len,
                    cof_us, brown_us, zip_us, zip2_us, hen_us, sub_us,
                    m_brown, m_zip, m_zip2, m_hen, m_sub);
        std::fflush(stdout);

        tot_cof += cof_us;
        if (brown_ok) { tot_brown += brown_us; ++n_brown; if (m_brown) ++n_brown_match; }
        if (zip_ok)   { tot_zip   += zip_us;   ++n_zip;   if (m_zip)   ++n_zip_match; }
        if (zip2_ok)  { tot_zip2  += zip2_us;  ++n_zip2;  if (m_zip2)  ++n_zip2_match; }
        if (hen_ok)   { tot_hen   += hen_us;   ++n_hen;   if (m_hen)   ++n_hen_match; }
        if (sub_ok)   { tot_sub   += sub_us;   ++n_sub;   if (m_sub)   ++n_sub_match; }

        fmpq_mpoly_clear(g_cof, ctx); fmpq_mpoly_clear(abar, ctx); fmpq_mpoly_clear(bbar, ctx);
        fmpq_mpoly_clear(g_brown, ctx); fmpq_mpoly_clear(g_zip, ctx);
        fmpq_mpoly_clear(g_zip2, ctx); fmpq_mpoly_clear(g_hen, ctx); fmpq_mpoly_clear(g_sub, ctx);
        fmpq_mpoly_clear(num, ctx); fmpq_mpoly_clear(den, ctx);
        fmpq_mpoly_ctx_clear(ctx);
    }

    std::fprintf(stderr,
        "\n[summary] pairs=%zu\n"
        "  cofactors total: %.2f us\n"
        "  brown    total: %.2f us  (%d/%zu ok, %d match)\n"
        "  zippel   total: %.2f us  (%d/%zu ok, %d match)\n"
        "  zippel2  total: %.2f us  (%d/%zu ok, %d match)\n"
        "  hensel   total: %.2f us  (%d/%zu ok, %d match)\n"
        "  subresult total: %.2f us  (%d/%zu ok, %d match)\n"
        "  ratios (vs cofactors):\n"
        "    brown:    %.3f%s\n"
        "    zippel:   %.3f%s\n"
        "    zippel2:  %.3f%s\n"
        "    hensel:   %.3f%s\n"
        "    subres:   %.3f%s\n",
        pairs.size(),
        tot_cof,
        tot_brown, n_brown, pairs.size(), n_brown_match,
        tot_zip,   n_zip,   pairs.size(), n_zip_match,
        tot_zip2,  n_zip2,  pairs.size(), n_zip2_match,
        tot_hen,   n_hen,   pairs.size(), n_hen_match,
        tot_sub,   n_sub,   pairs.size(), n_sub_match,
        n_brown ? tot_brown / tot_cof : 0.0, (tot_brown < tot_cof ? " (FASTER)" : ""),
        n_zip   ? tot_zip   / tot_cof : 0.0, (tot_zip   < tot_cof ? " (FASTER)" : ""),
        n_zip2  ? tot_zip2  / tot_cof : 0.0, (tot_zip2  < tot_cof ? " (FASTER)" : ""),
        n_hen   ? tot_hen   / tot_cof : 0.0, (tot_hen   < tot_cof ? " (FASTER)" : ""),
        n_sub   ? tot_sub   / tot_cof : 0.0, (tot_sub   < tot_cof ? " (FASTER)" : ""));

    return 0;
}
