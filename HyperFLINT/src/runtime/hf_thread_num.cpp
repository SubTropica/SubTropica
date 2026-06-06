// Phase 0.5 Item U Stage 2: thread_local slot override for GCD dispatch path.
// See include/hyperflint/runtime/hf_thread_num.hpp for the full design.

#include "hyperflint/runtime/hf_thread_num.hpp"

namespace hyperflint {
namespace runtime {

// Initialized to -1 (sentinel: no override; use omp_get_thread_num()).
thread_local int g_hf_thread_num_override = -1;

}  // namespace runtime
}  // namespace hyperflint
