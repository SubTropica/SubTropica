// HF FF Phase 5 §E Step E.2-impl-2 (iter-60-β.3).
//
// Canonical-signature factories for the operator-memo caches. Each entry-
// point produces a 64-bit XXH3-seeded signature suitable as a hash key in
// `OperatorMemo<KeyT, uint64_t>` and a `<Op>Key` struct used for the full-
// equality verification on cache HIT (defends against XXH3 64-bit
// collisions per FOLD-M3 BINDING).
//
// Per the §E implementation memo §2.2 + §iter-59 fold appendix:
//
//   Layer 1 — payload_sig(poly)        : fmpq_mpoly payload signature with
//                                        (nvars, ord) prefix + repacked bits.
//   Layer 2 — rat_sig(rat)             : lex-sorted summand-payload sigs +
//                                        lex-sorted denominator sigs.
//   Layer 3 — ctx_fingerprint(ctx)     : 64-bit packed (nvars, ord) pair.
//   Per-op factories:
//     make_lf_key(poly, var, zw_ptr, flags) → LfKey
//     make_pf_key(rat,  var, zw_ptr, flags) → PfKey
//     make_rat_add_key(a, b)                 → RatAddKey
//     make_reduce_key(num, den)              → ReduceKey
//     make_transform_shuffle_key(...)        → TransformShuffleKey  (placeholder; iter-61)
//
// SCALAR_REP=1 disposition (§iter-59-fold-REQ-3 option b):
//   make_lf_key + make_pf_key INCLUDE `zw_tab.get()` (raw pointer identity)
//   in the key. The predicates `operator_memo::lf_enabled()` and
//   `operator_memo::pf_enabled()` ALSO return false under HF_USE_SCALAR_REP=1,
//   so this is defense-in-depth: the cache is bypassed entirely before the
//   factory is even called. Including the pointer in the key remains useful
//   for SCALAR_REP=0 fixtures that may carry distinct ZWTable instances
//   (e.g. multiple bridge-CLI invocations in the same process).
//
// XXH3 seed (FOLD-N1 — boot-random):
//   The process-wide XXH3 seed is initialised once at first call to
//   `xxh3_seed()` via std::random_device. BYTE-ID-PRESERVED across same-
//   process boundaries (FOLD-D-DISCIPLINE-N) but NOT across process
//   restarts; acceptable per FOLD-M4 (cache lifetime = process lifetime).
//
// Header is forward-only; the implementations live in canonical_signature.cpp.
// Forward declarations of FLINT types are used so this header does not pull
// gmp.h / flint headers into every TU that includes operator_memo.hpp.

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace hyperflint {

class Poly;
class PolyCtx;
class Rat;
class ZWTable;
struct Word;

namespace canonical_signature {

// Process-wide XXH3 seed (boot-random; FOLD-N1).
// First call lazily initialises via std::random_device; subsequent calls
// are lock-free relaxed atomic reads.
std::uint64_t xxh3_seed();

// Re-seed for test harness only. Production code SHALL NOT call this.
// The test harness uses it to make per-test cache traffic deterministic.
void seed_for_testing(std::uint64_t seed);

// XXH3 64-bit hash of an arbitrary byte buffer, seeded with xxh3_seed().
std::uint64_t xxh3_seeded(const void* data, std::size_t len);

// XXH3 64-bit hash of a `uint64_t[N]` array, seeded with xxh3_seed().
template<std::size_t N>
inline std::uint64_t xxh3_seeded(const std::uint64_t (&arr)[N]) {
    return xxh3_seeded(static_cast<const void*>(arr), N * sizeof(std::uint64_t));
}

// XXH3 64-bit hash of a single uint64_t pair, seeded with xxh3_seed().
inline std::uint64_t xxh3_seeded_pair(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t buf[2] = {a, b};
    return xxh3_seeded(static_cast<const void*>(buf), sizeof(buf));
}

// XXH3 64-bit hash of a std::vector<uint64_t> (lex-sort safe).
std::uint64_t xxh3_seeded(const std::vector<std::uint64_t>& v);

// ---------------------------------------------------------------------------
// Layer 1 / 2 / 3 signatures.
// ---------------------------------------------------------------------------

// Layer 1: fmpq_mpoly payload signature with (nvars, ord) prefix (FOLD-M3)
// and `mpoly_fix_bits(MPOLY_MIN_BITS)` repack (FOLD-F2). The poly is read
// only — no in-place mutation.
//
// IMPLEMENTATION NOTE (iter-60-β.3): the iter-60 ship is a SCAFFOLDING
// implementation that uses `Poly::to_string()` byte payload as the
// signature surface. This is CORRECT (two equal Polys produce identical
// to_string()) but SUBOPTIMAL (the string round-trip allocates). The
// iter-61+ ship per §iter-59-fold-REQ-6 will replace with direct
// fmpq_mpoly_t coefficient + exponent traversal via FLINT API. The
// scaffolding semantics are preserved end-to-end so iter-61 is a
// performance-only swap.
std::uint64_t payload_sig(const Poly& p);

// Layer 2: Rat signature. Combines `payload_sig(num)` + `payload_sig(den)`
// in a deterministic (num, den)-ordered way. For unreduced Rat instances
// the signature distinguishes (a/b) from (b/a) even when canonical-form
// gives the same reduced fraction, which is the conservative choice
// (caching unreduced inputs separately is correctness-safe; reduced
// inputs collapse to a single cache entry post `reduce_inplace`).
std::uint64_t rat_sig(const Rat& r);

// Layer 3: PolyCtx-fingerprint stand-in (the iter-60 scaffolding uses a
// hash of the Poly's serialized context-derived (nvars, ord) prefix via
// Poly::to_string() length-and-prefix bytes; iter-61 will replace with
// direct fmpq_mpoly_ctx_t traversal).
//
// `zw_ptr_identity` is the raw `ZWTable*` pointer; null is permitted.
std::uint64_t ctx_fingerprint(const Poly& p);
std::uint64_t ctx_fingerprint(const PolyCtx& ctx);
std::uint64_t zw_pointer_identity(const ZWTable* zw_ptr);

// iter-62-β.2: wordlist payload signature for transform_shuffle.
// Iterates over the std::vector<Word>, folding each Word's pre-cached
// 128-bit structural hash (word.hpp:81 `Word::struct_hash()`) into a
// running 64-bit XXH3-seeded accumulator. The wordlist size is folded
// in as a final salt to disambiguate empty/singleton wordlists.
//
// Per §iter-59-fold-REQ-6: this signature is BYTE-ID-PRESERVED under
// the same XXH3 seed (process-wide). NOT preserved across process
// restarts — cache lifetime is process lifetime per FOLD-M4.
std::uint64_t wordlist_payload_sig(const std::vector<Word>& wordlist);

// ---------------------------------------------------------------------------
// Per-op canonical keys.
// ---------------------------------------------------------------------------

// `LfKey` — key for the §E `linear_factors` outer cache.
// FOLD-M3 BINDING: stored by value in the cache entry for full-equality
// verification on HIT. operator== is byte-equality across all 5 fields.
struct LfKey {
    std::uint64_t  poly_sig;
    std::size_t    var_idx;
    const ZWTable* zw_ptr_identity;
    bool           introduce_algebraic_letters;
    bool           compute_constant;

    bool operator==(const LfKey& other) const noexcept {
        return poly_sig                    == other.poly_sig
            && var_idx                     == other.var_idx
            && zw_ptr_identity             == other.zw_ptr_identity
            && introduce_algebraic_letters == other.introduce_algebraic_letters
            && compute_constant            == other.compute_constant;
    }
};

LfKey make_lf_key(const Poly& p, std::size_t var_idx,
                  const ZWTable* zw_ptr,
                  bool introduce_algebraic_letters,
                  bool compute_constant);

std::uint64_t hash_lf_key(const LfKey& k);

// `PfKey` — key for the §E `partial_fractions` outer cache.
struct PfKey {
    std::uint64_t  rat_sig;
    std::size_t    var_idx;
    const ZWTable* zw_ptr_identity;
    bool           introduce_algebraic_letters;

    bool operator==(const PfKey& other) const noexcept {
        return rat_sig                     == other.rat_sig
            && var_idx                     == other.var_idx
            && zw_ptr_identity             == other.zw_ptr_identity
            && introduce_algebraic_letters == other.introduce_algebraic_letters;
    }
};

PfKey make_pf_key(const Rat& f, std::size_t var_idx,
                  const ZWTable* zw_ptr,
                  bool introduce_algebraic_letters);

std::uint64_t hash_pf_key(const PfKey& k);

// `RatAddKey` — key for the §E `Rat::add` cache.
// `Rat::add` is symmetric in its operands at the value level (a+b == b+a),
// so the key lex-sorts the two operand signatures to canonicalise.
struct RatAddKey {
    std::uint64_t lhs_sig;
    std::uint64_t rhs_sig;

    bool operator==(const RatAddKey& other) const noexcept {
        return lhs_sig == other.lhs_sig && rhs_sig == other.rhs_sig;
    }
};

RatAddKey make_rat_add_key(const Rat& a, const Rat& b);

std::uint64_t hash_rat_add_key(const RatAddKey& k);

// `ReduceKey` — key for the §E `reduce_inplace` cache. The cached value is
// the pre-reduce (num, den) pair; the cached value is the post-reduce
// (num, den) pair (deep-copied).
struct ReduceKey {
    std::uint64_t num_sig;
    std::uint64_t den_sig;

    bool operator==(const ReduceKey& other) const noexcept {
        return num_sig == other.num_sig && den_sig == other.den_sig;
    }
};

ReduceKey make_reduce_key(const Poly& num, const Poly& den);

std::uint64_t hash_reduce_key(const ReduceKey& k);

// `TransformShuffleKey` — key for the §E `transform_shuffle` cache.
// iter-62-β.2 (implementation memo §3.5 + iter-59-fold-REQ-3 + REQ-5):
// 6-field canonicalisation of the transform_shuffle entry surface.
//   wordlist_sig                : Layer-W signature over std::vector<Word>
//                                  via `wordlist_payload_sig()`.
//   var_idx                     : integration variable index.
//   ctx_fp                      : Layer-3 PolyCtx fingerprint (nvars + var-name list).
//   zw_ptr_identity             : raw ZWTable* pointer (FOLD-ER3 defense-in-depth
//                                  under SCALAR_REP=0; transform_shuffle_enabled()
//                                  forces false under SCALAR_REP=1 so the cache is
//                                  bypassed entirely before this key is even built).
//   introduce_algebraic_letters : positive-letter branch flag.
//   wordlist_size               : redundant with `wordlist_sig` BUT cheap and
//                                  gives operator==() an early-out under collision.
struct TransformShuffleKey {
    std::uint64_t  wordlist_sig;
    std::size_t    var_idx;
    std::uint64_t  ctx_fp;
    const ZWTable* zw_ptr_identity;
    bool           introduce_algebraic_letters;
    std::size_t    wordlist_size;

    bool operator==(const TransformShuffleKey& other) const noexcept {
        return wordlist_sig                == other.wordlist_sig
            && var_idx                     == other.var_idx
            && ctx_fp                      == other.ctx_fp
            && zw_ptr_identity             == other.zw_ptr_identity
            && introduce_algebraic_letters == other.introduce_algebraic_letters
            && wordlist_size               == other.wordlist_size;
    }
};

TransformShuffleKey make_transform_shuffle_key(
    const std::vector<Word>& wordlist,
    std::size_t var_idx,
    const PolyCtx& ctx,
    const ZWTable* zw_ptr,
    bool introduce_algebraic_letters);

std::uint64_t hash_transform_shuffle_key(const TransformShuffleKey& k);

} // namespace canonical_signature
} // namespace hyperflint
