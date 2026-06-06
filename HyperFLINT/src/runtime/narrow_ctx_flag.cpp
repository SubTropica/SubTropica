// narrow_ctx_flag — implementation.  See header for design rationale.

#include "hyperflint/runtime/narrow_ctx_flag.hpp"

// iter-93 §T7 twenty-third chunk Track-bridge macro-layer relocation:
// the tolerant-parse env-var literal now lives in the Track-bridge
// section of runtime/env_flags.hpp as HF_FLAG_PARSE_TOLERANT
// (rule-1 effect-domain = runtime; cached once-init below is the
// canonical reader). VALUE-family macro consistent with the other
// 16 macros resident in runtime/env_flags.hpp.
#include "hyperflint/runtime/env_flags.hpp"

#include <cstdlib>

namespace hyperflint {

std::atomic<bool> g_narrow_ctx_too_narrow{false};

void reset_narrow_ctx_flag() {
    g_narrow_ctx_too_narrow.store(false, std::memory_order_relaxed);
}

bool narrow_ctx_was_too_narrow() {
    return g_narrow_ctx_too_narrow.load(std::memory_order_relaxed);
}

bool tolerance_enabled() {
    // Static-local once-init: the lambda runs exactly once at first
    // call across all threads, and `cached` is treated as immutable
    // thereafter.  Standard C++11 magic-static guarantee.
    static const bool cached = []() {
        const char* v = HF_FLAG_PARSE_TOLERANT;
        return v && *v && v[0] != '0';
    }();
    return cached;
}

}  // namespace hyperflint
