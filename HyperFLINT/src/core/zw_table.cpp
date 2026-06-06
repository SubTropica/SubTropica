// ZWTable: phase-A implementation.
//
// Commit (2) landed `intern`, `get`, `total_bytes`. Commit (4) lands
// the remaining ops: `multiply`, `add`, `negate`, `merge_into`,
// `intern_opaque`, `get_opaque`. All operate over the wide ctx F;
// `multiply`/`add` dedup their result handles by content hash and
// memoize on the (sorted) handle pair.

#include "hyperflint/core/zw_table.hpp"

#include "hyperflint/algebra/poly_struct_hash.hpp"
#include "hyperflint/runtime/zw_aggregate.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace hyperflint {

namespace {

// 128-bit content hash for a wide-ctx Poly. Reuses the existing
// `poly_struct_hash_raw` primitive that the LR cache uses.
U128 hash_poly_content(const Poly& p) {
    auto seed = poly_struct_hash_seed();
    uint64_t h1 = seed.first;
    uint64_t h2 = seed.second;
    poly_struct_hash_raw(h1, h2, p);
    return {h1, h2};
}

// 128-bit content hash for an opaque (num, den) pair.
U128 hash_opaque_content(const Poly& num, const Poly& den) {
    auto seed = poly_struct_hash_seed();
    uint64_t h1 = seed.first;
    uint64_t h2 = seed.second;
    poly_struct_hash_raw(h1, h2, num);
    // Distinct sentinel between num and den so (n, d) pairs don't
    // alias (n', d') with the same concatenated limb stream.
    poly_struct_hash_mix(h1, h2, 0xfffffffffffffffdULL);
    poly_struct_hash_raw(h1, h2, den);
    return {h1, h2};
}

// Pack a sorted ZWHandle pair into a u64 cache key. Commutative ops
// (multiply, add) sort first so `multiply(a,b) == multiply(b,a)`
// hits the same cache slot.
uint64_t pair_key_sorted(ZWHandle a, ZWHandle b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
}

}  // namespace

ZWTable::ZWTable(const PolyCtx& F) : F_(&F) {
    entries_.reserve(64);
    by_hash_.reserve(64);
    // ZW_ONE = 0 holds the polynomial 1.
    entries_.push_back(Poly::one_of(*F_));
    U128 one_hash = hash_poly_content(entries_.front());
    by_hash_.emplace(one_hash, ZW_ONE);
    intern_regular_calls_         = 0;
    intern_opaque_calls_          = 0;
    would_have_been_opaque_calls_ = 0;
}

// Iter-34 (C-prep.1 plumbing): destructor flushes per-table counters
// into the process-global atomics declared in
// `runtime/zw_aggregate.cpp`. The `entries_.size()` flushed as
// `distinct_entries` is the size of the non-opaque dense table; opaque
// entries have their own per-call counter (intern_opaque_calls_)
// already aggregated.
//
// A moved-from object has all three counters zero AND F_ == nullptr
// (see explicit move ctor / move-assignment below); flushing zeros is
// a no-op on the global accumulators (since fetch_add is the operation
// and the addends are zero). We still call `flush_to_aggregate` from
// the moved-from path to keep the destruction discipline uniform.
ZWTable::~ZWTable() {
    detail::zw_table_flush_to_aggregate(
        intern_regular_calls_,
        intern_opaque_calls_,
        would_have_been_opaque_calls_,
        entries_.size());
}

// Explicit move ctor: transfer all members, then zero the moved-from
// object's counters AND clear `entries_`. Without this zero-out the
// moved-from destructor would re-flush the now-already-flushed counts
// to the globals at scope exit, double-counting. (`std::vector` move
// ctor leaves the source empty, so `entries_.size()` is naturally
// zero post-move; we still explicitly assign for clarity.)
ZWTable::ZWTable(ZWTable&& other) noexcept
    : F_(other.F_),
      entries_(std::move(other.entries_)),
      opaque_entries_(std::move(other.opaque_entries_)),
      by_hash_(std::move(other.by_hash_)),
      opaque_by_hash_(std::move(other.opaque_by_hash_)),
      mul_cache_(std::move(other.mul_cache_)),
      add_cache_(std::move(other.add_cache_)),
      intern_regular_calls_(other.intern_regular_calls_),
      intern_opaque_calls_(other.intern_opaque_calls_),
      would_have_been_opaque_calls_(other.would_have_been_opaque_calls_),
      zero_poly_holder_(std::move(other.zero_poly_holder_)) {
    other.F_                            = nullptr;
    other.intern_regular_calls_         = 0;
    other.intern_opaque_calls_          = 0;
    other.would_have_been_opaque_calls_ = 0;
    // The moved-from `entries_` vector is left empty by std::vector's
    // own move ctor, so a subsequent flush at moved-from destruction
    // contributes 0 to total_distinct.
}

