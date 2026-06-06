// 2026-04-26 structural hash on fmpq_mpoly_t.
//
// Replaces `linear_factors_cache_key()`'s `Poly::to_string()` +
// ostringstream call (measured at ~89 µs/call wall on tst2 = ~91 wall-s
// for 1014k cache lookups, see notes/benchmark_smirnov/sqf_round/
// cache_disable_analysis.md) with a 128-bit term-iterating hash that
// targets ~ 1 µs/call wall.
//
// Design (per adversarial-reviewer 2026-04-26):
//   - 128-bit key via std::pair<uint64_t,uint64_t>
//   - Two seeded FNV-1a hashes computed in lockstep
//   - Use FLINT COEFF_IS_MPZ macro for direct limb access (no malloc per term)
//   - sqf flag baked in as a bit in the second word, not a string prefix
//   - No defensive equality (collision prob ~ 1.5e-27 with 1M keys)
//
// Soundness: the FLINT canonicalisation audit (grep _unsafe / _set_term
// in HyperFLINT/src/) found zero non-canonicalising mutation paths.
// All fmpq_mpoly_t reaching `linear_factors` have been canonicalised
// by FLINT, so structurally equal polys hash identically.

#pragma once

#include "hyperflint/core/poly.hpp"

#include <gmp.h>
#include <flint/flint.h>
#include <flint/fmpq.h>
#include <flint/fmpq_mpoly.h>
#include <flint/fmpz.h>
#include <flint/fmpz_mpoly.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace hyperflint {

namespace detail {

// FNV-1a 64-bit primes, two distinct values for the 128-bit pair.
constexpr uint64_t kFNV1aPrime1 = 0x100000001b3ULL;
constexpr uint64_t kFNV1aPrime2 = 0x880355f21e6d1965ULL;
constexpr uint64_t kFNV1aOffset1 = 0xcbf29ce484222325ULL;
constexpr uint64_t kFNV1aOffset2 = 0x9ae16a3b2f90404fULL;

// Mix one 64-bit value into both halves of a 128-bit hash.
inline void mix_u64(uint64_t& h1, uint64_t& h2, uint64_t v) {
    h1 ^= v; h1 *= kFNV1aPrime1;
    h2 ^= v; h2 *= kFNV1aPrime2;
}

// Hash a single fmpz_t into both halves. Uses FLINT's small/big
// representation directly: small fmpz are stored in-place in the
// slong (no allocation); big fmpz heap-allocate an mpz_t and store
// a tagged pointer. COEFF_IS_MPZ / COEFF_TO_PTR are the FLINT
// macros that disambiguate.
inline void hash_fmpz(uint64_t& h1, uint64_t& h2, const fmpz_t z) {
    const fmpz v = *z;
    if (!COEFF_IS_MPZ(v)) {
        // Small case: hash the slong directly (cast to u64 preserves
        // bit pattern for both signs).
        mix_u64(h1, h2, static_cast<uint64_t>(v));
    } else {
        // Big case: hash sign-and-size, then limb data.
        mpz_ptr p = COEFF_TO_PTR(v);
        const int sz = p->_mp_size;
        mix_u64(h1, h2, static_cast<uint64_t>(static_cast<int64_t>(sz)));
        const int n = sz < 0 ? -sz : sz;
        const mp_limb_t* d = p->_mp_d;
        for (int i = 0; i < n; ++i) {
            mix_u64(h1, h2, static_cast<uint64_t>(d[i]));
        }
    }
}

// Hash an fmpq_t (numerator and denominator) into both halves.
inline void hash_fmpq(uint64_t& h1, uint64_t& h2, const fmpq_t q) {
    hash_fmpz(h1, h2, fmpq_numref(q));
    // Denominator separator so (n, d) pairs don't alias (n', d')
    // with the same concatenated limb stream.
    mix_u64(h1, h2, 0xfffffffffffffffeULL);
    hash_fmpz(h1, h2, fmpq_denref(q));
}

}  // namespace detail

