// SymCoefSplit: implementation of basic ops + adapter.
//
// HF MZV-rewrite Phase A commit (5). The class is exercised by the
// round-trip test test/unit/test_sym_coef_split_roundtrip.cpp; it is
// not yet wired into any production hot path. Phase B switches the
// integrator's call sites file by file.

#include "hyperflint/core/sym_coef_split.hpp"

#include "hyperflint/core/rat_split.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace hyperflint {

namespace {

// Canonicalization-key tuple: we collapse like-monomial-splits by
// the canonical fingerprint
//   (pi_power, i_power, log_powers, delta_powers, num_zw, den_zw)
// — exactly the design v2 §3.4 like-monomial-collapse rule.
struct SplitKey {
    int                          pi_power;
    int                          i_power;
    std::map<long, int>          log_powers;
    std::map<std::string, int>   delta_powers;
    ZWHandle                     num_zw;
    ZWHandle                     den_zw;
};

bool operator<(const SplitKey& a, const SplitKey& b) {
    if (a.pi_power     != b.pi_power)     return a.pi_power     < b.pi_power;
    if (a.i_power      != b.i_power)      return a.i_power      < b.i_power;
    if (a.log_powers   != b.log_powers)   return a.log_powers   < b.log_powers;
    if (a.delta_powers != b.delta_powers) return a.delta_powers < b.delta_powers;
    if (a.num_zw       != b.num_zw)       return a.num_zw       < b.num_zw;
    return a.den_zw < b.den_zw;
}

bool operator==(const SplitKey& a, const SplitKey& b) {
    return a.pi_power     == b.pi_power
        && a.i_power      == b.i_power
        && a.log_powers   == b.log_powers
        && a.delta_powers == b.delta_powers
        && a.num_zw       == b.num_zw
        && a.den_zw       == b.den_zw;
}

SplitKey key_of(const SymMonomialSplit& s) {
    return SplitKey{s.pi_power, s.i_power, s.log_powers, s.delta_powers,
                    s.num_zw, s.den_zw};
}

}  // namespace

SymCoefSplit SymCoefSplit::from_rat(const SymCoef& src,
                                    const PolyCtx& N,
                                    std::shared_ptr<ZWTable> zw_table) {
    SymCoefSplit out(src.ctx(), N, std::move(zw_table));
    FNIndexMaps maps = build_fn_index_maps(src.ctx(), N);

    for (const auto& m : src.terms()) {
        // Split the prefactor Rat.
        std::vector<SymMonomialSplit> leaves =
            split_rat_by_w_monomial(m.prefactor, N, *out.zw_table_,
                                    maps.F_to_N_idx);
        // Copy the transcendentals onto each leaf and append.
        for (auto& leaf : leaves) {
            leaf.pi_power     = m.pi_power;
            leaf.i_power      = m.i_power;
            leaf.log_powers   = m.log_powers;
            leaf.delta_powers = m.delta_powers;
            out.push_term(std::move(leaf));
        }
    }
    return out.canonicalize();
}

SymCoef SymCoefSplit::as_rat() const {
    // Group splits by transcendental key + den_zw — each group's
    // accumulated numerator is one SymMonomial in the resulting
    // SymCoef. Different den_zw groups become different SymMonomials
    // (the wide-ctx Rat is constructed per-group).
    //
    // For Phase A, we walk the (already canonical) terms_ once and
    // build a fresh SymCoef. Each SymMonomialSplit becomes one
    // SymMonomial. Like-monomial collapse downstream is handled by
    // SymCoef::canonicalize().

    SymCoef out(*wide_ctx_);
    if (terms_.empty()) {
        return out;
    }

    FNIndexMaps maps = build_fn_index_maps(*wide_ctx_, *narrow_ctx_);

    std::vector<SymMonomial> mons;
    mons.reserve(terms_.size());

    for (const auto& s : terms_) {
        // Recombine just this leaf's contribution into a wide-ctx Rat:
        //   numerator = transplant(num_N -> F) * tab.get(num_zw)
        //   denom     = tab.get(den_zw)
        std::vector<SymMonomialSplit> single;
        single.push_back(SymMonomialSplit{
            s.num_N, s.num_zw, s.den_zw,
            /*pi*/0, /*i*/0, {}, {}});
        Rat r = recombine_rat_split(single, *wide_ctx_, *zw_table_,
                                    maps.N_to_F_idx);

        SymMonomial m(std::move(r));
        m.pi_power     = s.pi_power;
        m.i_power      = s.i_power;
        m.log_powers   = s.log_powers;
        m.delta_powers = s.delta_powers;
        mons.push_back(std::move(m));
    }

    return SymCoef::from_monomials(*wide_ctx_, std::move(mons));
}

