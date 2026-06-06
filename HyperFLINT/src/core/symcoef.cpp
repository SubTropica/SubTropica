// Phase 6d-v-i: SymCoef implementation.

#include "hyperflint/core/symcoef.hpp"
#include "hyperflint/core/period_table.hpp"
#include "hyperflint/algebra/poly_struct_hash.hpp"
#include "hyperflint/core/addpf_probe.hpp"

#include <chrono>
#include "hyperflint/core/rat_split.hpp"
#include "hyperflint/core/zw_table.hpp"
#include "hyperflint/reduce/mzv_reduce.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace hyperflint {

// Declared in hyperflint/reduce/period_scratch.hpp; forward-declared here
// to avoid a core->reduce header dependency (review fold, 2026-06-04).
bool period_tuples_enabled();

// -------- SymMonomial --------

std::string SymMonomial::power_key() const {
    std::ostringstream o;
    o << 'P' << pi_power << '|';
    o << 'I' << i_power  << '|';
    o << 'L';
    for (const auto& kv : log_powers) {
        o << kv.first << ':' << kv.second << ',';
    }
    o << '|';
    o << 'D';
    for (const auto& kv : delta_powers) {
        o << kv.first << ':' << kv.second << ',';
    }
    o << '|';
    o << 'Q';  // period-tuples Phase 1: opaque period generators
    for (const auto& kv : period_powers) {
        o << kv.first << ':' << kv.second << ',';
    }
    return o.str();
}

bool SymMonomial::is_pure_rat() const {
    return pi_power == 0 && i_power == 0
        && log_powers.empty() && delta_powers.empty()
        && period_powers.empty();  // period-tuples: periods are NOT rats
}

std::pair<uint64_t, uint64_t> SymMonomial::power_hash() const {
    // 2026-04-30 (Tier 1.4b): 128-bit FNV-1a hash mirroring power_key()
    // but with no string allocation. Mixes the same content with
    // section-tag sentinels (P, I, L, D) so that, e.g., a delta power
    // can't alias a log power with the same numeric value. log_powers
    // and delta_powers are std::map (sorted by key), so the iteration
    // order is canonical — the hash is independent of insertion order.
    auto [h1, h2] = poly_struct_hash_seed();
    // Section: pi_power
    poly_struct_hash_mix(h1, h2, 0x5000000000000000ULL);  // 'P'
    poly_struct_hash_mix(h1, h2, static_cast<uint64_t>(pi_power));
    // Section: i_power
    poly_struct_hash_mix(h1, h2, 0x4900000000000000ULL);  // 'I'
    poly_struct_hash_mix(h1, h2, static_cast<uint64_t>(i_power));
    // Section: log_powers (sorted by long key by std::map invariant).
    poly_struct_hash_mix(h1, h2, 0x4c00000000000000ULL);  // 'L'
    for (const auto& kv : log_powers) {
        poly_struct_hash_mix(h1, h2, static_cast<uint64_t>(kv.first));
        poly_struct_hash_mix(h1, h2, static_cast<uint64_t>(kv.second));
    }
    // Section: delta_powers (sorted by string key by std::map invariant).
    poly_struct_hash_mix(h1, h2, 0x4400000000000000ULL);  // 'D'
    for (const auto& kv : delta_powers) {
        for (char c : kv.first) {
            poly_struct_hash_mix(h1, h2, static_cast<uint64_t>(static_cast<unsigned char>(c)));
        }
        // Length sentinel so {"x", "12"} can't alias {"x12", ""}.
        poly_struct_hash_mix(h1, h2, 0xfffffffffffffffeULL);
        poly_struct_hash_mix(h1, h2, static_cast<uint64_t>(kv.second));
    }
    // Section: period_powers (sorted by id by std::map invariant).
    poly_struct_hash_mix(h1, h2, 0x5100000000000000ULL);  // 'Q'
    for (const auto& kv : period_powers) {
        poly_struct_hash_mix(h1, h2, static_cast<uint64_t>(kv.first));
        poly_struct_hash_mix(h1, h2, static_cast<uint64_t>(kv.second));
    }
    return {h1, h2};
}

std::string SymMonomial::to_string() const {
    std::ostringstream o;
    o << '(' << prefactor.to_string() << ')';
    if (pi_power == 1)      o << "*Pi";
    else if (pi_power != 0) o << "*Pi^" << pi_power;
    if (i_power == 1)       o << "*I";
    else if (i_power != 0)  o << "*I^" << i_power;
    for (const auto& kv : log_powers) {
        if (kv.second == 1) o << "*Log[" << kv.first << "]";
        else                o << "*Log[" << kv.first << "]^" << kv.second;
    }
    for (const auto& kv : delta_powers) {
        if (kv.second == 1) o << "*delta[" << kv.first << "]";
        else                o << "*delta[" << kv.first << "]^" << kv.second;
    }
    for (const auto& kv : period_powers) {
        if (kv.second == 1) o << "*Period[" << kv.first << "]";
        else                o << "*Period[" << kv.first << "]^" << kv.second;
    }
    return o.str();
}

// -------- SymCoef constructors --------

