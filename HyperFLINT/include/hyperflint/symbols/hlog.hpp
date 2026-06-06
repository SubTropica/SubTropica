// Hlog(z, word) -- the iterated-integral hyperlogarithm.
//
//   Hlog[z, {w1, ..., wn}] = int_{0<tn<...<t1<z} dt1/(t1-w1) & ... & dtn/(tn-wn)
//
// Special values:
//   Hlog[z, {}]    = 1
//   Hlog[z, {0}]   = log z
//   Hlog[z, {1}]   = -log(1 - z)
//   Hlog[z, {0,1}] = -Li_2(z)
//
// Phase 3 treats Hlog as a pure symbolic form. Differentiation,
// series expansion, and Mpl conversion arrive in 3c/3d.

#pragma once

#include "hyperflint/core/rat.hpp"
#include "hyperflint/symbols/word.hpp"

#include <sstream>
#include <string>

namespace hyperflint {

struct Hlog {
    Rat  z;      // the upper limit (typically a Rat in the integration vars)
    Word word;   // singularity list

    std::string to_string() const {
        std::ostringstream o;
        o << "Hlog[" << z.to_string() << ", " << word.to_string() << "]";
        return o.str();
    }

    bool equal(const Hlog& other) const {
        return z.equal(other.z) && word.equal(other.word);
    }
};

}  // namespace hyperflint