SymCoefSplit SymCoefSplit::add(const SymCoefSplit& o) const {
    if (wide_ctx_   != o.wide_ctx_   ||
        narrow_ctx_ != o.narrow_ctx_ ||
        zw_table_   != o.zw_table_) {
        throw std::runtime_error(
            "SymCoefSplit::add: ctx / ZWTable mismatch");
    }
    SymCoefSplit out(*wide_ctx_, *narrow_ctx_, zw_table_);
    out.terms_.reserve(terms_.size() + o.terms_.size());
    for (const auto& s : terms_)   out.terms_.push_back(s);
    for (const auto& s : o.terms_) out.terms_.push_back(s);
    return out.canonicalize();
}

SymCoefSplit SymCoefSplit::neg() const {
    SymCoefSplit out(*wide_ctx_, *narrow_ctx_, zw_table_);
    out.terms_.reserve(terms_.size());
    for (const auto& s : terms_) {
        SymMonomialSplit ns = s;
        ns.num_N = ns.num_N.neg();
        out.terms_.push_back(std::move(ns));
    }
    // No canonicalize here — negating preserves the canonical
    // ordering (the keys don't change, no like-monomials become
    // equal that weren't already, no zero coefs introduced unless
    // input had zero num_N which canonicalize already filters).
    return out;
}

SymCoefSplit SymCoefSplit::mul_rat(const Rat& r) const {
    // Multiplication by a wide-ctx Rat: split r into its own
    // (num_N, num_zw, den_zw) form first, then distribute over
    // *this's terms. Each (s, r_split) pair produces one new leaf:
    //   num_N'  = s.num_N * r.num_N    (narrow-ctx mul)
    //   num_zw' = tab.multiply(s.num_zw, r.num_zw)
    //   den_zw' = tab.multiply(s.den_zw, r.den_zw)
    // and the transcendentals carry through unchanged from `s`
    // (Rat carries no transcendental factor).
    FNIndexMaps maps = build_fn_index_maps(*wide_ctx_, *narrow_ctx_);
    std::vector<SymMonomialSplit> r_leaves =
        split_rat_by_w_monomial(r, *narrow_ctx_, *zw_table_,
                                maps.F_to_N_idx);

    SymCoefSplit out(*wide_ctx_, *narrow_ctx_, zw_table_);
    out.terms_.reserve(terms_.size() * r_leaves.size());
    for (const auto& s : terms_) {
        for (const auto& rl : r_leaves) {
            SymMonomialSplit prod{
                s.num_N.mul(rl.num_N),
                zw_table_->multiply(s.num_zw, rl.num_zw),
                zw_table_->multiply(s.den_zw, rl.den_zw),
                s.pi_power, s.i_power,
                s.log_powers, s.delta_powers};
            out.terms_.push_back(std::move(prod));
        }
    }
    return out.canonicalize();
}

