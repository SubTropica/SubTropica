// operand_sparsity — env-gated, sample-rate-limited per-monomial
// sparsity probe for `Rat::add` operands.  Phase 2-pre.1 (2026-05-03)
// of the rep-swap campaign (chain-18 surface option B).
//
// Purpose. Phase 2-A's sparse-per-monomial Rat representation depends
// on HF's typical k/N ratio (where k = # of nonzero exponent slots
// per monomial, N = ctx.vars().size() ≈ 718 on parity-1).  At
// k/N ≪ 1 the rep swap saves 7×+ on per-term storage; at k/N ≳ 0.3
// the swap loses to FLINT's bit-packed exponents.  This probe
// emits per-call PolyStats so the campaign can compute median and
// distribution of k across {a.num, a.den, b.num, b.den} from a
// real production hot path.
//
// Wire-up.  At the entry of `Rat::add(const Rat& b) const`:
//   if (sparsity_probe_enabled() && sparsity_probe_should_emit())
//       emit_sparsity_row(*this, b);
// Default-off (HF_DUMP_OPERAND_SPARSITY unset or =0): the gate is a
// single static-local-cached load + branch (predicted-not-taken),
// zero atomic increment, zero perturbation to production wall.
//
// Output.  One JSONL row per emit, written to stderr under a mutex
// (mirrors HF_REGKEY_DUMP at integration_step.cpp ~2076).  Format
// documented in `emit_sparsity_row` below.
//
// Sampling.  At HF_OPERAND_SPARSITY_RATE=N (default 1000), every
// N-th call to should_emit returns true (counter %  rate == 0).
// At rate=1000, parity-1's ~1.9 M Rat::add calls/integration emit
// ~1900 rows.

#pragma once

#include <cstddef>
#include <cstdint>

namespace hyperflint {

class Poly;
class Rat;

// Cached env-gate. First call reads `HF_DUMP_OPERAND_SPARSITY`;
// returns true iff the env var is set and not "0". Static-local
// once-init guarantees thread-safe single read.
bool sparsity_probe_enabled();

// Cached sample rate. First call reads `HF_OPERAND_SPARSITY_RATE`;
// returns the parsed positive integer (default 1000). Static-local
// once-init.
size_t sparsity_probe_sample_rate();

// Increment a process-global atomic counter and return true iff
// the new counter value is divisible by `sparsity_probe_sample_rate()`.
// Atomic; safe to call from any thread.
bool sparsity_probe_should_emit();

// Per-Poly sparsity statistics over the polynomial's monomial
// exponent vectors. `k` for a single term is the count of nonzero
// entries in its exponent slot vector (length n_vars).
//
// Convention for an empty polynomial (n_terms=0): all fields = 0
// (k_avg defined as 0.0, not NaN, so JSON serialization is robust).
struct PolyStats {
    size_t n_terms;
    size_t k_min;
    size_t k_max;
    double k_avg;       // mean across all terms, 0.0 if n_terms == 0
};

// Walk every term of `p` via `fmpq_mpoly_get_term_exp_si`, popcount
// the nonzero exponent entries per term, and aggregate min/avg/max.
// Pure read; safe to call concurrently on disjoint Polys.  O(n_terms
// · n_vars) — n_vars per term is unpacked into a slong[n_vars]
// scratch buffer, scanned once.  This is the same complexity class
// as `Poly::used_var_indices`'s slow path (which today uses FLINT's
// faster `fmpq_mpoly_used_vars`); we don't use the faster path here
// because we need per-term granularity, not just the union over
// terms.
PolyStats compute_sparsity(const Poly& p);

// Emit one JSONL row to stderr.  Format (single line, no spaces
// between key/value pairs):
//   {"hf_sparsity":true,"call":<N>,"n_vars":<NV>,
//    "a_n_terms":<...>,"a_n_k_min":<...>,"a_n_k_avg":<...>,
//    "a_n_k_max":<...>,
//    "a_d_n_terms":<...>,"a_d_k_min":<...>,"a_d_k_avg":<...>,
//    "a_d_k_max":<...>,
//    "b_n_terms":<...>,"b_n_k_min":<...>,"b_n_k_avg":<...>,
//    "b_n_k_max":<...>,
//    "b_d_n_terms":<...>,"b_d_k_min":<...>,"b_d_k_avg":<...>,
//    "b_d_k_max":<...>}
// where the `call` field is the post-increment counter value
// reported by `should_emit` (always positive), `n_vars` is
// `a.num().ctx().vars().size()`, and the four operand prefixes
// {a_n, a_d, b_n, b_d} stand for (a.num, a.den, b.num, b.den).
//
// Thread-safe via an internal `std::mutex`; concurrent emits from
// the OMP region serialize cleanly (the format string is short, so
// the mutex is held for sub-microsecond intervals; at default
// sample rate=1000 the overall production-overhead is < 1 %).
void emit_sparsity_row(const Rat& a, const Rat& b);

}  // namespace hyperflint
