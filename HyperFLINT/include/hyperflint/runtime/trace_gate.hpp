// Single process-static gate for HF_STEP_TRACE-only timing probes.
// Read once at first call, cache for the process lifetime via a
// function-local `static const` (C++11 magic statics, thread-safe).
// Branch predictor handles the always-false case at zero cost in tight
// loops.
//
// Used to gate the per-thread chrono accumulators in:
//   - core/poly.cpp        (Poly::mul narrow + wide branches)
//   - core/rat.cpp         (Rat::add Tier A1 dispatch + add_legacy
//                            per-Poly-op + Rat::operator+= legacy +
//                            Rat::operator+= via_qu)
//   - integrator/primitive.cpp        (integrate_ii bump/push_ibp/
//                                       run_parfr/queue-copy/antideriv/
//                                       pole-arith/pole-word-ctor)
//   - integrator/integration_step.cpp (per-OMP-iteration probes
//                                       _entry_t0, _ts_t0, _ii_t0,
//                                       _pze_t0, _bb_t0, _pie_*_t0,
//                                       _pie_se_t0, _pie_exp_t0,
//                                       _pie_rvc*_t0)
//   - algebra/linear_factors.cpp      (cache_key_build _ck_t0;
//                                       Probe-2 _ts_t0 free-of-var,
//                                       _cfr_t0 clone_from_raw,
//                                       _tp_t0/_tp2_t0/_tp3_t0
//                                       transplant, _rc_t0 Rat ctor,
//                                       _ts_t0 final to_string)
//
// The READ side of these accumulators (HF_STEP_TRACE-emitted JSON
// counters: hyper_int.cpp:329-353, integration_step.cpp's flush at
// step end) is already env-gated. Gating the WRITE side closes the
// remaining cost.
//
// NOT used to gate:
//   - the user's HF_REC_LF_MEASURE / Scope-A.5 chrono blocks (those
//     have their own level-cache early-return and live in
//     run_rec_lf_probe_*).
//   - HF_DUMP_*, HF_REGKEY_DUMP, HF_BUCKET_HASH_STATS, HF_PROBE_*,
//     HF_LF_SQF, HF_DISABLE_*_CACHE (already env-gated elsewhere).
//   - omp_parallel_wall_s / omp_post_merge_s (once-per-step scouts,
//     negligible cost; the user actively reads these even when
//     HF_STEP_TRACE=0; do NOT gate).
//   - closure_body_s / closure_canon_s (once per closure pass).
//   - g_pf_global_gen / g_narrow_ctx_too_narrow (correctness, not
//     instrumentation).

#pragma once

#include <cstdlib>
// HF_FLAG_STEP_TRACE macro provides the std::getenv("HF_STEP_TRACE")
// literal (iter-85 §T7 fifteenth chunk; see docs/env_flags.md §5/§5.1).
// The macro lives in runtime/env_flags.hpp under §5.1 rule 1
// effect-domain placement (companion to this header).
#include "hyperflint/runtime/env_flags.hpp"

namespace hyperflint {

// Returns true iff HF_STEP_TRACE is set to a non-empty, non-"0" value.
// Read once at first call, cached for the process lifetime via the
// function-local `static const` lambda. Thread-safe per C++11 §6.7p4
// ("magic statics"). Each translation unit that includes this header
// gets its own static, but the value is identical (parsed from the
// same env var with no side effects beyond the parse) so there is no
// observable difference.
inline bool step_trace_enabled() {
    static const bool enabled = []() {
        const char* s = HF_FLAG_STEP_TRACE;
        if (s == nullptr || *s == '\0') return false;
        if (s[0] == '0' && s[1] == '\0') return false;
        return true;
    }();
    return enabled;
}

}  // namespace hyperflint
