// Mpl -- multiple polylogarithm symbol.
//
//   Mpl[{n1, ..., nk}, {z1, ..., zk}]
//     = sum_{m1 > ... > mk >= 1}  prod_i sign(n_i)^{m_i} / (m_i^{|n_i|} * ...)
//
//   Mpl[{2}, {z}]         = Li_2(z)
//   Mpl[{n1,...,nk},{1,..,1}] = zeta(n1,...,nk)  (classical MZV at unit args)
//
// Phase 3 holds Mpl as a typed bag of (indices, args). Bidirectional
// conversion to/from Hlog comes in 3c.

#pragma once

#include "hyperflint/core/rat.hpp"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace hyperflint {

struct Mpl {
    std::vector<long> indices;   // n1, ..., nk (signed integers)
    std::vector<Rat>  args;      // z1, ..., zk

    std::string to_string() const {
        std::ostringstream o;
        o << "Mpl[{";
        for (size_t i = 0; i < indices.size(); ++i) {
            if (i) o << ",";
            o << indices[i];
        }
        o << "},{";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) o << ",";
            o << args[i].to_string();
        }
        o << "}]";
        return o.str();
    }

    bool equal(const Mpl& other) const {
        if (indices != other.indices) return false;
        if (args.size() != other.args.size()) return false;
        for (size_t i = 0; i < args.size(); ++i) {
            if (!args[i].equal(other.args[i])) return false;
        }
        return true;
    }
};

}  // namespace hyperflint
