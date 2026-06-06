// HF_ADDPF_PROBE -- additive-parfrac Phase 0 attribution probe
// (docs/superpowers/plans/2026-06-03-hf-additive-parfrac.md, Task 0.1).
//
// Measures, on real fixtures, the candidate saving of the
// ADDITIVE_PARFRAC lever (keep same-key prefactor contributions as
// unfused Rat sums through to partial_fractions) and the data needed
// for its ON-arm overhead estimate:
//
//   - merge-side: every key-match `m.prefactor += other.prefactor`
//     inside SymCoef::merge_sorted_canonical, split by denominator
//     equality (same-den adds skip the cross-multiply + GCD blowup;
//     diff-den adds are the class the lever would relocate to residue
//     space), with wall time and den-size watermarks;
//   - drain-side: addends-per-key histogram at the tree_merge fusion
//     site (integration_step.cpp post-OMP merge) plus fused-numerator
//     size watermark;
//   - PF-side: unconditional partial_fractions call counter and the
//     fraction of calls with a nonzero polynomial part (D6 eager
//     poly-part fusion re-examination data).
//
// All counters are process-global atomics (the probe is default-OFF;
// contention is irrelevant on an attribution run). One JSONL line per
// integration step is emitted to stderr from integration_step's
// epilogue and the counters reset.
//
// Env gate: HF_ADDPF_PROBE=1 (unset => OFF, zero work on the hot path
// beyond one branch on a function-local static).

#pragma once

#include <cstddef>

namespace hyperflint {
namespace addpf_probe {

// True iff HF_ADDPF_PROBE=1 (first char '1'); read once per process.
bool enabled();

// Key-match prefactor add inside merge_sorted_canonical.
void record_merge_add(bool same_den, double seconds,
                      std::size_t den_a_terms, std::size_t den_b_terms,
                      std::size_t fused_den_terms);

// Per-key drain at the post-OMP tree_merge site.
void record_drain_key(std::size_t n_chunks,
                      std::size_t max_fused_num_terms);

// One partial_fractions call (public entry); nonzero_poly_part is
// whether the returned decomposition has a nonzero polynomial part.
void record_pf_call(bool nonzero_poly_part);

// --- bump-layer extension (2026-06-04, group-by-denominator probe) ---
// The integrate_ii row accumulator (`rows[i].coef += c` in
// primitive.cpp's bump lambda) carries ~3x the add cost of the
// merge layer on tst2 (56 vs 19 CPU-s). These recorders measure the
// statistic that decides the group-by-denominator lever: how often an
// incoming contribution's denominator equals the row's current
// denominator (same-den += pays only the post-add canonicalization),
// and how many DISTINCT incoming denominators a row sees over its
// lifetime (the group count a grouped accumulator would maintain).

// One row `+=`: same_den is (incoming c.den() == row coef.den()).
void record_bump_add(bool same_den, double seconds);

// One finished row at the integrate_ii drain: n_groups = number of
// distinct incoming denominator hashes (capped; capped=true if the
// cap was hit and n_groups is a lower bound).
void record_bump_row(std::size_t n_groups, bool capped);

// Period-density census (period-tuples Phase 0, Task 0.2): set the
// kinematic-prefix length once per request; record one monomial whose
// prefactor uses (or not) any ctx var with index >= that prefix.
void set_user_var_count(std::size_t n);
std::size_t user_var_count();
void record_monomial(bool uses_period_var);

// Emit one JSONL summary line to stderr and reset all counters.
// var_name: the integration variable of the step just finished.
void emit_and_reset(const char* var_name);

}  // namespace addpf_probe
}  // namespace hyperflint
