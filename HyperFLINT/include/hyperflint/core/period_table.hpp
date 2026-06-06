// PeriodTable -- period-tuples Phase 1 (spec: docs/superpowers/specs/
// 2026-06-04-hf-period-tuple-representation-design.md §2.2).
//
// Immortal process-global registry mapping canonical OPAQUE period keys
// (un-reduced G-word / Z-word / MZV-index strings, exactly today's
// placeholder semantics) to dense uint32 ids used in
// SymMonomial::period_powers. Ids are opaque: NO reduction happens here
// (reduction stays lazy at the evaluate_periods / zero-test / emission
// boundary, Phase 3). Thread-safe; ids are stable for process lifetime
// (HyperInt lesson: period lookups are permanent).

#pragma once

#include <cstdint>
#include <string>

namespace hyperflint {

class PeriodTable {
public:
    static PeriodTable& instance();

    // Intern `canonical_key` (insert if absent); returns its stable id.
    std::uint32_t id_for(const std::string& canonical_key);

    // Reverse lookup; throws std::out_of_range on unknown id. Returns by
    // VALUE: the backing vector may reallocate under a concurrent
    // id_for() (review fold A4).
    std::string key_for(std::uint32_t id) const;

    std::size_t size() const;

private:
    PeriodTable() = default;
    struct Impl;
    Impl& impl() const;
};

}  // namespace hyperflint
