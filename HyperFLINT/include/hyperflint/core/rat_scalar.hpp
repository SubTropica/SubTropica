// RatScalar: rational over a NARROW PolyCtx (Feynman-params + MZV-basis).
//
// Phase-A commit (1) introduces this type per the HF MZV-as-scalar
// rewrite design v2 §3.2 (notes/hf_mzv_rewrite_design_2026-05-05/
// design.md). RatScalar is structurally identical to Rat — same
// canonical-form invariant (gcd(num,den)=1, leading-coef-positive
// denominator) and the same arithmetic API. The only difference is
// the contract that its PolyCtx contains only narrow-ctx variables
// (no Mandelstam, mass, Wm/Wp), which lets the inner-loop arithmetic
// run over polynomials with ~15-20 vars instead of ~150-200 wide-ctx
// vars on tst3-scale fixtures.
//
// Phase-A scope: the type exists and compiles. It is not yet wired
// into any hot path. Phase-B commits switch SymCoef call sites to use
// it; until then the type is exercised only by the round-trip
// adapter (commit 2) and unit tests (commit 5).

#pragma once

#include "hyperflint/core/rat.hpp"

namespace hyperflint {

// Phase-A: thin typedef alias to Rat. The implementation is
// literally identical; the distinction is contractual (callers that
// take a RatScalar promise the ctx is narrow). Phase-B may swap to
// a wrapping struct if measurement justifies extra type-level
// rigidity.
using RatScalar = Rat;

}  // namespace hyperflint
