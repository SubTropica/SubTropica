// HyperFLINT stable C ABI — Track 8.2 (iter-56): typed entry points
// now actually delegate to hyperflint::handlers::* and splice the
// envelope fields (schema_version + hf_version) into the CLI-form
// response body.  This replaces the iter-47/iter-50 hand-mirrored
// serializer with a single source of truth in src/bridge/handlers.cpp,
// fixing the iter-51 reviewer B3-rec-1 BINDING defect (the C ABI
// re-implemented serialization, so byte-identity with the CLI was
// coincidental and the snapshot tests asserted a false invariant).
//
// Scope (iter-56 §T8.2 + iter-58 §T8.4 + iter-61 §T8.4-real-op-1):
//   - hf_free_string: minimal char* deallocator paired with `new char[N]`
//     in dup_to_owned.  Asymmetric ownership: caller MUST route
//     deallocation through hf_free_string.
//   - hf_partial_fractions / hf_linear_factors: parse-validate, delegate
//     to hyperflint::handlers::*, splice {"schema_version":N,
//     "hf_version":"X"} into the returned CLI body, and hand back a
//     caller-owned char*.  Errors are in-band JSON (`"error"` field);
//     exceptions never propagate across the ABI boundary.  NULL return
//     is reserved for catastrophic allocation failure.
//   - hf_find_lr_orders (iter-58 §T8.4): promoted from the iter-56 stub
//     (which returned {"error":"chunk_3_pending"}) to a real delegating
//     entry point.  Calls hyperflint::handlers::find_lr_orders directly.
//     Unlike pfrac / linear_factors, this handler already emits the
//     {schema_version, hf_version} envelope inline at line 700-702 of
//     src/bridge/handlers.cpp, so splice_envelope detects the envelope
//     and returns the handler body unchanged (branch 1).  Net byte
//     content matches the CLI shim (modulo the trailing newline the CLI
//     adds), enabling a strict CLI-vs-ABI snapshot ctest at
//     test/unit/test_c_abi_find_lr_orders_cli_snapshot.cpp.
//   - hf_hyperflint_sym (iter-61 §T8.4-real-op-1): real delegation to
//     the integrator handler hyperflint::handlers::hyperflint_sym.
//     Like pfrac / linear_factors (and unlike find_lr_orders), the
//     handler's happy path emits a CLI-form body WITHOUT the
//     {schema_version, hf_version} envelope (src/bridge/handlers.cpp:
//     1164-1181 emits {"op":"hyperflint","result":...,"timing_compute_s":
//     ...,"vars":[...],"algebraic_letters":...}), so splice_envelope
//     detects the absence and inserts the envelope (branch 2). The op
//     name passed to splice_envelope is "hyperflint" (matching the
//     handler's emitted "op" field), not "hyperflint_sym". The
//     CLI-vs-ABI byte-identity ctest at test/unit/
//     test_c_abi_hyperflint_sym_cli_snapshot.cpp reuses the
//     findroots21_b W-non-empty integrator fixture from iter-60's
//     determinism probe, strips both the envelope (from the ABI side)
//     and the wall-clock `timing_compute_s` field (from both sides),
//     and asserts byte-identity.
//
// See also:
//   - include/hyperflint/c_abi.h                  (HF_SCHEMA_VERSION SSOT)
//   - include/hyperflint/bridge/handlers.hpp      (handlers contract)
//   - src/bridge/handlers.cpp                     (handlers implementation)
//   - bridge/cli/main.cpp:916                     (CLI sibling; same handler, no envelope)
//   - test/unit/test_c_abi_pfrac_cli_snapshot.cpp (byte-identity gate)
//   - HyperFLINT development notes §T8.2          (iter-56 B3-rec-1 fix)

#include "hyperflint/c_abi.h"

#include "hyperflint/bridge/handlers.hpp"

#include <cstddef>
#include <cstring>
#include <exception>
#include <sstream>
#include <string>

#ifndef HF_VERSION_STRING
#define HF_VERSION_STRING "unknown"
#endif

