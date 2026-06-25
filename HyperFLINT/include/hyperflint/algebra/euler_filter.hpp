// Euler chi-drop genuineness filter for the LR-order search (Doppio C
// port, phase 2 of the Doppio->HF port).
//
// Mirrors dpGenuineDKQ / the DoppioFubini variant-C filter
// (scripts/doppiofubini/doppio/doppio_lib.wl): a candidate letter L at
// subset S of the integration variables is GENUINE iff the on-locus
// Euler count of the S-marginal cleared-dlog twist drops below the
// generic count:
//
//   chi_gen  = max of two seeded generic chi_count_sectors draws
//              (an unlucky prime/point can only undercount),
//   genuine <=> chi(Constraint -> L) < chi_gen,
//
// with the reference's conservative rules: a PositiveDim (Indeterminate)
// constrained count KEEPS the letter (the singularity is conservatively
// considered included); any failure KEEPS the letter; the DESTRUCTIVE
// verdict (drop) requires TWO independent no-drop draws.  Twist
// exponents are letter-independent seeded primes (values free in the
// cleared-dlog representation).  The factor list is the caller's
// ALREADY-AUGMENTED per-face group (group polys + boundary monomials),
// which reproduces the CII hyperplane semantics: the x_{v in S} factors
// fold the sub-sectors, and the x_{v not in S} factors numericize to
// harmless constants.  When every factor is homogeneous in the S
// variables the Euler relation kills every count silently; the filter
// then applies the C*-quotient chart (last S variable -> 1), exactly
// the reference's auto-chart.
//
// Gating: consumers (lr_search.cpp) call this only under
// HF_EULER_FILTER=1 (default OFF; registry row in docs/env_flags.md).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "hyperflint/core/poly.hpp"

namespace hyperflint {

struct ChiFilterCache {
    // (face-hash, S-mask) -> generic chi (max-of-two-draws); ~0UL marks
    // an unusable generic count (keep every letter at that marginal)
    std::unordered_map<uint64_t, unsigned long> chi_gen;
    bool warned_failure = false;
};

// Is letter L genuine at subset S (indices into the Poly ctx vars) of
// the augmented group?  Conservative: keeps on Indeterminate/failure.
bool chi_letter_genuine(const std::vector<Poly>& augmented_group,
                        const std::vector<size_t>& subset_var_indices,
                        const Poly& letter,
                        ChiFilterCache& cache,
                        unsigned long base_seed = 87178);

// The per-subset letter filter for find_lr_orders: returns the letters
// of `letters` that survive (boundary letters — single integration-
// variable monomials — are exempt, the trailing-coefficient channel).
std::vector<Poly> chi_filter_letters(
    const std::vector<Poly>& augmented_group,
    const std::vector<size_t>& subset_var_indices,
    const std::vector<Poly>& letters,
    ChiFilterCache& cache,
    unsigned long base_seed = 87178);

// Utility-study accounting (2026-06-06): judged / dropped /
// boundary-exempt letter counts and chi_count_sectors invocations
// (the msolve cost driver) since the last reset.  Reset per request
// by the consumer (lr_search / lr_scan); read for HF_LR_TRACE
// summaries and the chi-utility A/B study.
struct ChiFilterStats {
    unsigned long judged = 0;
    unsigned long dropped = 0;
    unsigned long boundary_exempt = 0;
    unsigned long msolve_calls = 0;
};
ChiFilterStats chi_filter_stats();
void reset_chi_filter_stats();
void chi_stats_count_sectors_call();  // incremented inside euler_chi

// Explicitly re-arm the msolve spawn circuit breaker (defined in euler_chi.cpp):
// when msolve spawns keep failing (rc=-1; under contention this is spawn-level
// EAGAIN, not an input crash) the filter abstains, but the thousands of failing
// spawns per request can starve concurrent subprocess launches (the
// EulerFilter->True parallel-integration break).  After N
// consecutive msolve failures the spawn is skipped for the rest of the process
// (filter still abstains => identical result, storm capped).  This is
// DELIBERATELY NOT called per find_lr_orders / find_lr_orders_scan request: the
// breaker must persist across requests so a permanently-broken msolve is capped
// once, while a working msolve never trips (each success resets the streak
// before the threshold).  Call this only to force a fresh attempt mid-process
// (e.g. after fixing HF_MSOLVE_PATH).
void reset_chi_spawn_breaker();

}  // namespace hyperflint
