// Phase 3-a: recursive-descent parser for ConvertToHlogRegInf input.
//
// Accepts a Mathematica-style syntax covering the narrow grammar the
// Phase-3 converter needs:
//
//   expr       := sum
//   sum        := product (("+" | "-") product)*
//   product    := unary (("*" | "/") unary)*
//   unary      := ("-" | "+") unary | power
//   power      := call ("^" unary)?
//
//   PRECEDENCE (fix d74f8c88a, 2026-06-04): '^' binds TIGHTER than
//   unary minus, matching Mathematica/Maple: -f^n == -(f^n). The
//   earlier grammar (power := unary ("^" unary)?) folded a leading
//   minus into the base, computing (-f)^n and silently dropping the
//   sign for every even n; subtraction counterterms emit exactly the
//   exposed shape -(F)^(-2). Chained '^' is accepted RIGHT-
//   associatively (a^b^c == a^(b^c), matching Mathematica) as a side
//   effect of the exponent recursing through `unary`.
//   call       := "Hlog[" expr "," "[" letterlist "]" "]"
//               | "Log[" expr "]"
//               | "PolyLog[" expr "," expr "]"   // accepted, flagged
//               | atom
//   atom       := number | ident | "(" expr ")"
//   letterlist := expr ("," expr)*
//
// Two-pass operation (per the Phase-3 PolyCtx policy):
//   Pass 1 — identifier collection. Tokenize the input; any IDENT
//     that is not one of the recognized function names (Hlog, Log,
//     PolyLog) and is not already in `user_vars` becomes a free
//     variable and is appended to the augmented var list.
//   Pass 2 — AST build. Construct a PolyCtx over
//     (user_vars + free_vars) and recursive-descent-parse into an
//     Expr.
//
// Folding. Every Plus/Times/Power reduction checks whether all its
// children are Leaves. If so, the result is folded into a single
// Leaf via Rat arithmetic (FLINT canonicalizes the output). This
// means a pure-rational sub-expression like `1/(1+x)` produces
// `Leaf(Rat "1/(1+x)")` rather than nested Power/Times nodes.
// Hlog/Log/PolyLog sub-expressions are never folded — they stay as
// their dedicated node kinds (or as the rewrite `Log[arg] →
// Hlog[arg, {0}]` for Log).
//
// Negative exponents. The grammar accepts unary-minus on the
// exponent (`x^(-2)`). When the base is a Leaf, the result is
// folded to `Rat::pow(base, n)` which handles both signs; when the
// base is a non-Leaf (e.g. `Hlog[x, [0]]^(-1)`), the parser throws
// because the symbolic reciprocal of a non-rational object is
// outside the Phase-3 scope.

#pragma once

#include "hyperflint/convert/expr.hpp"
#include "hyperflint/core/poly.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace hyperflint {
namespace convert {

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& msg)
        : std::runtime_error("parse: " + msg) {}
};

struct ParseResult {
    Expr                     expr;
    // The PolyCtx under which every Rat in `expr` was constructed.
    // unique_ptr because PolyCtx is non-copyable and non-movable
    // (holds a FLINT `fmpq_mpoly_ctx_t` whose lifetime must equal
    // this object's); ParseResult is itself move-only.
    std::unique_ptr<PolyCtx> ctx;
    // user_vars + any free identifiers the parser discovered,
    // in the order they were encountered.
    std::vector<std::string> augmented_vars;
};

ParseResult parse_expression(const std::string& input,
                              const std::vector<std::string>& user_vars);

}  // namespace convert
}  // namespace hyperflint
