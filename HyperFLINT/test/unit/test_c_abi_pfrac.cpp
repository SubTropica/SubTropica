// Track 8.1b chunk-2 (iter-47): minimal smoke for hf_partial_fractions
// + hf_free_string + the chunk-3 stubs.  Tier-1 ctest; sub-second wall.
//
// Coverage axes:
//   A. envelope prefix (op + schema_version + hf_version) on success.
//   B. payload field presence (polynomial_part + poles + vars) on a
//      trivial 1/(x-1) input.
//   C. error envelope on missing "f" field.
//   D. error envelope on NULL request_json.
//   E. hf_linear_factors: chunk-3 LAND (iter-50) — typed entry returns
//      missing-fields error on empty request; envelope op stamped.
//      hf_find_lr_orders: §T8.4 LAND (iter-58) — promoted from the
//      iter-56 chunk_3_pending stub to a real delegating entry point
//      via hyperflint::handlers::find_lr_orders.  Empty body now
//      surfaces the handler's missing-fields error ("need \"xvars\"")
//      stamped with the envelope, not the stub sentinel.
//   F. hf_free_string(NULL) is a no-op (no crash).
//
// Linked against libhyperflint.a (the static library); the C ABI is
// exposed by the same library that handlers.cpp + the rest of the
// engine live in, so the symbols are guaranteed present.

#include "hyperflint/c_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int n_pass = 0;
int n_fail = 0;

void check(bool cond, const char* tag) {
    if (cond) {
        std::printf("[PASS] %s\n", tag);
        ++n_pass;
    } else {
        std::printf("[FAIL] %s\n", tag);
        ++n_fail;
    }
}

bool contains(const char* haystack, const char* needle) {
    if (haystack == nullptr) return false;
    return std::strstr(haystack, needle) != nullptr;
}

}  // namespace

int main() {
    // ---- A + B: success path, trivial input f = 1/(x-1) ----
    {
        const char* req =
            "{\"op\":\"partial_fractions\","
            "\"f\":\"1/(x-1)\","
            "\"var\":\"x\","
            "\"vars\":[\"x\"]}";
        char* resp = hf_partial_fractions(req);
        check(resp != nullptr, "A1 partial_fractions: non-NULL return");
        check(contains(resp, "\"op\":\"partial_fractions\""),
              "A2 envelope: op field");
        check(contains(resp, "\"schema_version\":1"),
              "A3 envelope: schema_version=1");
        check(contains(resp, "\"hf_version\":"),
              "A4 envelope: hf_version field");
        check(contains(resp, "\"polynomial_part\":"),
              "B1 payload: polynomial_part field");
        check(contains(resp, "\"poles\":"),
              "B2 payload: poles field");
        check(contains(resp, "\"vars\":[\"x\"]"),
              "B3 payload: vars echo");
        check(!contains(resp, "\"error\":"),
              "B4 payload: no error on valid input");
        hf_free_string(resp);
    }

    // ---- C: missing "f" → error envelope ----
    {
        char* resp = hf_partial_fractions(
            "{\"op\":\"partial_fractions\",\"var\":\"x\"}");
        check(resp != nullptr, "C1 missing-f: non-NULL return");
        check(contains(resp, "\"error\":"),
              "C2 missing-f: error field present");
        check(contains(resp, "\"schema_version\":1"),
              "C3 missing-f: envelope intact");
        hf_free_string(resp);
    }

    // ---- D: NULL request_json → error envelope ----
    {
        char* resp = hf_partial_fractions(nullptr);
        check(resp != nullptr, "D1 NULL-input: non-NULL return");
        check(contains(resp, "\"error\":"),
              "D2 NULL-input: error field present");
        check(contains(resp, "NULL"),
              "D3 NULL-input: error message names NULL");
        hf_free_string(resp);
    }

    // ---- E: chunk-3 LAND (iter-50) for linear_factors; chunk-4 stub
    // remains for find_lr_orders ----
    {
        // Empty body: typed entry point now returns the missing-fields
        // error envelope (chunk-3 LAND iter-50), not the "chunk_3_pending"
        // stub.  The envelope op + error structure is what gates the
        // smoke contract; full success-path round-trip is covered by the
        // iter-51 chunk-3b byte-identity CLI snapshot ctest.
        char* resp = hf_linear_factors("{}");
        check(resp != nullptr, "E1 linear_factors: non-NULL return");
        check(contains(resp, "\"op\":\"linear_factors\""),
              "E2 linear_factors: op field");
        check(contains(resp, "\"error\":\"need \\\"poly\\\" and \\\"var\\\" fields\""),
              "E3 linear_factors: missing-fields error");
        hf_free_string(resp);
    }
    {
        // §T8.4 LAND (iter-58): hf_find_lr_orders no longer returns the
        // iter-56 chunk_3_pending stub.  Empty body now travels through
        // hyperflint::handlers::find_lr_orders, which fails the missing
        // "xvars" check (handlers.cpp:576) and returns the
        // schema_version+hf_version-stamped error envelope via the
        // error_json helper.  The smoke gate verifies (i) non-NULL,
        // (ii) op identity, (iii) the new missing-fields error, and
        // (iv) absence of the retired stub sentinel — so a regression
        // back to the stub fails loud here, not just at the byte-
        // identity snapshot.
        char* resp = hf_find_lr_orders("{}");
        check(resp != nullptr, "E4 find_lr_orders: non-NULL return");
        check(contains(resp, "\"op\":\"find_lr_orders\""),
              "E5 find_lr_orders: op field");
        check(contains(resp, "\"error\":\"need \\\"xvars\\\"\""),
              "E6 find_lr_orders: missing-fields error (need \"xvars\")");
        // E7 (iter-59 tightening per Q-19 advisory A3): the iter-58
        // landing replaced the chunk_3_pending stub with the handler's
        // missing-fields error envelope.  E7 was originally a pure
        // negative substring check on the stub sentinel; tightened here
        // to anchor the error field positionally on its exact canonical
        // value.  A future regression that introduces a *different*
        // stub sentinel (e.g. "chunk_4_pending", "todo_iter62") would
        // pass the old absence check but fail this anchored match.
        const char* expected_err = "\"error\":\"need \\\"xvars\\\"\"";
        const char* err_pos = resp ? std::strstr(resp, "\"error\":") : nullptr;
        check(err_pos != nullptr
              && std::strncmp(err_pos, expected_err,
                              std::strlen(expected_err)) == 0
              && !contains(resp, "chunk_3_pending"),
              "E7 find_lr_orders: error field anchors at exact 'need \"xvars\"' (and no stub sentinel)");
        hf_free_string(resp);
    }

    // ---- F: hf_free_string(NULL) is a no-op ----
    hf_free_string(nullptr);
    check(true, "F1 hf_free_string(NULL): no-op (reached next statement)");

    std::printf("[SUMMARY] PASS=%d FAIL=%d\n", n_pass, n_fail);
    return n_fail == 0 ? 0 : 1;
}
