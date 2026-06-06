// Euler-characteristic counter (DK port) — implementation.
//
// See include/hyperflint/algebra/euler_chi.hpp for the contract and
// scripts/doppiofubini/doppio/doppio_lib.wl (dpCountInSectorF /
// dpCountSectorsF / dpFindIrredMonos) for the validated Mathematica
// reference this mirrors.  Oracle fixtures: t20_cleared_dlog.wl, ported
// into test/test_euler_chi.cpp.
//
// Deliberate deviations from the reference (documented, semantics-equal):
//   * parameter residues are drawn once per chi_count_sectors call from a
//     seeded mt19937_64 (the reference redraws inside every sector call;
//     both are generic points, ours is more deterministic);
//   * the linear-constraint branch is folded into the Diophantine one:
//     all params but the constraint variable are numericized FIRST and
//     the resulting univariate constraint is solved mod the matched prime
//     (for a linear constraint this is the unique residue root — the
//     same generic point the reference's symbolic Solve produces);
//   * variables are not GB-cost re-sorted (the reference's "Sort"->True):
//     the standard-monomial COUNT is a quotient dimension, invariant
//     under variable order; sorting only tunes msolve's runtime.

#include "hyperflint/algebra/euler_chi.hpp"

#include <flint/flint.h>
#include <flint/fmpz.h>
#include <flint/fmpz_mpoly.h>
#include <flint/nmod_poly.h>
#include <flint/nmod_poly_factor.h>
#include <flint/ulong_extras.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <queue>

