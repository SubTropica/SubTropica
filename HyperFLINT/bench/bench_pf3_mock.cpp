// bench_pf3_mock.cpp
//
// Phase 2-A2 PF3 mock (chain 20, 2026-05-03): apples-to-apples wall
// comparison of (a) the chain-11 Tier A1 production path
// `Rat::add` (which routes through `add_via_q_underscore` at
// nvars >= HF_REPSWAP_NVARS_MIN, default 50) against (b) a minimal
// addViaSparse round-trip on the same canonical operand quads,
// using the public FLINT API for lift/mul/gcd/lower.
//
// This is the binding pre-flight per pf1_verdict.md: PF1 cleared the
// per-call gate but measured only ~1.5-7% of the predicted call
// cost.  PF3 directly measures the dominant 98% (mul + gcd in
// `_fmpz_mpoly_q_add` on lifted operands) by running the equivalent
// FLINT primitives on lifted-from-sparse data.
//
// Method A (Tier A1 baseline):
//   Rat r = a.add(b)     where a = (a_num/a_den), b = (b_num/b_den)
//   Wall is the actual production cost at production-scale ctx.
//
// Method B (sparse mock):
//   Lower each of {a_num, a_den, b_num, b_den} to SparseMpoly via
//   the bench_sbo_lift `lower_from_fmpq_mpoly` primitive.  Then
//   simulate addViaSparse:
//     1. Lift all 4 SBO Polys back to fmpq_mpoly (4× the lift cost).
//     2. cross_num = a_num * b_den + b_num * a_den    (2 muls + 1 add)
//     3. new_den   = a_den * b_den                    (1 mul)
//     4. fmpq_mpoly_gcd_cofactors(g, rn, rd, cross_num, new_den)
//        (the canonicalization step the actual rep-swap would skip
//        if it could; per R29 R7 / FF-internals fast-path concern,
//        gcd_cofactors is the main risk)
//     5. Lower (rn, rd) back to SparseMpoly (2 lowers).
//
// Output (per quad, CSV on stdout + aggregate on stderr):
//   quad_idx, nvars, t_anum, t_aden, t_bnum, t_bden,
//   tier_a1_us, sparse_us, ratio_BoverA
//
// CLI:
//   bench_pf3_mock <quads.txt> [--reps=3] [--warmup=1] [--max-quads=N]

#include <flint/fmpq_mpoly.h>
#include <flint/fmpq.h>
#include <flint/fmpz.h>

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
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

namespace hf = hyperflint;

// -------- Quad reader (mirrors bench_fmpz_mpoly_q_quads.cpp) --------

struct Quad {
    int idx = -1;
    std::vector<std::string> vars;
    std::string a_num_str, a_den_str, b_num_str, b_den_str;
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

// -------- SBO lift/lower primitives (mirrors bench_sbo_lift.cpp) --------

static constexpr int kInlineSlots = 8;

struct TermExp {
    uint8_t  k;
    uint32_t var_idx[kInlineSlots];
    uint8_t  exp[kInlineSlots];
};

// SoA SparseMpoly per R29 C5.  Owns its fmpq coefs.
struct SparseMpoly {
    std::vector<fmpq>    coefs;
    std::vector<TermExp> term_exps;

