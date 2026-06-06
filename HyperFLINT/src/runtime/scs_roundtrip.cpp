// Phase-B B1.c / B2+: shared SymCoef <-> SymCoefSplit round-trip
// helper. See header for design notes.
//
// History: the helper, counters, and at-exit emitter were originally
// shipped in `transform.cpp` anonymous namespace under the name
// `hf_b1c_scs_verify:` (commit fa718d8d8, B1.c). Promoted here at B2
// (iter-13) so all Phase-B per-step regulator-producing functions
// share one set of counters and one at-exit line. Old line name
// `hf_b1c_scs_verify:` retired in favour of `hf_scs_roundtrip_verify:`;
// the gate parser only consumes `hf_rat_split_verify:` lines, so the
// rename does not perturb the per-commit gate.

#include "hyperflint/runtime/scs_roundtrip.hpp"

#include "hyperflint/core/symcoef.hpp"
#include "hyperflint/runtime/rat_split_verify.hpp"
#include "hyperflint/runtime/scalar_rep.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace hyperflint::runtime {

namespace {

// Counters: cumulative across all sites (B1.c, B2, ..., B7).
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_terms_checked{0};

struct ScsRoundtripAtExit {
    ~ScsRoundtripAtExit() {
        const bool v1 = scalar_rep_enabled();
        std::fprintf(stderr,
            "hf_scs_roundtrip_verify: enabled=%d calls=%llu terms_checked=%llu\n",
            v1 ? 1 : 0,
            static_cast<unsigned long long>(g_calls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_terms_checked.load(std::memory_order_relaxed)));
        std::fflush(stderr);
    }
};
ScsRoundtripAtExit g_scs_roundtrip_atexit;

}  // namespace

hyperflint::RegulatorSym roundtrip_regulator_through_scs(
    const hyperflint::RegulatorSym& r,
    const hyperflint::PolyCtx& F,
    std::shared_ptr<hyperflint::ZWTable> zw_tab,
    const char* site_tag) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    hyperflint::RegulatorSym out;
    out.reserve(r.size());
    const bool verify = hyperflint::rat_split_verify_enabled();
    for (const auto& t : r) {
        hyperflint::SymCoefSplit split =
            hyperflint::SymCoefSplit::from_rat(t.coef, F, zw_tab);
        hyperflint::SymCoef back = split.as_rat();
        if (verify) {
            // Re-split the as_rat output and check canonical-form
            // equality (design v2 §3.5a verifier site; B1.a
            // equals_canonical predicate).
            hyperflint::SymCoefSplit roundtrip =
                hyperflint::SymCoefSplit::from_rat(back, F, zw_tab);
            if (!split.equals_canonical(roundtrip)) {
                std::fprintf(stderr,
                    "{\"hf_scs_roundtrip_verify\":\"FAIL\",\"site\":\"%s\"}\n",
                    site_tag);
                std::fflush(stderr);
                std::abort();
            }
            g_terms_checked.fetch_add(1, std::memory_order_relaxed);
        }
        out.push_back(hyperflint::RegTermSym{std::move(back), t.key});
    }
    return out;
}

hyperflint::Rat roundtrip_rat_through_scs(
    const hyperflint::Rat& r,
    const hyperflint::PolyCtx& F,
    std::shared_ptr<hyperflint::ZWTable> zw_tab,
    const char* site_tag) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    // Promote the Rat to a single-monomial pure-rat SymCoef. Empty
    // SymCoef (when r is zero) round-trips trivially: from_rat ->
    // is_rat -> as_rat returns Rat::zero in F's ctx.
    hyperflint::SymCoef sc = hyperflint::SymCoef::from_rat(r);
    hyperflint::SymCoefSplit split =
        hyperflint::SymCoefSplit::from_rat(sc, F, zw_tab);
    hyperflint::SymCoef back_sc = split.as_rat();
    const bool verify = hyperflint::rat_split_verify_enabled();
    if (verify) {
        hyperflint::SymCoefSplit roundtrip =
            hyperflint::SymCoefSplit::from_rat(back_sc, F, zw_tab);
        if (!split.equals_canonical(roundtrip)) {
            std::fprintf(stderr,
                "{\"hf_scs_roundtrip_verify\":\"FAIL\",\"site\":\"%s\"}\n",
                site_tag);
            std::fflush(stderr);
            std::abort();
        }
        g_terms_checked.fetch_add(1, std::memory_order_relaxed);
    }
    // Unwrap back to a Rat. SymCoef::as_rat throws if the SymCoef
    // isn't pure-rat; the round-trip preserves the pure-rat property
    // by construction (input had zero transcendental flags).
    return back_sc.as_rat();
}

}  // namespace hyperflint::runtime
