// ZWTable: shared symbolic side-table for Mandelstam / mass / Wm/Wp
// dependence under the HF MZV-as-scalar rewrite.
//
// Phase-A commit (1) introduces the ZWTable type per design v2 §3.3
// (notes/hf_mzv_rewrite_design_2026-05-05/design.md). The class is
// declared here as a stub: ctor, sentinels, public-API surface. The
// real arithmetic (intern, get, multiply, add, merge_into,
// intern_opaque) is implemented in commit (4) of Phase A.
//
// Until commit (4) lands, the bodies in zw_table.cpp throw
// `std::logic_error` when called. Phase-B hot-path call sites do not
// reach them yet — the round-trip verifier wired in commit (3) flips
// the `HF_RAT_SPLIT_VERIFY` check off whenever the new code path is
// not yet wired, so production traffic continues to use the wide-ctx
// Rat representation.

#pragma once

#include "hyperflint/core/poly.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hyperflint {

// Opaque handle into a ZWTable. uint32 with a top-bit-set encoding
// for `intern_opaque` entries (§6.3 fallback path; the (num, den) Rat
// pair is parked in `opaque_entries_` instead of `entries_`).
//
// Reserved values:
//   ZW_ONE      = 0          : the polynomial 1 (no W dependence).
//   ZW_ZERO     = 0xFFFFFFFF : the polynomial 0 (used by negate /
//                              add when the result vanishes).
//   ZW_OPAQUE_BIT = 0x80000000 (top bit set) : opaque-bucket slot.
//
// Handle-space arithmetic limit:
//   non-opaque ∈ [0, 2^31)  — 2 147 483 647 entries
//   opaque     ∈ [2^31, 2^32) (with bit 31 set)
// Smirnov tst3 exercises ~10^4-10^5 distinct entries; we are safe by
// four orders of magnitude. On overflow, `intern` aborts (HF_DIE).
using ZWHandle = uint32_t;
constexpr ZWHandle ZW_ONE       = 0u;
constexpr ZWHandle ZW_ZERO      = 0xFFFFFFFFu;
constexpr uint32_t ZW_OPAQUE_BIT = 0x80000000u;

// 128-bit content key (FNV-1a output pair) used for ZWTable dedup.
// Same primitive as `poly_struct_hash_raw` in algebra/poly_struct_hash.hpp.
using U128 = std::pair<uint64_t, uint64_t>;

class ZWTable {
public:
    // Intent tag for `intern`: tells the table whether this entry is
    // a numerator W-monomial (sign-as-built; sign is part of the
    // content hash) or a denominator (sign-canonicalize before hash;
    // caller must propagate the introduced sign into `num_N` if a
    // negation occurred).
    enum class Intent {
        NumIntent,
        DenIntent
    };

    // Construct with a fixed wide ctx F = N_0 ⊕ MZV_basis ⊕
    // Mandelstam ⊕ mass ⊕ Wm/Wp pool (§3.3a). All entries — both
    // numerator W-monomials and full wide-ctx denominators — live
    // over this single ctx; no per-entry ctx flag is carried.
    explicit ZWTable(const PolyCtx& F);

    // Iter-34 (C-prep.1 plumbing): explicit destructor flushes the
    // per-table counters into the process-global atomics declared in
    // `runtime/zw_aggregate.cpp`; the static `ZwAggregateAtExit`
    // dtor there emits the `hf_zw_aggregate:` line at process exit.
    // Move ctor / move-assignment zero out the moved-from object's
    // counters so that the moved-from destructor flushes zeroes
    // (preventing double-counting).
    ~ZWTable();

    ZWTable(const ZWTable&) = delete;
    ZWTable& operator=(const ZWTable&) = delete;
    ZWTable(ZWTable&& other) noexcept;
    ZWTable& operator=(ZWTable&& other) noexcept;

    const PolyCtx& ctx() const { return *F_; }

    // Intern a wide-ctx polynomial. Dedups by 128-bit content hash.
    // `intent` controls sign canonicalization (see `Intent`).
    //
    // Phase-A stub: definition lives in zw_table.cpp; throws
    // `std::logic_error` until commit (4) lands.
    ZWHandle intern(Poly p, Intent intent);

    // Lookup. Returns a const-ref into the dense `entries_` vector.
    const Poly& get(ZWHandle h) const;

    // Memoized arithmetic in F. Multiplication / addition / negation
    // results are cached on `(a, b)` sorted-lex / `a` respectively.
    ZWHandle multiply(ZWHandle a, ZWHandle b);
    ZWHandle add(ZWHandle a, ZWHandle b);
    ZWHandle negate(ZWHandle a);

