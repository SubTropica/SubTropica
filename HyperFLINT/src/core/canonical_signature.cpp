// HF FF Phase 5 §E Step E.2-impl-2 (iter-60-β.3).
//
// Implementation of canonical-signature factories per
// HyperFLINT/include/hyperflint/core/canonical_signature.hpp.
//
// Iter-60 ships a SCAFFOLDING implementation that uses `Poly::to_string()`
// / `Rat::to_string()` byte payload as the signature surface. This is
// CORRECT (two semantically-equal Polys produce byte-identical to_string()
// because the FLINT printer is deterministic) but SUBOPTIMAL (string
// round-trip allocates ~50–500 bytes per call). The iter-61+ ship per
// §iter-59-fold-REQ-6 will replace with direct fmpq_mpoly_t coefficient +
// exponent traversal via FLINT API. The end-to-end semantics are preserved
// so iter-61 is a performance-only swap.
//
// The XXH3 seed is process-fixed (boot-time random; FOLD-N1). First call
// to `xxh3_seed()` lazily initialises via std::random_device. The seed is
// stored in a relaxed atomic so subsequent calls are lock-free. A
// pthread_once-style barrier is used to ensure exactly-once initialisation
// under concurrent first-callers.

#define XXH_INLINE_ALL  // header-only XXH3 per §iter-59-fold-REQ-6 option b
#include "xxhash.h"

#include "hyperflint/core/canonical_signature.hpp"
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/rat.hpp"
#include "hyperflint/symbols/word.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <random>
#include <string>

