// bench_fmpz_mpoly_q.cpp
//
// Branch I POC (2026-05-01 evening): A/B bench of the current Rat
// representation (pair of fmpq_mpoly + explicit reduce_inplace via
// fmpq_mpoly_gcd_cofactors) against FLINT's eager-canonical
// fmpz_mpoly_q (pair of fmpz_mpoly + content factor + implicit
// canonicalise on every op).
//
// Decision rule per the kickoff prompt: if fmpz_mpoly_q aggregate is
// ≥ 20 % faster on the equivalent of Rat::add AND maintains
// correctness, spec a full rep-swap; else close.
//
// Two A/B comparisons:
//   1. reduce-only:
//        Method A: fmpq_mpoly_gcd_cofactors(g, rn, rd, num, den, ctx_q)
//        Method B: fmpz_mpoly_q_canonicalise(q, ctx_z) where q was
//                  set with non-canonical (num, den) every iteration.
//   2. add-self:
//        Method A: new_num = 2 * num * den;  new_den = den * den;
//                  fmpq_mpoly_gcd_cofactors(...)
//        Method B: fmpz_mpoly_q_add(out, q, q, ctx_z)
//
// Inputs: a 50-pair dump from Avenue H/G probes, format documented in
//   HyperFLINT/bench/bench_brown_gcd.cpp:1-25.
//
// Output: CSV per pair on stdout + aggregate summary on stderr.
//
// CLI:
//   bench_fmpz_mpoly_q <pairs.txt> [--reps=3] [--warmup=1]
//                       [--mode=both|reduce|add]
//
// Verify-on-result: after the timed runs, compute Method B's
// canonical (num, den) and compare equal to Method A's (rn, rd) for
// the reduce bench, or compare equivalent fmpz_mpoly_q outputs
// derived from both methods for the add bench. A "match=0" flags a
// representation mismatch; the row is still printed for inspection.

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

// Convert a fmpq_mpoly polynomial to (fmpz_mpoly poly, fmpq scalar)
// such that fmpq_mpoly = scalar * fmpz_mpoly.  In FLINT's fmpq_mpoly,
// content is stored externally (raw->content, an fmpq_t) and the
// integer body is raw->zpoly (an fmpz_mpoly_t).  Just clone both.
static void fmpq_mpoly_to_zpoly_and_content(
    fmpz_mpoly_t out_zpoly, fmpq_t out_content,
    const fmpq_mpoly_t in, const fmpq_mpoly_ctx_t ctx_q,
    const fmpz_mpoly_ctx_t ctx_z)
{
    (void)ctx_q;
    fmpz_mpoly_set(out_zpoly, in->zpoly, ctx_z);
    fmpq_set(out_content, in->content);
}

