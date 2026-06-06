// HyperFLINT stable C ABI — Track 8.1b chunk-1 scaffolding (iter-46).
//
// This header is the single source of truth for the schema version and
// for the (forthcoming) typed C entry points that external consumers
// (Julia HJ.jl, Mma LibraryLink, future Python via cffi) will dlopen
// without compiling against the C++ HyperFLINT internals.
//
// Chunk-1 (iter-46): scaffolding only.
//   - HF_SCHEMA_VERSION macro: SSOT for the eval-json envelope schema
//     version. handlers.cpp::kSchemaVersion now sources this macro
//     (§K.1 SSOT retrofit per iter-44 reviewer B3 BINDING fold).
//   - Forward declarations for the chunk 2-4 typed entry points:
//       hf_partial_fractions   (chunk 2, iter-47)
//       hf_linear_factors      (chunk 3, iter-48)
//       hf_find_lr_orders      (chunk 3, iter-48)
//   - Lifecycle helpers (hf_free_string) for caller-side cleanup of
//     returned char* payloads.
//
// Chunk 2-4 will populate the function bodies. The decls land here so
// downstream consumers can begin wrapping the ABI surface in lockstep
// with chunk LANDs.
//
// Stability contract (Phase 2 §21 stable C ABI ruling iter-44):
//   - Functions take only POD C types (int, char*, const char*, etc.).
//   - Returned char* payloads MUST be freed via hf_free_string().
//   - All ops return a JSON UTF-8 string with the eval-json envelope
//     (schema_version, hf_version, op, ...) — same shape as eval-json.
//   - Errors travel in the returned JSON ("error" field), never via
//     exceptions across the ABI boundary or via stderr.
//   - Schema bumps follow the policy in handlers.cpp head comment:
//     backwards-compatible field additions keep HF_SCHEMA_VERSION;
//     renames/removals bump it and update the Mma-side
//     $SubTropicaHFSchemaVersionExpected gate in lockstep.
//
// Track-axis map:
//   - Track 8.1 (eval-json envelope, iter-43)   → kSchemaVersion in
//     handlers.cpp now sources HF_SCHEMA_VERSION from this header
//     (chunk-1 §K.1 SSOT retrofit, iter-46).
//   - Track 8.1b (stable C ABI, iter-44 PLAN)   → chunks 1-4 LAND
//     iter-46..iter-49.
//
// References:
//   - HyperFLINT development notes: Track 8.1b rewrite analysis (internal)
//   - HyperFLINT/docs/ARCHITECTURE.md §vi (bridge layer)
//
// Public vs internal symbols (iter-59 A4 fold of Q-19 advisory A1):
//   Only the `hf_*`-prefixed symbols declared in this header are part of
//   the stable, public C ABI surface. The compiled libhyperflint contains
//   many other symbols whose names happen to begin with `hf_` (internal
//   build-stamp probes, `hyperflint_*` LibraryLink entries, mangled C++
//   helpers, etc.); these are NOT public and are subject to change
//   without notice across iters.
//
//   Authoritative whitelist: `HyperFLINT/test/abi/symbols_golden.txt`
//   pins the exact set of exported symbols allowed in the stripped
//   library. The ctest `hf-abi-break-detection` enforces equality
//   between the live nm-emitted symbol set and the whitelist; adding or
//   removing a public symbol without updating the golden file is an
//   intentional break.
//
//   Checklist for adding a NEW public C-ABI op:
//     (i)  Declare the entry point in this header (extern "C" block).
//     (ii) Append the mangled (`_hf_<name>`) entry to symbols_golden.txt
//          in sorted order.
//     (iii) Add a CLI-vs-ABI byte-identity snapshot ctest under
//          test/unit/, mirroring test_c_abi_pfrac_cli_snapshot.cpp
//          (in-line envelope) or test_c_abi_linear_factors_cli_snapshot
//          (envelope via splice_envelope), depending on whether the
//          underlying handler emits the envelope inline.
//     (iv) Implement the entry in src/bridge/c_abi.cpp using the
//          try/catch + splice_envelope pattern (handler delegation).
//
//   Removing a public symbol requires the same four steps in reverse and
//   should be paired with a MINOR bump of HF_VERSION (semver minor for
//   additions, semver MAJOR for removals/renames; see §T10/iter-57).

#ifndef HYPERFLINT_C_ABI_H_
#define HYPERFLINT_C_ABI_H_

