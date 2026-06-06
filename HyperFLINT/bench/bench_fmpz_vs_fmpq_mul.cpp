// bench_fmpz_vs_fmpq_mul.cpp
//
// Branch I Pre-flight A (2026-05-01): on each canonical operand quad
// (a_num/a_den, b_num/b_den) dumped from production Rat::add via
// HF_DUMP_RAT_QUADS=1, compare:
//
//   Method A (timed, aggregate of 3):
//     fmpq_mpoly_mul(t1, a_num, b_den)   // cross-mult
//     fmpq_mpoly_mul(t2, b_num, a_den)   // cross-mult
//     fmpq_mpoly_mul(t3, a_den, b_den)   // new denom
//
//   Method B (timed, aggregate of 3):
//     fmpz_mpoly_mul(z1, mZ_anum, mZ_bden)
//     fmpz_mpoly_mul(z2, mZ_bnum, mZ_aden)
//     fmpz_mpoly_mul(z3, mZ_aden, mZ_bden)
//
// where mZ_* are fmpz_mpoly transduced from each fmpq_mpoly by scaling
// the content's numerator into zpoly and dropping the content's
// denominator (a pure content factor; we are NOT comparing A and B
// numerically, only timing the raw mul cost).
//
// Goal: characterise whether a planned Rat-rep swap (fmpq_mpoly pair
// → fmpz_mpoly_q) has a *multiplicative* tailwind on the cross-mults,
// beyond the +75.5% it shows on the add aggregate of v4 quads bench.
//
// Decision rule:
//   Δ% = 100 * (A - B) / A
//     ≥ +10%  → fmpz mul ≥10% faster: multiplicative tailwind
//                CONFIRMED.
//     ∈ (-10, +10) → wash: rep-swap's win is add-only.
//     ≤ -10%  → fmpz mul slower: flag for synthesis step.
//
// Input format (lines), same as bench_fmpz_mpoly_q_quads.cpp:
//   QUAD <i>
//   VARS: v1,v2,...
//   A_NUM: ...
//   A_DEN: ...
//   B_NUM: ...
//   B_DEN: ...
//   ENDQUAD
//
// CLI:
//   bench_fmpz_vs_fmpq_mul <quads.txt> [--reps=3] [--warmup=1]
//                                       [--max-quads=N]

#include <flint/fmpq_mpoly.h>
#include <flint/fmpz_mpoly.h>
#include <flint/fmpq.h>
#include <flint/fmpz.h>

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

// Translate a fmpq_mpoly into an fmpz_mpoly by scaling the content's
// numerator into the zpoly.  We deliberately DROP the content's
// denominator: the result differs from the original by a global
// rational factor, which does not affect the per-monomial cost
// pattern of the multiplication.  This bench times raw mul shape
// only; correctness comparison between methods is not performed.
static void fmpz_mpoly_set_from_fmpq(
    fmpz_mpoly_t out,
    fmpq_mpoly_t in_q,            // FLINT _t arrays don't survive const-decay; take non-const
    const fmpq_mpoly_ctx_t ctx_q,
    const fmpz_mpoly_ctx_t ctx_z)
{
    // out := in_q->zpoly * fmpq_numref(in_q->content)
    // (drops fmpq_denref(content) — pure global rational factor; we
    //  do not compare A/B numerically, only time the raw-mul shape.)
    fmpz_mpoly_set(out, fmpq_mpoly_zpoly_ref(in_q, ctx_q), ctx_z);
    fmpq* c = fmpq_mpoly_content_ref(in_q, ctx_q);
    fmpz_mpoly_scalar_mul_fmpz(out, out, fmpq_numref(c), ctx_z);
}

static double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    if (p <= 0.0) return v.front();
    if (p >= 1.0) return v.back();
    const double idx = p * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, v.size() - 1);
    const double f = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - f) + v[hi] * f;
}

