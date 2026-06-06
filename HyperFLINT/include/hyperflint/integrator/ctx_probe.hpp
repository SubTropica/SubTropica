#ifndef HF_INTEGRATOR_CTX_PROBE_HPP
#define HF_INTEGRATOR_CTX_PROBE_HPP

#include <flint/fmpq_mpoly.h>
#include <string>

#include "hyperflint/core/poly.hpp"

namespace hyperflint {

bool ctx_probe_enabled();

void ctx_probe_record(const fmpq_mpoly_struct* p, const PolyCtx& ctx);

std::string ctx_probe_dump_and_clear(const PolyCtx& ctx);

}  // namespace hyperflint

#endif