// Single source of truth for the eval-json response envelope schema
// version. handlers.cpp::kSchemaVersion sources this macro.
//
// Bump policy: see handlers.cpp head comment ("Bump policy"). In short:
// non-breaking additions keep the version; renames/removals bump it and
// require lockstep update of SubTropica.wl's
// $SubTropicaHFSchemaVersionExpected.
#define HF_SCHEMA_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

// ---- Lifecycle ----
//
// Frees a char* payload returned by any hf_* function below. Calling
// hf_free_string(NULL) is a no-op. The contract is asymmetric: callers
// MUST NOT free() these pointers directly because HyperFLINT may
// allocate them via new[] or a custom allocator (chunk-2 implementation
// detail). Always route deallocation through hf_free_string.
//
// Chunk-2 (iter-47) lands the implementation.
void hf_free_string(char* s);

// ---- Op entry points (chunks 2-4) ----
//
// All ops follow the same convention:
//   - Input: a JSON UTF-8 string (the same request shape that
//     `hyperflint eval-json` accepts on stdin).
//   - Output: a JSON UTF-8 string (the same eval-json envelope, with
//     "schema_version" = HF_SCHEMA_VERSION, "hf_version", "op", and
//     either op-specific result fields or "error":"...").
//   - Memory: caller owns the returned char*; must free via
//     hf_free_string().
//   - Errors: in-band JSON ("error" field); never via return value or
//     stderr.
//
// Returning NULL is reserved for catastrophic allocation failure (e.g.
// std::bad_alloc inside the JSON serializer); in that case the caller
// must treat the call as failed without further diagnostic, since by
// definition we could not allocate the error envelope itself.

// chunk 2 (iter-47): partial_fractions typed entry.
char* hf_partial_fractions(const char* request_json);

// chunk 3 (iter-48): linear_factors typed entry.
char* hf_linear_factors(const char* request_json);

// chunk 3 (iter-48): find_lr_orders typed entry. The existing
// LibraryLink entry hyperflint_find_lr_orders_json delegates to
// handlers::handle_find_lr_orders; this C-ABI variant additionally
// wraps the return in HyperFLINT's caller-owned allocation contract,
// so external consumers do not need to link Mma headers.
char* hf_find_lr_orders(const char* request_json);

// Track 8.4-real-op-1 (iter-61): hyperflint_sym typed entry. Delegates
// to hyperflint::handlers::hyperflint_sym, the transport-neutral
// integrator handler shared with `hyperflint eval-json` (CLI) and the
// LibraryLink dispatch. Request shape: identical to the CLI
// `hyperflint` op (see bridge/cli/main.cpp::handle_hyperflint and
// the documentation block above hyperflint::handlers::hyperflint_sym
// in include/hyperflint/bridge/handlers.hpp). The handler emits a
// CLI-form response body WITHOUT the {schema_version, hf_version}
// envelope on its happy path; src/bridge/c_abi.cpp::splice_envelope
// detects the absence and inserts the envelope at the head of the
// returned body (branch 2). Failure paths inside the handler emit
// either bare {"op":"hyperflint","failed":true,...} bodies (which
// splice_envelope likewise stamps) or envelope-already-present bodies
// via error_json_op (branch 1: returned unchanged). Either way, the
// ABI caller observes a uniformly envelope-stamped response. iter-60
// landed a CLI cross-process determinism probe
// (test/unit/test_hyperflint_sym_response_determinism.cpp) that
// asserts byte-identical CLI stdout modulo `timing_compute_s` across
// independent processes on the findroots21_b W-non-empty fixture; the
// CLI-vs-ABI snapshot ctest landed by this iter reuses that fixture
// and strip pattern.
char* hf_hyperflint_sym(const char* request_json);

// ---- Versioning (§T10, iter-57) ----
//
// Returns a static-lifetime NUL-terminated string with the HyperFLINT
// build version in the documented MAJOR.MINOR.PATCH.BUILD format (cmake
// cache var `HF_VERSION`, injected via -DHF_VERSION_STRING=... in
// HyperFLINT/CMakeLists.txt:181-185). The returned pointer is owned by
// the library; callers MUST NOT pass it to hf_free_string() or free().
//
// Intended for downstream consumers (Julia HJ.jl, Mma LibraryLink,
// future Python via cffi) to attribute results to a specific HF build
// and to gate forward-compatibility checks. The string format is
// stable: four dot-separated unsigned-decimal components, matching the
// existing `hf_version` envelope field already emitted by handlers.cpp
// (eval-json transport) and spliced by c_abi.cpp::splice_envelope.
const char* hf_version_string(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // HYPERFLINT_C_ABI_H_