namespace {

// Minimal JSON string escape for envelope-only fields (op name +
// hf_version + error message synthesized by this wrapper).  Handlers
// already escape their own payload; this helper only protects the
// envelope chrome added by splice_envelope / error_envelope.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

// Splice {"schema_version":N,"hf_version":"X"} into a handler-returned
// CLI-form body of shape {"op":"<op>",...rest}.  Field-order convention:
// the envelope fields go immediately after `"op":"<op>"`, matching the
// iter-43 Track 8.1 envelope contract and the (previously hand-
// mirrored) iter-47/50 c_abi.cpp serializer.  Three branches:
//   1. Body already envelope-stamped (handlers.cpp::error_json_op
//      exception path emits {"op":"...","schema_version":N,
//      "hf_version":"X","error":"..."}): return body unchanged.
//   2. Body starts with the expected {"op":"<op>"} prefix and is NOT
//      envelope-stamped: insert envelope fields at the splice point.
//   3. Body is structurally malformed (handler returned something
//      unexpected): fail loud per iter-48 reviewer A2 — synthesize a
//      clean envelope-stamped error so the C-ABI caller observes the
//      defect explicitly.
std::string splice_envelope(const std::string& body, const char* op) {
    const std::string prefix = std::string("{\"op\":\"") + op + "\"";
    if (body.size() < prefix.size()
        || body.compare(0, prefix.size(), prefix) != 0) {
        std::ostringstream o;
        o << "{\"op\":\"" << op << "\""
          << ",\"schema_version\":" << HF_SCHEMA_VERSION
          << ",\"hf_version\":\"" << json_escape(HF_VERSION_STRING) << "\""
          << ",\"error\":\"c_abi: handler returned malformed body\"}";
        return o.str();
    }
    const std::string already = ",\"schema_version\":";
    if (body.size() >= prefix.size() + already.size()
        && body.compare(prefix.size(), already.size(), already) == 0) {
        return body;
    }
    std::ostringstream o;
    o << body.substr(0, prefix.size())
      << ",\"schema_version\":" << HF_SCHEMA_VERSION
      << ",\"hf_version\":\"" << json_escape(HF_VERSION_STRING) << "\""
      << body.substr(prefix.size());
    return o.str();
}

std::string error_envelope(const char* op, const std::string& msg) {
    std::ostringstream o;
    o << "{\"op\":\"" << op << "\""
      << ",\"schema_version\":" << HF_SCHEMA_VERSION
      << ",\"hf_version\":\"" << json_escape(HF_VERSION_STRING) << "\""
      << ",\"error\":\"" << json_escape(msg) << "\"}";
    return o.str();
}

// Allocate a caller-owned char* duplicate of `s` (NUL-terminated).
// Paired with hf_free_string (extern "C"); see header contract.
// Returns NULL only on allocation failure (caller treats as fatal).
char* dup_to_owned(const std::string& s) noexcept {
    try {
        char* buf = new char[s.size() + 1];
        std::memcpy(buf, s.data(), s.size());
        buf[s.size()] = '\0';
        return buf;
    } catch (...) {
        return nullptr;
    }
}

}  // namespace

// =====================================================================
// extern "C" entry points
// =====================================================================

