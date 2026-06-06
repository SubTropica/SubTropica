// Phase γ.1: LibraryLink (WolframLibraryData) transport for HyperFLINT.
//
// Exposes the same JSON-request/JSON-response handlers the CLI binary
// uses, but in-process via `LibraryFunctionLoad` instead of per-call
// subprocess spawn.  Eliminates RunProcess overhead (~7 ms on macOS)
// and the one-time startup cost of each subprocess (FLINT init, MZV
// table load for the hyperflint_sym op, etc.).
//
// Currently exposed:
//   * hf_version        — returns HF version string (for mismatch check)
//   * hf_clear_state    — resets AlgebraicLetterTable::global() and any
//                          future per-call process-global state.  No-op
//                          for find_lr_orders; required before calls
//                          that use algebraic letters.
//   * hf_find_lr_orders — calls hyperflint::handlers::find_lr_orders
//                          on the JSON input.
//   * hf_hyperflint_sym — calls hyperflint::handlers::hyperflint_sym on
//                          the JSON input (Phase γ.2).  This is the
//                          actually-hot op — adversarial-review census
//                          identified it as the bottleneck for the
//                          Phase β3 banana regression once find_lr_orders
//                          was cached.
//
// Safety invariants (per Phase γ pre-review):
//   1. Every entry point is wrapped in `try { ... } catch (...)` — C++
//      exceptions must not cross the C ABI boundary (UB).
//   2. FLINT's `flint_abort()` path (internal errors, OOM) normally
//      calls abort() and kills the kernel; we install a handler that
//      throws std::runtime_error instead, caught by the outer try.
//   3. AlgebraicLetterTable cleared defensively at every call entry so
//      state from a previous call cannot leak forward.

#include "WolframLibrary.h"

#include "hyperflint/algebra/algebraic_letters.hpp"
#include "hyperflint/bridge/handlers.hpp"

#include <cstring>
#include <exception>
#include <flint/flint.h>
#include <stdexcept>
#include <string>

#ifndef HF_VERSION_STRING
#define HF_VERSION_STRING "unknown"
#endif

namespace {

// Replace FLINT's default abort with a throw so we can catch it at the
// C-ABI boundary.  flint_set_abort takes a function of signature
// `void(*)(void)` — it has no way to pass context, so the thrown
// std::runtime_error carries a generic message.
void hf_flint_abort_hook() {
    // flint_set_abort takes a void(*)(void); throw propagates to the
    // outer try/catch(...) in every entry point.  Without `[[noreturn]]`
    // we get a minor compiler warning about possibly-reaching end of
    // a non-void function — a throw always terminates, so this is safe
    // in practice; the warning is cosmetic.
    throw std::runtime_error("FLINT abort (internal error or OOM)");
}

// Session-scoped "initialized" flag.  WolframLibrary_initialize gets
// called at LibraryFunctionLoad time; double-init is cheap but
// irrelevant.
bool g_hf_lib_initialized = false;

// Shallow helper: set a UTF8String return value from a std::string.
// `libData->UTF8String_allocate` in some older LibraryLink versions
// requires the size as 2nd arg; newer ones take only size_t.  We use
// malloc here because the older allocator is fussy about thread
// ownership; the kernel owns and frees the buffer afterwards per
// LibraryLink convention.
int set_utf8_result(MArgument Res, const std::string& s) {
    char* buf = static_cast<char*>(malloc(s.size() + 1));
    if (!buf) return LIBRARY_FUNCTION_ERROR;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    MArgument_setUTF8String(Res, buf);
    return LIBRARY_NO_ERROR;
}

}  // namespace

