// Differentiation of Hlog and Mpl symbols.
//
//   diff_hlog(z, word, var_idx)
//     returns sum_i c_i * Hlog[z, word_i]     (as HlogList)
//
//   diff_mpl(ns, zs, var_idx)
//     returns sum_i c_i * Mpl[ns_i, zs_i]     (as MplList)
//
// Mirrors HyperIntica.wl:3949 (DiffHlog) and 3985 (DiffMpl).
// We assume arg is finite (the Infinity case is handled as part of
// ZeroInfPeriod in Phase 6).

#pragma once

#include "hyperflint/core/rat.hpp"
#include "hyperflint/symbols/hlog.hpp"
#include "hyperflint/symbols/mpl.hpp"

#include <vector>

namespace hyperflint {

struct HlogTerm { Rat coef; Hlog hlog; };
struct MplTerm  { Rat coef; Mpl  mpl;  };
using HlogList = std::vector<HlogTerm>;
using MplList  = std::vector<MplTerm>;

HlogList diff_hlog(const Rat& z, const Word& word, size_t var_idx);
MplList  diff_mpl(const std::vector<long>& ns,
                  const std::vector<Rat>& zs,
                  size_t var_idx);

}  // namespace hyperflint