extern "C" {

void hf_free_string(char* s) {
    // Asymmetric ownership: paired with `new char[N]` in dup_to_owned.
    // delete[] on a nullptr is well-defined (no-op), so the public
    // contract `hf_free_string(NULL) is a no-op` is satisfied directly.
    delete[] s;
}

char* hf_partial_fractions(const char* request_json) {
    // Contract: never throw, never abort.  Errors travel as JSON in the
    // returned envelope's "error" field.  NULL return is reserved for
    // catastrophic allocation failure (caller treats as fatal).
    try {
        if (request_json == nullptr) {
            return dup_to_owned(error_envelope(
                "partial_fractions", "request_json is NULL"));
        }
        return dup_to_owned(splice_envelope(
            hyperflint::handlers::partial_fractions(std::string(request_json)),
            "partial_fractions"));
    } catch (const std::exception& e) {
        return dup_to_owned(error_envelope("partial_fractions", e.what()));
    } catch (...) {
        return dup_to_owned(error_envelope(
            "partial_fractions", "unknown exception"));
    }
}

char* hf_linear_factors(const char* request_json) {
    // Same shape as hf_partial_fractions above: delegate to the
    // transport-neutral handler, splice the envelope, dup to owned.
    try {
        if (request_json == nullptr) {
            return dup_to_owned(error_envelope(
                "linear_factors", "request_json is NULL"));
        }
        return dup_to_owned(splice_envelope(
            hyperflint::handlers::linear_factors(std::string(request_json)),
            "linear_factors"));
    } catch (const std::exception& e) {
        return dup_to_owned(error_envelope("linear_factors", e.what()));
    } catch (...) {
        return dup_to_owned(error_envelope(
            "linear_factors", "unknown exception"));
    }
}

char* hf_find_lr_orders(const char* request_json) {
    // Track 8.4 (iter-58): real delegation, mirroring the §T8.2 shape
    // used by hf_partial_fractions / hf_linear_factors.  Difference:
    // hyperflint::handlers::find_lr_orders already emits the
    // {schema_version, hf_version} envelope at the head of its
    // response body (src/bridge/handlers.cpp:700-702), so splice_envelope
    // detects the envelope-already-present pattern and returns the body
    // verbatim (branch 1).  Net byte content equals the CLI shim's
    // stdout modulo the trailing newline.
    try {
        if (request_json == nullptr) {
            return dup_to_owned(error_envelope(
                "find_lr_orders", "request_json is NULL"));
        }
        return dup_to_owned(splice_envelope(
            hyperflint::handlers::find_lr_orders(std::string(request_json)),
            "find_lr_orders"));
    } catch (const std::exception& e) {
        return dup_to_owned(error_envelope("find_lr_orders", e.what()));
    } catch (...) {
        return dup_to_owned(error_envelope(
            "find_lr_orders", "unknown exception"));
    }
}

char* hf_hyperflint_sym(const char* request_json) {
    // Track 8.4-real-op-1 (iter-61): real delegation to the integrator
    // handler, mirroring the §T8.2 shape used by hf_partial_fractions
    // / hf_linear_factors.  Like those two (and unlike hf_find_lr_orders),
    // hyperflint::handlers::hyperflint_sym emits a CLI-form body
    // WITHOUT the {schema_version, hf_version} envelope on its happy
    // path (src/bridge/handlers.cpp:1164-1181 emits {"op":"hyperflint",
    // "result":...,"timing_compute_s":...,"vars":[...],"algebraic_letters":
    // ...} with no envelope fields).  splice_envelope detects the
    // absence and inserts the envelope at the splice point
    // immediately after `"op":"hyperflint"` (branch 2).
    //
    // Failure paths in the handler also emit bare CLI-form bodies
    // ({"op":"hyperflint","failed":true,...},
    //  {"op":"hyperflint","divergent":true,...},
    //  {"op":"hyperflint","narrow_ctx_insufficient":true,...}) which
    // splice_envelope likewise stamps. The catch-all exception path
    // routes through handlers.cpp::error_json_op which already emits
    // envelope inline; splice_envelope branch 1 returns it unchanged.
    // Either way, the ABI caller observes a uniformly envelope-stamped
    // response.
    //
    // Feasibility evidence for the CLI-vs-ABI byte-identity ctest:
    // iter-60 landed test_hyperflint_sym_response_determinism.cpp,
    // which asserted byte-identical CLI stdout modulo
    // `timing_compute_s` across two independent processes on the
    // findroots21_b W-non-empty integrator fixture (8/8 PASS at
    // commit 231a063e4). The iter-61 CLI-vs-ABI snapshot
    // (test_c_abi_hyperflint_sym_cli_snapshot.cpp) reuses that
    // fixture and the same strip pattern.
    try {
        if (request_json == nullptr) {
            return dup_to_owned(error_envelope(
                "hyperflint", "request_json is NULL"));
        }
        return dup_to_owned(splice_envelope(
            hyperflint::handlers::hyperflint_sym(std::string(request_json)),
            "hyperflint"));
    } catch (const std::exception& e) {
        return dup_to_owned(error_envelope("hyperflint", e.what()));
    } catch (...) {
        return dup_to_owned(error_envelope(
            "hyperflint", "unknown exception"));
    }
}

const char* hf_version_string(void) {
    // Returns a static-lifetime pointer to the HF_VERSION_STRING macro
    // injected at compile time by HyperFLINT/CMakeLists.txt:181-185.
    // Caller MUST NOT free; lifetime is the duration of the loaded
    // library. See include/hyperflint/c_abi.h for the contract.
    return HF_VERSION_STRING;
}

}  // extern "C"