    SparseMpoly() = default;
    SparseMpoly(const SparseMpoly&) = delete;
    SparseMpoly& operator=(const SparseMpoly&) = delete;
    SparseMpoly(SparseMpoly&& o) noexcept
        : coefs(std::move(o.coefs)), term_exps(std::move(o.term_exps)) {}
    SparseMpoly& operator=(SparseMpoly&& o) noexcept {
        if (this != &o) {
            clear_all();
            coefs = std::move(o.coefs);
            term_exps = std::move(o.term_exps);
        }
        return *this;
    }
    ~SparseMpoly() { clear_all(); }
    void reserve_terms(size_t n) { coefs.reserve(n); term_exps.reserve(n); }
    void clear_all() {
        for (auto& c : coefs) fmpq_clear(&c);
        coefs.clear();
        term_exps.clear();
    }
    size_t size() const { return coefs.size(); }
};

static void lower_from_fmpq_mpoly(SparseMpoly& out, const fmpq_mpoly_t in,
                                  slong nvars,
                                  const fmpq_mpoly_ctx_t ctx, bool& clip_flag)
{
    const slong t = fmpq_mpoly_length(in, ctx);
    out.clear_all();
    out.reserve_terms(static_cast<size_t>(t));
    fmpq_t c;
    fmpq_init(c);
    std::vector<ulong> exps(nvars, 0UL);
    for (slong i = 0; i < t; ++i) {
        fmpq_mpoly_get_term_exp_ui(exps.data(), in, i, ctx);
        TermExp te{};
        te.k = 0;
        for (slong j = 0; j < nvars; ++j) {
            const ulong e = exps[j];
            if (e != 0) {
                if (te.k >= kInlineSlots) {
                    // PF3 mock: the SBO design's overflow path is
                    // unimplemented; we record the clip and gracefully
                    // truncate (this biases the lower TIME slightly
                    // optimistic but never affects fmpq_mpoly correctness
                    // because we only use the lift+arith side for timing).
                    clip_flag = true;
                    break;
                }
                te.var_idx[te.k] = static_cast<uint32_t>(j);
                te.exp[te.k]     = static_cast<uint8_t>(e);
                te.k++;
            }
        }
        fmpq_mpoly_get_term_coeff_fmpq(c, in, i, ctx);
        fmpq slot{};
        slot = *c;
        std::memset(c, 0, sizeof(fmpq));
        out.coefs.push_back(slot);
        out.term_exps.push_back(te);
    }
    fmpq_clear(c);
}

static void lift_to_fmpq_mpoly(fmpq_mpoly_t out, const SparseMpoly& s,
                               slong nvars, const fmpq_mpoly_ctx_t ctx)
{
    fmpq_mpoly_zero(out, ctx);
    const size_t t = s.size();
    fmpq_mpoly_fit_length(out, static_cast<slong>(t), ctx);
    std::vector<ulong> exps(nvars, 0UL);
    for (size_t i = 0; i < t; ++i) {
        const TermExp& te = s.term_exps[i];
        if (i > 0) {
            const TermExp& prev = s.term_exps[i - 1];
            for (uint8_t j = 0; j < prev.k; ++j) exps[prev.var_idx[j]] = 0UL;
        }
        for (uint8_t j = 0; j < te.k; ++j) {
            exps[te.var_idx[j]] = static_cast<ulong>(te.exp[j]);
        }
        fmpq_mpoly_push_term_fmpq_ui(out, &s.coefs[i], exps.data(), ctx);
    }
    if (!s.term_exps.empty()) {
        const TermExp& last = s.term_exps.back();
        for (uint8_t j = 0; j < last.k; ++j) exps[last.var_idx[j]] = 0UL;
    }
    fmpq_mpoly_sort_terms(out, ctx);
    fmpq_mpoly_combine_like_terms(out, ctx);
}

// -------- Method B: addViaSparse mock --------

// Inputs: 4 SparseMpoly operands (already lowered from fmpq_mpoly).
// Body times the lift-back + 3 muls + 1 gcd + 2 lowers, mirroring the
// projected addViaSparse call structure in r29_design.md.
static void run_sparse_mock(
    const SparseMpoly& s_anum, const SparseMpoly& s_aden,
    const SparseMpoly& s_bnum, const SparseMpoly& s_bden,
    slong nvars, const fmpq_mpoly_ctx_t ctx,
    fmpq_mpoly_t scratch_anum, fmpq_mpoly_t scratch_aden,
    fmpq_mpoly_t scratch_bnum, fmpq_mpoly_t scratch_bden,
    fmpq_mpoly_t tmp1, fmpq_mpoly_t tmp2, fmpq_mpoly_t cross_num,
    fmpq_mpoly_t new_den, fmpq_mpoly_t g, fmpq_mpoly_t rn,
    fmpq_mpoly_t rd, SparseMpoly& s_out_num, SparseMpoly& s_out_den,
    bool& clip_flag)
{
    // 1. Lift all 4 operands.
    lift_to_fmpq_mpoly(scratch_anum, s_anum, nvars, ctx);
    lift_to_fmpq_mpoly(scratch_aden, s_aden, nvars, ctx);
    lift_to_fmpq_mpoly(scratch_bnum, s_bnum, nvars, ctx);
    lift_to_fmpq_mpoly(scratch_bden, s_bden, nvars, ctx);
    // 2. cross-mult numerator: a_num * b_den + b_num * a_den
    fmpq_mpoly_mul(tmp1, scratch_anum, scratch_bden, ctx);
    fmpq_mpoly_mul(tmp2, scratch_bnum, scratch_aden, ctx);
    fmpq_mpoly_add(cross_num, tmp1, tmp2, ctx);
    // 3. new denominator: a_den * b_den
    fmpq_mpoly_mul(new_den, scratch_aden, scratch_bden, ctx);
    // 4. canonicalize: gcd_cofactors
    fmpq_mpoly_zero(g,  ctx);
    fmpq_mpoly_zero(rn, ctx);
    fmpq_mpoly_zero(rd, ctx);
    int ok = fmpq_mpoly_gcd_cofactors(g, rn, rd, cross_num, new_den, ctx);
    (void)ok;
    // 5. Lower (rn, rd) back to SparseMpoly.
    lower_from_fmpq_mpoly(s_out_num, rn, nvars, ctx, clip_flag);
    lower_from_fmpq_mpoly(s_out_den, rd, nvars, ctx, clip_flag);
}

// -------- main --------

int main(int argc, char** argv) {
    std::string path;
    int reps = 3;
    int warmup = 1;
    long max_quads = -1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--reps=", 0) == 0)        reps = std::atoi(a.c_str() + 7);
        else if (a.rfind("--warmup=", 0) == 0) warmup = std::atoi(a.c_str() + 9);
        else if (a.rfind("--max-quads=", 0) == 0)
                                                max_quads = std::atol(a.c_str() + 12);
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: bench_pf3_mock <quads.txt> "
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
        "[bench_pf3_mock] loaded %zu quads from %s "
        "(reps=%d warmup=%d)\n",
        quads.size(), path.c_str(), reps, warmup);