    // Bookkeeping: distinct entries; cumulative intern calls (==
    // count of non-deduped intern calls + dedup hits combined; the
    // ratio `size() / intern_calls()` is the §6.1 dedup hypothesis
    // probe).
    //
    // Iter-33 (C-prep.1): the legacy `intern_calls()` returns the
    // SUM of regular and opaque-bucket calls so existing diagnostic
    // output is preserved. Three disjoint per-table counters are
    // exposed for the iter-22 amendment §3.2 wiring:
    //
    //   - `intern_regular_calls()` — non-opaque `intern()` invocations.
    //   - `intern_opaque_calls()`  — `intern_opaque()` invocations
    //                                (the §6.3 fallback bucket).
    //   - `would_have_been_opaque_calls()` — caller-bumped count of
    //                                cases where the §6.3 predicate
    //                                fires but `intern()` is taken
    //                                anyway for performance reasons;
    //                                used by `split_rat_by_w_monomial`.
    //
    // **Iter-33 scope**: per-table getters only. Process-global
    // accumulation + the `hf_zw_aggregate:` at-exit line (mirroring
    // the `hf_rat_split_verify:` pattern in
    // `runtime/rat_split_verify.cpp`) ship at iter-34 alongside the
    // §6.3 predicate wiring at `split_rat_by_w_monomial`.
    size_t size() const { return entries_.size(); }
    size_t intern_calls() const {
        return intern_regular_calls_ + intern_opaque_calls_;
    }
    size_t intern_regular_calls() const { return intern_regular_calls_; }
    size_t intern_opaque_calls() const { return intern_opaque_calls_; }
    size_t would_have_been_opaque_calls() const {
        return would_have_been_opaque_calls_;
    }

    // Caller-side hook: increment `would_have_been_opaque_calls_` when
    // the §6.3 fallback predicate fires but `intern()` is taken instead
    // of `intern_opaque()`. Wired into `split_rat_by_w_monomial` once
    // the predicate lands (iter-34); a no-op until then.
    void bump_would_have_been_opaque(size_t n = 1) {
        would_have_been_opaque_calls_ += n;
    }

    // Build-profile only: sum of poly bytes across entries, including
    // opaque (num + den). Default-OFF env-gated callers only.
    size_t total_bytes() const;

    // §3.6a cross-merge: merge `secondary` into *this. Returns the
    // remap `secondary_handle -> primary_handle` table built during
    // merge. Iteration is canonical-content-keyed (FNV-1a over the
    // entry's canonical fmpq_mpoly bytes, with lexicographic tiebreak
    // on `to_string`); the result is OMP-schedule-deterministic.
    std::unordered_map<ZWHandle, ZWHandle>
        merge_into(const ZWTable& secondary);

    // Opaque-entry storage (§6.3 fallback). Opaque entries hold a
    // wide-ctx (num, den) Rat pair, NOT a single Poly. They are
    // emitted only when a `from_rat_split` denominator does NOT
    // factor cleanly per §3.6 AND the numerator-by-W-monomial split
    // would be more expensive than just keeping the original Rat
    // intact. Always counted by the `zw_opaque_calls` build-profile
    // probe.
    struct OpaqueEntry { Poly num_F; Poly den_F; };
    ZWHandle intern_opaque(Poly num, Poly den);
    const OpaqueEntry& get_opaque(ZWHandle h) const;
    static bool is_opaque(ZWHandle h) {
        return (h & ZW_OPAQUE_BIT) != 0;
    }

private:
    const PolyCtx* F_ = nullptr;

    // Dense storage of distinct interned polynomials. Index = handle
    // (for non-opaque entries; opaque ones live in `opaque_entries_`).
    std::vector<Poly>          entries_;
    std::vector<OpaqueEntry>   opaque_entries_;

    // Hash-content -> handle, separate maps for non-opaque vs. opaque
    // (their content shapes differ — single Poly vs. Rat pair).
    struct U128Hash {
        size_t operator()(const U128& p) const noexcept {
            return static_cast<size_t>(
                p.first ^ ((p.second << 31) | (p.second >> 33)));
        }
    };
    std::unordered_map<U128, ZWHandle, U128Hash>  by_hash_;
    std::unordered_map<U128, ZWHandle, U128Hash>  opaque_by_hash_;

    // Op caches keyed by sorted-lex handle pairs.
    std::unordered_map<uint64_t, ZWHandle> mul_cache_;
    std::unordered_map<uint64_t, ZWHandle> add_cache_;

    // Iter-33 C-prep.1: split counters per iter-22 amendment §3.2.
    // Per-table (non-atomic) since each ZWTable is owned by a single
    // execution context. The destructor accumulates these plus
    // `entries_.size()` into process-global atomics (see zw_table.cpp).
    size_t intern_regular_calls_       = 0;
    size_t intern_opaque_calls_        = 0;
    size_t would_have_been_opaque_calls_ = 0;

    // Lazy-built zero polynomial returned by `get(ZW_ZERO)`. Owned
    // by the ZWTable so its lifetime tracks the F_ pointer's. The
    // pointer is mutable because `get` is logically `const` (the
    // cache is not part of the table's observable state).
    mutable std::unique_ptr<Poly> zero_poly_holder_;
};

}  // namespace hyperflint