SymCoef SymCoef::from_monomials(const PolyCtx& ctx,
                                 std::vector<SymMonomial> ms) {
    SymCoef out(ctx);
    out.terms_ = std::move(ms);
    // iter-24b Patch I: `out` is a local consumed by the return path;
    // route through the rvalue canonicalize overload to move terms_
    // through normalization without copying.
    return std::move(out).canonicalize();
}

SymCoef SymCoef::from_rat(const Rat& r) {
    SymCoef out(r.ctx());
    if (!r.is_zero()) out.terms_.push_back(SymMonomial{r});
    return out;
}

SymCoef SymCoef::pi_factor(const PolyCtx& ctx) {
    SymMonomial m{Rat::one_of(ctx)};
    m.pi_power = 1;
    return from_monomials(ctx, {std::move(m)});
}

SymCoef SymCoef::im_factor(const PolyCtx& ctx) {
    SymMonomial m{Rat::one_of(ctx)};
    m.i_power = 1;
    return from_monomials(ctx, {std::move(m)});
}

SymCoef SymCoef::log_factor(const PolyCtx& ctx, long n) {
    if (n <= 0) {
        throw std::runtime_error("SymCoef::log_factor: n must be > 0, got "
                                 + std::to_string(n));
    }
    SymMonomial m{Rat::one_of(ctx)};
    m.log_powers[n] = 1;
    return from_monomials(ctx, {std::move(m)});
}

SymCoef SymCoef::delta_factor(const PolyCtx& ctx,
                               const std::string& var_name) {
    SymMonomial m{Rat::one_of(ctx)};
    m.delta_powers[var_name] = 1;
    return from_monomials(ctx, {std::move(m)});
}

// -------- Canonicalization --------