namespace hyperflint {

// =====================================================================
// staircase count (port of dpFindIrredMonos' walk)
// =====================================================================
ChiCount chi_staircase_count(
    const std::vector<std::vector<unsigned long>>& lead_exps,
    unsigned long nvars)
{
    if (nvars == 0) return {ChiStatus::Finite, 1};
    if (lead_exps.empty()) return {ChiStatus::PositiveDim, 0};

    const unsigned long NOPURE = ~0UL;
    bool has_origin = false;
    // pure power bound per variable: min exponent over entries supported
    // on that single variable (the reference's pure/bounds), NOPURE if none
    std::vector<unsigned long> pure_min(nvars, NOPURE);
    bool any_pure = false;
    for (const auto& v : lead_exps) {
        unsigned long nnz = 0, idx = 0;
        for (unsigned long i = 0; i < nvars; ++i)
            if (v[i] > 0) { ++nnz; idx = i; }
        if (nnz == 0) has_origin = true;
        if (nnz == 1) {
            any_pure = true;
            pure_min[idx] = std::min(pure_min[idx], v[idx]);
        }
    }
    // positive-dimension detection, exactly the reference's two clauses:
    //   pureSums == {}  and  origin not in lt        -> infinite staircase
    //   pureSums != {}  and  some var lacks a pure power -> infinite
    if (!any_pure && !has_origin) return {ChiStatus::PositiveDim, 0};
    if (any_pure) {
        for (unsigned long i = 0; i < nvars; ++i)
            if (pure_min[i] == NOPURE && !has_origin)
                return {ChiStatus::PositiveDim, 0};
    }

    auto divides = [&](const std::vector<unsigned long>& e,
                       const std::vector<unsigned long>& m) {
        for (unsigned long i = 0; i < nvars; ++i)
            if (m[i] < e[i]) return false;
        return true;
    };

    // BFS over candidate standard monomials, bounded per variable by
    // (pure power - 1); expansion only happens from COUNTED nodes, so a
    // missing bound (unit-ideal corner) is never dereferenced.
    std::set<std::vector<unsigned long>> seen;
    std::queue<std::vector<unsigned long>> todo;
    todo.push(std::vector<unsigned long>(nvars, 0));
    unsigned long count = 0;
    while (!todo.empty()) {
        auto v = todo.front();
        todo.pop();
        if (!seen.insert(v).second) continue;
        bool standard = true;
        for (const auto& e : lead_exps)
            if (divides(e, v)) { standard = false; break; }
        if (!standard) continue;
        ++count;
        for (unsigned long i = 0; i < nvars; ++i) {
            if (pure_min[i] == NOPURE) continue;   // cannot grow there
            if (v[i] + 1 <= pure_min[i] - 1) {
                auto w = v;
                ++w[i];
                todo.push(w);
            }
        }
    }
    return {ChiStatus::Finite, count};
}

// =====================================================================
// msolve subprocess + GB-output parsing
// =====================================================================
namespace {

// grevlex comparison: a > b iff deg(a) > deg(b), or equal degrees and the
// LAST nonzero entry of a-b is negative.
bool grevlex_greater(const std::vector<unsigned long>& a,
                     const std::vector<unsigned long>& b)
{
    long da = 0, db = 0;
    for (auto x : a) da += (long) x;
    for (auto x : b) db += (long) x;
    if (da != db) return da > db;
    for (long i = (long) a.size() - 1; i >= 0; --i) {
        if (a[i] != b[i]) return a[i] < b[i];
    }
    return false;
}

// parse one msolve monomial term "c", "c*x^a*y", "x^2", ... into an
// exponent vector over var_names; returns false on parse failure.
bool parse_term_exps(const std::string& term,
                     const std::vector<std::string>& var_names,
                     std::vector<unsigned long>& out)
{
    out.assign(var_names.size(), 0);
    std::stringstream ss(term);
    std::string piece;
    while (std::getline(ss, piece, '*')) {
        if (piece.empty()) return false;
        // pure number (the coefficient) — contributes no exponents
        if (piece.find_first_not_of("0123456789") == std::string::npos)
            continue;
        std::string name = piece;
        unsigned long exp = 1;
        auto caret = piece.find('^');
        if (caret != std::string::npos) {
            name = piece.substr(0, caret);
            exp = std::stoul(piece.substr(caret + 1));
        }
        auto it = std::find(var_names.begin(), var_names.end(), name);
        if (it == var_names.end()) return false;
        out[(unsigned long) (it - var_names.begin())] += exp;
    }
    return true;
}

// serialize an fmpz_mpoly to msolve input syntax with every coefficient
// REDUCED into [0, prime).  msolve 0.9.4 must never see coefficients
// >= 2^32: on multi-term systems it then either computes a silently WRONG
// basis or crashes (SIGBUS) -- found 2026-06-05 via an on-locus box
// system whose msolve "solution" failed to satisfy its own input; the
// same system with pre-reduced coefficients yields the correct unit
// ideal.  Returns "" when the polynomial vanishes identically mod prime
// (the caller drops such generators: they are zero in F_p[x]).
std::string msolve_poly_string(const fmpz_mpoly_t f,
                               const std::vector<std::string>& names,
                               unsigned long prime,
                               const fmpz_mpoly_ctx_t ctx)
{
    const slong n = fmpz_mpoly_ctx_nvars(ctx);
    const slong len = fmpz_mpoly_length(f, ctx);
    fmpz_t c;
    fmpz_init(c);
    std::vector<ulong> exps((unsigned long) n, 0);
    std::ostringstream out;
    bool any = false;
    for (slong t = 0; t < len; ++t) {
        fmpz_mpoly_get_term_coeff_fmpz(c, f, t, ctx);
        unsigned long cr = fmpz_fdiv_ui(c, prime);
        if (cr == 0) continue;
        fmpz_mpoly_get_term_exp_ui(exps.data(), f, t, ctx);
        if (any) out << "+";
        out << cr;
        for (slong v = 0; v < n; ++v)
            if (exps[(unsigned long) v] > 0) {
                out << "*" << names[(unsigned long) v];
                if (exps[(unsigned long) v] > 1)
                    out << "^" << exps[(unsigned long) v];
            }
        any = true;
    }
    fmpz_clear(c);
    return any ? out.str() : std::string();
}

}  // namespace

std::optional<std::vector<std::vector<unsigned long>>> msolve_leading_exps(
    const std::vector<std::string>& polys,
    const std::vector<std::string>& var_names,
    unsigned long prime,
    const std::string& msolve_path)
{
    if (polys.empty() || var_names.empty()) return std::nullopt;

    char in_tmpl[] = "/tmp/hf_chi_in_XXXXXX";
    char out_tmpl[] = "/tmp/hf_chi_out_XXXXXX";
    int in_fd = mkstemp(in_tmpl);
    int out_fd = mkstemp(out_tmpl);
    if (in_fd < 0 || out_fd < 0) return std::nullopt;
    close(out_fd);
    {
        std::ostringstream body;
        for (size_t i = 0; i < var_names.size(); ++i)
            body << (i ? "," : "") << var_names[i];
        body << "\n" << prime << "\n";
        for (size_t i = 0; i < polys.size(); ++i)
            body << polys[i] << (i + 1 < polys.size() ? ",\n" : "\n");
        const std::string s = body.str();
        if (write(in_fd, s.data(), s.size()) != (ssize_t) s.size()) {
            close(in_fd);
            return std::nullopt;
        }
        close(in_fd);
    }

    // posix_spawn, not std::system: no shell parses msolve_path (review
    // finding [2]: a crafted HF_MSOLVE_PATH could inject commands and a
    // spaced path failed silently), stdout/stderr to /dev/null.
    int rc = -1;
    {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        const char* argv[] = {msolve_path.c_str(), "-g", "2",
            "-f", in_tmpl, "-o", out_tmpl, nullptr};
        pid_t pid = 0;
        int sp = posix_spawnp(&pid, msolve_path.c_str(), &fa, nullptr,
            const_cast<char* const*>(argv), environ);
        posix_spawn_file_actions_destroy(&fa);
        if (sp == 0) {
            int st = 0;
            if (waitpid(pid, &st, 0) == pid && WIFEXITED(st))
                rc = WEXITSTATUS(st);
        }
    }
    const bool dbg = std::getenv("HF_CHI_DEBUG") != nullptr;
    if (dbg)
        std::fprintf(stderr, "[chi] msolve rc=%d in=%s out=%s (kept)\n",
            rc, in_tmpl, out_tmpl);
    std::optional<std::vector<std::vector<unsigned long>>> result;
    if (rc == 0) {
        std::ifstream out(out_tmpl);
        std::string line, payload;
        while (std::getline(out, line)) {
            if (!line.empty() && line[0] == '#') continue;
            payload += line;
        }
        // strip whitespace and the [ ... ]: wrapper
        payload.erase(std::remove_if(payload.begin(), payload.end(),
                          [](unsigned char c) { return std::isspace(c); }),
            payload.end());
        auto lb = payload.find('[');
        auto rb = payload.rfind(']');
        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
            payload = payload.substr(lb + 1, rb - lb - 1);
            std::vector<std::vector<unsigned long>> leads;
            std::stringstream ss(payload);
            std::string poly;
            bool ok = true;
            while (std::getline(ss, poly, ',')) {
                if (poly.empty()) continue;
                // terms of a mod-p polynomial are '+'-separated
                std::stringstream ts(poly);
                std::string term;
                std::vector<unsigned long> best;
                bool first = true;
                while (std::getline(ts, term, '+')) {
                    std::vector<unsigned long> e;
                    if (!parse_term_exps(term, var_names, e)) { ok = false; break; }
                    if (first || grevlex_greater(e, best)) { best = e; first = false; }
                }
                if (!ok || first) { ok = false; break; }
                leads.push_back(best);
            }
            if (ok && !leads.empty()) result = leads;
        }
    }
    if (!dbg) {
        std::remove(in_tmpl);
        std::remove(out_tmpl);
    }
    return result;
}

