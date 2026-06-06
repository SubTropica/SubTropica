// iter-42 Track 6.5b: LibraryLink-boundary roundtrip ctest for the
// "strategy" field emitted by hf_find_lr_orders (the LibraryLink wrapper
// in bridge/librarylink/hf_library.cpp:149 around hyperflint::handlers::find_lr_orders).
//
// Independence axis vs iter-41 ctest test_find_lr_orders_strategy_roundtrip.cpp
// (which exercises handlers::find_lr_orders DIRECTLY via the C++ header):
//
//   This ctest dlopens libhyperflint_librarylink.dylib at RUNTIME and
//   invokes hf_find_lr_orders via dlsym, crossing the LibraryLink C ABI:
//
//     (a) dyld resolution of the dylib (-fvisibility=hidden +
//         exports.txt allowlist restricts symbols to _hf_* + _WolframLibrary_*);
//     (b) the C ABI boundary (WolframLibraryData, mint, MArgument);
//     (c) MArgument UTF8String unpack (char** dereference in
//         MArgument_getUTF8String macro);
//     (d) set_utf8_result -> MArgument_setUTF8String + the malloc/free
//         ownership convention (kernel ordinarily owns; here the test
//         owns + frees).
//
// A regression that produces the correct StepStrategy enum internally
// but mis-marshals it across the LibraryLink boundary (typo in the
// hf_library.cpp dispatch wrapper, MArgument_setUTF8String pointer-slot
// mismatch, ABI breakage from a future LibraryLink header revision,
// exports.txt allowlist drop, etc.) would slip past the iter-41 in-process
// ctest and surface here.
//
// STRUCTURAL_TAUTOLOGY_ROUND_TRIP four-transcription chain at this layer:
//   step_strategy.hpp truth-table comment (spec)
//     -> handlers.cpp switch + JSON emission
//     -> hf_library.cpp dispatch wrapper (set_utf8_result)
//     -> hand-typed expected strings here (this test).
// Four independent transcriptions of the same decision rule, none reading
// the others at runtime.
//
// All 6 truth-table rows exercised, same JSON requests as iter-41 ctest;
// difference is the entry path (LibraryLink dispatch vs direct call).
//
// Tier-1 wall: ~few-ms each post dlopen, 6 rows total well under 0.5 s.

#include "WolframLibrary.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