SymCoef SymCoef::canonicalize() const & {
    // 2026-04-30 (axis-E small-input fast paths): for terms_.size() in
    // {0, 1, 2}, skip the like-term-collection + unordered_map + sort
    // machinery entirely. Probe-4 + dynamic-1 measurement on parity-1
    // ord_1_face_1 step 6 showed 1054 slots (avg 6.2 mons each, but
    // heavy-tailed) consume 56 wall-s of parallel canonicalize. Step 6
    // bucket_symcoef_add (260 CPU-s) hammers from_monomials with
    // exactly size-2 inputs (one term per side at the bucket-collision
    // call site). Each fast path normalises in place, sums on like-key
    // collision (size 2 only), and emits a sorted vector — no map, no
    // std::sort, no auxiliary allocations.
    auto normalize_in_place = [](SymMonomial& m) {
        const int total_i = m.i_power;
        const int sign    = (((total_i % 4) + 4) % 4) >= 2 ? -1 : 1;
        const int new_i   = ((total_i % 2) + 2) % 2;
        if (sign < 0) m.prefactor = -m.prefactor;
        m.i_power = new_i;
        for (auto it = m.log_powers.begin(); it != m.log_powers.end(); ) {
            if (it->second == 0) it = m.log_powers.erase(it);
            else                 ++it;
        }
        for (auto it = m.period_powers.begin();
             it != m.period_powers.end(); ) {
            if (it->second == 0) it = m.period_powers.erase(it);
            else                 ++it;
        }
        for (auto it = m.delta_powers.begin(); it != m.delta_powers.end(); ) {
            const int reduced = ((it->second % 2) + 2) % 2;
            if (reduced == 0) it = m.delta_powers.erase(it);
            else              { it->second = reduced; ++it; }
        }
    };

    if (terms_.size() <= 1) {
        SymCoef out(*ctx_);
        if (terms_.empty()) return out;
        SymMonomial m = terms_[0];
        normalize_in_place(m);
        if (!m.prefactor.is_zero()) out.terms_.push_back(std::move(m));
        return out;
    }

    if (terms_.size() == 2) {
        SymCoef out(*ctx_);
        SymMonomial a = terms_[0];
        SymMonomial b = terms_[1];
        normalize_in_place(a);
        normalize_in_place(b);
        const bool a_zero = a.prefactor.is_zero();
        const bool b_zero = b.prefactor.is_zero();
        if (a_zero && b_zero) return out;
        if (a_zero) { out.terms_.push_back(std::move(b)); return out; }
        if (b_zero) { out.terms_.push_back(std::move(a)); return out; }
        const std::string ka = a.power_key();
        const std::string kb = b.power_key();
        if (ka == kb) {
            a.prefactor += b.prefactor;
            if (!a.prefactor.is_zero()) out.terms_.push_back(std::move(a));
            return out;
        }
        if (ka < kb) {
            out.terms_.push_back(std::move(a));
            out.terms_.push_back(std::move(b));
        } else {
            out.terms_.push_back(std::move(b));
            out.terms_.push_back(std::move(a));
        }
        return out;
    }

    // Slow path: ≥3 input monomials, full like-term collection.
    // First, fold I^2 = -1 in every monomial individually (no-op when
    // the monomial was already produced via a canonicalized op, but
    // safe to repeat).
    std::vector<SymMonomial> normalized;
    normalized.reserve(terms_.size());
    for (auto m : terms_) {
        int total_i  = m.i_power;
        int sign     = (((total_i % 4) + 4) % 4) >= 2 ? -1 : 1;
        int new_i    = ((total_i % 2) + 2) % 2;
        if (sign < 0) m.prefactor = -m.prefactor;
        m.i_power = new_i;

        // Drop zero entries from the maps (a +1/-1 cancellation can
        // leave 0 in log/delta on some code paths).
        for (auto it = m.log_powers.begin(); it != m.log_powers.end(); ) {
            if (it->second == 0) it = m.log_powers.erase(it);
            else                 ++it;
        }
        for (auto it = m.period_powers.begin();
             it != m.period_powers.end(); ) {
            if (it->second == 0) it = m.period_powers.erase(it);
            else                 ++it;
        }
        // delta[v] is a sign indicator (±1, per HyperIntica.wl:59), so
        // delta[v]^k is 1 for even k and delta[v] for odd k — reduce
        // every delta power mod 2 here in canonicalize so the invariant
        // "delta_powers[v] ∈ {0, 1}" holds at every SymCoef boundary,
        // regardless of how the SymMonomial was built.
        for (auto it = m.delta_powers.begin(); it != m.delta_powers.end(); ) {
            int reduced = ((it->second % 2) + 2) % 2;
            if (reduced == 0) it = m.delta_powers.erase(it);
            else              { it->second = reduced; ++it; }
        }
        if (!m.prefactor.is_zero()) normalized.push_back(std::move(m));
    }

    // Tier 0 (axis-B power-key cache): build power_key strings ONCE per
    // monomial and reuse them for both like-term collection and the sort
    // comparator. The previous implementation rebuilt the ostringstream
    // inside the sort comparator twice per comparison, which empirically
    // dominated step 6 (x4) of the 3l3pt ord_1 trace — 259.75 s in
    // bucket_symcoef_add over 49,194 collisions, ~94 % of step wall.
    // Falsifier registered: this change should drop step 6 by ≥50 s with
    // step 5 / step 7 stationary. See notes/3l3pt_pass2_structural_memo.md
    // §5 and the Claus / JC exchange #158-#159 for the full pre-registration.
    // 2026-04-30 (Tier 1.4b u128 power_hash — RETIRED null result):
    // Tried replacing the per-monomial power_key() string with a
    // 128-bit FNV-1a structural hash for the equality map. n=3
    // untraced + n=1 traced on parity-1 ord_1_face_1: parity-1 wall
    // 207.7 -> 215.2 s (+3.6%, σ-significant), step-5 par_canon CPU
    // 54.9 -> 65.7 CPU-s (+20%). The u128 mix (15-25 FNV-1a multiply-
    // xor pairs per monomial including delta-string char hashing) is
    // *more* expensive than ostringstream for the typical SymMonomial
    // (small, SSO-fit string). Reviewer's "30-60% drop" prediction
    // assumed string-allocation-dominated cost; for this workload
    // SSO + no-malloc means the string was already cheap. Reverted to
    // power_key() string for the equality map. SymMonomial::power_hash()
    // kept as a public method (no harm; future callers may want it).
    std::vector<std::string> keys;
    keys.reserve(normalized.size());
    for (const auto& m : normalized) {
        keys.push_back(m.power_key());
    }

    // Like-term collection using the cached keys.
    std::unordered_map<std::string, size_t> idx;
    std::vector<SymMonomial> collected;
    std::vector<std::string>  collected_keys;
    collected.reserve(normalized.size());
    collected_keys.reserve(normalized.size());
    for (size_t i = 0; i < normalized.size(); ++i) {
        auto it = idx.find(keys[i]);
        if (it == idx.end()) {
            idx.emplace(keys[i], collected.size());
            collected.push_back(std::move(normalized[i]));
            collected_keys.push_back(std::move(keys[i]));
        } else {
            // 2026-04-30 (axis-E): in-place += avoids the temporary
            // Rat + 4 Poly moves vs `prefactor = prefactor + x`.
            // Same lever as the integrate_ii bump cell-assign in
            // commit 06bf026f1.
            collected[it->second].prefactor += normalized[i].prefactor;
        }
    }
    // Drop zero-prefactor terms that the sum produced. Maintain the
    // parallel keys vector by index-copying the survivors.
    std::vector<size_t> keep;
    keep.reserve(collected.size());
    for (size_t i = 0; i < collected.size(); ++i) {
        if (!collected[i].prefactor.is_zero()) keep.push_back(i);
    }

    // Sort over indices; comparator reads cached_keys[i] (no allocation).
    // Tiebreaker on prefactor string is unreachable post-collection (keys
    // are unique by construction) but kept as a defensive fallback in
    // case a future code path injects duplicates.
    std::sort(keep.begin(), keep.end(),
              [&](size_t a, size_t b) {
                  const std::string& ka = collected_keys[a];
                  const std::string& kb = collected_keys[b];
                  if (ka != kb) return ka < kb;
                  return collected[a].prefactor.to_string()
                       < collected[b].prefactor.to_string();
              });

    std::vector<SymMonomial> sorted_terms;
    sorted_terms.reserve(keep.size());
    for (size_t i : keep) sorted_terms.push_back(std::move(collected[i]));

    SymCoef out(*ctx_);
    out.terms_ = std::move(sorted_terms);
    return out;
}