ZWTable& ZWTable::operator=(ZWTable&& other) noexcept {
    if (this == &other) return *this;
    // Flush *this's existing counters before overwriting with
    // moved-from values; otherwise our pre-move counts would be lost
    // (the destructor we are short-circuiting wouldn't fire on *this).
    detail::zw_table_flush_to_aggregate(
        intern_regular_calls_,
        intern_opaque_calls_,
        would_have_been_opaque_calls_,
        entries_.size());

    F_                            = other.F_;
    entries_                      = std::move(other.entries_);
    opaque_entries_               = std::move(other.opaque_entries_);
    by_hash_                      = std::move(other.by_hash_);
    opaque_by_hash_               = std::move(other.opaque_by_hash_);
    mul_cache_                    = std::move(other.mul_cache_);
    add_cache_                    = std::move(other.add_cache_);
    intern_regular_calls_         = other.intern_regular_calls_;
    intern_opaque_calls_          = other.intern_opaque_calls_;
    would_have_been_opaque_calls_ = other.would_have_been_opaque_calls_;
    zero_poly_holder_             = std::move(other.zero_poly_holder_);

    other.F_                            = nullptr;
    other.intern_regular_calls_         = 0;
    other.intern_opaque_calls_          = 0;
    other.would_have_been_opaque_calls_ = 0;
    return *this;
}

ZWHandle ZWTable::intern(Poly p, Intent /*intent*/) {
    // Iter-33 (C-prep.1): bump the regular-intern counter only.
    // `intern_calls()` returns regular + opaque at the public API.
    intern_regular_calls_++;
    // Phase-A scope: Intent only documents caller, no behavior diff.
    // Sign-canonicalization is the caller's responsibility.

    U128 key = hash_poly_content(p);
    auto it = by_hash_.find(key);
    if (it != by_hash_.end()) {
        return it->second;
    }

    if (entries_.size() >= 0x80000000u) {
        throw std::runtime_error(
            "ZWTable::intern: non-opaque handle space exhausted "
            "(2^31 distinct entries); raise HF_DIE.");
    }
    ZWHandle h = static_cast<ZWHandle>(entries_.size());
    entries_.push_back(std::move(p));
    by_hash_.emplace(key, h);
    return h;
}

const Poly& ZWTable::get(ZWHandle h) const {
    if (h == ZW_ZERO) {
        // ZW_ZERO is a sentinel handle (0xFFFFFFFF), not an index, so
        // it has no in-band entry. Per-ZWTable lazy zero-Poly storage:
        // we cache one Poly inside the table itself (mutable) and
        // construct it on first access. This avoids the
        // pointer-keyed thread_local cache (reviewer round 3 finding:
        // a borrowed PolyCtx* can be re-used across ctor/dtor cycles
        // by the allocator, leaving the cached Poly stale and tied
        // to the WRONG ctx). The lifetime of `zero_poly_holder_` is
        // bounded by the ZWTable's own lifetime, which itself bounds
        // the F_ pointer.
        if (!zero_poly_holder_) {
            zero_poly_holder_.reset(new Poly(Poly::zero_of(*F_)));
        }
        return *zero_poly_holder_;
    }
    if ((h & ZW_OPAQUE_BIT) != 0) {
        throw std::logic_error(
            "ZWTable::get: handle is opaque; use get_opaque instead.");
    }
    if (h >= entries_.size()) {
        throw std::out_of_range(
            "ZWTable::get: handle out of range (" +
            std::to_string(h) + " >= " +
            std::to_string(entries_.size()) + ").");
    }
    return entries_[h];
}

