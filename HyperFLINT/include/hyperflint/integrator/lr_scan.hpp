// Doppio-port phase 3: projective Cheng-Wu GAUGE SCAN + the Doppio
// keep-rules (Euler-conic strict tier, FindRoots carried-sqrt tier) as a
// VERDICT ENGINE on top of the lr_search table machinery.
//
// This is deliberately a SEPARATE entry point from find_lr_orders: the
// classic scorer stays byte-identical (its quadratic acceptance mirrors
// what HF's integrator can actually execute), while the scan implements
// the Mathematica DoppioFubini semantics (doppio_lib.wl) for
// triage/certification:
//
//   * PROJECTIVE input detection: every factor homogeneous in the
//     integration variables and  sum_i e_i deg P_i == -n  identically in
//     eps, with exponents e_i = a_i + b_i*eps supplied as integer pairs
//     (so the check is  sum a_i d_i == -n  AND  sum b_i d_i == 0).
//   * GAUGE SCAN ("ChengWu" -> "Scan"): crawl the UNGAUGED n-variable
//     system to depth n-1; the un-integrated variable IS the Cheng-Wu
//     gauge.  One subset table serves all n gauges; per gauge the
//     letters are DEHOMOGENIZED at x_gauge -> 1 before judgment
//     (dehomogenization commutes with the disc/resultant letter
//     generation and preserves irreducibility for gauge-coprime
//     letters, so this equals the explicit gauged runs -- validated
//     against the Mathematica t24 battery).
//   * KeepRule::Strict  == Doppio's strict rule: a letter may have
//     degree <= 1 in the pivot, or be a quadratic whose Euler conic
//     rationalizes (leading coefficient A and constant term C both
//     perfect squares as polynomials; FLINT fmpq_mpoly_sqrt ==
//     dpSquareQ semantics, rejecting 2x^2 and -x^2).
//   * KeepRule::FindRoots adds the relaxed tiers: TERMINAL quadratics
//     accepted (their roots are kinematic letters of the answer);
//     otherwise the ODD-MULTIPLICITY part of the pivot-discriminant is
//     examined -- empty => rational roots (zero-disc double lines
//     accepted too); pure-kinematic => kinematic sqrt letter; pending-
//     variable-dependent => the letter is accepted and the obligation
//     CARRIED: it joins the letter set of every later step of the same
//     path and is re-judged by these same tiers recursively, discharged
//     once free of pending variables.  Orders report
//     {carried_sqrts, kin_sqrts, terminal_quads} profiles.  This tier
//     is NECESSARY-only (same status as production FindRoots->True);
//     prefer the score-minimal order, treat deep-carry orders as
//     speculative.
//
// The optional chi filter (euler_filter.hpp, HF_EULER_FILTER semantics)
// can be applied to the table letters before judgment, exactly like the
// Mathematica variant C.
#pragma once
#include <cstdint>
#include <vector>

#include "hyperflint/core/poly.hpp"

namespace hyperflint {
namespace lr_scan {

enum class KeepRule { Strict, FindRoots };

// twist exponent  a + b*eps  (integer pairs cover the Symanzik cases:
// U^(omega - D/2) F^(-omega) at D = 4 - 2 eps has integer a, b)
struct ScanExponent {
    long a = 0;
    long b = 0;
};

struct ScanOrder {
    std::vector<size_t> order;   // the n-1 integrated vars (ctx indices)
    size_t gauge = 0;            // the un-integrated variable (ctx index)
    double score = 0.0;
    unsigned long carried_sqrts = 0;
    unsigned long kin_sqrts = 0;
    unsigned long terminal_quads = 0;
};

struct ScanResult {
    bool projective = false;     // detection verdict (scan ran iff true)
    bool truncated = false;      // order enumeration hit max_orders
    std::vector<ScanOrder> orders;   // score-ascending
};

// ---------------------------------------------------------------------
// projectivity detection (dpProjectiveQ): every factor of every group
// homogeneous in the xvar set, and sum e_i deg P_i == -n identically.
// exps shape matches group_polys (RAW groups, no boundary monomials).
// ---------------------------------------------------------------------
bool projective_input(
    const std::vector<std::vector<Poly>>& group_polys,
    const std::vector<size_t>& xvar_indices,
    const std::vector<std::vector<ScanExponent>>& exps);

// ---------------------------------------------------------------------
// the per-letter FindRoots judgment (exposed for unit tests; the port
// of dpFRJudge).  `pivot` and `pending` are ctx var indices; `pending`
// excludes the pivot and the gauge.  Returns ok + the new obligations
// (canonicalized) + counter increments.
// ---------------------------------------------------------------------
struct FrJudgment {
    bool ok = false;
    std::vector<Poly> carry;
    unsigned long kin = 0;
    unsigned long term = 0;
};
FrJudgment fr_judge(const Poly& letter, size_t pivot,
                    const std::vector<size_t>& pending,
                    const std::vector<size_t>& all_xvars);

// Euler-conic rationalizability of a deg-2-in-pivot letter (exposed for
// unit tests): leading and constant coefficients both perfect squares.
bool conic_rationalizable(const Poly& letter, size_t pivot);

// ---------------------------------------------------------------------
// the gauge scan.  group_polys are the RAW per-face groups (boundary
// monomials are added internally, mirroring dpLungoCore's seed).
// max_orders caps the enumeration (truncated flag set on hit).
// ---------------------------------------------------------------------
ScanResult find_lr_orders_scan(
    const std::vector<std::vector<Poly>>& group_polys,
    const std::vector<size_t>& xvar_indices,
    const std::vector<std::vector<ScanExponent>>& exps,
    KeepRule keep_rule = KeepRule::Strict,
    bool euler_filter = false,
    size_t max_orders = 8192);

}  // namespace lr_scan
}  // namespace hyperflint
