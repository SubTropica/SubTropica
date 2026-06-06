// Euler chi-drop genuineness filter — implementation.
// See include/hyperflint/algebra/euler_filter.hpp for the contract and
// scripts/doppiofubini/doppio/doppio_lib.wl (dpGenuineDKQ + the
// DoppioFubini variant-C filter) for the validated reference.

#include "hyperflint/algebra/euler_filter.hpp"

#include <flint/fmpq_mpoly.h>
#include <flint/ulong_extras.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "hyperflint/algebra/euler_chi.hpp"

namespace hyperflint {

namespace {

// FNV-1a over a string (stable across runs: determinism contract)
uint64_t fnv1a(const std::string& s, uint64_t h = 1469598103934665603ULL)
{
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

// is `p` homogeneous as a polynomial in the variables `idx` (coefficients
// in everything else)?  Port of dpHomogeneousQ: the per-term total degree
// over `idx` is constant across terms.
bool homogeneous_in(const Poly& p, const std::vector<size_t>& idx)
{
    const auto* ctx = p.ctx().raw();
    const auto* raw = p.raw();
    const slong nt = fmpq_mpoly_length(raw, ctx);
    if (nt <= 1) return true;
    const size_t nv = p.ctx().vars().size();
    std::vector<slong> exps(nv);
    slong deg0 = -1;
    for (slong t = 0; t < nt; ++t) {
        fmpq_mpoly_get_term_exp_si(exps.data(),
            const_cast<fmpq_mpoly_struct*>(raw), t, ctx);
        slong d = 0;
        for (size_t v : idx) d += exps[v];
        if (t == 0) deg0 = d;
        else if (d != deg0) return false;
    }
    return true;
}

// is the letter a boundary monomial c * x_v (single term, total degree 1)?
// Those are integration-domain boundaries / the trailing-coefficient
// channel and are EXEMPT from the filter (the reference's
// MemberQ[allVars, letter] exemption; filtering them reproduces the
// boundary-poly draining bug).
bool is_boundary_monomial(const Poly& p)
{
    const auto* ctx = p.ctx().raw();
    const auto* raw = p.raw();
    if (fmpq_mpoly_length(raw, ctx) != 1) return false;
    const size_t nv = p.ctx().vars().size();
    std::vector<slong> exps(nv);
    fmpq_mpoly_get_term_exp_si(exps.data(),
        const_cast<fmpq_mpoly_struct*>(raw), 0, ctx);
    slong total = 0;
    for (size_t v = 0; v < nv; ++v) total += exps[v];
    return total == 1;
}

// letter-independent twist exponents: distinct primes drawn by a seeded
// shuffle (values free in the cleared-dlog representation; only relative
// genericity matters — the reference draws prime ranks 30..230)
std::vector<long> twist_exponents(size_t n, uint64_t seed)
{
    static const std::vector<long> pool = [] {
        std::vector<long> v;
        unsigned long p = 127;
        while (v.size() < 64) {
            v.push_back((long) p);
            p = n_nextprime(p, 0);
        }
        return v;
    }();
    std::vector<long> shuffled = pool;
    std::mt19937_64 rng(seed);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    shuffled.resize(n);
    return shuffled;
}

}  // namespace

bool chi_letter_genuine(const std::vector<Poly>& augmented_group,
                        const std::vector<size_t>& subset_var_indices,
                        const Poly& letter,
                        ChiFilterCache& cache,
                        unsigned long base_seed)
{
    if (augmented_group.empty() || subset_var_indices.empty()) return true;
    const auto& all_names = letter.ctx().vars();

    // ---- C*-quotient auto-chart (Euler-relation guard) ----
    std::vector<size_t> pv = subset_var_indices;
    std::vector<Poly> facs = augmented_group;
    bool all_homog = true;
    for (const auto& f : facs)
        if (!homogeneous_in(f, pv)) { all_homog = false; break; }
    if (all_homog && pv.size() >= 2) {
        const size_t chart = pv.back();
        pv.pop_back();
        for (auto& f : facs) f = f.substitute_one_rat(chart, "1");
    }

    // ---- string forms (canonical primitive: variety-equal, integer) ----
    std::vector<std::string> fac_strs;
    fac_strs.reserve(facs.size());
    uint64_t face_hash = 1469598103934665603ULL;
    for (const auto& f : facs) {
        std::string s = f.canonical_prop_form().to_string();
        face_hash = fnv1a(s, face_hash);
        fac_strs.push_back(std::move(s));
    }
    uint64_t mask_hash = 0;
    for (size_t v : pv) mask_hash |= (1ULL << (v % 63));
    const uint64_t key = face_hash ^ (mask_hash * 0x9E3779B97F4A7C15ULL);
    const uint64_t seed_tw = base_seed ^ key;

    std::vector<std::string> prop_names, param_names;
    for (size_t v = 0; v < all_names.size(); ++v) {
        if (std::find(pv.begin(), pv.end(), v) != pv.end())
            prop_names.push_back(all_names[v]);
        else
            param_names.push_back(all_names[v]);
    }
    const std::vector<long> exps = twist_exponents(fac_strs.size(), seed_tw);

    // ---- generic chi (memoized; max of two draws) ----
    const unsigned long UNUSABLE = ~0UL;
    unsigned long chi_gen;
    auto it = cache.chi_gen.find(key);
    if (it != cache.chi_gen.end()) {
        chi_gen = it->second;
    } else {
        ChiCount a = chi_count_sectors(fac_strs, exps, prop_names,
            param_names, "0", (unsigned long) (seed_tw + 1));
        ChiCount b = chi_count_sectors(fac_strs, exps, prop_names,
            param_names, "0", (unsigned long) (seed_tw + 2));
        if (a.status == ChiStatus::Finite && b.status == ChiStatus::Finite)
            chi_gen = std::max(a.count, b.count);
        else if (a.status == ChiStatus::Finite)
            chi_gen = a.count;
        else if (b.status == ChiStatus::Finite)
            chi_gen = b.count;
        else
            chi_gen = UNUSABLE;
        cache.chi_gen.emplace(key, chi_gen);
    }
    if (chi_gen == UNUSABLE) {
        if (!cache.warned_failure) {
            cache.warned_failure = true;
            std::fprintf(stderr,
                "[hf-euler-filter] WARNING: generic chi unusable at a "
                "marginal; keeping letters (conservative).\n");
        }
        return true;
    }

    // ---- constrained chi: drop needs TWO independent no-drop draws ----
    const std::string lstr = letter.canonical_prop_form().to_string();
    const uint64_t seed_draw = seed_tw ^ fnv1a(lstr);
    ChiCount on1 = chi_count_sectors(fac_strs, exps, prop_names,
        param_names, lstr, (unsigned long) (seed_draw + 3));
    if (on1.status == ChiStatus::PositiveDim) return true;  // Indet rule
    if (on1.status != ChiStatus::Finite) {
        if (!cache.warned_failure) {
            cache.warned_failure = true;
            std::fprintf(stderr,
                "[hf-euler-filter] WARNING: constrained chi failed; "
                "keeping letter (conservative).\n");
        }
        return true;
    }
    if (on1.count < chi_gen) return true;   // drop seen: genuine
    ChiCount on2 = chi_count_sectors(fac_strs, exps, prop_names,
        param_names, lstr, (unsigned long) (seed_draw + 4));
    if (on2.status == ChiStatus::PositiveDim) return true;
    if (on2.status != ChiStatus::Finite) return true;
    return on2.count < chi_gen;
}

std::vector<Poly> chi_filter_letters(
    const std::vector<Poly>& augmented_group,
    const std::vector<size_t>& subset_var_indices,
    const std::vector<Poly>& letters,
    ChiFilterCache& cache,
    unsigned long base_seed)
{
    std::vector<Poly> kept;
    kept.reserve(letters.size());
    for (const auto& L : letters) {
        if (is_boundary_monomial(L) ||
            chi_letter_genuine(augmented_group, subset_var_indices, L,
                cache, base_seed)) {
            kept.push_back(L);
        }
    }
    return kept;
}

}  // namespace hyperflint
