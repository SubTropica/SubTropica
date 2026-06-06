// Phase γ.1: transport-neutral JSON handlers.
//
// Both the CLI (`hyperflint eval-json`) and the LibraryLink shared
// library route through these pure string-in / string-out functions.
// The CLI prints the returned string to stdout with a trailing newline;
// the LibraryLink entry point passes it back to Mma via
// MArgument_setUTF8String.  Neither touches stderr or the process exit
// code — all errors are encoded as JSON-shaped responses with an
// `"error"` field so both transports can surface them uniformly.
//
// Currently migrated:
//   - find_lr_orders (Phase γ.1 scope)
//   - partial_fractions (Track 8.1b chunk-2b iter-48)
//   - linear_factors (Track 8.1b chunk-3 iter-50)
//   - hyperflint_sym
// Remaining CLI handlers still print to stdout directly; they migrate
// to this module as they're exposed through LibraryLink.

#pragma once

#include <string>

namespace hyperflint {
namespace handlers {

// Takes the full JSON request body (the usual {"op": "find_lr_orders",
// "xvars": [...], "groups": [[...]], ...} shape).  Returns the JSON
// response body (no newline, no leading/trailing whitespace).  Never
// throws — all exceptions are caught and converted to
// {"op":"find_lr_orders","error":"<msg>"} responses.  Caller chooses
// whether to append a newline / push to stdout.
std::string find_lr_orders(const std::string& body);

// Track 8.1b chunk-2b (iter-48): transport-neutral partial_fractions
// handler.  Request shape:
//   {"op":"partial_fractions", "f":<str>, "var":<str>,
//    "vars":[...], "algebraic_letters":<bool>}
// Returns the CLI-form response body (no envelope, no newline):
//   {"op":"partial_fractions","var":"X","polynomial_part":"...",
//    "poles":[...], "vars":[...]}
// Never throws; errors travel as {"op":"partial_fractions",
// "error":"<msg>"} (or envelope-stamped via error_json_op for
// internal exceptions).  CLI appends '\n' and writes to stdout; the
// C ABI wrapper in src/bridge/c_abi.cpp produces a parallel envelope-
// stamped form for external consumers.
std::string partial_fractions(const std::string& body);

// Track 8.1b chunk-3 (iter-50): transport-neutral linear_factors
// handler.  Request shape:
//   {"op":"linear_factors", "poly":<str>, "var":<str>,
//    "vars":[...], "introduce_algebraic_letters":<bool>}
// Returns the CLI-form response body (no envelope, no newline):
//   {"op":"linear_factors","constant":"...",
//    "linear":[[mult, pole_num, pole_den], ...],
//    "nonlinear":[[mult, factor_str, deg], ...],
//    "vars":[...]}
// Never throws; errors travel as
// {"op":"linear_factors","error":"<msg>"} (or envelope-stamped via
// error_json_op for internal exceptions).  CLI appends '\n' and writes
// to stdout; the C ABI wrapper in src/bridge/c_abi.cpp produces a
// parallel envelope-stamped form for external consumers.
std::string linear_factors(const std::string& body);

// Integrator handler.  JSON request / response shape identical to the
// CLI `hyperflint` op (see bridge/cli/main.cpp handle_hyperflint for
// the full schema).  Never throws; JSON-encodes divergence / failure /
// internal errors in the response.
std::string hyperflint_sym(const std::string& body);

}  // namespace handlers
}  // namespace hyperflint