namespace {

// Pointer type for hf_find_lr_orders (matches DLLEXPORT int hf_find_lr_orders(
// WolframLibraryData, mint, MArgument*, MArgument) in hf_library.cpp:149).
// libData is unused for find_lr_orders (handler is pure-C++ JSON-in/JSON-out),
// so nullptr is safe; see hf_library.cpp:149-198.
using HfFindLrOrdersFn =
    int (*)(WolframLibraryData, mint, MArgument*, MArgument);

struct Row {
    const char* label;
    std::string request;
    std::string expected_strategy;
};

std::string extract_strategy(const std::string& resp) {
    std::regex rx(R"~("strategy"\s*:\s*"([A-Za-z_]+)")~");
    std::smatch m;
    if (std::regex_search(resp, m, rx)) {
        return m[1].str();
    }
    return std::string("<MISSING>");
}

bool run_row(HfFindLrOrdersFn fn, const Row& r) {
    // Args[0]: MArgument whose .utf8string points to a char* slot holding
    // the input string. The library dereferences this via the
    // MArgument_getUTF8String macro (char** -> char*).
    char* input_buf = strdup(r.request.c_str());
    char* input_slot = input_buf;
    MArgument args[1];
    args[0].utf8string = &input_slot;

    // Res: MArgument whose .utf8string points to a char* slot the library
    // will write to via set_utf8_result -> MArgument_setUTF8String, which
    // assigns *Res.utf8string = malloc'd buf. We free that buf below to
    // simulate the kernel-side disown convention.
    char* result_slot = nullptr;
    MArgument res;
    res.utf8string = &result_slot;

    int rc = fn(nullptr /*libData*/, 1 /*Argc*/, args, res);
    if (rc != LIBRARY_NO_ERROR) {
        std::cerr << "[FAIL] " << r.label
                  << "  LibraryLink rc=" << rc << '\n';
        free(input_buf);
        if (result_slot) free(result_slot);
        return false;
    }

    if (!result_slot) {
        std::cerr << "[FAIL] " << r.label
                  << "  LibraryLink returned NO_ERROR but result slot is null\n";
        free(input_buf);
        return false;
    }

    const std::string resp(result_slot);
    free(input_buf);
    free(result_slot);

    const std::string got = extract_strategy(resp);
    if (got == r.expected_strategy) {
        std::cout << "[PASS] " << r.label
                  << "  strategy=" << got << '\n';
        return true;
    }
    std::cerr << "[FAIL] " << r.label
              << "  expected=" << r.expected_strategy
              << "  got=" << got
              << "\n        response: " << resp << '\n';
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    // argv[1] (optional) = absolute path to libhyperflint_librarylink.dylib.
    // CMake passes the build-tree path explicitly so the test is hermetic
    // and does not depend on dist/macos-arm64/ being up to date.
    const char* dylib_path =
        (argc > 1) ? argv[1] : "libhyperflint_librarylink.dylib";

    void* handle = dlopen(dylib_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::cerr << "[FAIL] dlopen " << dylib_path << ": "
                  << dlerror() << '\n';
        return 1;
    }

    HfFindLrOrdersFn fn = reinterpret_cast<HfFindLrOrdersFn>(
        dlsym(handle, "hf_find_lr_orders"));
    if (!fn) {
        std::cerr << "[FAIL] dlsym hf_find_lr_orders: " << dlerror() << '\n';
        dlclose(handle);
        return 1;
    }

    const std::string lr_polys =
        R"~("xvars":["x1","x2"],"polys":["x1","x2","x1+x2"])~";
    const std::string nolr_polys =
        R"~("xvars":["x1","x2"],"polys":["1+x1^2+x2^2"])~";

    const std::vector<Row> rows = {
        // Row 1: lr_found=true, db=1, hint default Lungo -> LR_NoOpt
        {"row1_lr_db1_lungo",
         "{\"op\":\"find_lr_orders\"," + lr_polys + "}",
         "LR_NoOpt"},

        // Row 2: lr_found=true, db=1, hint=Espresso -> LR_NoOpt
        {"row2_lr_db1_espresso",
         "{\"op\":\"find_lr_orders\"," + lr_polys +
             ",\"method_lr_hint\":\"Espresso\"}",
         "LR_NoOpt"},

        // Row 3: lr_found=true, db=2 (algebraic_letters), hint default
        // Lungo -> LR_OptOrdered
        {"row3_lr_db2_lungo",
         "{\"op\":\"find_lr_orders\"," + lr_polys +
             ",\"algebraic_letters\":true}",
         "LR_OptOrdered"},

        // Row 4: lr_found=true, db=2, hint=Espresso -> LR_OptOrdered
        {"row4_lr_db2_espresso",
         "{\"op\":\"find_lr_orders\"," + lr_polys +
             ",\"algebraic_letters\":true,\"method_lr_hint\":\"Espresso\"}",
         "LR_OptOrdered"},

        // Row 5: lr_found=false, db=1, hint default Lungo -> Fubini_Lungo
        {"row5_nolr_lungo",
         "{\"op\":\"find_lr_orders\"," + nolr_polys + "}",
         "Fubini_Lungo"},

        // Row 6: lr_found=false, db=1, hint=Espresso -> Fubini_Espresso
        {"row6_nolr_espresso",
         "{\"op\":\"find_lr_orders\"," + nolr_polys +
             ",\"method_lr_hint\":\"Espresso\"}",
         "Fubini_Espresso"},
    };

    int n_pass = 0;
    int n_fail = 0;
    for (const auto& r : rows) {
        if (run_row(fn, r)) ++n_pass; else ++n_fail;
    }

    std::cout << "Summary: " << n_pass << " PASS / "
              << n_fail << " FAIL out of " << rows.size() << '\n';
    dlclose(handle);
    return (n_fail == 0) ? 0 : 1;
}