// Mix one 64-bit value into both halves. Public re-export of the
// detail combiner so non-poly callers (Word::struct_hash, regkey
// folder) can mix their own separators / sentinels into a hash
// pair without re-implementing the FNV-1a step.
inline void poly_struct_hash_mix(uint64_t& h1, uint64_t& h2, uint64_t v) {
    detail::mix_u64(h1, h2, v);
}

// Default seed pair for non-poly callers that build their own hash
// out of poly_struct_hash_raw + their own sentinels.
inline std::pair<uint64_t, uint64_t> poly_struct_hash_seed() {
    return {detail::kFNV1aOffset1, detail::kFNV1aOffset2};
}

// 2026-04-26 (a-prime lever): factored discriminator-free body of
// `poly_struct_hash`. Mixes only `p`'s structural content (rational
// scalar + term stream) into two existing hash accumulators. Callers
// that want the original cache-key behaviour mix `(var_idx, intro_al,
// sqf)` discriminators in *first*, then call this helper; callers that
// want a raw poly hash (Word::struct_hash, etc.) initialize seeds and
// call this directly.
inline void poly_struct_hash_raw(uint64_t& h1, uint64_t& h2, const Poly& p) {
    using namespace detail;

    const fmpq_mpoly_struct* raw = p.raw();
    const fmpq_mpoly_ctx_struct* ctx = p.ctx().raw();

    // Hash the rational content (numerator/denominator).
    hash_fmpq(h1, h2, raw->content);

    // Hash the integer-polynomial part term by term. The zpoly's
    // term ordering is canonical (FLINT enforces sort + combine
    // after every mutation), so this is a stable structural
    // signature.
    const fmpz_mpoly_struct* z = raw->zpoly;
    const slong nterms = z->length;
    const slong nvars = ctx->zctx->minfo->nvars;

    // Mix term count to disambiguate empty vs. zero polys.
    mix_u64(h1, h2, static_cast<uint64_t>(nterms));

    if (nterms == 0) {
        return;
    }

    // Iterate terms via the public API. fmpq_mpoly_get_term_exp_ui
    // unpacks the packed exponent vector into a ulong array; cost
    // ~ O(nvars) per term. For a 50-var poly with ~100-2000 terms
    // this is the bulk of the hash work.
    std::vector<ulong> exps(static_cast<size_t>(nvars));

    for (slong i = 0; i < nterms; ++i) {
        // Hash exponents.
        fmpq_mpoly_get_term_exp_ui(exps.data(),
            const_cast<fmpq_mpoly_struct*>(raw), i, ctx);
        for (ulong e : exps) {
            mix_u64(h1, h2, static_cast<uint64_t>(e));
        }
        // Term separator so adjacent terms' streams don't merge.
        mix_u64(h1, h2, 0xffffffffffffffffULL);
        // Hash integer coefficient via direct fmpz limb access.
        const fmpz* zc = z->coeffs + i;
        hash_fmpz(h1, h2, zc);
    }
}

// Compute a 128-bit structural hash of `p`'s fmpq_mpoly content.
// Two equivalent canonical fmpq_mpolys produce the same key.
//
// 2026-04-29 (axis-C-lf-constant-defer): added `compute_constant` to
// the discriminator bitmask so cache entries produced under
// compute_constant=false (integration callers, with empty
// out.constant) cannot alias a compute_constant=true lookup
// (bridge-CLI consumer, which needs the constant). Without this,
// the bridge-CLI could silently get an empty constant string from
// a cache populated by a prior integration step.
inline std::pair<uint64_t, uint64_t>
poly_struct_hash(const Poly& p, size_t var_idx, bool intro_al, bool sqf,
                 bool compute_constant = false) {
    using namespace detail;

    uint64_t h1 = kFNV1aOffset1;
    uint64_t h2 = kFNV1aOffset2;

    // Mix the cache-key discriminators FIRST so polys cached under
    // different (var_idx, intro_al, sqf, compute_constant) settings
    // cannot alias.
    mix_u64(h1, h2, static_cast<uint64_t>(var_idx));
    mix_u64(h1, h2,
        (intro_al         ? 0x1ULL : 0x0ULL) |
        (sqf              ? 0x2ULL : 0x0ULL) |
        (compute_constant ? 0x4ULL : 0x0ULL));

    poly_struct_hash_raw(h1, h2, p);

    return std::make_pair(h1, h2);
}