int main(int argc, char** argv) {
    std::string path;
    int reps = 3;
    int warmup = 1;
    long max_quads = -1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--reps=", 0) == 0) reps = std::atoi(a.c_str() + 7);
        else if (a.rfind("--warmup=", 0) == 0) warmup = std::atoi(a.c_str() + 9);
        else if (a.rfind("--max-quads=", 0) == 0)
            max_quads = std::atol(a.c_str() + 12);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: bench_fmpz_vs_fmpq_mul <quads.txt> "
                        "[--reps=3] [--warmup=1] [--max-quads=N]\n");
            return 0;
        } else if (path.empty()) path = a;
    }
    if (path.empty()) {
        std::fprintf(stderr, "missing quads.txt path\n");
        return 1;
    }

    auto quads = read_quads(path, max_quads);
    std::fprintf(stderr,
        "[bench_fmpz_vs_fmpq_mul] loaded %zu quads from %s (max=%ld)\n",
        quads.size(), path.c_str(), max_quads);
    std::fprintf(stderr,
        "[bench_fmpz_vs_fmpq_mul] reps=%d warmup=%d\n", reps, warmup);

    std::printf(
        "quad_idx,nvars,a_num_len,a_den_len,b_num_len,b_den_len,"
        "A_us,B_us,ratio_BoverA\n");

    double tot_A = 0.0, tot_B = 0.0;
    int n_done = 0;
    int n_B_faster = 0;
    int worst_idx = -1;   // B slowest (largest ratio_BoverA)
    int best_idx  = -1;   // B fastest (smallest ratio_BoverA)
    double worst_ratio = -1.0;
    double best_ratio  = 1e30;
    std::vector<double> ratios;
    ratios.reserve(quads.size());

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

        fmpq_mpoly_t a_num_q, a_den_q, b_num_q, b_den_q;
        fmpq_mpoly_init(a_num_q, ctx_q);
        fmpq_mpoly_init(a_den_q, ctx_q);
        fmpq_mpoly_init(b_num_q, ctx_q);
        fmpq_mpoly_init(b_den_q, ctx_q);

        bool parse_ok = true;
        if (fmpq_mpoly_set_str_pretty(a_num_q, q.a_num_str.c_str(),
                                       vptrs.data(), ctx_q) != 0) parse_ok = false;
        if (parse_ok && fmpq_mpoly_set_str_pretty(a_den_q, q.a_den_str.c_str(),
                                       vptrs.data(), ctx_q) != 0) parse_ok = false;
        if (parse_ok && fmpq_mpoly_set_str_pretty(b_num_q, q.b_num_str.c_str(),
                                       vptrs.data(), ctx_q) != 0) parse_ok = false;
        if (parse_ok && fmpq_mpoly_set_str_pretty(b_den_q, q.b_den_str.c_str(),
                                       vptrs.data(), ctx_q) != 0) parse_ok = false;
        if (!parse_ok) {
            std::fprintf(stderr, "  quad %d parse failed\n", q.idx);
            fmpq_mpoly_clear(a_num_q, ctx_q);
            fmpq_mpoly_clear(a_den_q, ctx_q);
            fmpq_mpoly_clear(b_num_q, ctx_q);
            fmpq_mpoly_clear(b_den_q, ctx_q);
            fmpq_mpoly_ctx_clear(ctx_q);
            fmpz_mpoly_ctx_clear(ctx_z);
            continue;
        }

        const slong an_len = fmpq_mpoly_length(a_num_q, ctx_q);
        const slong ad_len = fmpq_mpoly_length(a_den_q, ctx_q);
        const slong bn_len = fmpq_mpoly_length(b_num_q, ctx_q);
        const slong bd_len = fmpq_mpoly_length(b_den_q, ctx_q);

        // ---------- Method A (fmpq_mpoly_mul aggregate of 3) ----------
        fmpq_mpoly_t qt1, qt2, qt3;
        fmpq_mpoly_init(qt1, ctx_q);
        fmpq_mpoly_init(qt2, ctx_q);
        fmpq_mpoly_init(qt3, ctx_q);
        const double A_us = time_us([&]() {
            fmpq_mpoly_mul(qt1, a_num_q, b_den_q, ctx_q);  // a_num * b_den
            fmpq_mpoly_mul(qt2, b_num_q, a_den_q, ctx_q);  // b_num * a_den
            fmpq_mpoly_mul(qt3, a_den_q, b_den_q, ctx_q);  // a_den * b_den
        }, reps, warmup);

        // ---------- Method B (fmpz_mpoly_mul aggregate of 3) ----------
        fmpz_mpoly_t mZ_an, mZ_ad, mZ_bn, mZ_bd;
        fmpz_mpoly_init(mZ_an, ctx_z);
        fmpz_mpoly_init(mZ_ad, ctx_z);
        fmpz_mpoly_init(mZ_bn, ctx_z);
        fmpz_mpoly_init(mZ_bd, ctx_z);
        fmpz_mpoly_set_from_fmpq(mZ_an, a_num_q, ctx_q, ctx_z);
        fmpz_mpoly_set_from_fmpq(mZ_ad, a_den_q, ctx_q, ctx_z);
        fmpz_mpoly_set_from_fmpq(mZ_bn, b_num_q, ctx_q, ctx_z);
        fmpz_mpoly_set_from_fmpq(mZ_bd, b_den_q, ctx_q, ctx_z);

        fmpz_mpoly_t zt1, zt2, zt3;
        fmpz_mpoly_init(zt1, ctx_z);
        fmpz_mpoly_init(zt2, ctx_z);
        fmpz_mpoly_init(zt3, ctx_z);
        const double B_us = time_us([&]() {
            fmpz_mpoly_mul(zt1, mZ_an, mZ_bd, ctx_z);
            fmpz_mpoly_mul(zt2, mZ_bn, mZ_ad, ctx_z);
            fmpz_mpoly_mul(zt3, mZ_ad, mZ_bd, ctx_z);
        }, reps, warmup);

        const double ratio = (A_us > 0.0) ? B_us / A_us : 0.0;
        std::printf(
            "%d,%lld,%lld,%lld,%lld,%lld,"
            "%.2f,%.2f,%.4f\n",
            q.idx, static_cast<long long>(nvars),
            static_cast<long long>(an_len),
            static_cast<long long>(ad_len),
            static_cast<long long>(bn_len),
            static_cast<long long>(bd_len),
            A_us, B_us, ratio);
        std::fflush(stdout);

        tot_A += A_us;
        tot_B += B_us;
        if (B_us < A_us) ++n_B_faster;
        if (ratio > worst_ratio) { worst_ratio = ratio; worst_idx = q.idx; }
        if (ratio < best_ratio)  { best_ratio  = ratio; best_idx  = q.idx; }
        ratios.push_back(ratio);
        ++n_done;

        fmpz_mpoly_clear(zt1, ctx_z);
        fmpz_mpoly_clear(zt2, ctx_z);
        fmpz_mpoly_clear(zt3, ctx_z);
        fmpz_mpoly_clear(mZ_an, ctx_z);
        fmpz_mpoly_clear(mZ_ad, ctx_z);
        fmpz_mpoly_clear(mZ_bn, ctx_z);
        fmpz_mpoly_clear(mZ_bd, ctx_z);
        fmpq_mpoly_clear(qt1, ctx_q);
        fmpq_mpoly_clear(qt2, ctx_q);
        fmpq_mpoly_clear(qt3, ctx_q);
        fmpq_mpoly_clear(a_num_q, ctx_q);
        fmpq_mpoly_clear(a_den_q, ctx_q);
        fmpq_mpoly_clear(b_num_q, ctx_q);
        fmpq_mpoly_clear(b_den_q, ctx_q);
        fmpq_mpoly_ctx_clear(ctx_q);
        fmpz_mpoly_ctx_clear(ctx_z);
    }

    const double agg_ratio = (tot_A > 0.0) ? tot_B / tot_A : 0.0;
    const double delta_pct = (tot_A > 0.0)
        ? 100.0 * (tot_A - tot_B) / tot_A : 0.0;
    const double p25  = percentile(ratios, 0.25);
    const double p50  = percentile(ratios, 0.50);
    const double p75  = percentile(ratios, 0.75);
    const double pmin = ratios.empty() ? 0.0 :
                        *std::min_element(ratios.begin(), ratios.end());
    const double pmax = ratios.empty() ? 0.0 :
                        *std::max_element(ratios.begin(), ratios.end());

    const char* verdict =
        (delta_pct >= 10.0)   ? "fmpz mul >=10% faster: multiplicative tailwind CONFIRMED" :
        (delta_pct <= -10.0)  ? "fmpz mul SLOWER: flag for synthesis" :
                                "WASH (rep-swap's win is add-only)";

    std::fprintf(stderr,
        "\n[bench_fmpz_vs_fmpq_mul] summary:\n"
        "  quads=%d  total_A=%.0f us  total_B=%.0f us\n"
        "  ratio_BoverA=%.4f  delta=%+.2f%%\n"
        "  per-quad ratio: min=%.4f p25=%.4f p50=%.4f p75=%.4f max=%.4f\n"
        "  B_faster_count=%d / %d (%.1f%%)\n"
        "  worst_quad (B slowest, ratio=%.3f) = %d\n"
        "  best_quad  (B fastest, ratio=%.3f) = %d\n"
        "  verdict: %s\n",
        n_done, tot_A, tot_B,
        agg_ratio, delta_pct,
        pmin, p25, p50, p75, pmax,
        n_B_faster, n_done,
        (n_done > 0) ? 100.0 * n_B_faster / n_done : 0.0,
        worst_ratio, worst_idx,
        best_ratio, best_idx,
        verdict);
    return 0;
}