// 2026-05-10 (Phase 4 §B.1 iter-24b Patches G+H — rvalue canonicalize):
// consumes *this. Moves terms_ out at entry; from then on operates on
// the local vector, moving each SymMonomial when constructing
// `normalized` and again when emitting the final sorted vector. After
// return, *this is in moved-from state (terms_ empty). Algorithm,
// fast-path branches, and canonical-output invariant match the const
// overload byte-for-byte.
SymCoef SymCoef::canonicalize() && {
    std::vector<SymMonomial> local_terms = std::move(terms_);

    auto normalize_in_place = [](SymMonomial& m) {
        const int total_i = m.i_power;
        const int sign    = (((total_i % 4) + 4) % 4) >= 2 ? -1 : 1;
        const int new_i   = ((total_i % 2) + 2) % 2;
        if (sign < 0) m.prefactor = -m.prefactor;
        m.i_power = new_i;
        for (auto it = m.log_powers.begin(); it != m.log_powers.end(); ) {
            if (it->second == 0) it = m.log_powers.erase(it);
            else                 ++it;
        }
        for (auto it = m.period_powers.begin();
             it != m.period_powers.end(); ) {
            if (it->second == 0) it = m.period_powers.erase(it);
            else                 ++it;
        }
        for (auto it = m.delta_powers.begin(); it != m.delta_powers.end(); ) {
            const int reduced = ((it->second % 2) + 2) % 2;
            if (reduced == 0) it = m.delta_powers.erase(it);
            else              { it->second = reduced; ++it; }
        }
    };

    if (local_terms.size() <= 1) {
        SymCoef out(*ctx_);
        if (local_terms.empty()) return out;
        SymMonomial m = std::move(local_terms[0]);
        normalize_in_place(m);
        if (!m.prefactor.is_zero()) out.terms_.push_back(std::move(m));
        return out;
    }

    if (local_terms.size() == 2) {
        SymCoef out(*ctx_);
        SymMonomial a = std::move(local_terms[0]);
        SymMonomial b = std::move(local_terms[1]);
        normalize_in_place(a);
        normalize_in_place(b);
        const bool a_zero = a.prefactor.is_zero();
        const bool b_zero = b.prefactor.is_zero();
        if (a_zero && b_zero) return out;
        if (a_zero) { out.terms_.push_back(std::move(b)); return out; }
        if (b_zero) { out.terms_.push_back(std::move(a)); return out; }
        const std::string ka = a.power_key();
        const std::string kb = b.power_key();
        if (ka == kb) {
            a.prefactor += b.prefactor;
            if (!a.prefactor.is_zero()) out.terms_.push_back(std::move(a));
            return out;
        }
        if (ka < kb) {
            out.terms_.push_back(std::move(a));
            out.terms_.push_back(std::move(b));
        } else {
            out.terms_.push_back(std::move(b));
            out.terms_.push_back(std::move(a));
        }
        return out;
    }

    // Slow path: ≥3 input monomials, full like-term collection. Moves
    // each src monomial into the normalized vector instead of copying.
    std::vector<SymMonomial> normalized;
    normalized.reserve(local_terms.size());
    for (size_t k = 0; k < local_terms.size(); ++k) {
        SymMonomial m = std::move(local_terms[k]);
        int total_i  = m.i_power;
        int sign     = (((total_i % 4) + 4) % 4) >= 2 ? -1 : 1;
        int new_i    = ((total_i % 2) + 2) % 2;
        if (sign < 0) m.prefactor = -m.prefactor;
        m.i_power = new_i;
        for (auto it = m.log_powers.begin(); it != m.log_powers.end(); ) {
            if (it->second == 0) it = m.log_powers.erase(it);
            else                 ++it;
        }
        for (auto it = m.period_powers.begin();
             it != m.period_powers.end(); ) {
            if (it->second == 0) it = m.period_powers.erase(it);
            else                 ++it;
        }
        for (auto it = m.delta_powers.begin(); it != m.delta_powers.end(); ) {
            int reduced = ((it->second % 2) + 2) % 2;
            if (reduced == 0) it = m.delta_powers.erase(it);
            else              { it->second = reduced; ++it; }
        }
        if (!m.prefactor.is_zero()) normalized.push_back(std::move(m));
    }

    std::vector<std::string> keys;
    keys.reserve(normalized.size());
    for (const auto& m : normalized) {
        keys.push_back(m.power_key());
    }

    std::unordered_map<std::string, size_t> idx;
    std::vector<SymMonomial> collected;
    std::vector<std::string>  collected_keys;
    collected.reserve(normalized.size());
    collected_keys.reserve(normalized.size());
    for (size_t i = 0; i < normalized.size(); ++i) {
        auto it = idx.find(keys[i]);
        if (it == idx.end()) {
            idx.emplace(keys[i], collected.size());
            collected.push_back(std::move(normalized[i]));
            collected_keys.push_back(std::move(keys[i]));
        } else {
            collected[it->second].prefactor += normalized[i].prefactor;
        }
    }
    std::vector<size_t> keep;
    keep.reserve(collected.size());
    for (size_t i = 0; i < collected.size(); ++i) {
        if (!collected[i].prefactor.is_zero()) keep.push_back(i);
    }

    std::sort(keep.begin(), keep.end(),
              [&](size_t a, size_t b) {
                  const std::string& ka = collected_keys[a];
                  const std::string& kb = collected_keys[b];
                  if (ka != kb) return ka < kb;
                  return collected[a].prefactor.to_string()
                       < collected[b].prefactor.to_string();
              });

    std::vector<SymMonomial> sorted_terms;
    sorted_terms.reserve(keep.size());
    for (size_t i : keep) sorted_terms.push_back(std::move(collected[i]));

    SymCoef out(*ctx_);
    out.terms_ = std::move(sorted_terms);
    return out;
}

