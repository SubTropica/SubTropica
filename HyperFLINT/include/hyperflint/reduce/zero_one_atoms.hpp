#pragma once
// Reserved opaque atoms for UNSUPPORTED 0->1 boundary periods.
//
// Issue #52 round 5.  zero_one_period (reduce/periods.cpp) evaluates the
// final 0->1 periods of a run in the MZV/alternating basis, whose
// alphabet is {-1, 0, 1}.  A (regularized) word with any other integer
// letter used to throw std::runtime_error from inside an OpenMP region,
// i.e. std::terminate (CLI exit 134; a dead Wolfram kernel on the
// LibraryLink transport).  Such a period is a perfectly good constant --
// the Goncharov polylogarithm G(w; 1) -- so instead the engine now mints
// one of kZeroOneAtomPoolSize reserved ring variables "zop_<k>" for the
// word, carries it through the remaining SymCoef arithmetic like any
// other period generator, and reports the (atom -> word) table in the
// response under "zero_one_periods".  The Mathematica side decodes each
// atom directly into HyperIntica`Hlog[1, word] (the Goncharov
// polylogarithm G(word; 1), numerically evaluable via STToGinsh).
//
// The atoms are appended to the variable lists that can reach the final
// period evaluation: the atoms-only scratch ring (build_mzv_var_list,
// the period-tuples default) and the wide / narrow contexts.  The basis
// context (build_basis_var_list, HF_USE_BASIS_CTX, default-off) is NOT
// extended: a mint there fails closed (unknown variable), never silently.
// The registry is process-global and session-cumulative: an atom keeps
// its meaning across the requests of one LibraryLink kernel session,
// because engine caches (e.g. the MZV rhs cache) may hold expressions
// that mention an atom by name, so the names must never be reassigned
// while the process lives.  Every response carries the full table, so a
// consumer never needs state from an earlier response.  Exhausting the
// pool raises ZeroOneAtomsExhausted, which the OMP exception transport
// (integrator/integration_step.cpp) turns into a structured
// {"failed":true} response; on the in-process transport the cure is a
// fresh kernel (the CLI transport is one process per request).
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hyperflint {

constexpr int kZeroOneAtomPoolSize = 64;

struct ZeroOneAtomsExhausted : std::runtime_error {
    ZeroOneAtomsExhausted(const std::string& word_json, int pool)
        : std::runtime_error(
              "zero_one_period: more than " + std::to_string(pool) +
              " distinct unsupported 0->1 boundary periods in one process "
              "(last word " + word_json + "); the pool is cumulative over "
              "the requests of an in-process LibraryLink session: restart "
              "the kernel, or use the CLI transport "
              "($STHyperFlintUseLibraryLink = False)") {}
};

std::string zero_one_atom_name(int k);                       // "zop_<k>", 1-based
void append_zero_one_atoms(std::vector<std::string>& vars);  // idempotent append
std::string zero_one_atom_for(const std::string& word_json); // register-or-lookup
bool zero_one_atoms_used();
std::string zero_one_atoms_json();                           // {"zop_1":[..],...}
std::vector<std::pair<std::string, std::string>> zero_one_atom_table();
void zero_one_atoms_reset_for_tests();

}  // namespace hyperflint