namespace hyperflint {
namespace canonical_signature {

// ---------------------------------------------------------------------------
// XXH3 seed: boot-random; std::random_device on first call.
// ---------------------------------------------------------------------------
namespace {

std::atomic<std::uint64_t> g_xxh3_seed{0};
std::once_flag             g_xxh3_seed_init_flag;
bool                       g_xxh3_seed_overridden_for_testing = false;

void init_xxh3_seed_once() {
    std::call_once(g_xxh3_seed_init_flag, [] {
        if (g_xxh3_seed_overridden_for_testing) return;
        std::random_device rd;
        // Combine two 32-bit draws into a 64-bit seed.
        std::uint64_t hi = static_cast<std::uint64_t>(rd());
        std::uint64_t lo = static_cast<std::uint64_t>(rd());
        std::uint64_t seed = (hi << 32) | lo;
        // Avoid 0 as the seed (would degenerate XXH3 to its unseeded
        // variant; not a correctness bug but breaks the FOLD-N1 invariant).
        if (seed == 0) seed = 0x9E3779B97F4A7C15ULL;  // golden-ratio fallback
        g_xxh3_seed.store(seed, std::memory_order_relaxed);
    });
}

} // namespace

std::uint64_t xxh3_seed() {
    init_xxh3_seed_once();
    return g_xxh3_seed.load(std::memory_order_relaxed);
}

void seed_for_testing(std::uint64_t seed) {
    // Test-only seam; lock-free + idempotent under repeated calls.
    g_xxh3_seed_overridden_for_testing = true;
    g_xxh3_seed.store(seed, std::memory_order_relaxed);
    // Mark the once-flag as completed by entering it with a no-op.
    std::call_once(g_xxh3_seed_init_flag, [] {});
}

std::uint64_t xxh3_seeded(const void* data, std::size_t len) {
    return XXH3_64bits_withSeed(data, len, xxh3_seed());
}

std::uint64_t xxh3_seeded(const std::vector<std::uint64_t>& v) {
    return XXH3_64bits_withSeed(
        v.data(),
        v.size() * sizeof(std::uint64_t),
        xxh3_seed());
}

// ---------------------------------------------------------------------------
// Layer 1 — payload_sig(poly)
// ---------------------------------------------------------------------------
std::uint64_t payload_sig(const Poly& p) {
    // iter-60 scaffolding: use the FLINT-printer string payload as the
    // hash surface. Two semantically-equal Polys produce byte-identical
    // to_string() because the printer iterates the canonical monomial
    // order. iter-61+ replaces this with direct fmpq_mpoly traversal.
    const std::string& s = p.to_string();
    return xxh3_seeded(s.data(), s.size());
}

// ---------------------------------------------------------------------------
// Layer 2 — rat_sig(r)
// ---------------------------------------------------------------------------
std::uint64_t rat_sig(const Rat& r) {
    // Combine num + den signatures in a deterministic order. The Rat
    // class caches its to_string() via cached_str_ so the cost is one
    // FLINT print per unique Rat (the cache hit-rate at the call site
    // is the primary perf lever, not the print cost).
    const std::uint64_t num_sig = payload_sig(r.num());
    const std::uint64_t den_sig = payload_sig(r.den());
    return xxh3_seeded_pair(num_sig, den_sig);
}

// ---------------------------------------------------------------------------
// Layer 3 — ctx_fingerprint(p)
// ---------------------------------------------------------------------------
std::uint64_t ctx_fingerprint(const PolyCtx& ctx) {
    // iter-60 scaffolding: hash the (nvars + var-name) string list of
    // the PolyCtx. Two Polys built against the same PolyCtx produce the
    // same fingerprint; Polys against semantically-equivalent-but-distinct
    // PolyCtx instances also produce the same fingerprint (var-name list
    // is the source of truth, not pointer identity). iter-61+ may swap
    // to a struct-hashed (nvars, ord) pair via fmpq_mpoly_ctx_t direct
    // access.
    const auto& vars = ctx.vars();
    std::uint64_t h = xxh3_seed();
    for (auto const& v : vars) {
        const std::uint64_t one = xxh3_seeded(v.data(), v.size());
        h = xxh3_seeded_pair(h, one);
    }
    // Include nvars as a salt to disambiguate empty-string-only contexts
    // (degenerate but possible).
    const std::uint64_t nvars = static_cast<std::uint64_t>(vars.size());
    return xxh3_seeded_pair(h, nvars);
}

std::uint64_t ctx_fingerprint(const Poly& p) {
    return ctx_fingerprint(p.ctx());
}

std::uint64_t zw_pointer_identity(const ZWTable* zw_ptr) {
    // Pointer identity used in the cache key per §iter-59-fold-REQ-3
    // option b defense-in-depth (the cache is also bypassed entirely
    // under SCALAR_REP=1 via lf_enabled()/pf_enabled()).
    return reinterpret_cast<std::uint64_t>(zw_ptr);
}

// ---------------------------------------------------------------------------
// wordlist_payload_sig (iter-62-β.2)
// ---------------------------------------------------------------------------
std::uint64_t wordlist_payload_sig(const std::vector<Word>& wl) {
    // Fold each Word's pre-cached 128-bit `struct_hash()` pair into a
    // running 64-bit XXH3 accumulator. Word::struct_hash() already
    // canonicalises the letters' (num, den) Polys via the global
    // poly_struct_hash machinery; we layer the wordlist's positional
    // ordering on top so {a,b,c} ≠ {b,a,c}.
    //
    // Determinism (FOLD-D-DISCIPLINE-N): same XXH3 seed in the same
    // process produces byte-identical sigs for byte-identical wordlists.
    std::uint64_t h = xxh3_seed();
    for (const auto& w : wl) {
        // word.hpp:81-98 — auto-memoized 128-bit hash; first call walks
        // the letters, subsequent calls are a pair-load. Mutates
        // `cached_hash_`/`hash_cached_` on a const Word per the
        // documented caller-contract.
        const auto sh = w.struct_hash();
        h = xxh3_seeded_pair(h, sh.first);
        h = xxh3_seeded_pair(h, sh.second);
    }
    // Length salt: disambiguates wordlists whose Word::struct_hash()
    // accumulators happen to collide with a longer one's prefix.
    return xxh3_seeded_pair(h, static_cast<std::uint64_t>(wl.size()));
}

// ---------------------------------------------------------------------------
// LfKey
// ---------------------------------------------------------------------------
LfKey make_lf_key(const Poly& p, std::size_t var_idx,
                  const ZWTable* zw_ptr,
                  bool introduce_algebraic_letters,
                  bool compute_constant)
{
    return LfKey{
        payload_sig(p),
        var_idx,
        zw_ptr,
        introduce_algebraic_letters,
        compute_constant};
}

std::uint64_t hash_lf_key(const LfKey& k) {
    // Pack the 5 fields into a fixed-width buffer + XXH3 over it. The
    // bool fields are widened to uint64_t so the buffer layout is
    // stable across compilers (no struct-padding traps).
    const std::uint64_t buf[5] = {
        k.poly_sig,
        static_cast<std::uint64_t>(k.var_idx),
        reinterpret_cast<std::uint64_t>(k.zw_ptr_identity),
        k.introduce_algebraic_letters ? 1ULL : 0ULL,
        k.compute_constant            ? 1ULL : 0ULL};
    return xxh3_seeded(buf);
}

// ---------------------------------------------------------------------------
// PfKey
// ---------------------------------------------------------------------------
PfKey make_pf_key(const Rat& f, std::size_t var_idx,
                  const ZWTable* zw_ptr,
                  bool introduce_algebraic_letters)
{
    return PfKey{
        rat_sig(f),
        var_idx,
        zw_ptr,
        introduce_algebraic_letters};
}

std::uint64_t hash_pf_key(const PfKey& k) {
    const std::uint64_t buf[4] = {
        k.rat_sig,
        static_cast<std::uint64_t>(k.var_idx),
        reinterpret_cast<std::uint64_t>(k.zw_ptr_identity),
        k.introduce_algebraic_letters ? 1ULL : 0ULL};
    return xxh3_seeded(buf);
}

// ---------------------------------------------------------------------------
// RatAddKey
// ---------------------------------------------------------------------------
RatAddKey make_rat_add_key(const Rat& a, const Rat& b) {
    // Rat::add is symmetric at the value level (a+b == b+a); lex-sort
    // the two operand sigs to canonicalise so commuted call orders
    // produce identical cache keys.
    const std::uint64_t sa = rat_sig(a);
    const std::uint64_t sb = rat_sig(b);
    return (sa <= sb)
        ? RatAddKey{sa, sb}
        : RatAddKey{sb, sa};
}

std::uint64_t hash_rat_add_key(const RatAddKey& k) {
    return xxh3_seeded_pair(k.lhs_sig, k.rhs_sig);
}

// ---------------------------------------------------------------------------
// ReduceKey
// ---------------------------------------------------------------------------
ReduceKey make_reduce_key(const Poly& num, const Poly& den) {
    return ReduceKey{payload_sig(num), payload_sig(den)};
}

std::uint64_t hash_reduce_key(const ReduceKey& k) {
    return xxh3_seeded_pair(k.num_sig, k.den_sig);
}

// ---------------------------------------------------------------------------
// TransformShuffleKey
// ---------------------------------------------------------------------------
TransformShuffleKey make_transform_shuffle_key(
    const std::vector<Word>& wordlist,
    std::size_t var_idx,
    const PolyCtx& ctx,
    const ZWTable* zw_ptr,
    bool introduce_algebraic_letters)
{
    return TransformShuffleKey{
        wordlist_payload_sig(wordlist),
        var_idx,
        ctx_fingerprint(ctx),
        zw_ptr,
        introduce_algebraic_letters,
        wordlist.size()};
}

std::uint64_t hash_transform_shuffle_key(const TransformShuffleKey& k) {
    // Pack all 6 fields into a fixed-width buffer + XXH3 over it.
    const std::uint64_t buf[6] = {
        k.wordlist_sig,
        static_cast<std::uint64_t>(k.var_idx),
        k.ctx_fp,
        reinterpret_cast<std::uint64_t>(k.zw_ptr_identity),
        k.introduce_algebraic_letters ? 1ULL : 0ULL,
        static_cast<std::uint64_t>(k.wordlist_size)};
    return xxh3_seeded(buf);
}

} // namespace canonical_signature
} // namespace hyperflint