// 2026-04-30 (Tier 1.6a): linear merge of two already-canonical
// SymCoefs. See header comment for the invariant + recipe.
//
// Implementation: two iterators, gather-cluster-then-emit. We must
// drain ALL contributions at a given key before deciding "drop on
// zero" (per adversarial reviewer 2026-04-30): consider three
// chunks contributing prefactors {p, -p, q} at the same key — the
// pair {p, -p} cancels, but only if we sum all three. With K=2
// inputs the cluster is at most one element from each side, so a
// single equality check suffices.
SymCoef SymCoef::merge_sorted_canonical(const SymCoef& a,
                                         const SymCoef& b) {
    if (a.ctx_ != b.ctx_) {
        throw std::runtime_error(
            "SymCoef::merge_sorted_canonical: ctx mismatch");
    }
    const auto& A = a.terms_;
    const auto& B = b.terms_;
    SymCoef out(*a.ctx_);
    if (A.empty()) {
        out.terms_ = B;
        return out;
    }
    if (B.empty()) {
        out.terms_ = A;
        return out;
    }
    out.terms_.reserve(A.size() + B.size());
    size_t ia = 0, ib = 0;
    // Cache power_key strings for the comparator; rebuilding inside
    // the loop would dominate the merge cost. Each side is already
    // canonical so each call to power_key() is O(per-monomial state).
    std::vector<std::string> ka(A.size()), kb(B.size());
    for (size_t i = 0; i < A.size(); ++i) ka[i] = A[i].power_key();
    for (size_t i = 0; i < B.size(); ++i) kb[i] = B[i].power_key();
    while (ia < A.size() && ib < B.size()) {
        if (ka[ia] < kb[ib]) {
            out.terms_.push_back(A[ia]);
            ++ia;
        } else if (kb[ib] < ka[ia]) {
            out.terms_.push_back(B[ib]);
            ++ib;
        } else {
            // Equal key: gather both sides' contributions. With
            // K=2 inputs, gathering is just summing the two heads.
            SymMonomial m = A[ia];
            // HF_ADDPF_PROBE (additive-parfrac Phase 0): attribute the
            // key-match fusion add by denominator equality. diff-den
            // adds pay the cross-multiply + GCD; they are the class
            // the additive-parfrac lever would relocate to residue
            // space. Default-OFF; one branch on a static when off.
            if (addpf_probe::enabled()) {
                const bool same_den =
                    m.prefactor.den().equal(B[ib].prefactor.den());
                const std::size_t da = m.prefactor.den().n_terms();
                const std::size_t db = B[ib].prefactor.den().n_terms();
                const auto t0 = std::chrono::steady_clock::now();
                m.prefactor += B[ib].prefactor;
                addpf_probe::record_merge_add(
                    same_den,
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - t0).count(),
                    da, db, m.prefactor.den().n_terms());
            } else {
                m.prefactor += B[ib].prefactor;
            }
            if (!m.prefactor.is_zero()) {
                out.terms_.push_back(std::move(m));
            }
            ++ia;
            ++ib;
        }
    }
    while (ia < A.size()) { out.terms_.push_back(A[ia]); ++ia; }
    while (ib < B.size()) { out.terms_.push_back(B[ib]); ++ib; }
    return out;
}

