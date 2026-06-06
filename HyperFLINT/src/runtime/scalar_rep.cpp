// HF_USE_SCALAR_REP harness — Phase-B commit (1) [B1.b].
//
// Iter-91 §T7 21st chunk Track-cache-scalar-rep macro-layer LAND:
// the two env-var literals consumed by the static one-time-cache
// readers below are sourced from the runtime macro home
// (`hyperflint/runtime/env_flags.hpp`). Rule-1 effect-domain
// placement ratified by adversarial-reviewer (Q-19 B1
// substantive-pattern dispatch); see the env_flags.hpp comment
// block above the two `HF_FLAG_*SCALAR_REP*` macros for the
// binding rationale and the two logged advisory follow-ups.

#include "hyperflint/runtime/scalar_rep.hpp"
#include "hyperflint/runtime/env_flags.hpp"

#include <atomic>
#include <cstdlib>

namespace hyperflint::runtime {

namespace {

// One-time cache of the scalar-rep representation knob.
std::atomic<int> g_scalar_rep_state{-1};  // -1 = unread, 0 = off, 1 = on

bool read_env_once() {
    int s = g_scalar_rep_state.load(std::memory_order_relaxed);
    if (s >= 0) return s == 1;
    const char* v = HF_FLAG_USE_SCALAR_REP;
    bool on = (v != nullptr) && v[0] != '\0' && v[0] != '0';
    g_scalar_rep_state.store(on ? 1 : 0, std::memory_order_relaxed);
    return on;
}

// One-time cache of the iter-44 require-persistent debug-build assertion gate.
std::atomic<int> g_require_persistent_state{-1};

bool read_require_persistent_env_once() {
    int s = g_require_persistent_state.load(std::memory_order_relaxed);
    if (s >= 0) return s == 1;
    const char* v = HF_FLAG_SCALAR_REP_REQUIRE_PERSISTENT;
    bool on = (v != nullptr) && v[0] != '\0' && v[0] != '0';
    g_require_persistent_state.store(on ? 1 : 0, std::memory_order_relaxed);
    return on;
}

}  // namespace

bool scalar_rep_enabled() {
    return read_env_once();
}

bool require_persistent_enabled() {
    return read_require_persistent_env_once();
}

}  // namespace hyperflint::runtime
