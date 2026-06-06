// Phase 1 (poles streaming, 2026-05-03): PolesBucket — per-thread
// regulator-bump accumulator, file-extracted from
// src/integrator/integration_step.cpp so Task 1.3's flush hook is
// unit-testable from outside the integration_step TU.
//
// History: this struct was file-local in integration_step.cpp's
// anonymous namespace until Task 1.3 added flush_threshold +
// flush_callback + drain() + a behavioural test
// (test/unit/test_poles_bucket_flush.cpp). The bump() body still
// inlines into the OMP region; the timer-storage helpers it touches
// were promoted from the anonymous namespace into hyperflint::detail
// so they're reachable from this header (declarations here,
// definitions still in integration_step.cpp).
//
// Reviewer R28 binding fixes shipped in this file:
//   C1: the threshold check fires AFTER the new entry is fully
//       appended; the flush is one atomic block (callback walks all
//       three vectors and the production callback clears them
//       atomically).
//   R1: raw function-pointer + void* user_data, NOT std::function, to
//       avoid type-erasure overhead in the hot bump() loop. When
//       flush_callback is null the threshold check is one branch +
//       one pointer compare per bump.
//
// Reviewer R28 R5 (advisory): per-thread bucket is stack-local; on a
// thrown IntegrationStepFailed the destructor runs on stack unwind.
// The default destructor is safe — it doesn't depend on
// flush_callback being non-null.

#pragma once

#include "hyperflint/runtime/hf_thread_num.hpp"

#include "hyperflint/algebra/poly_struct_hash.hpp"   // PairU64Hash
#include "hyperflint/core/symcoef.hpp"               // SymCoef
#include "hyperflint/integrator/transform.hpp"       // RegKey, RegTermSym,
                                                     // canonicalize_regkey,
                                                     // regkey_struct_hash

#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef HF_HAVE_OPENMP
#  include <omp.h>
#endif

namespace hyperflint {
namespace detail {

// Per-thread sub-timers + counters used by PolesBucket::bump.
// Storage lives in src/integrator/integration_step.cpp; declarations
// here so the inline bump() body can address them.
std::vector<double>& bucket_canon_regkey_storage();
std::vector<double>& bucket_struct_hash_storage();
std::vector<double>& bucket_index_find_storage();
std::vector<double>& bucket_symcoef_add_storage();
std::vector<double>& bucket_emplace_storage();
std::vector<long>&   bucket_collision_calls_storage();
std::vector<long>&   bucket_collision_pre_terms_storage();
std::vector<long>&   bucket_collision_post_terms_storage();

// Avenue F gate (HF_DEFER_BUMP env var). True = defer canonicalize on
// collision; false = eager += (pre-Avenue-F path).
bool defer_bump_enabled();

}  // namespace detail

struct PolesBucket {
    std::vector<RegTermSym>            terms;
    std::vector<std::vector<SymCoef>>  pending;   // pending[i] parallel to terms[i]
    std::unordered_map<std::pair<uint64_t, uint64_t>, std::size_t,
                       PairU64Hash>    index;

    // Phase 1 (B): streaming threshold. Default SIZE_MAX = no
    // streaming (legacy bit-identical behaviour). When set, bump()
    // calls flush_callback(*this, flush_user_data) AFTER the new
    // entry is fully appended if terms.size() exceeds the threshold.
    // R28 R1: raw function pointer (no std::function type erasure).
    std::size_t flush_threshold = SIZE_MAX;
    using FlushFn = void (*)(PolesBucket& self, void* user_data);
    FlushFn flush_callback  = nullptr;
    void*   flush_user_data = nullptr;