// Three-way comparison on canonical fmpq_mpoly content. Walks the
// same (content, term-stream) shape as `poly_struct_hash_raw` and
// returns a stable negative / zero / positive int analogous to
// `strcmp`. Defines a total order on canonical fmpq_mpoly polys
// without ever calling `Poly::to_string()` (which mallocs +
// serialises every term — measured at the `to_string`-tiebreak site
// in `ZWTable::merge_into` as the dominant cost).
//
// Order definition (decided lexicographically; first nonzero wins):
//   1. Rational content: numerator (`fmpz_cmp`), then denominator.
//   2. Term count: shorter < longer.
//   3. For each term in FLINT's canonical (ORD_LEX-by-ctor) order:
//      a. Exponent vector (per variable, low index first).
//      b. Integer coefficient (`fmpz_cmp`).
//
// Soundness: identical to `poly_struct_hash_raw`'s scan in iteration
// order and granularity, with `fmpz_cmp` substituted for the FNV-1a
// mix — so any two polys structurally equal under the hash compare
// equal here, and any tiebreak after a hash collision is decided by
// content rather than by serialised string. Both inputs MUST share
// the same `PolyCtx`; the caller is responsible for that invariant.
inline int poly_struct_compare(const Poly& a, const Poly& b) {
    using namespace detail;

    const fmpq_mpoly_struct* ra = a.raw();
    const fmpq_mpoly_struct* rb = b.raw();
    const fmpq_mpoly_ctx_struct* ctx = a.ctx().raw();

    int c = fmpz_cmp(fmpq_numref(ra->content),
                     fmpq_numref(rb->content));
    if (c != 0) return c < 0 ? -1 : 1;
    c = fmpz_cmp(fmpq_denref(ra->content),
                 fmpq_denref(rb->content));
    if (c != 0) return c < 0 ? -1 : 1;

    const fmpz_mpoly_struct* za = ra->zpoly;
    const fmpz_mpoly_struct* zb = rb->zpoly;
    const slong na = za->length;
    const slong nb = zb->length;
    if (na != nb) return na < nb ? -1 : 1;
    if (na == 0) return 0;

    const slong nvars = ctx->zctx->minfo->nvars;
    std::vector<ulong> exps_a(static_cast<size_t>(nvars));
    std::vector<ulong> exps_b(static_cast<size_t>(nvars));

    for (slong i = 0; i < na; ++i) {
        fmpq_mpoly_get_term_exp_ui(exps_a.data(),
            const_cast<fmpq_mpoly_struct*>(ra), i, ctx);
        fmpq_mpoly_get_term_exp_ui(exps_b.data(),
            const_cast<fmpq_mpoly_struct*>(rb), i, ctx);
        for (slong v = 0; v < nvars; ++v) {
            if (exps_a[v] != exps_b[v]) {
                return exps_a[v] < exps_b[v] ? -1 : 1;
            }
        }
        c = fmpz_cmp(za->coeffs + i, zb->coeffs + i);
        if (c != 0) return c < 0 ? -1 : 1;
    }
    return 0;
}

// Hash combiner for std::unordered_map<std::pair<u64,u64>, ...>.
struct PairU64Hash {
    size_t operator()(const std::pair<uint64_t, uint64_t>& p) const noexcept {
        // Cheap mix: the two halves are already well-distributed FNV-1a
        // outputs; XOR + rotate is enough for unordered_map's bucket index.
        const uint64_t a = p.first;
        const uint64_t b = p.second;
        const uint64_t mixed = a ^ ((b << 31) | (b >> 33));
        return static_cast<size_t>(mixed);
    }
};

}  // namespace hyperflint