// Build a fmpz_mpoly_q from (num_q, den_q) pair of fmpq_mpoly:
// q->num <- (num content scaled into integer) * num_zpoly ?  Simpler:
// q_num = N_z * D_dq (where D_dq is the integer-scale of den's content)
// q_den = D_z * N_dq (where N_dq is the integer-scale of num's content)
// Actually the cleanest:
//   q = num_q / den_q
//     = (Nc * Nz) / (Dc * Dz)        (Nc = num content, Nz = num zpoly)
//     = (Nc.num * Dc.den * Nz) / (Nc.den * Dc.num * Dz)
// This puts the rational scalars purely as fmpz multipliers, and the
// fmpz_mpoly_q canonicaliser will then reduce.
static void fmpz_mpoly_q_set_from_fmpq_pair(
    fmpz_mpoly_q_t q,
    const fmpq_mpoly_t num_q, const fmpq_mpoly_t den_q,
    const fmpz_mpoly_ctx_t ctx_z)
{
    // num scalar = num_q.content.num * den_q.content.den
    // den scalar = num_q.content.den * den_q.content.num
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
    std::string mode = "both";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--reps=", 0) == 0) reps = std::atoi(a.c_str() + 7);
        else if (a.rfind("--warmup=", 0) == 0) warmup = std::atoi(a.c_str() + 9);
        else if (a.rfind("--mode=", 0) == 0) mode = a.substr(7);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: bench_fmpz_mpoly_q <pairs.txt> "
                        "[--reps=3] [--warmup=1] [--mode=both|reduce|add]\n");
            return 0;
        } else if (path.empty()) path = a;
    }
    if (path.empty()) {
        std::fprintf(stderr, "missing pairs.txt path\n");
        return 1;
    }
    const bool do_reduce = (mode == "both" || mode == "reduce");
    const bool do_add    = (mode == "both" || mode == "add");

    auto pairs = read_pairs(path);
    std::fprintf(stderr,
        "[bench_fmpz_mpoly_q] loaded %zu pairs from %s\n",
        pairs.size(), path.c_str());
    std::fprintf(stderr,
        "[bench_fmpz_mpoly_q] reps=%d warmup=%d mode=%s\n",
        reps, warmup, mode.c_str());

    std::printf(
        "pair_idx,nvars,num_len,den_len,"
        "reduce_A_us,reduce_B_us,reduce_ratio_BoverA,reduce_match,"
        "add_A_us,add_B_us,add_ratio_BoverA,add_match\n");

    double tot_red_A = 0.0, tot_red_B = 0.0;
    double tot_add_A = 0.0, tot_add_B = 0.0;
    int n_red_match = 0, n_add_match = 0;
    int n_red = 0, n_add = 0;

    for (size_t pi = 0; pi < pairs.size(); ++pi) {
        const Pair& p = pairs[pi];
        const slong nvars = static_cast<slong>(p.vars.size());

        fmpq_mpoly_ctx_t ctx_q;
        fmpq_mpoly_ctx_init(ctx_q, nvars, ORD_LEX);
        // fmpz_mpoly_ctx with same nvars / order, used for fmpz_mpoly_q.
        fmpz_mpoly_ctx_t ctx_z;
        fmpz_mpoly_ctx_init(ctx_z, nvars, ORD_LEX);

        std::vector<const char*> vptrs;
        vptrs.reserve(p.vars.size());
        for (const auto& v : p.vars) vptrs.push_back(v.c_str());

        fmpq_mpoly_t num_q, den_q;
        fmpq_mpoly_init(num_q, ctx_q);
        fmpq_mpoly_init(den_q, ctx_q);
        if (fmpq_mpoly_set_str_pretty(num_q, p.num_str.c_str(),
                                       vptrs.data(), ctx_q) != 0) {
            std::fprintf(stderr, "  pair %d NUM parse failed\n", p.idx);
            fmpq_mpoly_clear(num_q, ctx_q);
            fmpq_mpoly_clear(den_q, ctx_q);
            fmpq_mpoly_ctx_clear(ctx_q);
            fmpz_mpoly_ctx_clear(ctx_z);
            continue;
        }
        if (fmpq_mpoly_set_str_pretty(den_q, p.den_str.c_str(),
                                       vptrs.data(), ctx_q) != 0) {
            std::fprintf(stderr, "  pair %d DEN parse failed\n", p.idx);
            fmpq_mpoly_clear(num_q, ctx_q);
            fmpq_mpoly_clear(den_q, ctx_q);
            fmpq_mpoly_ctx_clear(ctx_q);
            fmpz_mpoly_ctx_clear(ctx_z);
            continue;
        }
        const slong num_len = fmpq_mpoly_length(num_q, ctx_q);
        const slong den_len = fmpq_mpoly_length(den_q, ctx_q);

        // ------------------ reduce bench ------------------
        double red_A_us = 0.0, red_B_us = 0.0;
        int red_match = 0;
        if (do_reduce) {
            fmpq_mpoly_t g_A, rn_A, rd_A;
            fmpq_mpoly_init(g_A, ctx_q);
            fmpq_mpoly_init(rn_A, ctx_q);
            fmpq_mpoly_init(rd_A, ctx_q);
            red_A_us = time_us([&]() {
                fmpq_mpoly_zero(g_A,  ctx_q);
                fmpq_mpoly_zero(rn_A, ctx_q);
                fmpq_mpoly_zero(rd_A, ctx_q);
                int ok = fmpq_mpoly_gcd_cofactors(
                    g_A, rn_A, rd_A, num_q, den_q, ctx_q);
                if (!ok) std::fprintf(stderr,
                    "    pair %d cofactors failed\n", p.idx);
            }, reps, warmup);

            // Method B: fresh non-canonical fmpz_mpoly_q each iteration,
            // canonicalise.  Construction is amortised below the timing
            // window — only the canonicalise call is timed.
            red_B_us = time_us([&]() {
                fmpz_mpoly_q_t q;
                fmpz_mpoly_q_init(q, ctx_z);
                fmpz_mpoly_q_set_from_fmpq_pair(q, num_q, den_q, ctx_z);
                // fmpz_mpoly_q_set_from_fmpq_pair already produces a
                // canonical (or near-canonical) representation if the
                // pair was canonical in the fmpq world.  To force real
                // work we clear and re-set inside the timed block —
                // measure construction + canonicalise + the call
                // pattern that fmpz_mpoly_q_canonicalise does.  This
                // matches Method A's cost which always does a fresh
                // gcd_cofactors call.
                fmpz_mpoly_q_canonicalise(q, ctx_z);
                fmpz_mpoly_q_clear(q, ctx_z);
            }, reps, warmup);

            // Verify equivalence of canonical forms.  Build a
            // canonical fmpz_mpoly_q from Method A's output (rn_A,
            // rd_A) and compare against a freshly canonicalised
            // Method B q.
            fmpz_mpoly_q_t q_from_A, q_B_check;
            fmpz_mpoly_q_init(q_from_A, ctx_z);
            fmpz_mpoly_q_init(q_B_check, ctx_z);
            // q_from_A := (rn_A / rd_A) via the helper; canonicalise.
            fmpz_mpoly_q_set_from_fmpq_pair(q_from_A, rn_A, rd_A, ctx_z);
            fmpz_mpoly_q_canonicalise(q_from_A, ctx_z);
            // q_B_check := (num_q / den_q) via the helper; canonicalise.
            fmpz_mpoly_q_set_from_fmpq_pair(q_B_check, num_q, den_q, ctx_z);
            fmpz_mpoly_q_canonicalise(q_B_check, ctx_z);
            red_match = fmpz_mpoly_q_equal(q_from_A, q_B_check, ctx_z);

            fmpz_mpoly_q_clear(q_from_A, ctx_z);
            fmpz_mpoly_q_clear(q_B_check, ctx_z);
            fmpq_mpoly_clear(g_A,  ctx_q);
            fmpq_mpoly_clear(rn_A, ctx_q);
            fmpq_mpoly_clear(rd_A, ctx_q);

            tot_red_A += red_A_us;
            tot_red_B += red_B_us;
            ++n_red;
            if (red_match) ++n_red_match;
        }

        // ------------------ add bench ------------------
        // We use (a = num/den) and (b = num/(den+1)), distinct
        // operands so that Method B cannot short-circuit on alias.
        // The add formula:
        //   a + b = (num*(den+1) + num*den) / (den*(den+1))
        //         = num*(2*den + 1) / (den*(den+1))
        // Both methods do real work; the work shapes are equivalent
        // up to fmpq vs fmpz internal representation.
        double add_A_us = 0.0, add_B_us = 0.0;
        int add_match = 0;
        if (do_add) {
            // Pre-canonicalise the Method A operand pair (num_q,
            // den_q).  In the production hot path Rat::add is called
            // on already-canonical Rats; the dump is of the
            // reduce_inplace INPUT, i.e., the non-canonical form
            // _before_ reduction.  To match what Rat::add actually
            // pays, we canonicalise the operands ONCE here (outside
            // the timed block) — the same pre-condition Method B
            // already enjoys via its q_a / q_b canonicalise.
            fmpq_mpoly_t na_q, da_q;
            fmpq_mpoly_init(na_q, ctx_q);
            fmpq_mpoly_init(da_q, ctx_q);
            {
                fmpq_mpoly_t g_pre;
                fmpq_mpoly_init(g_pre, ctx_q);
                fmpq_mpoly_gcd_cofactors(g_pre, na_q, da_q,
                                          num_q, den_q, ctx_q);
                fmpq_mpoly_clear(g_pre, ctx_q);
            }
            // Build db_q := da_q + 1 — distinct, coprime to da_q.
            fmpq_mpoly_t db_q;
            fmpq_mpoly_init(db_q, ctx_q);
            fmpq_mpoly_set(db_q, da_q, ctx_q);
            fmpq_mpoly_add_si(db_q, db_q, 1, ctx_q);

            fmpq_mpoly_t tmp1, tmp2, new_num, new_den, g, rn, rd;
            fmpq_mpoly_init(tmp1,    ctx_q);
            fmpq_mpoly_init(tmp2,    ctx_q);
            fmpq_mpoly_init(new_num, ctx_q);
            fmpq_mpoly_init(new_den, ctx_q);
            fmpq_mpoly_init(g,       ctx_q);
            fmpq_mpoly_init(rn,      ctx_q);
            fmpq_mpoly_init(rd,      ctx_q);
            add_A_us = time_us([&]() {
                // a = na/da, b = na/db
                fmpq_mpoly_mul(tmp1, na_q, db_q, ctx_q);
                fmpq_mpoly_mul(tmp2, na_q, da_q, ctx_q);
                fmpq_mpoly_add(new_num, tmp1, tmp2, ctx_q);
                fmpq_mpoly_mul(new_den, da_q, db_q, ctx_q);
                int ok = fmpq_mpoly_gcd_cofactors(
                    g, rn, rd, new_num, new_den, ctx_q);
                if (!ok) std::fprintf(stderr,
                    "    pair %d add cofactors failed\n", p.idx);
            }, reps, warmup);

            // Method B: pre-construct q_a, q_b outside the timed
            // block from the SAME canonical (na_q, da_q) and
            // (na_q, db_q) pairs Method A uses, so both methods
            // have apples-to-apples canonical inputs.
            fmpz_mpoly_q_t q_a, q_b, q_out;
            fmpz_mpoly_q_init(q_a,   ctx_z);
            fmpz_mpoly_q_init(q_b,   ctx_z);
            fmpz_mpoly_q_init(q_out, ctx_z);
            fmpz_mpoly_q_set_from_fmpq_pair(q_a, na_q, da_q, ctx_z);
            fmpz_mpoly_q_canonicalise(q_a, ctx_z);
            fmpz_mpoly_q_set_from_fmpq_pair(q_b, na_q, db_q, ctx_z);
            fmpz_mpoly_q_canonicalise(q_b, ctx_z);
            add_B_us = time_us([&]() {
                fmpz_mpoly_q_zero(q_out, ctx_z);
                fmpz_mpoly_q_add(q_out, q_a, q_b, ctx_z);
            }, reps, warmup);

            // Verify: Method A's (rn, rd) and Method B's q_out
            // should represent the same rational.  Compare via
            // canonical fmpz_mpoly_q.
            fmpz_mpoly_q_t q_from_A;
            fmpz_mpoly_q_init(q_from_A, ctx_z);
            fmpz_mpoly_q_set_from_fmpq_pair(q_from_A, rn, rd, ctx_z);
            fmpz_mpoly_q_canonicalise(q_from_A, ctx_z);
            add_match = fmpz_mpoly_q_equal(q_from_A, q_out, ctx_z);

            fmpz_mpoly_q_clear(q_from_A, ctx_z);
            fmpz_mpoly_q_clear(q_a,   ctx_z);
            fmpz_mpoly_q_clear(q_b,   ctx_z);
            fmpz_mpoly_q_clear(q_out, ctx_z);
            fmpq_mpoly_clear(na_q,    ctx_q);
            fmpq_mpoly_clear(da_q,    ctx_q);
            fmpq_mpoly_clear(db_q,    ctx_q);
            fmpq_mpoly_clear(tmp1,    ctx_q);
            fmpq_mpoly_clear(tmp2,    ctx_q);
            fmpq_mpoly_clear(new_num, ctx_q);
            fmpq_mpoly_clear(new_den, ctx_q);
            fmpq_mpoly_clear(g,       ctx_q);
            fmpq_mpoly_clear(rn,      ctx_q);
            fmpq_mpoly_clear(rd,      ctx_q);

            tot_add_A += add_A_us;
            tot_add_B += add_B_us;
            ++n_add;
            if (add_match) ++n_add_match;
        }

        const double red_ratio = (red_A_us > 0.0) ? red_B_us / red_A_us : 0.0;
        const double add_ratio = (add_A_us > 0.0) ? add_B_us / add_A_us : 0.0;
        std::printf(
            "%d,%lld,%lld,%lld,"
            "%.2f,%.2f,%.4f,%d,"
            "%.2f,%.2f,%.4f,%d\n",
            p.idx,
            static_cast<long long>(nvars),
            static_cast<long long>(num_len),
            static_cast<long long>(den_len),
            red_A_us, red_B_us, red_ratio, red_match,
            add_A_us, add_B_us, add_ratio, add_match);
        std::fflush(stdout);

        fmpq_mpoly_clear(num_q, ctx_q);
        fmpq_mpoly_clear(den_q, ctx_q);
        fmpq_mpoly_ctx_clear(ctx_q);
        fmpz_mpoly_ctx_clear(ctx_z);
    }

    std::fprintf(stderr,
        "\n[bench_fmpz_mpoly_q] summary:\n");
    if (do_reduce) {
        const double red_ratio = (tot_red_A > 0.0) ? tot_red_B / tot_red_A : 0.0;
        std::fprintf(stderr,
            "  reduce: pairs=%d match=%d  total_A=%.0f us  total_B=%.0f us  "
            "ratio_BoverA=%.4f  Δ=%+.1f%%  (%s)\n",
            n_red, n_red_match, tot_red_A, tot_red_B, red_ratio,
            100.0 * (tot_red_A - tot_red_B) / tot_red_A,
            (tot_red_B < tot_red_A) ? "B faster" : "A faster");
    }
    if (do_add) {
        const double add_ratio = (tot_add_A > 0.0) ? tot_add_B / tot_add_A : 0.0;
        std::fprintf(stderr,
            "  add:    pairs=%d match=%d  total_A=%.0f us  total_B=%.0f us  "
            "ratio_BoverA=%.4f  Δ=%+.1f%%  (%s)\n",
            n_add, n_add_match, tot_add_A, tot_add_B, add_ratio,
            100.0 * (tot_add_A - tot_add_B) / tot_add_A,
            (tot_add_B < tot_add_A) ? "B faster" : "A faster");
    }
    return 0;
}