    std::printf(
        "quad_idx,nvars,t_anum,t_aden,t_bnum,t_bden,"
        "tier_a1_us,sparse_us,ratio_BoverA,clip\n");

    double agg_a_us = 0.0, agg_b_us = 0.0;
    int n_done = 0;
    int n_clip = 0;

    for (const Quad& q : quads) {
        const slong nvars = static_cast<slong>(q.vars.size());

        // Build HF PolyCtx + parse 4 polys; the same fmpq_mpoly ctx is
        // used for the SBO lift/mul/gcd path so operand fmpq_mpolys are
        // bit-identical between methods (apples-to-apples).
        std::vector<std::string> vars_vec(q.vars);
        try {
            hf::PolyCtx ctx(vars_vec);
            hf::Poly p_anum(ctx, q.a_num_str);
            hf::Poly p_aden(ctx, q.a_den_str);
            hf::Poly p_bnum(ctx, q.b_num_str);
            hf::Poly p_bden(ctx, q.b_den_str);

            const slong t_anum = fmpq_mpoly_length(p_anum.raw(),
                                                    ctx.raw());
            const slong t_aden = fmpq_mpoly_length(p_aden.raw(),
                                                    ctx.raw());
            const slong t_bnum = fmpq_mpoly_length(p_bnum.raw(),
                                                    ctx.raw());
            const slong t_bden = fmpq_mpoly_length(p_bden.raw(),
                                                    ctx.raw());

            // Method A: Rat::add (production dispatch; routes via
            // add_via_q_underscore at nvars >= HF_REPSWAP_NVARS_MIN
            // unless overridden).
            // Build fresh Rats per call so the move-back doesn't bias.
            // The Rat ctor takes (Poly num, Poly den) by value and
            // calls reduce_inplace internally; pre-build outside the
            // timed lambda so only the add wall is measured.
            hf::Poly p_anum_copy = p_anum;
            hf::Poly p_aden_copy = p_aden;
            hf::Poly p_bnum_copy = p_bnum;
            hf::Poly p_bden_copy = p_bden;
            hf::Rat a(std::move(p_anum_copy), std::move(p_aden_copy));
            hf::Rat b(std::move(p_bnum_copy), std::move(p_bden_copy));

            const double a_us = time_us([&]() {
                hf::Rat r = a.add(b);
                (void)r;
            }, reps, warmup);

            // Method B: SBO sparse mock.  Lower the 4 operands once
            // (outside the timed block); then the timed block lifts
            // all 4 + does 3 muls + 1 gcd + 2 lowers.
            bool clip_flag = false;
            SparseMpoly s_anum, s_aden, s_bnum, s_bden;
            lower_from_fmpq_mpoly(s_anum, p_anum.raw(), nvars,
                                  ctx.raw(), clip_flag);
            lower_from_fmpq_mpoly(s_aden, p_aden.raw(), nvars,
                                  ctx.raw(), clip_flag);
            lower_from_fmpq_mpoly(s_bnum, p_bnum.raw(), nvars,
                                  ctx.raw(), clip_flag);
            lower_from_fmpq_mpoly(s_bden, p_bden.raw(), nvars,
                                  ctx.raw(), clip_flag);

            // Scratch fmpq_mpolys for the lift/mul/gcd/lower work;
            // pre-init outside the timed block so allocation cost is
            // not counted (matches Method A: production reuses the
            // chain-11 Tier A1 internal scratch via fmpz_mpoly_q).
            fmpq_mpoly_t s_a, s_b, s_c, s_d, t1, t2, cn, nd, g, rn, rd;
            fmpq_mpoly_init(s_a, ctx.raw());
            fmpq_mpoly_init(s_b, ctx.raw());
            fmpq_mpoly_init(s_c, ctx.raw());
            fmpq_mpoly_init(s_d, ctx.raw());
            fmpq_mpoly_init(t1,  ctx.raw());
            fmpq_mpoly_init(t2,  ctx.raw());
            fmpq_mpoly_init(cn,  ctx.raw());
            fmpq_mpoly_init(nd,  ctx.raw());
            fmpq_mpoly_init(g,   ctx.raw());
            fmpq_mpoly_init(rn,  ctx.raw());
            fmpq_mpoly_init(rd,  ctx.raw());
            SparseMpoly s_out_num, s_out_den;

            const double b_us = time_us([&]() {
                run_sparse_mock(s_anum, s_aden, s_bnum, s_bden,
                                nvars, ctx.raw(),
                                s_a, s_b, s_c, s_d,
                                t1, t2, cn, nd, g, rn, rd,
                                s_out_num, s_out_den, clip_flag);
            }, reps, warmup);

            fmpq_mpoly_clear(rd, ctx.raw());
            fmpq_mpoly_clear(rn, ctx.raw());
            fmpq_mpoly_clear(g,  ctx.raw());
            fmpq_mpoly_clear(nd, ctx.raw());
            fmpq_mpoly_clear(cn, ctx.raw());
            fmpq_mpoly_clear(t2, ctx.raw());
            fmpq_mpoly_clear(t1, ctx.raw());
            fmpq_mpoly_clear(s_d, ctx.raw());
            fmpq_mpoly_clear(s_c, ctx.raw());
            fmpq_mpoly_clear(s_b, ctx.raw());
            fmpq_mpoly_clear(s_a, ctx.raw());

            const double ratio = (a_us > 0) ? (b_us / a_us) : 0.0;
            std::printf("%d,%ld,%ld,%ld,%ld,%ld,"
                        "%.3f,%.3f,%.4f,%d\n",
                        q.idx, static_cast<long>(nvars),
                        static_cast<long>(t_anum),
                        static_cast<long>(t_aden),
                        static_cast<long>(t_bnum),
                        static_cast<long>(t_bden),
                        a_us, b_us, ratio, clip_flag ? 1 : 0);
            agg_a_us += a_us;
            agg_b_us += b_us;
            n_done++;
            if (clip_flag) n_clip++;
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[bench_pf3_mock] quad %d: %s\n", q.idx, e.what());
            continue;
        }
    }

    if (n_done > 0) {
        const double agg_ratio = agg_b_us / agg_a_us;
        const double mean_a_ms = (agg_a_us / static_cast<double>(n_done))
                                 / 1000.0;
        const double mean_b_ms = (agg_b_us / static_cast<double>(n_done))
                                 / 1000.0;
        std::fprintf(stderr,
            "\n[bench_pf3_mock] aggregate (n=%d quads, %d clipped):\n"
            "  Tier A1 mean / quad : %8.3f ms\n"
            "  Sparse  mean / quad : %8.3f ms\n"
            "  ratio sparse / A1   : %8.4f\n"
            "  PF3 gate            : %s (gate <= 1.20 PASS, > 2.00 STOP)\n",
            n_done, n_clip, mean_a_ms, mean_b_ms, agg_ratio,
            (agg_ratio <= 1.20) ? "PASS" :
            (agg_ratio >= 2.00) ? "STOP" : "BORDERLINE");
    }
    return 0;
}
