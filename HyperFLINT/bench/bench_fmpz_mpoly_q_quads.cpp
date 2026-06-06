// bench_fmpz_mpoly_q_quads.cpp
//
// Branch I POC v2 (2026-05-01 evening): A/B add bench on canonical
// (a, b) operand quads dumped from production Rat::add /
// Rat::operator+= via HF_DUMP_RAT_QUADS=1.  Addresses adversarial
// reviewer F1+F2 (the v1 bench's reduce_inplace-input dump was in
// the wrong vars regime — max 7 vars vs production 718).
//
// Method A: the actual Rat::add cross-mult formula on canonical
// (a_num/a_den, b_num/b_den), then fmpq_mpoly_gcd_cofactors.
// Method B: fmpz_mpoly_q_add(out, q_a, q_b).
//
// Both methods get the SAME canonical operands, pre-built outside
// the timed block.  Apples-to-apples.
//
// Input format (lines):
//   QUAD <i>
//   VARS: v1,v2,...
//   A_NUM: ...
//   A_DEN: ...
//   B_NUM: ...
//   B_DEN: ...
//   ENDQUAD
//
// CLI:
//   bench_fmpz_mpoly_q_quads <quads.txt> [--reps=3] [--warmup=1]
//                             [--max-quads=N]

#include <flint/fmpq_mpoly.h>
#include <flint/fmpz_mpoly.h>
#include <flint/fmpz_mpoly_q.h>
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

// Build fmpz_mpoly_q from a fmpq_mpoly pair: q = (Nc * Nz) / (Dc * Dz)
// where Nc = num_q.content (fmpq), Nz = num_q.zpoly (fmpz_mpoly).
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
            std::printf("usage: bench_fmpz_mpoly_q_quads <quads.txt> "
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
        "[bench_quads] loaded %zu quads from %s (max=%ld)\n",
        quads.size(), path.c_str(), max_quads);
    std::fprintf(stderr,
        "[bench_quads] reps=%d warmup=%d\n", reps, warmup);

    std::printf(
        "quad_idx,nvars,a_num_len,a_den_len,b_num_len,b_den_len,"
        "add_A_us,add_B_us,add_ratio_BoverA,add_match\n");

    double tot_A = 0.0, tot_B = 0.0;
    int n_match = 0;
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

        // ---- Method A: actual Rat::add equivalent ----
        // new_num = a_num * b_den + b_num * a_den
        // new_den = a_den * b_den
        // gcd_cofactors(new_num, new_den) → (rn, rd)
        fmpq_mpoly_t tmp1, tmp2, new_num, new_den, g, rn, rd;
        fmpq_mpoly_init(tmp1,    ctx_q);
        fmpq_mpoly_init(tmp2,    ctx_q);
        fmpq_mpoly_init(new_num, ctx_q);
        fmpq_mpoly_init(new_den, ctx_q);
        fmpq_mpoly_init(g,       ctx_q);
        fmpq_mpoly_init(rn,      ctx_q);
        fmpq_mpoly_init(rd,      ctx_q);
        const double add_A_us = time_us([&]() {
            fmpq_mpoly_mul(tmp1, a_num_q, b_den_q, ctx_q);
            fmpq_mpoly_mul(tmp2, b_num_q, a_den_q, ctx_q);
            fmpq_mpoly_add(new_num, tmp1, tmp2, ctx_q);
            fmpq_mpoly_mul(new_den, a_den_q, b_den_q, ctx_q);
            int ok = fmpq_mpoly_gcd_cofactors(
                g, rn, rd, new_num, new_den, ctx_q);
            if (!ok) std::fprintf(stderr,
                "    quad %d cofactors failed\n", q.idx);
        }, reps, warmup);

        // ---- Method B: fmpz_mpoly_q_add ----
        fmpz_mpoly_q_t qa, qb, qout;
        fmpz_mpoly_q_init(qa,   ctx_z);
        fmpz_mpoly_q_init(qb,   ctx_z);
        fmpz_mpoly_q_init(qout, ctx_z);
        fmpz_mpoly_q_set_from_fmpq_pair(qa, a_num_q, a_den_q, ctx_z);
        fmpz_mpoly_q_canonicalise(qa, ctx_z);
        fmpz_mpoly_q_set_from_fmpq_pair(qb, b_num_q, b_den_q, ctx_z);
        fmpz_mpoly_q_canonicalise(qb, ctx_z);
        const double add_B_us = time_us([&]() {
            fmpz_mpoly_q_zero(qout, ctx_z);
            fmpz_mpoly_q_add(qout, qa, qb, ctx_z);
        }, reps, warmup);

        // Verify equivalence: build q_from_A from (rn, rd) and compare.
        fmpz_mpoly_q_t q_from_A;
        fmpz_mpoly_q_init(q_from_A, ctx_z);
        fmpz_mpoly_q_set_from_fmpq_pair(q_from_A, rn, rd, ctx_z);
        fmpz_mpoly_q_canonicalise(q_from_A, ctx_z);
        const int add_match = fmpz_mpoly_q_equal(q_from_A, qout, ctx_z);

        const double add_ratio = (add_A_us > 0.0) ? add_B_us / add_A_us : 0.0;
        std::printf(
            "%d,%lld,%lld,%lld,%lld,%lld,"
            "%.2f,%.2f,%.4f,%d\n",
            q.idx, static_cast<long long>(nvars),
            static_cast<long long>(an_len),
            static_cast<long long>(ad_len),
            static_cast<long long>(bn_len),
            static_cast<long long>(bd_len),
            add_A_us, add_B_us, add_ratio, add_match);
        std::fflush(stdout);

        tot_A += add_A_us;
        tot_B += add_B_us;
        if (add_match) ++n_match;
        ++n_done;

        fmpz_mpoly_q_clear(q_from_A, ctx_z);
        fmpz_mpoly_q_clear(qa,   ctx_z);
        fmpz_mpoly_q_clear(qb,   ctx_z);
        fmpz_mpoly_q_clear(qout, ctx_z);
        fmpq_mpoly_clear(tmp1,    ctx_q);
        fmpq_mpoly_clear(tmp2,    ctx_q);
        fmpq_mpoly_clear(new_num, ctx_q);
        fmpq_mpoly_clear(new_den, ctx_q);
        fmpq_mpoly_clear(g,       ctx_q);
        fmpq_mpoly_clear(rn,      ctx_q);
        fmpq_mpoly_clear(rd,      ctx_q);
        fmpq_mpoly_clear(a_num_q, ctx_q);
        fmpq_mpoly_clear(a_den_q, ctx_q);
        fmpq_mpoly_clear(b_num_q, ctx_q);
        fmpq_mpoly_clear(b_den_q, ctx_q);
        fmpq_mpoly_ctx_clear(ctx_q);
        fmpz_mpoly_ctx_clear(ctx_z);
    }

    const double ratio = (tot_A > 0.0) ? tot_B / tot_A : 0.0;
    std::fprintf(stderr,
        "\n[bench_quads] summary:\n"
        "  add: quads=%d match=%d  total_A=%.0f us  total_B=%.0f us  "
        "ratio_BoverA=%.4f  Δ=%+.1f%%  (%s)\n",
        n_done, n_match, tot_A, tot_B, ratio,
        100.0 * (tot_A - tot_B) / tot_A,
        (tot_B < tot_A) ? "B faster" : "A faster");
    return 0;
}