    void bump(const RegKey& key, const SymCoef& coef) {
        if (coef.is_zero()) return;
        // Phase 1 Task 1.E: hf_get_thread_num() returns omp_get_thread_num() in OMP mode;
        // under HF_USE_GCD=1 returns the GCD slot index.
        const int _bk_tid = ::hyperflint::runtime::hf_get_thread_num();
        const auto _bk_t0 = std::chrono::steady_clock::now();
        RegKey canon = canonicalize_regkey(key);
        const auto _bk_t1 = std::chrono::steady_clock::now();
        const auto k = regkey_struct_hash(canon);
        const auto _bk_t2 = std::chrono::steady_clock::now();
        auto it = index.find(k);
        const auto _bk_t3 = std::chrono::steady_clock::now();
        {
            auto& _crv = detail::bucket_canon_regkey_storage();
            if (static_cast<std::size_t>(_bk_tid) < _crv.size())
                _crv[_bk_tid] += std::chrono::duration<double>(_bk_t1 - _bk_t0).count();
            auto& _shv = detail::bucket_struct_hash_storage();
            if (static_cast<std::size_t>(_bk_tid) < _shv.size())
                _shv[_bk_tid] += std::chrono::duration<double>(_bk_t2 - _bk_t1).count();
            auto& _ifv = detail::bucket_index_find_storage();
            if (static_cast<std::size_t>(_bk_tid) < _ifv.size())
                _ifv[_bk_tid] += std::chrono::duration<double>(_bk_t3 - _bk_t2).count();
        }
        if (it == index.end()) {
            index[k] = terms.size();
            terms.push_back(RegTermSym{coef, std::move(canon)});
            // Mirror new-slot allocation in the parallel pending vector.
            pending.emplace_back();
            const auto _bk_t4 = std::chrono::steady_clock::now();
            auto& _emv = detail::bucket_emplace_storage();
            if (static_cast<std::size_t>(_bk_tid) < _emv.size())
                _emv[_bk_tid] += std::chrono::duration<double>(_bk_t4 - _bk_t3).count();
        } else if (detail::defer_bump_enabled()) {
            // Avenue F: defer canonicalize. Push raw SymCoef to the
            // per-slot pending list; collect_into_flat drains it later.
            const long _pre_n = static_cast<long>(
                terms[it->second].coef.terms().size());
            pending[it->second].push_back(coef);
            const long _post_n = _pre_n
                + static_cast<long>(coef.terms().size());  // upper bound
            const auto _bk_t4 = std::chrono::steady_clock::now();
            auto& _sav = detail::bucket_symcoef_add_storage();
            if (static_cast<std::size_t>(_bk_tid) < _sav.size())
                _sav[_bk_tid] += std::chrono::duration<double>(_bk_t4 - _bk_t3).count();
            auto& _ccv = detail::bucket_collision_calls_storage();
            if (static_cast<std::size_t>(_bk_tid) < _ccv.size())
                _ccv[_bk_tid] += 1;
            auto& _prv = detail::bucket_collision_pre_terms_storage();
            if (static_cast<std::size_t>(_bk_tid) < _prv.size())
                _prv[_bk_tid] += _pre_n;
            auto& _pov = detail::bucket_collision_post_terms_storage();
            if (static_cast<std::size_t>(_bk_tid) < _pov.size())
                _pov[_bk_tid] += _post_n;
        } else {
            // Eager fall-back (HF_DEFER_BUMP=0): pre-Avenue-F path.
            const long _pre_n = static_cast<long>(
                terms[it->second].coef.terms().size());
            terms[it->second].coef += coef;
            const long _post_n = static_cast<long>(
                terms[it->second].coef.terms().size());
            const auto _bk_t4 = std::chrono::steady_clock::now();
            auto& _sav = detail::bucket_symcoef_add_storage();
            if (static_cast<std::size_t>(_bk_tid) < _sav.size())
                _sav[_bk_tid] += std::chrono::duration<double>(_bk_t4 - _bk_t3).count();
            auto& _ccv = detail::bucket_collision_calls_storage();
            if (static_cast<std::size_t>(_bk_tid) < _ccv.size())
                _ccv[_bk_tid] += 1;
            auto& _prv = detail::bucket_collision_pre_terms_storage();
            if (static_cast<std::size_t>(_bk_tid) < _prv.size())
                _prv[_bk_tid] += _pre_n;
            auto& _pov = detail::bucket_collision_post_terms_storage();
            if (static_cast<std::size_t>(_bk_tid) < _pov.size())
                _pov[_bk_tid] += _post_n;
        }

        // R28 C1: threshold check fires AFTER the new entry is fully
        // appended. One branch + one pointer compare when the hook is
        // unwired (flush_threshold == SIZE_MAX or flush_callback ==
        // nullptr). The flush itself is one atomic block: the
        // production callback walks (terms, pending, index) and
        // clears all three.
        if (terms.size() > flush_threshold && flush_callback) {
            flush_callback(*this, flush_user_data);
        }
    }

    // Unconditional flush: invokes flush_callback unconditionally
    // when set. Caller's callback must tolerate empty input — calling
    // drain() on an empty bucket still fires the callback. Safe
    // no-op when no callback is wired. Called from host code at end
    // of OMP region to flush whatever didn't trigger threshold;
    // production callbacks (Task 1.4 ShardedFlatMap merge) are
    // empty-safe by construction.
    void drain() {
        if (flush_callback) flush_callback(*this, flush_user_data);
    }
};

}  // namespace hyperflint