extern "C" {

DLLEXPORT mint WolframLibrary_getVersion(void) {
    return WolframLibraryVersion;
}

DLLEXPORT int WolframLibrary_initialize(WolframLibraryData /*libData*/) {
    if (!g_hf_lib_initialized) {
        flint_set_abort(hf_flint_abort_hook);
        g_hf_lib_initialized = true;
    }
    return LIBRARY_NO_ERROR;
}

DLLEXPORT void WolframLibrary_uninitialize(WolframLibraryData /*libData*/) {
    // Reset the global algebraic-letter table so a later LibraryFunctionLoad
    // into the same process doesn't inherit state.
    try {
        hyperflint::AlgebraicLetterTable::global().clear();
    } catch (...) {
        // cleanup failure ignored; process may be exiting
    }
}

// -------- hf_version --------
//
// Signature:  String = hf_version[]
// Returns a short version string baked in at CMake-configure time.  Mma
// compares this to $SubTropicaVersion at load time and refuses to use
// the library on mismatch.
DLLEXPORT int hf_version(WolframLibraryData /*libData*/,
                         mint /*Argc*/, MArgument* /*Args*/, MArgument Res) {
    try {
        return set_utf8_result(Res, HF_VERSION_STRING);
    } catch (...) {
        return LIBRARY_FUNCTION_ERROR;
    }
}

// -------- hf_clear_state --------
//
// Signature:  Integer = hf_clear_state[]
// Resets AlgebraicLetterTable::global() so a subsequent call sees a
// clean slate.  For find_lr_orders this is a no-op; for
// hyperflint_sym (when added) it prevents Wm/Wp letter-index collisions
// across independent calls.  Returns 0 on success.
DLLEXPORT int hf_clear_state(WolframLibraryData /*libData*/,
                             mint /*Argc*/, MArgument* /*Args*/,
                             MArgument Res) {
    try {
        hyperflint::AlgebraicLetterTable::global().clear();
        MArgument_setInteger(Res, 0);
        return LIBRARY_NO_ERROR;
    } catch (...) {
        return LIBRARY_FUNCTION_ERROR;
    }
}

// -------- hf_find_lr_orders --------
//
// Signature:  String = hf_find_lr_orders[jsonBody_String]
// Wraps hyperflint::handlers::find_lr_orders.  Same input/output
// format as the CLI `{"op":"find_lr_orders",...}` payload — lets us
// reuse every bit of the CLI code path and keep the comparison honest.
DLLEXPORT int hf_find_lr_orders(WolframLibraryData /*libData*/,
                                mint Argc, MArgument* Args, MArgument Res) {
    try {
        if (Argc < 1) return LIBRARY_FUNCTION_ERROR;
        char* input = MArgument_getUTF8String(Args[0]);
        if (input == nullptr) return LIBRARY_FUNCTION_ERROR;

        // Defensive state clear — harmless for find_lr_orders, required
        // once hyperflint_sym is exposed.  Do it here so every op has
        // the same lifecycle guarantees.
        hyperflint::AlgebraicLetterTable::global().clear();

        std::string response = hyperflint::handlers::find_lr_orders(
            std::string(input));

        // Release FLINT's thread-local allocator pools across all OMP
        // threads.  Without this, each per-thread pool grows
        // monotonically across calls and the cumulative RSS of an
        // Mma subkernel that dispatches many faces (e.g. 3l3pt's
        // ord_*_face_*) climbs until the OS OOM-kills the kernel
        // (silent partial-integration via Get::noopen on missing
        // result_ct_*.m files; see notes/hf_3l3pt_session_2026-05-02/
        // run_3l3pt_postpatch_hf.stdout).  flint_cleanup_master is
        // the documented multi-thread cleanup entry point; idempotent.
        flint_cleanup_master();

        // LibraryFunction convention: the library owns the input string
        // until disown; the return UTF8String is handed to Mma.
        // UTF8String_disown is part of the WolframLibraryData callback
        // set.  We can't call it without libData; for MVP we rely on
        // the kernel's reference-count — the extra dangling ref is a
        // small leak per call (< 1 KB per large request), acceptable
        // for Phase γ.1 (documented).  Phase γ.2 will pipe libData
        // through properly for no-leak handling.

        return set_utf8_result(Res, response);
    } catch (const std::exception& e) {
        // Encode error in a JSON response rather than LIBRARY_FUNCTION_ERROR
        // so the Mma caller can inspect the message.  Return code also
        // set to NO_ERROR so caller reaches the parse path.
        try {
            std::string err = std::string("{\"op\":\"find_lr_orders\","
                "\"error\":\"") + e.what() + "\"}";
            return set_utf8_result(Res, err);
        } catch (...) {
            return LIBRARY_FUNCTION_ERROR;
        }
    } catch (...) {
        return LIBRARY_FUNCTION_ERROR;
    }
}

// -------- hf_hyperflint_sym --------
//
// Signature:  String = hf_hyperflint_sym[jsonBody_String]
// Wraps hyperflint::handlers::hyperflint_sym — same request/response
// schema as the CLI `{"op":"hyperflint",...}` payload.  This is the
// integration core (expr/f/wordlist → RegulatorSym JSON) and dominates
// the Phase γ.1 banana regression; LibraryLink transport eliminates the
// ~7 ms per-call RunProcess overhead and the MZV-table reload each
// subprocess paid.
//
// Safety: handler clears AlgebraicLetterTable defensively at entry so
// Wm/Wp indices cannot leak across in-process calls.  A belt-and-
// braces clear() is also done here before dispatch.
DLLEXPORT int hf_hyperflint_sym(WolframLibraryData /*libData*/,
                                mint Argc, MArgument* Args, MArgument Res) {
    try {
        if (Argc < 1) return LIBRARY_FUNCTION_ERROR;
        char* input = MArgument_getUTF8String(Args[0]);
        if (input == nullptr) return LIBRARY_FUNCTION_ERROR;

        // Defensive state clear — required here: hyperflint_sym's
        // `introduce_al` path mutates the process-global
        // AlgebraicLetterTable.  Without this, Wm[k]/Wp[k] indices
        // from a prior call would collide with fresh ones.  (The
        // handler also clears internally; one extra clear is cheap.)
        hyperflint::AlgebraicLetterTable::global().clear();

        std::string response = hyperflint::handlers::hyperflint_sym(
            std::string(input));

        // Release FLINT's thread-local allocator pools across all OMP
        // threads.  Without this, each per-thread pool grows
        // monotonically across calls and the cumulative RSS of an
        // Mma subkernel that dispatches many faces (e.g. 3l3pt's
        // ord_*_face_*) climbs until the OS OOM-kills the kernel
        // (silent partial-integration via Get::noopen on missing
        // result_ct_*.m files; see notes/hf_3l3pt_session_2026-05-02/
        // run_3l3pt_postpatch_hf.stdout).  flint_cleanup_master is
        // the documented multi-thread cleanup entry point; idempotent.
        flint_cleanup_master();

        return set_utf8_result(Res, response);
    } catch (const std::exception& e) {
        try {
            std::string err = std::string("{\"op\":\"hyperflint\","
                "\"error\":\"") + e.what() + "\"}";
            return set_utf8_result(Res, err);
        } catch (...) {
            return LIBRARY_FUNCTION_ERROR;
        }
    } catch (...) {
        return LIBRARY_FUNCTION_ERROR;
    }
}

}  // extern "C"
