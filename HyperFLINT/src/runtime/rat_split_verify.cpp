// HF_RAT_SPLIT_VERIFY harness — Phase-A commit (3).

#include "hyperflint/runtime/rat_split_verify.hpp"

#include "hyperflint/core/rat.hpp"
#include "hyperflint/core/rat_split.hpp"
#include "hyperflint/core/zw_table.hpp"
#include "hyperflint/integrator/transform.hpp"
#include "hyperflint/runtime/env_flags.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace hyperflint {

namespace {

// One-time cache of HF_RAT_SPLIT_VERIFY.
std::atomic<int> g_verify_state{-1};  // -1 = unread, 0 = off, 1 = on

// Proof-of-coverage counters (drift-check item 1, iter 4 -> iter 5).
// The verifier emits stderr only on FAIL, so empty stderr is consistent
// with both "ran clean" and "silently skipped". A future Phase B commit
// could `#ifdef`-out a call site or refactor it past an early return and
// the gate would silently clear. These atomic counters record:
//   - g_calls: top-level entries to verify_regulator_sym_rat_split with
//     the env gate ON (i.e. calls that did real work).
//   - g_skips_ctx: per-term skips taken at the ctx-mismatch early-out
//     (rat_split_verify.cpp original line 111). Surfaces silent
//     coverage holes in the SymCoef ctx invariants.
//   - g_terms_checked: number of (RegTermSym, SymCoef-monomial) pairs
//     that actually executed the round-trip + bit-compare. The right
//     granularity for proof-of-coverage: g_calls > 0 alone does not
//     prove a Rat was round-tripped if every term took the ctx-mismatch
//     branch.
// Emitted at process exit via a static destructor on the gate-line
// `hf_rat_split_verify: enabled=N calls=N skips_ctx=N terms_checked=N`.
// Gate scripts MUST parse this line and assert terms_checked > 0 when
// HF_RAT_SPLIT_VERIFY=1.
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_skips_ctx{0};
std::atomic<uint64_t> g_terms_checked{0};

bool read_env_once() {
    int s = g_verify_state.load(std::memory_order_relaxed);
    if (s >= 0) return s == 1;
    const char* v = HF_FLAG_RAT_SPLIT_VERIFY;
    bool on = (v != nullptr) && v[0] != '\0' && v[0] != '0';
    g_verify_state.store(on ? 1 : 0, std::memory_order_relaxed);
    return on;
}

struct RatSplitVerifyAtExit {
    ~RatSplitVerifyAtExit() {
        // Force-read the env at exit so `enabled` reflects the
        // process-start env-var state regardless of whether the
        // integrator hot path ran (a `version`/`help` invocation
        // would otherwise leave g_verify_state at -1 and report
        // `enabled=0` even with HF_RAT_SPLIT_VERIFY=1).
        const bool enabled = read_env_once();
        std::fprintf(stderr,
            "hf_rat_split_verify: enabled=%d calls=%llu skips_ctx=%llu terms_checked=%llu\n",
            enabled ? 1 : 0,
            static_cast<unsigned long long>(g_calls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_skips_ctx.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_terms_checked.load(std::memory_order_relaxed)));
        std::fflush(stderr);
    }
};
RatSplitVerifyAtExit g_atexit_emitter;

// Heuristic identification of MZV-basis variable names. The transitive
// closure over the reduction table is more accurate, but for the
// Phase-A verifier it suffices to match the canonical names that
// `build_mzv_var_list` adds to the user-vars list: `Log2` and any
// identifier matching `mzv_*`.
bool is_mzv_basis_name(const std::string& v) {
    if (v == "Log2") return true;
    if (v.size() >= 4 && std::strncmp(v.data(), "mzv_", 4) == 0) return true;
    return false;
}

// Build the narrow ctx N = (Feynman vars) ∪ (MZV basis vars). Anything
// in F that is not in either set is treated as W-side and excluded.
PolyCtx build_narrow_ctx_for_verify(
    const PolyCtx& F,
    const std::vector<size_t>& feynman_var_indices) {
    std::vector<std::string> N_vars;
    N_vars.reserve(feynman_var_indices.size() + 16);
    // Use a set-membership test against feynman_var_indices.
    std::vector<bool> is_feynman(F.vars().size(), false);
    for (size_t i : feynman_var_indices) {
        if (i < is_feynman.size()) is_feynman[i] = true;
    }
    for (size_t i = 0; i < F.vars().size(); ++i) {
        const std::string& v = F.vars()[i];
        if (is_feynman[i] || is_mzv_basis_name(v)) {
            N_vars.push_back(v);
        }
    }
    return PolyCtx(std::move(N_vars));
}

void emit_failure_json(const char* call_site_tag,
                       const PolyCtx& F,
                       const PolyCtx& N,
                       const std::string& src,
                       const std::string& recon) {
    std::ostream& o = std::cerr;
    o << "{\"hf_rat_split_verify\":\"FAIL\",\"site\":\"";
    o << call_site_tag << "\",\"src\":";
    o << "\"" << src << "\",\"recon\":\"" << recon << "\",\"F_vars\":[";
    for (size_t i = 0; i < F.vars().size(); ++i) {
        if (i) o << ",";
        o << "\"" << F.vars()[i] << "\"";
    }
    o << "],\"N_vars\":[";
    for (size_t i = 0; i < N.vars().size(); ++i) {
        if (i) o << ",";
        o << "\"" << N.vars()[i] << "\"";
    }
    o << "]}\n";
}

}  // namespace

bool rat_split_verify_enabled() {
    return read_env_once();
}

void verify_regulator_sym_rat_split(
    const RegulatorSym& r,
    const PolyCtx& F,
    const std::vector<size_t>& feynman_var_indices,
    const char* call_site_tag) {
    if (!read_env_once()) return;

    // Proof-of-coverage: count this top-level call before any branch
    // that could short-circuit (e.g. an empty regulator, all-ctx-mismatch
    // terms). g_calls > 0 alone does not prove a Rat was round-tripped;
    // see g_terms_checked below for the load-bearing assertion.
    g_calls.fetch_add(1, std::memory_order_relaxed);

    // Construct the narrow ctx once per call. ZWTable is per-RegTermSym
    // since each one is independent; sharing a table doesn't reduce
    // round-trip correctness but does compress the dedup hash bookkeeping.
    PolyCtx N = build_narrow_ctx_for_verify(F, feynman_var_indices);
    FNIndexMaps maps = build_fn_index_maps(F, N);

    for (const auto& t : r) {
        const SymCoef& sc = t.coef;
        // Defensive: if ctx mismatches, skip the term (the verifier is
        // a non-fatal diagnostic — failure to match ctx is a structural
        // bug elsewhere, surfaced by the existing F-ctx invariants).
        if (&sc.ctx() != &F) {
            g_skips_ctx.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        ZWTable tab(F);
        for (const auto& m : sc.terms()) {
            const Rat& src = m.prefactor;
            std::vector<SymMonomialSplit> parts =
                split_rat_by_w_monomial(src, N, tab, maps.F_to_N_idx);
            Rat recon = recombine_rat_split(parts, F, tab, maps.N_to_F_idx);
            const std::string& s = src.to_string();
            const std::string& r_s = recon.to_string();
            if (s != r_s) {
                emit_failure_json(call_site_tag, F, N, s, r_s);
                std::abort();
            }
            g_terms_checked.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace hyperflint
