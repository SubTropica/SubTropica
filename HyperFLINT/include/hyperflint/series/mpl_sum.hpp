// MplSum: truncated defining partial sum of a multiple polylog.
//
//   MplSum(ns, zs, maxN) = sum_{N >= m_1 > m_2 > ... > m_k >= 1}
//                              z_1^{m_1} * ... * z_k^{m_k}
//                              / (m_1^{n_1} * ... * m_k^{n_k})
//
// Empty ns returns 1. maxN < 1 returns 0. All arithmetic is in the
// Rat ring of the zs' PolyCtx.
//
// Mirrors HyperIntica.wl:4158-4167.
//
// Used by MplSeries when the first or last argument of an Mpl
// approaches 0 as var -> 0 (then the defining sum truncates at
// O(var^{maxN+1})).

#pragma once

#include "hyperflint/core/rat.hpp"
#include <vector>

namespace hyperflint {

Rat mpl_sum(const std::vector<long>& ns,
            const std::vector<Rat>& zs,
            long max_n);

}  // namespace hyperflint
