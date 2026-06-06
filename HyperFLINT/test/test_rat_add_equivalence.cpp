// Layer 1 — per-call semantic equivalence test for the Branch I
// rep-swap (`Rat::add` migrating to `_fmpz_mpoly_q_add` underscore
// primitive + narrow-ctx hoist).
//
// Plan: notes/3l3pt_rep_swap_plan/plan.md (§"Falsifier protocol"
// Layer 1).  Reviewer round 14 explicitly demoted bit-identity to
// semantic equivalence after observing that `Rat::reduce_inplace`'s
// Avenue I path absorbs constants into `num` while
// `_fmpz_mpoly_q_canonicalise` distributes content as the integer-GCD
// lift; the two canonical forms differ as strings but represent the
// same rational function.
//
// Equivalence check: convert each Rat into `fmpz_mpoly_q_t` via the
// bench's `fmpz_mpoly_q_set_from_fmpq_pair` helper, run
// `fmpz_mpoly_q_canonicalise`, and compare via `fmpz_mpoly_q_equal`.
// This canonical form absorbs every content-distribution difference.
//
// Workload: 200 production-canonical operand quads dumped from
// `Rat::add` / `Rat::operator+=` entry on parity-1 ord_1 face_1
// (see notes/3l3pt_branch_I_fmpz_q_poc/findings.md §"Production
// workload reality").  Same input file as
// HyperFLINT/bench/bench_fmpz_mpoly_q_quads_v5.cpp.
//
// Failure mode: prints the failing quad index, the legacy result
// string, the new result string, and exits with non-zero status.
//
// Usage:
//   timeout 300 OMP_NUM_THREADS=13
//     ./hyperflint-test-rat-add-equivalence
//     <path_to_rat_quads.txt> [--max-quads=N]

#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"

#include <flint/fmpq.h>
#include <flint/fmpq_mpoly.h>
#include <flint/fmpz.h>
#include <flint/fmpz_mpoly.h>
#include <flint/fmpz_mpoly_q.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace hyperflint;