ZWHandle ZWTable::multiply(ZWHandle a, ZWHandle b) {
    // Algebraic shortcuts: x * 0 = 0, 1 * x = x.
    if (a == ZW_ZERO || b == ZW_ZERO) return ZW_ZERO;
    if (a == ZW_ONE)  return b;
    if (b == ZW_ONE)  return a;
    if ((a & ZW_OPAQUE_BIT) || (b & ZW_OPAQUE_BIT)) {
        throw std::logic_error(
            "ZWTable::multiply: opaque-handle arithmetic not "
            "supported in Phase-A scope.");
    }

    uint64_t key = pair_key_sorted(a, b);
    auto it = mul_cache_.find(key);
    if (it != mul_cache_.end()) {
        return it->second;
    }

    Poly product = entries_[a].mul(entries_[b]);
    ZWHandle h = intern(std::move(product), Intent::NumIntent);
    mul_cache_.emplace(key, h);
    return h;
}

ZWHandle ZWTable::add(ZWHandle a, ZWHandle b) {
    if (a == ZW_ZERO) return b;
    if (b == ZW_ZERO) return a;
    if ((a & ZW_OPAQUE_BIT) || (b & ZW_OPAQUE_BIT)) {
        throw std::logic_error(
            "ZWTable::add: opaque-handle arithmetic not supported "
            "in Phase-A scope.");
    }

    uint64_t key = pair_key_sorted(a, b);
    auto it = add_cache_.find(key);
    if (it != add_cache_.end()) {
        return it->second;
    }

    Poly sum = entries_[a].add(entries_[b]);
    if (sum.is_zero()) {
        add_cache_.emplace(key, ZW_ZERO);
        return ZW_ZERO;
    }
    ZWHandle h = intern(std::move(sum), Intent::NumIntent);
    add_cache_.emplace(key, h);
    return h;
}

ZWHandle ZWTable::negate(ZWHandle a) {
    if (a == ZW_ZERO) return ZW_ZERO;
    if ((a & ZW_OPAQUE_BIT) != 0) {
        throw std::logic_error(
            "ZWTable::negate: opaque-handle arithmetic not "
            "supported in Phase-A scope.");
    }
    Poly neg = entries_[a].neg();
    return intern(std::move(neg), Intent::NumIntent);
}

size_t ZWTable::total_bytes() const {
    size_t bytes = 0;
    for (const auto& p : entries_) {
        bytes += p.total_bytes();
    }
    for (const auto& e : opaque_entries_) {
        bytes += e.num_F.total_bytes() + e.den_F.total_bytes();
    }
    return bytes;
}

