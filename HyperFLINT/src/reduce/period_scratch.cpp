#include "hyperflint/reduce/period_scratch.hpp"

#include "hyperflint/core/period_table.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/reduce/periods.hpp"

#include <flint/fmpq.h>
#include <flint/fmpq_mpoly.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace hyperflint {

bool period_tuples_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("HF_PERIOD_TUPLES");
        const bool v = e && e[0] == '1';
        if (v) {
            const char* sr = std::getenv("HF_USE_SCALAR_REP");
            const char* bc = std::getenv("HF_USE_BASIS_CTX");
            if ((sr && sr[0] == '1') || (bc && bc[0] == '1')) {
                std::fprintf(stderr,
                    "HF_PERIOD_TUPLES is incompatible with "
                    "HF_USE_SCALAR_REP / HF_USE_BASIS_CTX (three "
                    "representations of the mzv atoms cannot coexist); "
                    "aborting.\n");
                std::abort();
            }
        }
        return v;
    }();
    return on;
}

namespace {

// Immortal atoms-only scratch ring. Built once from the same atom list
// the wide-ctx handler would append (build_mzv_var_list with no user
// vars), so atom NAMES and reduction behavior match production exactly.
const PolyCtx& scratch_ctx(const MzvReductionTable& table) {
    static std::mutex mu;
    static std::unique_ptr<PolyCtx> ctx;
    static std::size_t atoms_n = 0;
    std::lock_guard<std::mutex> lk(mu);
    std::vector<std::string> atoms =
        build_mzv_var_list(table, /*user_vars=*/{});
    if (!ctx) {
        atoms_n = atoms.size();
        ctx = std::make_unique<PolyCtx>(atoms);
    } else if (atoms.size() != atoms_n) {
        // Review fold A3: the singleton is keyed to the first table
        // seen; a different table would mint in a stale ring. Fail
        // loud rather than silently mis-mint.
        throw std::runtime_error(
            "period_scratch: MzvReductionTable changed after scratch "
            "ctx construction (multi-table processes unsupported)");
    }
    // Thread-safety note (review fold A5): concurrent mints are safe --
    // PolyCtx is immutable post-construction and concurrent-read-safe;
    // the Words reaching mint sites are per-thread deep copies, so the
    // Rat::to_string cached_str_ mutation in word_to_scratch touches
    // thread-local Letters only.
    return *ctx;
}

// Re-express a numeric-letter Word in the scratch ctx. Site gating
// guarantees letters are constants (-2/-1/0 or numeric rationals).
Word word_to_scratch(const PolyCtx& sc, const Word& w) {
    Word out;
    out.letters.reserve(w.letters.size());
    for (const auto& l : w.letters) {
        out.letters.push_back(Rat::parse(sc, l.to_string()));
    }
    return out;
}

}  // namespace

SymCoef mint_period_sym(const PolyCtx& slim, const Word& w,
                        const MzvReductionTable& table, bool zero_one) {
    const PolyCtx& sc = scratch_ctx(table);
    const Word sw = word_to_scratch(sc, w);
    const Rat period = zero_one ? zero_one_period(sc, sw, table)
                                : zero_inf_period(sc, sw, table);
    if (period.is_zero()) return SymCoef(slim);

    // Physics M4: period values have numeric denominators only.
    const Poly& den = period.den();
    if (!den.used_var_indices().empty()) {
        throw std::runtime_error(
            "mint_period_sym: non-constant denominator in period value "
            "(violates the M4 numeric-denominator invariant)");
    }

    // Decompose num = sum_t c_t * prod_j atom_j^{e_tj} into SymCoef
    // monomials: prefactor = (c_t / den) lifted to the slim ctx,
    // period_powers = the atom monomial via PeriodTable ids.
    auto& pt = PeriodTable::instance();
    const fmpq_mpoly_struct* np = period.num().raw();
    fmpq_mpoly_ctx_struct* nctx = sc.raw();
    const long nterms = fmpq_mpoly_length(np, nctx);
    const long nvars = static_cast<long>(sc.vars().size());

    std::vector<SymMonomial> ms;
    ms.reserve(static_cast<size_t>(nterms));
    fmpq_t cf, dencf, q;
    fmpq_init(cf); fmpq_init(dencf); fmpq_init(q);
    // den is a constant poly: its content as fmpq.
    {
        // den = dencf (constant term coefficient).
        fmpq_mpoly_get_term_coeff_fmpq(dencf, den.raw(), 0, nctx);
    }
    std::vector<ulong> exps(static_cast<size_t>(nvars));
    for (long t = 0; t < nterms; ++t) {
        fmpq_mpoly_get_term_coeff_fmpq(cf, np, t, nctx);
        fmpq_div(q, cf, dencf);
        fmpq_mpoly_get_term_exp_ui(exps.data(), np, t, nctx);
        // prefactor = q over the SLIM ctx (numeric).
        char* qs = fmpq_get_str(nullptr, 10, q);
        SymMonomial m{Rat::parse(slim, qs)};
        flint_free(qs);
        for (long v = 0; v < nvars; ++v) {
            if (exps[static_cast<size_t>(v)] != 0) {
                const std::uint32_t id =
                    pt.id_for(sc.vars()[static_cast<size_t>(v)]);
                m.period_powers[id] =
                    static_cast<int>(exps[static_cast<size_t>(v)]);
            }
        }
        ms.push_back(std::move(m));
    }
    fmpq_clear(cf); fmpq_clear(dencf); fmpq_clear(q);
    return SymCoef::from_monomials(slim, std::move(ms));
}

}  // namespace hyperflint