// =====================================================================
// the DK sector-summed count
// =====================================================================
namespace {

struct MpolyCtx {
    fmpz_mpoly_ctx_t ctx;
    std::vector<std::string> names;       // z, props..., params...
    std::vector<const char*> names_c;
    MpolyCtx(const std::vector<std::string>& props,
             const std::vector<std::string>& params)
    {
        names.push_back("zzRab");
        names.insert(names.end(), props.begin(), props.end());
        names.insert(names.end(), params.begin(), params.end());
        for (auto& n : names) names_c.push_back(n.c_str());
        fmpz_mpoly_ctx_init(ctx, (slong) names.size(), ORD_DEGREVLEX);
    }
    ~MpolyCtx() { fmpz_mpoly_ctx_clear(ctx); }
};

// first 32 primes below 2^31 (msolve's characteristic cap), generated at
// first use so no hand-typed primality mistakes are possible (32, not the
// reference's 201, but residues are redrawn per prime so the whole-call
// honest-Failed probability is negligible; review finding [4])
const std::vector<unsigned long>& chi_primes()
{
    static std::vector<unsigned long> ps = [] {
        std::vector<unsigned long> v;
        unsigned long p = (1UL << 31) - 1;
        while (v.size() < 32) {
            while (!n_is_prime(p)) --p;
            v.push_back(p);
            --p;
        }
        return v;
    }();
    return ps;
}

}  // namespace