// 2026-05-10 (Phase 4 §B.1 iter-24b Patches G+H — rvalue merge): same
// algorithm as the const overload, but moves terms out of both
// operands. Both a and b are in moved-from state after return.
SymCoef SymCoef::merge_sorted_canonical(SymCoef&& a, SymCoef&& b) {
    if (a.ctx_ != b.ctx_) {
        throw std::runtime_error(
            "SymCoef::merge_sorted_canonical: ctx mismatch");
    }
    const PolyCtx* ctx_ptr = a.ctx_;
    std::vector<SymMonomial> A = std::move(a.terms_);
    std::vector<SymMonomial> B = std::move(b.terms_);
    SymCoef out(*ctx_ptr);
    if (A.empty()) {
        out.terms_ = std::move(B);
        return out;
    }
    if (B.empty()) {
        out.terms_ = std::move(A);
        return out;
    }
    out.terms_.reserve(A.size() + B.size());
    size_t ia = 0, ib = 0;
    std::vector<std::string> ka(A.size()), kb(B.size());
    for (size_t i = 0; i < A.size(); ++i) ka[i] = A[i].power_key();
    for (size_t i = 0; i < B.size(); ++i) kb[i] = B[i].power_key();
    while (ia < A.size() && ib < B.size()) {
        if (ka[ia] < kb[ib]) {
            out.terms_.push_back(std::move(A[ia]));
            ++ia;
        } else if (kb[ib] < ka[ia]) {
            out.terms_.push_back(std::move(B[ib]));
            ++ib;
        } else {
            SymMonomial m = std::move(A[ia]);
            // HF_ADDPF_PROBE: same attribution as the const overload.
            if (addpf_probe::enabled()) {
                const bool same_den =
                    m.prefactor.den().equal(B[ib].prefactor.den());
                const std::size_t da = m.prefactor.den().n_terms();
                const std::size_t db = B[ib].prefactor.den().n_terms();
                const auto t0 = std::chrono::steady_clock::now();
                m.prefactor += B[ib].prefactor;
                addpf_probe::record_merge_add(
                    same_den,
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - t0).count(),
                    da, db, m.prefactor.den().n_terms());
            } else {
                m.prefactor += B[ib].prefactor;
            }
            if (!m.prefactor.is_zero()) {
                out.terms_.push_back(std::move(m));
            }
            ++ia;
            ++ib;
        }
    }
    while (ia < A.size()) { out.terms_.push_back(std::move(A[ia])); ++ia; }
    while (ib < B.size()) { out.terms_.push_back(std::move(B[ib])); ++ib; }
    return out;
}

// 2026-04-30 (Tier 1.6b): pairwise tree-reduce of K canonical SymCoefs.
SymCoef SymCoef::tree_merge(std::vector<SymCoef>&& chunks) {
    if (chunks.empty()) {
        throw std::runtime_error("SymCoef::tree_merge: empty chunks");
    }
    while (chunks.size() > 1) {
        std::vector<SymCoef> next;
        next.reserve((chunks.size() + 1) / 2);
        // Pair up consecutive chunks; the trailing odd one (if any)
        // carries over unchanged.
        size_t i = 0;
        for (; i + 1 < chunks.size(); i += 2) {
            // iter-24b Patch J: chunks[i] and chunks[i+1] are unused
            // after this call (only the merged result is pushed into
            // `next`; chunks itself is overwritten by the `chunks =
            // std::move(next)` at the end of the level). Route through
            // the rvalue merge overload to move terms out of both
            // operands.
            next.push_back(
                SymCoef::merge_sorted_canonical(std::move(chunks[i]),
                                                std::move(chunks[i + 1])));
        }
        if (i < chunks.size()) {
            next.push_back(std::move(chunks[i]));
        }
        chunks = std::move(next);
    }
    return std::move(chunks[0]);
}

// -------- Arithmetic --------

SymCoef SymCoef::add(const SymCoef& o) const {
    if (ctx_ != o.ctx_) {
        throw std::runtime_error("SymCoef::add: ctx mismatch");
    }
    std::vector<SymMonomial> ms;
    ms.reserve(terms_.size() + o.terms_.size());
    for (const auto& m : terms_)   ms.push_back(m);
    for (const auto& m : o.terms_) ms.push_back(m);
    return from_monomials(*ctx_, std::move(ms));
}

// 2026-04-30 (axis-E): in-place += avoids the LHS deep-copy of
// `terms_` that operator+ pays. Hot at integration_step.cpp:610.
// Always canonicalises the result (same invariant as operator+).
//
// 2026-04-30 (Tier 1.7 / Lever #4 — explored, REVERTED): tried
// routing through `merge_sorted_canonical` instead of append-then-
// canonicalize. n=3 paired on parity-1 ord_1 face_1 post-1.6b:
// 168.5 → 167.7 s (Δ = −0.8, σ widened 0.85 → 2.6, Welch t ≈ −0.5,
// not significant). Reviewer predicted +5–6 s; in practice, the
// upstream parallel-for is η ≈ 0.99 even post-1.6b, and the 80
// CPU-s saving on bucket_symcoef_add translates to wall savings
// only at the rate of (η × n_threads)⁻¹ ≈ 1/12.9 of CPU saving.
// Bit-identity preserved on tst0/1/2 + parity-1 across n=3.
// `merge_sorted_canonical` remains the post-merge primitive in
// 1.6b — it's still the right tool for cross-thread chunk
// merging, just not for per-bump operator+=.
SymCoef& SymCoef::operator+=(const SymCoef& o) {
    if (ctx_ != o.ctx_) {
        throw std::runtime_error("SymCoef::operator+=: ctx mismatch");
    }
    if (o.terms_.empty()) {
        if (terms_.empty()) return *this;
        // Non-empty terms_, but += of a zero-SymCoef. Still must
        // canonicalise this->terms_ for invariant — but since terms_
        // is presumably already canonical (came from a prior op),
        // skip the work.
        return *this;
    }
    if (terms_.empty()) {
        terms_ = o.terms_;
    } else {
        terms_.reserve(terms_.size() + o.terms_.size());
        for (const auto& m : o.terms_) terms_.push_back(m);
    }
    SymCoef canon = canonicalize();
    terms_ = std::move(canon.terms_);
    return *this;
}