SymCoefSplit SymCoefSplit::mul(const SymCoefSplit& o) const {
    if (wide_ctx_   != o.wide_ctx_   ||
        narrow_ctx_ != o.narrow_ctx_ ||
        zw_table_   != o.zw_table_) {
        throw std::runtime_error(
            "SymCoefSplit::mul: ctx / ZWTable mismatch");
    }
    SymCoefSplit out(*wide_ctx_, *narrow_ctx_, zw_table_);
    out.terms_.reserve(terms_.size() * o.terms_.size());
    for (const auto& a : terms_) {
        for (const auto& b : o.terms_) {
            // Build the per-pair fields first; SymMonomialSplit's Poly
            // member has no default ctor, so we aggregate-init at end.
            Poly num_N = a.num_N.mul(b.num_N);

            // I^2 = -1 reduction. Under the canonical-input invariant
            // (i_power ∈ {0,1}) the sum lives in {0,1,2}. A '2' folds
            // into a sign on num_N, leaving i_power = sum mod 2.
            const int isum = a.i_power + b.i_power;
            if ((isum & 2) != 0) {
                num_N = num_N.neg();
            }
            const int i_power = isum & 1;

            // log_powers: entrywise sum, drop zero entries.
            std::map<long, int> log_powers = a.log_powers;
            for (const auto& kv : b.log_powers) {
                log_powers[kv.first] += kv.second;
            }
            for (auto it = log_powers.begin(); it != log_powers.end(); ) {
                if (it->second == 0) it = log_powers.erase(it);
                else                 ++it;
            }

            // delta^2 = +1 reduction: per-key sum mod 2; drop zeros.
            std::map<std::string, int> delta_powers = a.delta_powers;
            for (const auto& kv : b.delta_powers) {
                const int sum_dp = delta_powers[kv.first] + kv.second;
                const int reduced = ((sum_dp % 2) + 2) % 2;
                if (reduced == 0) delta_powers.erase(kv.first);
                else              delta_powers[kv.first] = reduced;
            }

            out.terms_.push_back(SymMonomialSplit{
                std::move(num_N),
                zw_table_->multiply(a.num_zw, b.num_zw),
                zw_table_->multiply(a.den_zw, b.den_zw),
                a.pi_power + b.pi_power,
                i_power,
                std::move(log_powers),
                std::move(delta_powers)});
        }
    }
    return out.canonicalize();
}

bool SymCoefSplit::equals_canonical(const SymCoefSplit& o) const {
    if (wide_ctx_   != o.wide_ctx_   ||
        narrow_ctx_ != o.narrow_ctx_ ||
        zw_table_   != o.zw_table_) {
        return false;
    }
    SymCoefSplit ca = canonicalize();
    SymCoefSplit cb = o.canonicalize();
    if (ca.terms_.size() != cb.terms_.size()) return false;
    for (size_t i = 0; i < ca.terms_.size(); ++i) {
        const SymMonomialSplit& sa = ca.terms_[i];
        const SymMonomialSplit& sb = cb.terms_[i];
        // SplitKey equality (canonicalize sorts by SplitKey, so
        // matched-by-position implies matched-by-key under the
        // canonical-output invariant).
        if (sa.pi_power     != sb.pi_power)     return false;
        if (sa.i_power      != sb.i_power)      return false;
        if (sa.log_powers   != sb.log_powers)   return false;
        if (sa.delta_powers != sb.delta_powers) return false;
        if (sa.num_zw       != sb.num_zw)       return false;
        if (sa.den_zw       != sb.den_zw)       return false;
        if (!sa.num_N.equal(sb.num_N))          return false;
    }
    return true;
}

SymCoefSplit SymCoefSplit::canonicalize() const {
    // Bin terms by SplitKey, summing num_N within each bin. Then
    // emit in sorted-by-key order.
    std::map<SplitKey, Poly> bins;

    for (const auto& s : terms_) {
        SplitKey k = key_of(s);
        auto it = bins.find(k);
        if (it == bins.end()) {
            bins.emplace(std::move(k), s.num_N);
        } else {
            it->second = it->second.add(s.num_N);
        }
    }

    SymCoefSplit out(*wide_ctx_, *narrow_ctx_, zw_table_);
    out.terms_.reserve(bins.size());
    for (auto& kv : bins) {
        if (kv.second.is_zero()) continue;
        out.terms_.push_back(SymMonomialSplit{
            std::move(kv.second),
            kv.first.num_zw, kv.first.den_zw,
            kv.first.pi_power, kv.first.i_power,
            kv.first.log_powers, kv.first.delta_powers});
    }
    return out;
}

}  // namespace hyperflint