namespace {

struct Quad {
    int idx = -1;
    std::vector<std::string> vars;
    std::string a_num_str;
    std::string a_den_str;
    std::string b_num_str;
    std::string b_den_str;
};

std::vector<Quad> read_quads(const std::string& path, long max_quads) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr,
            "could not open quads file %s: %s\n",
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

// Build fmpz_mpoly_q from a (Poly num, Poly den) pair.  Inverse of HF's
// fmpq_mpoly representation: q = (Cn * Nz) / (Cd * Dz) collapsed into
// fmpz_mpoly form (Cn = num content, Cd = den content). After
// fmpz_mpoly_q_canonicalise this gives the unique-up-to-content
// canonical form FLINT enforces (gcd(num,den) = 1, integer content
// gcd factored, den's leading coefficient positive).
//
// Same logic as bench/bench_fmpz_mpoly_q_quads_v5.cpp's
// fmpz_mpoly_q_set_from_fmpq_pair, refactored to consume Poly handles.
void fmpz_mpoly_q_set_from_rat_polys(
    fmpz_mpoly_q_t q,
    const Poly& num_q, const Poly& den_q,
    const fmpz_mpoly_ctx_t ctx_z)
{
    fmpq_mpoly_struct* num_raw =
        const_cast<fmpq_mpoly_struct*>(num_q.raw());
    fmpq_mpoly_struct* den_raw =
        const_cast<fmpq_mpoly_struct*>(den_q.raw());
    fmpz_t s_num, s_den;
    fmpz_init(s_num);
    fmpz_init(s_den);
    fmpz_mul(s_num, fmpq_numref(num_raw->content),
                    fmpq_denref(den_raw->content));
    fmpz_mul(s_den, fmpq_denref(num_raw->content),
                    fmpq_numref(den_raw->content));
    fmpz_mpoly_set(fmpz_mpoly_q_numref(q), num_raw->zpoly, ctx_z);
    fmpz_mpoly_scalar_mul_fmpz(fmpz_mpoly_q_numref(q),
        fmpz_mpoly_q_numref(q), s_num, ctx_z);
    fmpz_mpoly_set(fmpz_mpoly_q_denref(q), den_raw->zpoly, ctx_z);
    fmpz_mpoly_scalar_mul_fmpz(fmpz_mpoly_q_denref(q),
        fmpz_mpoly_q_denref(q), s_den, ctx_z);
    fmpz_clear(s_num);
    fmpz_clear(s_den);
}

// Canonical equivalence: convert both Rats to fmpz_mpoly_q, canonicalise,
// compare via fmpz_mpoly_q_equal. Returns true if equal.
bool canonical_equivalent(const Rat& legacy, const Rat& new_path) {
    const PolyCtx& ctx = legacy.ctx();
    const fmpz_mpoly_ctx_struct* ctx_z = ctx.raw()->zctx;
    fmpz_mpoly_q_t qL, qN;
    fmpz_mpoly_q_init(qL, ctx_z);
    fmpz_mpoly_q_init(qN, ctx_z);
    fmpz_mpoly_q_set_from_rat_polys(qL, legacy.num(), legacy.den(), ctx_z);
    fmpz_mpoly_q_set_from_rat_polys(qN, new_path.num(), new_path.den(),
                                    ctx_z);
    fmpz_mpoly_q_canonicalise(qL, ctx_z);
    fmpz_mpoly_q_canonicalise(qN, ctx_z);
    const bool ok = fmpz_mpoly_q_equal(qL, qN, ctx_z);
    fmpz_mpoly_q_clear(qL, ctx_z);
    fmpz_mpoly_q_clear(qN, ctx_z);
    return ok;
}

// Diagnostic: emit a (truncated) form for both Rats on failure. With
// 718-var ctx the Rat strings can be ~10 MB; truncate at 4 KB so the
// stderr log stays readable.
void print_truncated(const char* tag, const std::string& s) {
    constexpr size_t kMax = 4096;
    if (s.size() <= kMax) {
        std::fprintf(stderr, "  %s: %s\n", tag, s.c_str());
    } else {
        std::fprintf(stderr, "  %s [truncated %zu→%zu]: %.*s ... %s\n",
            tag, s.size(), kMax,
            static_cast<int>(kMax / 2), s.c_str(),
            s.c_str() + (s.size() - kMax / 2));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    long max_quads = -1;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--max-quads=", 0) == 0) {
            max_quads = std::atol(a.c_str() + 12);
        } else if (a == "--verbose" || a == "-v") {
            verbose = true;
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: hyperflint-test-rat-add-equivalence "
                "<rat_quads.txt> [--max-quads=N] [--verbose]\n");
            return 0;
        } else if (path.empty()) {
            path = a;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr,
            "missing rat_quads.txt path; run with --help for usage\n");
        return 1;
    }

    auto quads = read_quads(path, max_quads);
    std::fprintf(stderr,
        "[test_rat_add_equivalence] loaded %zu quads from %s "
        "(max=%ld)\n", quads.size(), path.c_str(), max_quads);

    int n_done = 0;
    int n_pass = 0;
    int n_fail = 0;
    int n_skip = 0;

    for (size_t qi = 0; qi < quads.size(); ++qi) {
        const Quad& q = quads[qi];
        // Build a PolyCtx that owns the variable list.  Each quad gets
        // a fresh ctx — same pattern as the bench, since 200 quads is
        // small and the ctx setup is ms-scale.
        PolyCtx ctx(std::vector<std::string>(q.vars));

        // Parse the four polys.  Use try/catch so a parser error on
        // one quad doesn't kill the whole run.
        Poly a_num(ctx), a_den(ctx), b_num(ctx), b_den(ctx);
        try {
            a_num = Poly(ctx, q.a_num_str);
            a_den = Poly(ctx, q.a_den_str);
            b_num = Poly(ctx, q.b_num_str);
            b_den = Poly(ctx, q.b_den_str);
        } catch (const std::exception& ex) {
            std::fprintf(stderr,
                "[skip] quad %d: parse error: %s\n", q.idx, ex.what());
            ++n_skip;
            continue;
        }

        // Build the two Rat operands. Rat ctor runs reduce_inplace
        // → operands enter the add as canonical, matching production.
        Rat a(std::move(a_num), std::move(a_den));
        Rat b(std::move(b_num), std::move(b_den));

        // Legacy path (cross-mult + gcd_cofactors).
        Rat r_legacy = a.add_legacy(b);
        // New path (env-default; routes through add_via_q_underscore).
        Rat r_new    = a.add(b);

        const bool ok = canonical_equivalent(r_legacy, r_new);
        if (ok) {
            ++n_pass;
            if (verbose) {
                std::printf("[PASS] quad %d\n", q.idx);
            }
        } else {
            ++n_fail;
            std::fprintf(stderr,
                "[FAIL] quad %d: legacy and new paths disagree\n",
                q.idx);
            print_truncated("legacy.num", r_legacy.num().to_string());
            print_truncated("legacy.den", r_legacy.den().to_string());
            print_truncated("new.num   ", r_new.num().to_string());
            print_truncated("new.den   ", r_new.den().to_string());
            // Per spec: STOP on first failure.  Don't continue with
            // the rest of the quads (the bug shape is what matters,
            // not how many trip it).
            std::fprintf(stderr,
                "[test_rat_add_equivalence] STOPPING on first "
                "mismatch (per Layer 1 spec).\n");
            break;
        }
        ++n_done;
    }

    std::fprintf(stderr,
        "[test_rat_add_equivalence] summary: done=%d pass=%d "
        "fail=%d skip=%d total=%zu\n",
        n_done, n_pass, n_fail, n_skip, quads.size());
    return n_fail == 0 ? 0 : 1;
}
