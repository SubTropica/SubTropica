// HF C-prep.1 (iter-22 amendment §3.2): process-global at-exit emitter
// for ZWTable accounting.
//
// Each ZWTable instance carries three per-table counters:
//   intern_regular_calls_         : non-opaque intern() invocations.
//   intern_opaque_calls_          : intern_opaque() invocations
//                                   (the §6.3 fallback bucket).
//   would_have_been_opaque_calls_ : caller-bumped count of cases where
//                                   the §6.3 predicate fires but
//                                   intern() is taken anyway. Iter-34
//                                   ships the plumbing only; the
//                                   predicate that bumps this counter
//                                   lands at iter-35 in
//                                   src/core/rat_split.cpp:97.
//
// On ZWTable destruction (or, for ZWTables transferred via move ctor,
// only the surviving "destination" table), counters are flushed into
// process-global atomics here. At process exit, a static destructor on
// `ZwAggregateAtExit` writes a single line to stderr:
//
//   hf_zw_aggregate: total_intern_regular=N total_intern_opaque=M
//   total_would_have_been_opaque=K total_distinct=D
//
// Gate scripts (notes/hf_mzv_rewrite_design_2026-05-05/scripts/
// run_phase_c_prep_2_wpm_gate.py and run_full_phase_c_prep_gate.sh)
// MUST parse this line and assert opaque_rate > 0 on at least one
// iter-25 fixture cell once the §6.3 predicate ships at iter-35.
//
// Pattern mirrors `runtime/rat_split_verify.cpp`:
//   - global atomic counters in an anonymous namespace,
//   - flush function called from ZWTable destructor,
//   - static `ZwAggregateAtExit g_zw_atexit_emitter` whose ~dtor
//     emits the line at process exit.
//
// This is a build-profile probe. The line fires unconditionally (no
// HF_* env gate) so production gate harnesses can always parse the
// output. Cost is negligible: one stderr fprintf at exit, four atomic
// fetch_adds per ZWTable destroyed.

#pragma once

#include <cstddef>
#include <cstdint>

namespace hyperflint {
namespace detail {

// Called from ZWTable::~ZWTable. Adds the four counter values to the
// process-global atomic accumulators. Safe under concurrent destroy
// (relaxed atomics are sufficient: the at-exit emitter reads after
// every thread has joined).
void zw_table_flush_to_aggregate(
    std::size_t intern_regular,
    std::size_t intern_opaque,
    std::size_t would_have_been_opaque,
    std::size_t distinct_entries);

// Test-only: read the current global aggregate without resetting it.
// The fields are populated in-place. Used by test_zw_table_basic to
// verify the move-ctor zero-out invariant deterministically.
void zw_aggregate_peek(
    std::uint64_t* intern_regular,
    std::uint64_t* intern_opaque,
    std::uint64_t* would_have_been_opaque,
    std::uint64_t* distinct_entries);

}  // namespace detail
}  // namespace hyperflint