ChiCount chi_count_sectors(
    const std::vector<std::string>& factor_strs,
    const std::vector<long>& exponents,
    const std::vector<std::string>& prop_vars,
    const std::vector<std::string>& param_vars,
    const std::string& constraint,
    unsigned long seed,
    const std::string& msolve_path)
{
    if (factor_strs.empty() || factor_strs.size() != exponents.size())
        return {ChiStatus::Failed, 0};
    const unsigned long np = prop_vars.size();
    if (np > 20) return {ChiStatus::Failed, 0};   // 2^np sector sanity cap
    // review finding [3]: the Rabinowitsch slot is named zzRab; a caller
    // symbol of that name would silently alias it
    for (const auto& nm : prop_vars)
        if (nm == "zzRab") return {ChiStatus::Failed, 0};
    for (const auto& nm : param_vars)
        if (nm == "zzRab") return {ChiStatus::Failed, 0};

    MpolyCtx C(prop_vars, param_vars);
    const unsigned long nfac = factor_strs.size();

    std::vector<fmpz_mpoly_struct> facs(nfac);
    for (unsigned long i = 0; i < nfac; ++i) {
        fmpz_mpoly_init(&facs[i], C.ctx);
        if (fmpz_mpoly_set_str_pretty(&facs[i], factor_strs[i].c_str(),
                C.names_c.data(), C.ctx) != 0) {
            for (unsigned long j = 0; j <= i; ++j)
                fmpz_mpoly_clear(&facs[j], C.ctx);
            return {ChiStatus::Failed, 0};
        }
    }
    const bool constrained =
        !(constraint.empty() || constraint == "0");
    fmpz_mpoly_t cpoly;
    fmpz_mpoly_init(cpoly, C.ctx);
    if (constrained &&
        fmpz_mpoly_set_str_pretty(cpoly, constraint.c_str(),
            C.names_c.data(), C.ctx) != 0) {
        fmpz_mpoly_clear(cpoly, C.ctx);
        for (auto& f : facs) fmpz_mpoly_clear(&f, C.ctx);
        return {ChiStatus::Failed, 0};
    }

    std::mt19937_64 rng(seed);
    ChiCount total{ChiStatus::Failed, 0};

    // try primes until the parameter point is usable (constraint root
    // exists; matched-prime discipline of the reference)
    for (unsigned long prime : chi_primes()) {
        std::uniform_int_distribution<unsigned long> draw(2, prime - 2);

        // constraint variable choice: lowest max-degree among its vars.
        // PARAM-ONLY (review finding [1]): the reference's contract is a
        // kinematic constraint; a prop-containing constraint would pick a
        // prop as cvar and then silently overwrite its residue in the
        // sector loop, ignoring the constraint.  Reject loudly instead.
        long cvar = -1;
        if (constrained) {
            for (unsigned long v = 1; v < 1 + np; ++v)
                if (fmpz_mpoly_degree_si(cpoly, (slong) v, C.ctx) > 0) {
                    fmpz_mpoly_clear(cpoly, C.ctx);
                    for (auto& f : facs) fmpz_mpoly_clear(&f, C.ctx);
                    return {ChiStatus::Failed, 0};
                }
            unsigned long best_deg = ~0UL;
            for (unsigned long v = 1 + np; v < C.names.size(); ++v) {
                slong d = fmpz_mpoly_degree_si(cpoly, (slong) v, C.ctx);
                if (d > 0 && (unsigned long) d < best_deg) {
                    best_deg = (unsigned long) d;
                    cvar = (long) v;
                }
            }
            if (cvar < 0) continue;   // constraint param-free: degenerate
        }

        // draw residues for every param (params live after 1+np)
        std::vector<unsigned long> resid(C.names.size(), 0);
        for (unsigned long v = 1 + np; v < C.names.size(); ++v)
            resid[v] = draw(rng);

        // solve the (now univariate) constraint mod prime
        if (constrained) {
            fmpz_mpoly_t cu;
            fmpz_mpoly_init(cu, C.ctx);
            fmpz_mpoly_set(cu, cpoly, C.ctx);
            fmpz_t val;
            fmpz_init(val);
            for (unsigned long v = 1 + np; v < C.names.size(); ++v) {
                if ((long) v == cvar) continue;
                fmpz_set_ui(val, resid[v]);
                fmpz_mpoly_evaluate_one_fmpz(cu, cu, (slong) v, val, C.ctx);
            }
            // cu is univariate in cvar: lift to nmod_poly
            nmod_poly_t up;
            nmod_poly_init(up, prime);
            slong d = fmpz_mpoly_degree_si(cu, cvar, C.ctx);
            fmpz_t coef;
            fmpz_init(coef);
            fmpz_mpoly_t pow, tmp;
            fmpz_mpoly_init(pow, C.ctx);
            fmpz_mpoly_init(tmp, C.ctx);
            for (slong k = 0; k <= d; ++k) {
                fmpz_mpoly_gen(pow, cvar, C.ctx);
                fmpz_mpoly_pow_ui(pow, pow, (ulong) k, C.ctx);
                // coefficient extraction: evaluate d/dcvar trick is messy;
                // use get_coeff via monomial: build monomial cvar^k
                fmpz_mpoly_get_coeff_fmpz_monomial(coef, cu, pow, C.ctx);
                nmod_poly_set_coeff_ui(up, k,
                    fmpz_fdiv_ui(coef, prime));
            }
            fmpz_mpoly_clear(pow, C.ctx);
            fmpz_mpoly_clear(tmp, C.ctx);
            fmpz_clear(coef);
            fmpz_mpoly_clear(cu, C.ctx);
            fmpz_clear(val);
            if (nmod_poly_degree(up) < 1) {
                nmod_poly_clear(up);
                continue;   // degenerate at this draw: next prime
            }
            nmod_poly_factor_t rt;
            nmod_poly_factor_init(rt);
            nmod_poly_roots(rt, up, 0);
            unsigned long root = 0;
            bool found = false;
            if (rt->num > 0) {
                // root of the linear factor a*x + b: x = -b/a
                const nmod_poly_struct* lin = rt->p + 0;
                unsigned long b = nmod_poly_get_coeff_ui(lin, 0);
                root = n_negmod(b, prime);
                found = true;
            }
            nmod_poly_factor_clear(rt);
            nmod_poly_clear(up);
            if (!found) continue;     // no root at this prime: try next
            resid[(unsigned long) cvar] = root;
        }

        // ---- sector sum at this (prime, residues) point ----
        bool positive_dim = false, failed = false;
        unsigned long sum = 0;
        fmpz_t val;
        fmpz_init(val);
        for (unsigned long mask = 0; mask < (1UL << np) && !failed; ++mask) {
            // restrict factors: complement props -> 0, params -> residues
            std::vector<fmpz_mpoly_struct> fs(nfac);
            bool zero_factor = false;
            for (unsigned long i = 0; i < nfac; ++i) {
                fmpz_mpoly_init(&fs[i], C.ctx);
                fmpz_mpoly_set(&fs[i], &facs[i], C.ctx);
                for (unsigned long v = 0; v < np; ++v)
                    if (!(mask & (1UL << v))) {
                        fmpz_set_ui(val, 0);
                        fmpz_mpoly_evaluate_one_fmpz(&fs[i], &fs[i],
                            (slong) (1 + v), val, C.ctx);
                    }
                for (unsigned long v = 1 + np; v < C.names.size(); ++v) {
                    fmpz_set_ui(val, resid[v]);
                    fmpz_mpoly_evaluate_one_fmpz(&fs[i], &fs[i],
                        (slong) v, val, C.ctx);
                }
                if (fmpz_mpoly_is_zero(&fs[i], C.ctx)) zero_factor = true;
            }
            if (zero_factor) {
                for (auto& f : fs) fmpz_mpoly_clear(&f, C.ctx);
                continue;   // vanishing factor: sector contributes 0
            }

            // cleared dlog numerators over the sector variables
            std::vector<std::string> sys;
            std::vector<std::string> sysvars{C.names[0]};
            for (unsigned long v = 0; v < np; ++v)
                if (mask & (1UL << v)) sysvars.push_back(prop_vars[v]);

            fmpz_mpoly_t prod, term, deriv, num;
            fmpz_mpoly_init(prod, C.ctx);
            fmpz_mpoly_init(term, C.ctx);
            fmpz_mpoly_init(deriv, C.ctx);
            fmpz_mpoly_init(num, C.ctx);
            fmpz_mpoly_one(prod, C.ctx);
            for (auto& f : fs) fmpz_mpoly_mul(prod, prod, &f, C.ctx);

            for (unsigned long v = 0; v < np; ++v) {
                if (!(mask & (1UL << v))) continue;
                fmpz_mpoly_zero(num, C.ctx);
                for (unsigned long i = 0; i < nfac; ++i) {
                    fmpz_mpoly_derivative(deriv, &fs[i], (slong) (1 + v), C.ctx);
                    if (fmpz_mpoly_is_zero(deriv, C.ctx)) continue;
                    fmpz_mpoly_one(term, C.ctx);
                    for (unsigned long j = 0; j < nfac; ++j)
                        if (j != i) fmpz_mpoly_mul(term, term, &fs[j], C.ctx);
                    fmpz_mpoly_mul(term, term, deriv, C.ctx);
                    fmpz_mpoly_scalar_mul_si(term, term, exponents[i], C.ctx);
                    fmpz_mpoly_add(num, num, term, C.ctx);
                }
                if (fmpz_mpoly_is_zero(num, C.ctx)) continue;
                {
                    std::string ps = msolve_poly_string(num, C.names, prime, C.ctx);
                    if (!ps.empty()) sys.push_back(ps);
                }
            }
            // Rabinowitsch 1 - z * prod
            fmpz_mpoly_gen(term, 0, C.ctx);
            fmpz_mpoly_mul(term, term, prod, C.ctx);
            fmpz_mpoly_neg(term, term, C.ctx);
            fmpz_mpoly_add_si(term, term, 1, C.ctx);
            sys.push_back(msolve_poly_string(term, C.names, prime, C.ctx));

            fmpz_mpoly_clear(prod, C.ctx);
            fmpz_mpoly_clear(term, C.ctx);
            fmpz_mpoly_clear(deriv, C.ctx);
            fmpz_mpoly_clear(num, C.ctx);
            for (auto& f : fs) fmpz_mpoly_clear(&f, C.ctx);

            auto leads = msolve_leading_exps(sys, sysvars, prime, msolve_path);
            if (!leads) { failed = true; break; }
            ChiCount c = chi_staircase_count(*leads, sysvars.size());
            if (std::getenv("HF_CHI_DEBUG"))
                std::fprintf(stderr,
                    "[chi] prime=%lu mask=%lu nsys=%zu -> %s %lu\n",
                    prime, mask, sys.size(),
                    c.status == ChiStatus::Finite ? "fin"
                    : c.status == ChiStatus::PositiveDim ? "POSDIM" : "FAIL",
                    c.count);
            if (c.status == ChiStatus::PositiveDim) { positive_dim = true; continue; }
            if (c.status != ChiStatus::Finite) { failed = true; break; }
            sum += c.count;
        }
        fmpz_clear(val);
        if (failed) continue;   // msolve hiccup: try the next prime
        total = positive_dim ? ChiCount{ChiStatus::PositiveDim, 0}
                             : ChiCount{ChiStatus::Finite, sum};
        break;
    }

    fmpz_mpoly_clear(cpoly, C.ctx);
    for (auto& f : facs) fmpz_mpoly_clear(&f, C.ctx);
    return total;
}

}  // namespace hyperflint
