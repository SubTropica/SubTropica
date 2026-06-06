// Euler-characteristic counter for the Doppio chi-drop filter (DK port).
//
// C++ port of the cleared-dlog DiscKosky counting core that lives in
// scripts/doppiofubini/doppio/doppio_lib.wl (dpCountInSectorF /
// dpCountSectorsF / dpFindIrredMonos; adapted from DiscKosky 0.0.1,
// Crisanti / Lippstreu / McLeod / Polackova, with the FACTORED interface
// of 2026-06-03).  Reference oracle: t20_cleared_dlog.wl.
//
// The counted object: for a twist  Psi = prod_i P_i^{e_i}  in the sector
// variables x_v (every other symbol numericized to a random residue), the
// number of solutions of the CLEARED dlog critical system
//
//     N_v = sum_i e_i (prod_{j != i} P_j) d P_i / d x_v   (for each v),
//     1 - z * prod_i P_i                                   (Rabinowitsch)
//
// counted as the number of standard monomials of the ideal's grevlex
// Groebner basis modulo a prime ("staircase walk").  Exponents e_i enter
// as COEFFICIENTS only (the dlog observation: their values are free).
// The full chi is the sum over sectors (subsets of the propagator
// variables, complement set to 0; a sector on which any factor vanishes
// identically contributes 0).  A positive-dimensional sector yields
// Indeterminate (the caller's Euler-drop rule treats it as "drop").
//
// Groebner bases are computed by the external `msolve` binary (grevlex,
// characteristic < 2^31), spoken to over temp files:
//     input :  "x1,x2,...\n<char>\np1,\np2,..."
//     output:  '#'-comment header, then "[poly1, poly2, ...]:" with each
//              polynomial's terms in decreasing grevlex order (we
//              recompute the leading term defensively).
//
// Determinism: all random draws (parameter residues, prime choice) come
// from a caller-seeded generator, mirroring the BlockRandom discipline of
// the Mathematica reference.
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace hyperflint {

enum class ChiStatus {
    Finite,        // zero-dimensional count
    PositiveDim,   // Indeterminate in the reference (Euler-drop rule)
    Failed         // msolve/parse/root-finding failure
};

struct ChiCount {
    ChiStatus status = ChiStatus::Failed;
    unsigned long count = 0;   // valid iff status == Finite
};

// ---------------------------------------------------------------------
// staircase count (pure combinatorics; port of dpFindIrredMonos).
// lead_exps = the exponent vectors of the Groebner basis' leading
// monomials over nvars variables.  Returns PositiveDim when the
// leading-term ideal has an infinite staircase (a variable with no pure
// power; including the no-pure-power-at-all corner), Finite otherwise
// with the number of standard monomials (0 for the unit ideal).
// ---------------------------------------------------------------------
ChiCount chi_staircase_count(
    const std::vector<std::vector<unsigned long>>& lead_exps,
    unsigned long nvars);

// ---------------------------------------------------------------------
// grevlex leading-exponent vectors of the reduced GB of `polys` mod
// `prime`, via the msolve subprocess.  polys are strings in msolve
// syntax over `var_names` (in that order).  std::nullopt on failure.
//
// CALLER CONTRACT (review finding [5]): coefficients in `polys` MUST
// already be reduced into [0, prime) (or be small negatives msolve
// reduces safely).  msolve 0.9.4 fed coefficients >= 2^32 in multi-term
// systems computes silently WRONG bases or crashes; the internal
// counting path enforces the reduction via msolve_poly_string -- direct
// callers of this function must do the same.
// ---------------------------------------------------------------------
std::optional<std::vector<std::vector<unsigned long>>> msolve_leading_exps(
    const std::vector<std::string>& polys,
    const std::vector<std::string>& var_names,
    unsigned long prime,
    const std::string& msolve_path = "msolve");

// ---------------------------------------------------------------------
// The full DK count.
//
//   factor_strs : the factors P_i as FLINT pretty strings over
//                 prop_vars ++ param_vars (e.g. "x1+x2+s*x1*x3")
//   exponents   : the integer twist exponents e_i (values free; only
//                 relative genericity matters)
//   prop_vars   : sector/propagator variable names
//   param_vars  : every other symbol (numericized internally)
//   constraint  : "" or "0" for the generic count; otherwise a
//                 polynomial in the param_vars whose vanishing locus the
//                 count is restricted to (the on-locus count): all
//                 params but the lowest-degree constraint variable are
//                 numericized, the resulting univariate constraint is
//                 solved mod the MATCHED prime (nmod_poly roots; next
//                 prime tried when rootless), and the root substituted
//                 -- the Diophantine mechanism of the reference.
//   seed        : RNG seed (reproducibility)
//
// Returns the sector-summed count; PositiveDim if any nonzero sector is
// positive-dimensional (the reference's Indeterminate total).
// ---------------------------------------------------------------------
ChiCount chi_count_sectors(
    const std::vector<std::string>& factor_strs,
    const std::vector<long>& exponents,
    const std::vector<std::string>& prop_vars,
    const std::vector<std::string>& param_vars,
    const std::string& constraint,
    unsigned long seed,
    const std::string& msolve_path = "msolve");

}  // namespace hyperflint