SymCoef SymCoef::sub(const SymCoef& o) const {
    return add(o.neg());
}

SymCoef SymCoef::neg() const {
    std::vector<SymMonomial> ms;
    ms.reserve(terms_.size());
    for (auto m : terms_) {
        m.prefactor = -m.prefactor;
        ms.push_back(std::move(m));
    }
    return from_monomials(*ctx_, std::move(ms));
}

SymCoef SymCoef::mul(const SymCoef& o) const {
    if (ctx_ != o.ctx_) {
        throw std::runtime_error("SymCoef::mul: ctx mismatch");
    }
    std::vector<SymMonomial> ms;
    ms.reserve(terms_.size() * o.terms_.size());
    for (const auto& a : terms_) {
        for (const auto& b : o.terms_) {
            SymMonomial c{a.prefactor * b.prefactor};
            c.pi_power = a.pi_power + b.pi_power;
            c.i_power  = a.i_power  + b.i_power;
            c.log_powers = a.log_powers;
            for (const auto& kv : b.log_powers) c.log_powers[kv.first] += kv.second;
            c.delta_powers = a.delta_powers;
            for (const auto& kv : b.delta_powers) {
                int sum = c.delta_powers[kv.first] + kv.second;
                int reduced = ((sum % 2) + 2) % 2;
                if (reduced == 0) c.delta_powers.erase(kv.first);
                else              c.delta_powers[kv.first] = reduced;
            }
            // Period-tuples Phase 2: free commutative monoid -- pure
            // exponent addition, no reduction (spec 2026-06-04 §2.2).
            c.period_powers = a.period_powers;
            for (const auto& kv : b.period_powers)
                c.period_powers[kv.first] += kv.second;
            ms.push_back(std::move(c));
        }
    }
    return from_monomials(*ctx_, std::move(ms));
}

SymCoef SymCoef::mul_rat(const Rat& r) const {
    if (&r.ctx() != ctx_) {
        throw std::runtime_error("SymCoef::mul_rat: ctx mismatch");
    }
    if (r.is_zero()) return SymCoef(*ctx_);
    std::vector<SymMonomial> ms;
    ms.reserve(terms_.size());
    for (auto m : terms_) {
        m.prefactor = m.prefactor * r;
        ms.push_back(std::move(m));
    }
    return from_monomials(*ctx_, std::move(ms));
}

SymCoef SymCoef::div_rat(const Rat& r) const {
    if (r.is_zero()) {
        throw std::runtime_error("SymCoef::div_rat: division by zero");
    }
    if (&r.ctx() != ctx_) {
        throw std::runtime_error("SymCoef::div_rat: ctx mismatch");
    }
    std::vector<SymMonomial> ms;
    ms.reserve(terms_.size());
    for (auto m : terms_) {
        m.prefactor = m.prefactor / r;
        ms.push_back(std::move(m));
    }
    return from_monomials(*ctx_, std::move(ms));
}

// -------- is_rat / as_rat --------

bool SymCoef::is_rat() const {
    if (terms_.empty()) return true;
    if (terms_.size() != 1) return false;
    return terms_.front().is_pure_rat();
}

Rat SymCoef::as_rat() const {
    // Phase 2.B-prime iter-3: use Rat::zero_of fast path (rat.hpp:55-58)
    // to avoid 711-var fmpq_mpoly_set_str_pretty parse on every empty-branch hit.
    if (terms_.empty()) return Rat::zero_of(*ctx_);
    if (!is_rat()) {
        throw std::runtime_error("SymCoef::as_rat: residual symbolic powers");
    }
    return terms_.front().prefactor;
}

// -------- to_string --------

std::string SymCoef::to_string() const {
    if (terms_.empty()) return "0";
    std::ostringstream o;
    bool first = true;
    for (const auto& m : terms_) {
        if (!first) o << " + ";
        o << m.to_string();
        first = false;
    }
    return o.str();
}

// -------- simplify_symcoef / reduce_to_rat --------

namespace {

// Find the variable index for "mzv_2" in `ctx`, or return -1 if absent.
long find_mzv2_index(const PolyCtx& ctx) {
    const auto& vars = ctx.vars();
    for (size_t i = 0; i < vars.size(); ++i) {
        if (vars[i] == "mzv_2") return static_cast<long>(i);
    }
    return -1;
}

}  // namespace