std::unordered_map<ZWHandle, ZWHandle>
ZWTable::merge_into(const ZWTable& secondary) {
    if (F_ != secondary.F_) {
        throw std::runtime_error(
            "ZWTable::merge_into: secondary's wide ctx must match "
            "the primary's; cross-ctx merge is unsupported.");
    }

    // Build a deterministic iteration order over `secondary.entries_`
    // by sorting on (content-hash, canonical-content tiebreak). This
    // is the §3.6a cross-merge protocol's "canonical-content order";
    // running over it makes the resulting merge byte-identical
    // regardless of the secondary's intern sequence. Tiebreak walks
    // the same fmpq_mpoly content as the FNV-1a hash via
    // `poly_struct_compare`, avoiding the per-comparison
    // `fmpq_mpoly_get_str_pretty` allocation that the prior
    // `to_string()` lex compare paid on every OMP post-merge call.
    struct OrderEntry {
        ZWHandle src_h;
        U128     content_hash;
    };
    std::vector<OrderEntry> sorted_order;
    sorted_order.reserve(secondary.entries_.size());
    for (size_t i = 0; i < secondary.entries_.size(); ++i) {
        // Skip ZW_ONE — it's already in the primary at index 0 by
        // construction (ctor pre-populates ZW_ONE).
        if (i == ZW_ONE) continue;
        sorted_order.push_back({
            static_cast<ZWHandle>(i),
            hash_poly_content(secondary.entries_[i])
        });
    }
    std::sort(sorted_order.begin(), sorted_order.end(),
              [&](const OrderEntry& a, const OrderEntry& b) {
                  if (a.content_hash != b.content_hash)
                      return a.content_hash < b.content_hash;
                  return poly_struct_compare(
                             secondary.entries_[a.src_h],
                             secondary.entries_[b.src_h]) < 0;
              });

    std::unordered_map<ZWHandle, ZWHandle> remap;
    remap.reserve(secondary.entries_.size() + 1);
    // ZW_ONE is the same handle on both tables.
    remap.emplace(ZW_ONE, ZW_ONE);

    for (const auto& oe : sorted_order) {
        ZWHandle dst_h = intern(Poly(secondary.entries_[oe.src_h]),
                                Intent::NumIntent);
        remap.emplace(oe.src_h, dst_h);
    }

    // Opaque entries: same protocol, separate iteration.
    struct OpaqueOrder {
        ZWHandle src_h;
        U128     content_hash;
    };
    std::vector<OpaqueOrder> opaque_order;
    opaque_order.reserve(secondary.opaque_entries_.size());
    for (size_t i = 0; i < secondary.opaque_entries_.size(); ++i) {
        const auto& e = secondary.opaque_entries_[i];
        opaque_order.push_back({
            static_cast<ZWHandle>(ZW_OPAQUE_BIT | i),
            hash_opaque_content(e.num_F, e.den_F)
        });
    }
    std::sort(opaque_order.begin(), opaque_order.end(),
              [&](const OpaqueOrder& a, const OpaqueOrder& b) {
                  if (a.content_hash != b.content_hash)
                      return a.content_hash < b.content_hash;
                  uint32_t ai = a.src_h & ~ZW_OPAQUE_BIT;
                  uint32_t bi = b.src_h & ~ZW_OPAQUE_BIT;
                  const auto& ea = secondary.opaque_entries_[ai];
                  const auto& eb = secondary.opaque_entries_[bi];
                  // Tiebreak via canonical-content compare: numerator
                  // first, denominator on equal numerator. Replaces
                  // `(num/den).to_string() + "/" + ...` lex compare.
                  // Total order is preserved (lexicographic over the
                  // (num, den) pair) but the within-num order is no
                  // longer character-level lex of the serialised form;
                  // it is now structural. The §3.6a contract requires
                  // determinism, not parity with the prior order, so
                  // this is a safe replacement: only the rare hash
                  // collision (probability ~ 2^-128 for the 128-bit
                  // FNV-1a content key) ever observes the difference,
                  // and merge_into is a Phase A stub that is not yet
                  // reached by production traffic per design §3.3.
                  int nc = poly_struct_compare(ea.num_F, eb.num_F);
                  if (nc != 0) return nc < 0;
                  return poly_struct_compare(ea.den_F, eb.den_F) < 0;
              });

    for (const auto& oo : opaque_order) {
        uint32_t i = oo.src_h & ~ZW_OPAQUE_BIT;
        const auto& e = secondary.opaque_entries_[i];
        ZWHandle dst_h = intern_opaque(Poly(e.num_F), Poly(e.den_F));
        remap.emplace(oo.src_h, dst_h);
    }

    return remap;
}

ZWHandle ZWTable::intern_opaque(Poly num, Poly den) {
    // Iter-33 (C-prep.1): bump the §6.3-fallback counter only.
    // `intern_calls()` returns regular + opaque at the public API.
    intern_opaque_calls_++;
    U128 key = hash_opaque_content(num, den);
    auto it = opaque_by_hash_.find(key);
    if (it != opaque_by_hash_.end()) {
        return it->second;
    }
    if (opaque_entries_.size() >= 0x80000000u) {
        throw std::runtime_error(
            "ZWTable::intern_opaque: opaque handle space exhausted "
            "(2^31 distinct entries); raise HF_DIE.");
    }
    ZWHandle raw_idx = static_cast<ZWHandle>(opaque_entries_.size());
    opaque_entries_.push_back(OpaqueEntry{std::move(num), std::move(den)});
    ZWHandle h = static_cast<ZWHandle>(ZW_OPAQUE_BIT | raw_idx);
    opaque_by_hash_.emplace(key, h);
    return h;
}

const ZWTable::OpaqueEntry& ZWTable::get_opaque(ZWHandle h) const {
    if ((h & ZW_OPAQUE_BIT) == 0) {
        throw std::logic_error(
            "ZWTable::get_opaque: handle is non-opaque; use get instead.");
    }
    uint32_t i = h & ~ZW_OPAQUE_BIT;
    if (i >= opaque_entries_.size()) {
        throw std::out_of_range(
            "ZWTable::get_opaque: handle out of range (" +
            std::to_string(i) + " >= " +
            std::to_string(opaque_entries_.size()) + ").");
    }
    return opaque_entries_[i];
}

}  // namespace hyperflint