SymCoef simplify_symcoef(const SymCoef& s, const MzvReductionTable& /*table*/) {
    // Pi^(2k) -> (6 * mzv_2)^k folds even powers of Pi into the Rat
    // prefactor. Requires "mzv_2" to be a variable in s.ctx().
    const PolyCtx& ctx = s.ctx();
    // Period-tuples (binding review fold, 2026-06-04): under the slim
    // ctx mzv_2 is not a ring var; fold Pi^(2k) -> 6^k * mzv_2^k into
    // period_powers instead, preserving wide-path semantics.
    if (period_tuples_enabled()) {
        const std::uint32_t z2 = PeriodTable::instance().id_for("mzv_2");
        SymCoef result(ctx);
        std::vector<SymMonomial> ms;
        for (auto m : s.terms()) {
            const int k = m.pi_power / 2;
            m.pi_power -= 2 * k;
            if (k > 0) {
                long six_k = 1;
                for (int i = 0; i < k; ++i) six_k *= 6;
                m.prefactor = m.prefactor * Rat::from_int(ctx, six_k);
                m.period_powers[z2] += k;
            }
            ms.push_back(std::move(m));
        }
        return SymCoef::from_monomials(ctx, std::move(ms));
    }
    if (find_mzv2_index(ctx) < 0) {
        return s;   // no mzv_2 in ctx, nothing to fold
    }
    Rat six_mzv2 = Rat::parse(ctx, "6*mzv_2");

    SymCoef result(ctx);
    for (auto m : s.terms()) {
        int k        = m.pi_power / 2;     // pi_power >= 0 by construction
        int residual = m.pi_power - 2 * k; // 0 or 1
        if (k > 0) {
            Rat mult = six_mzv2;
            for (int i = 1; i < k; ++i) mult = mult * six_mzv2;
            m.prefactor = m.prefactor * mult;
        }
        m.pi_power = residual;

        // Rebuild a SymCoef carrying this monomial via the public API
        // (avoids a friend declaration just for this internal helper).
        SymCoef mono = SymCoef::from_rat(m.prefactor);
        if (m.pi_power == 1) mono = mono.mul(SymCoef::pi_factor(ctx));
        if (m.i_power == 1)  mono = mono.mul(SymCoef::im_factor(ctx));
        for (const auto& kv : m.log_powers) {
            for (int i = 0; i < kv.second; ++i)
                mono = mono.mul(SymCoef::log_factor(ctx, kv.first));
        }
        for (const auto& kv : m.delta_powers) {
            for (int i = 0; i < kv.second; ++i)
                mono = mono.mul(SymCoef::delta_factor(ctx, kv.first));
        }
        // Period-tuples: reattach period content dropped by the
        // from_rat rebuild (defensive parity; review fold B2c).
        if (!m.period_powers.empty()) {
            SymCoef withp(ctx);
            std::vector<SymMonomial> pm;
            for (auto t : mono.terms()) {
                for (const auto& kv : m.period_powers)
                    t.period_powers[kv.first] += kv.second;
                pm.push_back(std::move(t));
            }
            mono = SymCoef::from_monomials(ctx, std::move(pm));
        }
        result = result.add(mono);
    }
    return result;
}

Rat reduce_to_rat(const SymCoef& s, const MzvReductionTable& table) {
    SymCoef simplified = simplify_symcoef(s, table);
    // Phase 2.B-prime iter-3: use Rat::zero_of fast path (rat.hpp:55-58); see SymCoef::as_rat.
    if (simplified.is_zero()) return Rat::zero_of(s.ctx());
    for (const auto& m : simplified.terms()) {
        if (m.pi_power != 0) {
            throw std::runtime_error(
                "reduce_to_rat: residual Pi^" + std::to_string(m.pi_power)
                + " (must be even and absorbed into mzv_2)");
        }
        if (m.i_power != 0) {
            throw std::runtime_error("reduce_to_rat: residual imaginary unit I");
        }
        if (!m.log_powers.empty()) {
            throw std::runtime_error("reduce_to_rat: residual Log[n] factor");
        }
        if (!m.delta_powers.empty()) {
            throw std::runtime_error("reduce_to_rat: residual delta[var] factor");
        }
        if (!m.period_powers.empty()) {
            throw std::runtime_error(
                "reduce_to_rat: residual period generator (period-tuples "
                "content cannot be reduced to a kinematic Rat)");
        }
    }
    if (!simplified.is_rat()) {
        throw std::runtime_error("reduce_to_rat: simplified form not a single Rat");
    }
    return simplified.as_rat();
}

// HF MZV-rewrite Phase A commit (2): SymCoef-side hook for the
// rat_split round-trip verifier (notes/hf_mzv_rewrite_design_2026-05-05/
// design.md §3.5, §5.1). Called by the HF_RAT_SPLIT_VERIFY harness
// (commit (3)) on every SymCoef-producing code path; the cheap
// fast-out-of-the-loop discipline keeps the verifier zero-cost when
// the env var is unset.
bool SymCoef::verify_rat_split_roundtrip(const PolyCtx& N) const {
    // Build a per-call ZWTable over the wide ctx F. For Phase A we
    // accept the per-call cost (commit (3) shares one table across
    // an entire SymCoef-producing call site if the verifier overhead
    // is non-negligible).
    ZWTable tab(*ctx_);
    FNIndexMaps maps = build_fn_index_maps(*ctx_, N);
    for (const auto& m : terms_) {
        const Rat& src = m.prefactor;
        std::vector<SymMonomialSplit> parts =
            split_rat_by_w_monomial(src, N, tab, maps.F_to_N_idx);
        Rat recon = recombine_rat_split(parts, *ctx_, tab,
                                        maps.N_to_F_idx);
        if (src.to_string() != recon.to_string()) {
            return false;
        }
    }
    return true;
}

}  // namespace hyperflint
