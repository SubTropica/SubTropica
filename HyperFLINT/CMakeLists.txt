# HyperFLINT build configuration.
#
# Minimum CMake 3.20 (for modern FindPkgConfig behavior and target
# properties). FLINT is located via pkg-config; Homebrew ships a .pc
# file so this works out of the box on macOS.

cmake_minimum_required(VERSION 3.20)
project(HyperFLINT
    VERSION 0.1.0
    DESCRIPTION "C++ port of HyperIntica / HyperInt on FLINT"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Build-variant identification (iter-29 Track 5.1 PASS) ----
# HF_BUILD_VARIANT is set by CMakePresets.json to the active preset's
# name (release-portable, release-tuned, asan, tsan, etc.). Compiled
# into the hyperflint-cli binary as HF_BUILD_VARIANT_STR and surfaced
# via `hyperflint --version`. A ctest assertion (hf-build-variant-stamp)
# verifies the stamp at build-time. Allows post-hoc attribution of a
# binary back to its build configuration for benchmark provenance.
set(HF_BUILD_VARIANT "unknown" CACHE STRING
    "Build-variant name (auto-set by CMakePresets.json). \
Reported by hyperflint --version for runtime attribution.")

# ---- Sanitizers ----
# HF_ASAN=ON enables -fsanitize=address,undefined for the entire
# project. Use this when reproducing memory-corruption symptoms
# (e.g. non-deterministic SEGVs, stale references). The Phase
# 6d-v-vi-0 SEGV diagnosis used this build mode to localize a
# heap-use-after-free in integrate_ii within minutes.
#
# Recipe:
#   cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DHF_ASAN=ON
#   cmake --build build-asan
#   build-asan/hyperflint eval-json < failing-input.json
option(HF_ASAN "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(HF_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "HF_ASAN enabled (address + undefined sanitizers)")
endif()

# HF_TSAN=ON enables -fsanitize=thread for validating the outer-loop
# parallelism plan. Used to probe FLINT's internal concurrency safety
# (python-flint has flagged fmpq_mpoly_ctx_t may carry mutable caches);
# must pass before OpenMP lands.
#
# Recipe:
#   cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DHF_TSAN=ON
#   cmake --build build-tsan
option(HF_TSAN "Enable ThreadSanitizer" OFF)
if(HF_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=thread)
    message(STATUS "HF_TSAN enabled (thread sanitizer)")
endif()

# -------- Dependencies --------
find_package(PkgConfig REQUIRED)
pkg_check_modules(FLINT REQUIRED IMPORTED_TARGET flint)
find_library(GMP_LIB NAMES gmp HINTS ${FLINT_LIBRARY_DIRS})

# OpenMP — parallelizes the outer entry loop in integration_step.
# Optional: HF_OPENMP=OFF to build single-threaded (for debugging or
# environments without OpenMP). Apple's system clang ships with
# libomp at /opt/homebrew; we rely on brew's installation.
option(HF_OPENMP "Enable OpenMP parallelism" ON)
# HOISTED 2026-06-05 (was declared at the mimalloc link block ~line 1305,
# AFTER the if(HF_MIMALLOC)-gated test-target definitions at ~line 1119: on a
# FRESH cache the variable was undefined there, the targets were skipped, and
# the later link block -- where the option had defaulted ON -- referenced the
# missing target ("Cannot specify link libraries for target ... not built by
# this project").  Preset builds masked this by seeding -DHF_MIMALLOC.  Same
# variable-ordering class as the v1.2.0 GMP/MPFR_STATIC_ARCHIVE CI bug.
option(HF_MIMALLOC "Link CLI with mimalloc allocator (tst1/tst2 ~10% win)" ON)
if(HF_OPENMP)
    find_package(OpenMP COMPONENTS CXX)
    if(NOT OpenMP_CXX_FOUND)
        message(WARNING "HF_OPENMP=ON but OpenMP not found; "
            "disabling parallelism (falling back to serial). "
            "On macOS: `brew install libomp`.")
        set(HF_OPENMP OFF)
    else()
        message(STATUS "OpenMP enabled (${OpenMP_CXX_VERSION})")
    endif()
endif()

# -------- HF version string (eval-json + LibraryLink) --------
# Track 8.1 (iter-43): moved earlier in the file so the library
# targets (hyperflint, hyperflint_nomp, hyperflint_nomp_sd) can stamp
# HF_VERSION_STRING into the eval-json response built in
# src/bridge/handlers.cpp.  Previously this cache var lived inside the
# LibraryLink-only section (~line 1100) and was attached as a PRIVATE
# compile-def to the hyperflint_librarylink dylib; that only stamped
# the LibraryLink callback TU (hf_library.cpp), not handlers.cpp.
set(HF_VERSION "1.2.0" CACHE STRING
    "Version string baked into the LibraryLink hf_version() entry point \
and into the eval-json response (schema_version=1 / hf_version=...). \
Mma checks this against $SubTropicaVersion at load time and may also \
warn on mismatch at every eval-json roundtrip (Track 8.1).")

# -------- Library target --------
# Grows over Phases 1-10. Phase 0 implements the Poly wrapper + factor.
set(HF_SOURCES
    src/core/poly.cpp
    src/core/rat.cpp
    src/core/factored_rat.cpp
    src/core/rat_scalar.cpp
    src/core/rat_split.cpp
    src/core/sym_coef_split.cpp
    src/core/symcoef.cpp
    src/core/addpf_probe.cpp
    src/core/period_table.cpp
    src/reduce/period_scratch.cpp
    src/core/zw_table.cpp
    # HF FF Phase 5 §E Step E.2-impl-2 (iter-60-β): operator-call memoization.
    # operator_memo.cpp owns the per-op cache singletons + env-gate predicates
    # + counter_replay shims; canonical_signature.cpp owns the XXH3-seeded
    # canonical-key factories. iter-60 ships these TUs in library-linked
    # form but NO production callsite invokes them yet; iter-61 lands the
    # per-op wraps at the 5 boundaries (Rat::add, reduce_inplace,
    # linear_factors, partial_fractions, transform_shuffle).
    # See notes/.../lever_5_4_op_call_memoization/implementation/implementation_memo.md.
    src/core/operator_memo.cpp
    src/core/canonical_signature.cpp
    src/algebra/algebraic_letters.cpp
    src/algebra/linear_factors.cpp
    src/algebra/partial_fractions.cpp
    src/algebra/shuffle.cpp
    src/algebra/convert.cpp
    src/algebra/diff.cpp
    src/series/expansions.cpp
    src/series/mpl_sum.cpp
    src/series/hlog_series.cpp
    src/series/mpl_series.cpp
    src/series/laurent.cpp
    src/integrator/regularize.cpp
    src/integrator/differentiate.cpp
    src/integrator/transform.cpp
    src/integrator/primitive.cpp
    src/integrator/integration_step.cpp
    src/integrator/gcd_dispatch.cpp
    src/integrator/section_6d_dfs_cap.cpp
    src/integrator/hyper_int.cpp
    src/integrator/lr_search.cpp
    src/algebra/euler_chi.cpp     # Doppio port phase 1 (2026-06-05): DK chi counter
    src/algebra/euler_filter.cpp  # Doppio port phase 2: chi-drop letter filter
    src/integrator/lr_scan.cpp    # Doppio port phase 3: projective gauge scan + conic/carry keep-rules
    src/integrator/ctx_probe.cpp
    src/integrator/sharded_flat_map.cpp
    src/integrator/step_strategy.cpp  # Track 6.3 iter-35
    src/bridge/handlers.cpp
    src/bridge/c_abi.cpp  # Track 8.1b chunk-2 (iter-47): stable C ABI surface
    src/reduce/mzv_reduce.cpp
    src/reduce/mzv_expansion.cpp   # HF basis-ctx campaign (2026-05-28); design.md §5.1
    src/reduce/periods.cpp
    src/reduce/break_up_contour.cpp
    src/runtime/hf_thread_num.cpp
    src/runtime/narrow_ctx_flag.cpp
    src/runtime/rat_split_verify.cpp
    src/runtime/scalar_rep.cpp
    src/runtime/scs_roundtrip.cpp
    src/runtime/zw_aggregate.cpp
    src/diagnostics/operand_sparsity.cpp
    src/diagnostics/step_trace_rss.cpp
    src/diagnostics/integration_node_rss.cpp
    src/diagnostics/structural_sharing_probe.cpp
    src/instrumentation/dag_hashcons_probe.cpp
    src/instrumentation/mpz_pool_probe.cpp
    src/convert/expr.cpp
    src/convert/parse.cpp
    src/convert/convert_hlog.cpp)

add_library(hyperflint STATIC ${HF_SOURCES})
target_include_directories(hyperflint PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
# HF FF Phase 5 §E Step E.2-impl-2 (iter-60-β.1): xxHash header-only library
# vendored at third_party/xxhash/xxhash.h (BSD-2-Clause; v0.8.2 verbatim).
# Used by src/core/canonical_signature.cpp via `#define XXH_INLINE_ALL` +
# `#include "xxhash.h"` per upstream-blessed integration pattern.
# Private include path: xxhash.h is implementation detail of canonical_signature.cpp,
# not part of the public hyperflint API surface.
target_include_directories(hyperflint PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash)
target_link_libraries(hyperflint PUBLIC PkgConfig::FLINT)
if(GMP_LIB)
    target_link_libraries(hyperflint PUBLIC ${GMP_LIB})
endif()
target_compile_features(hyperflint PUBLIC cxx_std_17)
# Track 8.1 (iter-43): bake HF_VERSION_STRING into the eval-json
# handlers TU (src/bridge/handlers.cpp) for the versioned API
# response.  PUBLIC so downstream targets (hyperflint-cli) re-see it.
target_compile_definitions(hyperflint PUBLIC
    HF_VERSION_STRING="${HF_VERSION}")
if(HF_OPENMP)
    target_link_libraries(hyperflint PUBLIC OpenMP::OpenMP_CXX)
    target_compile_definitions(hyperflint PUBLIC HF_HAVE_OPENMP=1)
    # The CLI binary links brew's libomp (full LLVM OpenMP runtime), so
    # schedule(dynamic, 1) is available.  HF_OMP_FULL_SCHEDULES gates
    # the dynamic-schedule path in integration_step's parallel-for.  The
    # LibraryLink path's `hyperflint_nomp` static lib does NOT set this
    # define, because Wolfram's older libomp is missing the
    # `___kmpc_dispatch_deinit` symbol that schedule(dynamic) emits.
    target_compile_definitions(hyperflint PUBLIC HF_OMP_FULL_SCHEDULES=1)
endif()

# -------- Sibling static library: OpenMP-active, but no libomp link --------
# The LibraryLink shared library MUST NOT link Homebrew's libomp:
# WolframLibrary pulls Wolfram's own ARM64 libomp.dylib into the
# kernel process unconditionally (check with
# `otool -L $(mdfind WolframKernel)`).  Co-linking brew's libomp
# creates two OpenMP runtimes in the same process and they collide
# on the shared pthread_key_t namespace (PTHREAD_KEYS_MAX = 512 on
# macOS) — `pthread_mutex_init` fails with EINVAL after ~7 heavy
# Long-suite STIntegrate calls.
#
# Resolution: compile the sources WITH HF_HAVE_OPENMP=1 so the
# `#pragma omp` directives stay active, but do NOT link libomp.
# The emitted `_omp_get_thread_num` / `___kmpc_*` calls become
# undefined symbols in libhyperflint_nomp.a; the shared dylib
# resolves them at runtime via `-undefined dynamic_lookup` against
# Wolfram's already-loaded libomp.  One OMP runtime, full
# parallelism, no collision.
#
# The module is called `hyperflint_nomp` for historical reasons (it
# used to truly have no OMP); today its only difference from
# `hyperflint` is that OMP symbols are externally resolved rather
# than baked in via brew's libomp.
add_library(hyperflint_nomp STATIC ${HF_SOURCES})
target_include_directories(hyperflint_nomp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hyperflint_nomp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash)
target_link_libraries(hyperflint_nomp PUBLIC PkgConfig::FLINT)
if(GMP_LIB)
    target_link_libraries(hyperflint_nomp PUBLIC ${GMP_LIB})
endif()
target_compile_features(hyperflint_nomp PUBLIC cxx_std_17)
set_target_properties(hyperflint_nomp PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Track 8.1 (iter-43): see hyperflint target above for rationale.
target_compile_definitions(hyperflint_nomp PUBLIC
    HF_VERSION_STRING="${HF_VERSION}")
if(HF_OPENMP)
    # HF_HAVE_OPENMP=1 turns on the `#pragma omp` blocks in
    # integration_step.cpp; OMP compile flags come from
    # OpenMP::OpenMP_CXX's interface-only properties (headers + -fopenmp),
    # but we intentionally skip its transitive link lib so libomp
    # doesn't end up as a hard dep of the shared dylib.
    target_compile_definitions(hyperflint_nomp PUBLIC HF_HAVE_OPENMP=1)
    # Bring in the -fopenmp compile flag and the omp.h include
    # directory without the transitive link library.  FindOpenMP sets
    # INTERFACE_COMPILE_OPTIONS (`-fopenmp`), INTERFACE_INCLUDE_DIRECTORIES
    # (location of omp.h), and INTERFACE_LINK_LIBRARIES (libomp).  We
    # want the first two, not the third.
    get_target_property(_omp_compile_opts OpenMP::OpenMP_CXX INTERFACE_COMPILE_OPTIONS)
    if(_omp_compile_opts)
        target_compile_options(hyperflint_nomp PUBLIC ${_omp_compile_opts})
    endif()
    get_target_property(_omp_include_dirs OpenMP::OpenMP_CXX INTERFACE_INCLUDE_DIRECTORIES)
    if(_omp_include_dirs)
        target_include_directories(hyperflint_nomp PUBLIC ${_omp_include_dirs})
    endif()
endif()

# HF_LIBRARYLINK and HF_LIBRARYLINK_STATIC_DEPS are declared here — before
# the add_library(hyperflint_nomp_sd) block — so that both variables are
# defined on a fresh configure (empty cmake cache).  cmake processes this
# file top-to-bottom; if option() declarations only appeared later (in the
# LibraryLink build section ~700 lines below), the if() guard here would
# see undefined/false variables and silently skip building hyperflint_nomp_sd,
# causing a link failure for libhyperflint_librarylink.dylib.
# The full LibraryLink build section below depends on these same values.
option(HF_LIBRARYLINK "Build the Mathematica LibraryLink shared library" ON)
option(HF_LIBRARYLINK_STATIC_DEPS
    "Static-link FLINT/GMP/MPFR into the LibraryLink dylib (Mma ABI safety)"
    ON)
option(HF_CLI_STATIC_DEPS
    "Static-link FLINT/GMP/MPFR into the CLI binary (portable distribution)"
    OFF)

# Static FLINT/GMP/MPFR archive paths. Defined HERE -- before the CLI link
# (hyperflint-cli, below) and the LibraryLink dylib (further down) -- because
# CMake evaluates top-to-bottom. Defining them only near the LibraryLink
# section (as before) left them EMPTY at the earlier CLI link, silently
# dropping the GMP/MPFR archives and causing undefined _mpfr_*/___gmpz_*
# symbols at link (exit 2) whenever the GMP/MPFR paths were not seeded via -D
# (the arm64 CI leg). CI seeds HF_FLINT_STATIC_ARCHIVE via -D; the GMP/MPFR
# defaults below cover the arm64 runner, and the x86_64 leg overrides them via
# -D for /usr/local.
set(HF_FLINT_STATIC_ARCHIVE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/macos-arm64/libflint.a"
    CACHE FILEPATH "Static libflint.a archive (LFS-tracked; see HyperFLINT/third_party/README.md to rebuild)")
set(HF_GMP_STATIC_ARCHIVE  "/opt/homebrew/lib/libgmp.a"
    CACHE FILEPATH "Static libgmp.a archive (Homebrew default)")
set(HF_MPFR_STATIC_ARCHIVE "/opt/homebrew/lib/libmpfr.a"
    CACHE FILEPATH "Static libmpfr.a archive (Homebrew default)")

# -------- Static-deps variant for the CLI (portable distribution) --------
# When HF_CLI_STATIC_DEPS=ON, build a sibling static library that carries
# FLINT/GMP/MPFR headers but no transitive shared-library link (same
# principle as hyperflint_nomp_sd for LibraryLink).  The CLI binary then
# links this variant plus the three static archives, so the final
# hyperflint binary has no /opt/homebrew references and runs on any
# macOS arm64 machine with libomp as its only dyld dependency.
# NOTE: requires libflint.a built without merged compilation units
# (WANT_LTO=1 in FLINT's autotools make) so cross-module static symbols
# are globally visible; the default merged-objects archive produces
# undefined-symbol errors at link time on Mach-O.
if(HF_CLI_STATIC_DEPS)
    add_library(hyperflint_sd STATIC ${HF_SOURCES})
    target_include_directories(hyperflint_sd PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${FLINT_INCLUDE_DIRS})
    target_include_directories(hyperflint_sd PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash)
    # NOTE: deliberately NOT target_link_libraries(... PkgConfig::FLINT).
    target_compile_features(hyperflint_sd PUBLIC cxx_std_17)
    # Track 8.1 (iter-43): see hyperflint target above for rationale.
    target_compile_definitions(hyperflint_sd PUBLIC
        HF_VERSION_STRING="${HF_VERSION}")
    if(HF_OPENMP)
        target_compile_definitions(hyperflint_sd PUBLIC HF_HAVE_OPENMP=1 HF_OMP_FULL_SCHEDULES=1)
        target_link_libraries(hyperflint_sd PUBLIC OpenMP::OpenMP_CXX)
        if(_omp_compile_opts)
            target_compile_options(hyperflint_sd PUBLIC ${_omp_compile_opts})
        endif()
        if(_omp_include_dirs)
            target_include_directories(hyperflint_sd PUBLIC ${_omp_include_dirs})
        endif()
    endif()
endif()

# -------- Static-deps variant of hyperflint_nomp (LibraryLink ABI safety) --------
# Identical sources / OMP behaviour to hyperflint_nomp, but takes
# FLINT headers via include path only — no transitive PkgConfig::FLINT
# link.  Combined with -fvisibility=hidden, this lets the LibraryLink
# dylib link against a static libflint.a (whose symbols stay private to
# the dylib) instead of the shared libflint.22.0.dylib that collides
# with Mma's bundled libflint.17.dylib in dyld.  See
# notes/benchmark_1massDB/segfault_diagnosis.md for the root cause.
if(HF_LIBRARYLINK AND HF_LIBRARYLINK_STATIC_DEPS)
    add_library(hyperflint_nomp_sd STATIC ${HF_SOURCES})
    target_include_directories(hyperflint_nomp_sd PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${FLINT_INCLUDE_DIRS})
    target_include_directories(hyperflint_nomp_sd PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash)
    # NOTE: deliberately NOT target_link_libraries(... PkgConfig::FLINT).
    target_compile_features(hyperflint_nomp_sd PUBLIC cxx_std_17)
    set_target_properties(hyperflint_nomp_sd PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    # Track 8.1 (iter-43): see hyperflint target above for rationale.
    target_compile_definitions(hyperflint_nomp_sd PUBLIC
        HF_VERSION_STRING="${HF_VERSION}")
    # -fvisibility=hidden default-hides every symbol; the LibraryLink
    # dylib will explicitly export only its hf_* + LibraryLink callbacks
    # via -exported_symbols_list at link time.
    target_compile_options(hyperflint_nomp_sd PRIVATE -fvisibility=hidden)
    if(HF_OPENMP)
        target_compile_definitions(hyperflint_nomp_sd PUBLIC HF_HAVE_OPENMP=1)
        if(_omp_compile_opts)
            target_compile_options(hyperflint_nomp_sd PUBLIC ${_omp_compile_opts})
        endif()
        if(_omp_include_dirs)
            target_include_directories(hyperflint_nomp_sd PUBLIC ${_omp_include_dirs})
        endif()
    endif()
endif()

# -------- CLI --------
add_executable(hyperflint-cli
               bridge/cli/main.cpp
               bridge/cli/gmp_mimalloc_init.cpp)
# HF_CLI_STATIC_DEPS=ON: link the static-deps variant (no transitive
# PkgConfig::FLINT) and pull in FLINT/GMP/MPFR via direct archive link.
# Produces a portable binary with no /opt/homebrew references — only
# libomp is a dyld dependency.  Used by the release-dist preset.
# IMPORTANT: requires libflint.a built with WANT_LTO=1 (individual
# objects, not merged) to avoid Mach-O undefined-symbol errors caused by
# cross-module static function references in FLINT's merged-object archive.
if(HF_CLI_STATIC_DEPS)
    target_link_libraries(hyperflint-cli PRIVATE hyperflint_sd)
    if(NOT EXISTS "${HF_FLINT_STATIC_ARCHIVE}")
        message(FATAL_ERROR
            "HF_CLI_STATIC_DEPS=ON but HF_FLINT_STATIC_ARCHIVE not found at "
            "${HF_FLINT_STATIC_ARCHIVE}.\n"
            "Build it with:  ./scripts/build_static_flint.sh $PWD/build-flint-static\n"
            "then re-run cmake with:\n"
            "  -DHF_FLINT_STATIC_ARCHIVE=$PWD/build-flint-static/lib/libflint.a")
    endif()
    # Link static archives without -force_load so the linker only pulls
    # in the object files it needs (avoids loading all of FLINT's huge archive).
    target_link_libraries(hyperflint-cli PRIVATE
        "${HF_FLINT_STATIC_ARCHIVE}"
        "${HF_MPFR_STATIC_ARCHIVE}"
        "${HF_GMP_STATIC_ARCHIVE}")
else()
    target_link_libraries(hyperflint-cli PRIVATE hyperflint)
endif()
set_target_properties(hyperflint-cli PROPERTIES OUTPUT_NAME hyperflint)
# iter-29 Track 5.1: stamp the active preset name into the binary so
# `hyperflint --version` can report it. CMakePresets.json sets the
# cache var per preset; the ctest assertion hf-build-variant-stamp
# verifies the round-trip.
target_compile_definitions(hyperflint-cli PRIVATE
    HF_BUILD_VARIANT_STR="${HF_BUILD_VARIANT}")
# Phase-1.2 retrofit: gmp_mimalloc_init.cpp references mi_malloc / mi_realloc
# / mi_free / mi_calloc / mi_aligned_alloc as weak externs. With HF_MIMALLOC=ON
# the linker resolves them via -force_load,libmimalloc.a. With HF_MIMALLOC=OFF
# the symbols are undefined; pass -Wl,-undefined,dynamic_lookup so they remain
# weak-undefined and resolve to nullptr at runtime (the init function then
# silently no-ops, matching the existing mi_collect / mi_process_info pattern
# in src/integrator/hyper_int.cpp).
if(APPLE AND NOT HF_MIMALLOC)
    target_link_options(hyperflint-cli PRIVATE
        "-Wl,-U,_mi_malloc"
        "-Wl,-U,_mi_realloc"
        "-Wl,-U,_mi_free"
        "-Wl,-U,_mi_calloc"
        "-Wl,-U,_mi_aligned_alloc"
        "-Wl,-U,_mi_collect"
        "-Wl,-U,_mi_process_info")
endif()

# -------- Microbench: Poly::mul wide-vs-narrow --------
# Diagnostic for the Avenue A step-4 +240 us/call regression.
# Compares the wide-ctx fmpq_mpoly_mul path against the full Avenue A
# round-trip (PolyCtx narrow ctor + 2 transplants + mul + transplant
# back) at varied (nvars_wide, nvars_used, la, lb).
add_executable(hyperflint-bench-polymul bench/bench_polymul.cpp)
target_link_libraries(hyperflint-bench-polymul PRIVATE hyperflint)

add_executable(hyperflint-bench-polymul-v2 bench/bench_polymul_v2.cpp)
target_link_libraries(hyperflint-bench-polymul-v2 PRIVATE hyperflint)

# 2026-05-01 GCD variant bench (Brown vs Zippel vs Hensel vs Subresultant).
# Tests the dispatcher (cofactors) against each underlying algorithm
# directly on the Avenue-H pair file.
add_executable(hyperflint-bench-gcd-variants bench/bench_gcd_variants.cpp)
target_link_libraries(hyperflint-bench-gcd-variants PRIVATE hyperflint)

# NOTE: bench/bench_gcd_variants_v2.cpp was a closed Avenue H investigation
# whose source never made it into the dev/main merge (commit 9dd0e77d2).
# Target removed 2026-05-02 to unblock fresh builds. Restore from a perf
# worktree if revisiting Avenue H GCD variant sweeps.

# Branch I (2026-05-01 evening) POC: fmpz_mpoly_q vs current Rat
# (pair-of-fmpq_mpoly + reduce_inplace).  A/B reduce + add bench on
# the 50-pair Avenue-H dump.  Decision rule: ratio_BoverA ≤ 0.80 on
# add aggregate → spec a full Rat-rep swap.
add_executable(hyperflint-bench-fmpz-mpoly-q bench/bench_fmpz_mpoly_q.cpp)
target_link_libraries(hyperflint-bench-fmpz-mpoly-q PRIVATE hyperflint)

# Branch I POC v2 (post-reviewer-REVISE): canonical-quad operands
# from production hot path (HF_DUMP_RAT_QUADS).  Bench `Rat::add`
# equivalents on actual workload, not the small-vars reduce-input
# proxy.
add_executable(hyperflint-bench-fmpz-mpoly-q-quads
               bench/bench_fmpz_mpoly_q_quads.cpp)
target_link_libraries(hyperflint-bench-fmpz-mpoly-q-quads PRIVATE hyperflint)

# Branch I Pre-flight 8 (R14 #8) — _fmpz_mpoly_q_add (underscore
# primitive) variant of the canonical-quad bench.  Tests the
# *actually designed* implementation path (the one that lets HF wrap
# with a narrow-ctx hoist to preserve the d=3 size-gate lever),
# not the high-level fmpz_mpoly_q_add measured by the v4 quads bench.
# Bench-only; production untouched.
add_executable(hyperflint-bench-fmpz-mpoly-q-quads-v5
               bench/bench_fmpz_mpoly_q_quads_v5.cpp)
target_link_libraries(hyperflint-bench-fmpz-mpoly-q-quads-v5 PRIVATE hyperflint)

# Branch I Pre-flight A (2026-05-01): per-quad raw-mul A/B between
# fmpq_mpoly_mul (current Rat) and fmpz_mpoly_mul (post-rep-swap) on
# the three cross-mults that Rat::add performs.  Probes whether the
# rep-swap has a *multiplicative* tailwind beyond the +75.5% it shows
# on add aggregate.  Bench-only; production untouched.
add_executable(hyperflint-bench-fmpz-vs-fmpq-mul
               bench/bench_fmpz_vs_fmpq_mul.cpp)
target_link_libraries(hyperflint-bench-fmpz-vs-fmpq-mul PRIVATE hyperflint)

# Tier B Pre-flight 1 (kickoff 2026-05-02): per-call FF-reconstruction
# FIXED-overhead micro-bench.  Method A = fmpq_mpoly_gcd_cofactors on
# heavy quad operands; Method B = nmod_mpoly_ctx_init x N + multi-CRT
# init+precompute+clear (no actual GCD).  Gate: agg ratio_B/A < 0.20
# means the per-call FIXED overhead leaves >= 80% of the legacy GCD
# wall as headroom for the modular GCD work itself.  Bench-only;
# production untouched.
add_executable(hyperflint-bench-ff-ctx-init
               bench/bench_ff_ctx_init.cpp)
target_link_libraries(hyperflint-bench-ff-ctx-init PRIVATE hyperflint)

# Tier B Pre-flight 2 (kickoff 2026-05-02): full FF reconstruction
# vs legacy fmpq_mpoly_gcd_cofactors on heavy operand quads.  Method
# B: lift fmpq pair to fmpz_mpoly, project to N primes, nmod_gcd_cof
# per prime, CRT-lift {g, abar, bbar}, rational reconstruct.  Method
# A: legacy fmpq_mpoly_gcd_cofactors.  Gate: agg ratio_B/A < 0.80
# on the >=50-monomial subset → GO.  Bench-only; production untouched.
add_executable(hyperflint-bench-ff-full
               bench/bench_ff_full.cpp)
target_link_libraries(hyperflint-bench-ff-full PRIVATE hyperflint)

# C2 Pre-flight (post-Tier-B closure, 2026-05-02): per-call FF factor
# floor.  Method A = fmpq_mpoly_factor on quad denominators; Method B
# = single-prime nmod_mpoly_factor (lift + project + factor).  If
# Method B alone is already > 10% of Method A, then 10*B + CRT +
# reconstruct cannot beat — closes C2 by the same logic that closed
# C1 (Tier B, gcd_cofactors).  Bench-only; production untouched.
add_executable(hyperflint-bench-ff-factor
               bench/bench_ff_factor.cpp)
target_link_libraries(hyperflint-bench-ff-factor PRIVATE hyperflint)

# Phase 2-A2 PF3 mock (2026-05-03, chain-20 autonomous session): the
# real campaign-shaping gate per pf1_verdict.md. Times Rat::add (Tier
# A1 production path at nvars >= HF_REPSWAP_NVARS_MIN) against an SBO
# addViaSparse round-trip (lift 4 + 3 muls + 1 gcd + 2 lowers via
# public FLINT API) on the same canonical operand quads.  Bench-only.
add_executable(hyperflint-bench-pf3-mock bench/bench_pf3_mock.cpp)
target_link_libraries(hyperflint-bench-pf3-mock PRIVATE hyperflint)

# Phase 2-A2 PF1 (2026-05-03, chain-19 autonomous session): SBO sparse-
# per-monomial lift/lower microbench.  Pre-flight gate before any rep-
# swap source change per R29 C2.  Two modes: synthetic (k=3, t=100,
# 1000 reps; gate <=100 ns/term/{lift,lower}) and empirical (every row
# of notes/hf_memory_plan/phase2_pre/sparsity_*.jsonl; gate mean lift
# <=1 ms/call).  Bench-only; production untouched.
add_executable(hyperflint-bench-sbo-lift
               bench/bench_sbo_lift.cpp)
target_link_libraries(hyperflint-bench-sbo-lift PRIVATE hyperflint)

# R22 Step 1 smoke test (2026-05-03, chain-13 autonomous session):
# confirm fmpq_mpoly_set_str_pretty returns nonzero (does NOT abort)
# on unknown-variable input.  Load-bearing assumption for the
# sentinel-tolerance design (notes/hf_rss_campaign/r22_step1_sentinel_design.md).
# Bench-only; production untouched.
add_executable(hyperflint-bench-parse-unknown-var
               bench/bench_parse_unknown_var.cpp)
target_link_libraries(hyperflint-bench-parse-unknown-var
                      PRIVATE hyperflint)

# HF MZV-rewrite C-prep.3 microbench (notes/hf_mzv_rewrite_design_2026-05-05/
# three_month_checkpoint/measurement_plan_2026-05-06.md §4 row C-prep.3,
# reviewer E2 advisory iter-22 amendment commit 2d76917b4): pre-C0 sizing
# probe on SymCoefSplit::add(a,b) -> canonicalize over synthetic
# non-W-empty inputs. Catches O(N log N · log K) blow-up before
# per-B-commit caller-side refactors begin. Bench-only.
add_executable(hyperflint-bench-sym-coef-split-canon
               bench/bench_sym_coef_split_canon.cpp)
target_link_libraries(hyperflint-bench-sym-coef-split-canon
                      PRIVATE hyperflint)

# Tier 1.6a self-test: SymCoef::merge_sorted_canonical correctness.
# Always built; cheap to compile (one .cpp, links the existing lib).
add_executable(hyperflint-test-merge-sorted-canonical
               test/test_merge_sorted_canonical.cpp)
target_link_libraries(hyperflint-test-merge-sorted-canonical
                      PRIVATE hyperflint)

# Branch I rep-swap (2026-05-01): Layer-1 per-call semantic-equivalence
# test for the Rat::add migration to _fmpz_mpoly_q_add.  Runs Rat::add
# (new path) and Rat::add_legacy (cross-mult+gcd_cofactors) on each of
# the 200 canonical operand quads in
# notes/3l3pt_branch_I_fmpz_q_poc/rat_quads.txt and verifies the two
# results coincide after fmpz_mpoly_q_canonicalise (the canonical form
# that absorbs the Avenue-I content-distribution difference between the
# two paths).  Mandatory before any wall-time signal collection.
add_executable(hyperflint-test-rat-add-equivalence
               test/test_rat_add_equivalence.cpp)
target_link_libraries(hyperflint-test-rat-add-equivalence
                      PRIVATE hyperflint)

# FactoredRat (2026-05-30): Phase B0 standalone unit test for the
# factored-denominator coefficient type. Links the full hyperflint lib;
# run directly (no add_test), exit 0 = pass.
add_executable(hyperflint-test-factored-rat
               test/test_factored_rat.cpp)
target_link_libraries(hyperflint-test-factored-rat
                      PRIVATE hyperflint)

# B1 regression oracle (2026-05-30): partial_fractions reconstruction
# characterization. Pins the current partial_fractions behavior via the
# value round-trip identity  P + Σ c_{i,j}/(var-a_i)^j == f  so the Phase
# B1 FactoredRat refactor is provably value-preserving. Mirrors the
# factored-rat block idiom; run directly, exit 0 = pass.
add_executable(hyperflint-test-period-tuples
               test/unit/test_period_tuples.cpp)
target_link_libraries(hyperflint-test-period-tuples PRIVATE hyperflint)
add_test(NAME hf-period-tuples COMMAND hyperflint-test-period-tuples)

# Doppio port phase 1 (2026-06-05): Euler-characteristic counter (DK
# cleared-dlog core, msolve grevlex GBs over temp files).  The source is
# compiled INTO the test target (not the hyperflint lib) so the main
# library's source list — and other sessions' builds — are untouched
# until the Doppio port lands for real.  Oracle battery from
# t20_cleared_dlog.wl; needs `msolve` on PATH (or HF_MSOLVE_PATH).
add_executable(hyperflint-test-euler-chi
               test/test_euler_chi.cpp)
target_link_libraries(hyperflint-test-euler-chi PRIVATE hyperflint)
add_test(NAME hf-euler-chi COMMAND hyperflint-test-euler-chi)

# Doppio port phase 2 (2026-06-05): chi-drop letter filter + the
# HF_EULER_FILTER hook in find_lr_orders.  Oracle battery: t18/t20
# genuineness verdicts (box + split-pair) and the triple-verified uq5
# intermediate `yb` fictitious catch; OFF-mode parity asserted.
add_executable(hyperflint-test-euler-filter
               test/test_euler_filter.cpp)
target_link_libraries(hyperflint-test-euler-filter PRIVATE hyperflint)
add_test(NAME hf-euler-filter COMMAND hyperflint-test-euler-filter)

# Doppio port phase 3 (2026-06-05): projective gauge scan (depth n-1,
# per-gauge dehomogenized judgment) + Euler-conic strict tier + FindRoots
# carried-sqrt tier.  Mma cross-pins: uq5 Strict scan == 0 orders
# (gauge-exhaustive NOLR); FindRoots scan == 120 orders with the
# collaborator order (x2,x3,x1,x4) at gauge x5 and carriedSqrts == 2
# (t24 battery).
add_executable(hyperflint-test-lr-scan
               test/test_lr_scan.cpp)
target_link_libraries(hyperflint-test-lr-scan PRIVATE hyperflint)
add_test(NAME hf-lr-scan COMMAND hyperflint-test-lr-scan)

add_executable(hyperflint-test-partial-fractions-characterization
               test/test_partial_fractions_characterization.cpp)
target_link_libraries(hyperflint-test-partial-fractions-characterization
                      PRIVATE hyperflint)

# Phase 1 (poles streaming, 2026-05-03): ShardedFlatMap skeleton +
# empty-map invariant. Task 1.1 of the per-face RSS reduction plan.
add_executable(hyperflint-test-sharded-flat-map
               test/unit/test_sharded_flat_map.cpp)
target_link_libraries(hyperflint-test-sharded-flat-map
                      PRIVATE hyperflint)

# Phase 1 (poles streaming, 2026-05-03): Task 1.3 — PolesBucket
# flush_threshold + drain() behavioural test. Verifies the four R28
# contracts (default-off bit-identity, threshold fires, drain fires,
# atomic clear) on the new streaming hook.
add_executable(hyperflint-test-poles-bucket-flush
               test/unit/test_poles_bucket_flush.cpp)
target_link_libraries(hyperflint-test-poles-bucket-flush
                      PRIVATE hyperflint)

# Phase 2-pre.1 (rep-swap operand sparsity, 2026-05-03): unit tests
# for HF_DUMP_OPERAND_SPARSITY.  Verifies compute_sparsity on three
# canonical Poly shapes (linear, constant, mixed-sparsity), the
# should_emit cycle at rate=2, and that emit_sparsity_row writes
# parseable JSONL to stderr.
add_executable(hyperflint-test-operand-sparsity
               test/unit/test_operand_sparsity.cpp)
target_link_libraries(hyperflint-test-operand-sparsity
                      PRIVATE hyperflint)

# HF MZV-rewrite Phase A commit (2): round-trip test for the rat_split
# adapter (split_rat_by_w_monomial / recombine_rat_split).
# See notes/hf_mzv_rewrite_design_2026-05-05/design.md §3.5, §5.1.
add_executable(hyperflint-test-rat-split-roundtrip
               test/unit/test_rat_split_roundtrip.cpp)
target_link_libraries(hyperflint-test-rat-split-roundtrip
                      PRIVATE hyperflint)

# HF MZV-rewrite Phase A commit (4): ZWTable arithmetic + dedup +
# merge-determinism unit tests.
# See notes/hf_mzv_rewrite_design_2026-05-05/design.md §3.3, §3.6a.
add_executable(hyperflint-test-zw-table-basic
               test/unit/test_zw_table_basic.cpp)
target_link_libraries(hyperflint-test-zw-table-basic
                      PRIVATE hyperflint)

# HF MZV-rewrite Phase A commit (5): SymCoefSplit basic-op +
# round-trip-vs-SymCoef test.
# See notes/hf_mzv_rewrite_design_2026-05-05/design.md §3.4.
add_executable(hyperflint-test-sym-coef-split-roundtrip
               test/unit/test_sym_coef_split_roundtrip.cpp)
target_link_libraries(hyperflint-test-sym-coef-split-roundtrip
                      PRIVATE hyperflint)

# HF MZV-rewrite Phase B precondition (synthetic_w_side_corpus):
# adversarial-corpus synthetic battery. Folds adversarial L2 corpus
# seed per design v2 §5.1.
add_executable(hyperflint-test-rat-split-roundtrip-synthetic
               test/unit/test_rat_split_roundtrip_synthetic.cpp)
target_link_libraries(hyperflint-test-rat-split-roundtrip-synthetic
                      PRIVATE hyperflint)

# HF MZV-rewrite C-prep.4 (iter-27): canonical-emission writer unit
# tests. 7 tests: T1 Rat::to_string idempotency (drift-check M2);
# T2-T6 memo §6.1 #1-#5 (identity-on-canonical, order-insensitivity,
# gcd-cancellation, Pi^2 non-absorption, like-term collapse);
# T7 power_key collision under permuted insertion (drift-check M1).
# See notes/hf_mzv_rewrite_design_2026-05-05/c_prep_4_scoping_memo.md.
add_executable(hyperflint-test-sym-coef-canonical-string
               test/unit/test_sym_coef_canonical_string.cpp)

add_executable(hyperflint-test-parse-unary-precedence
               test/unit/test_parse_unary_precedence.cpp)
target_link_libraries(hyperflint-test-parse-unary-precedence PRIVATE hyperflint)
add_test(NAME hf-parse-unary-precedence
         COMMAND hyperflint-test-parse-unary-precedence)
target_link_libraries(hyperflint-test-sym-coef-canonical-string
                      PRIVATE hyperflint)

# HF MZV-rewrite C-prep.4 (iter-29 + iter-30 + iter-31 + iter-32):
# pre-C0a Rat content-normalization invariance test. 14 cases.
#
# Iter-29 shipped 4 PASS (T1, T2, T6, T8) + 6 TODO. T3 implemented
# and EMPIRICALLY FOUND a real divergence (FLINT's
# fmpq_mpoly_gcd_cofactors does not pull integer content across
# num/den), then demoted to TODO with a comment recording the
# finding for iter-30 to close.
#
# Iter-30 closed the audit: T4 + T5 promoted from TODO to PASS via
# the new public `Rat::add_repswap` method (mirrors `add_legacy`,
# bypasses static-cached env-gate lambdas). Both production hot
# paths produce byte-identical Rat::to_string at both nvars=60
# (rep-swap regime) and nvars=12 (Smirnov tst2 legacy regime) at
# unit integer content. Verdict (memo §5.4):
# `from_canonical_normalize_content` is NOT BINDING for C0a; T3's
# parse-level divergence is harness-only.
#
# Iter-31 strengthens the closure: T4b + T5b added at non-trivial
# pairwise-coprime integer content (aD content 2, bD content 3 per
# iter-30 reviewer F1 fold; internal review). Both
# backends produce byte-identical to_string under that regime;
# closure verdict upheld at content > 1 (memo §5.5).
#
# Iter-32 hardens the closure at the TRUE F2 falsifier: T4c + T5c
# added with operands aN=2*(x0+2*x1), aD=2*(x2+1), bN=3*(x3-x4),
# bD=3*(x5+x6+1) -- each input Rat carries shared num/den Z-content
# (gcd within input = 2 and 3 respectively). After cross-mult both
# num and den each carry Z-content 6; gcd-across-pair = 6. Per
# iter-31 reviewer F1 (internal review) this is the
# regime where rep-swap (`_fmpz_mpoly_q_add` per FLINT canonical-
# form spec) and legacy (`gcd_cofactors` over Q which does not pull
# integer content across pair) would in principle diverge.
# EMPIRICAL RESULT: T4c + T5c BOTH PASS at byte-identical Rat::to_
# string output. Both backends preserve the unreduced Z-content 6.
# Closure verdict empirically airtight at the actual falsifier
# regime (memo §5.6.2). T3 stays TODO permanently with audit-memo
# §5.4 cross-reference; T7/T9/T10 deferred indefinitely.
#
# Triggered by iter-28 post-commit adversarial-reviewer F1+F2
# flagging the iter-27 "Caveat 1 is benign for C0a" dismissal as
# asserted-not-argued. See c_prep_4_content_audit_memo.md §5.1
# for the parse-level divergence; §5.3 for the iter-30 closure;
# §5.5 for iter-31 strengthening at content > 1; §5.6 for iter-32
# closure-hardening at TRUE F2 falsifier + F4 parity-1 SHA-match
# re-verify + F2 parse-boundary safety rail.
# Total iter-32 outcome: 10 PASS + 4 TODO -> ctest exit 0.
add_executable(hyperflint-test-rat-content-invariance
               test/unit/test_rat_content_invariance.cpp)
target_link_libraries(hyperflint-test-rat-content-invariance
                      PRIVATE hyperflint)

# HF MZV-rewrite C0c.1 (iter-52c-Tests): OMP-determinism
# unit test for Protocol A's combined->master 2-stage canonical merge.
# Three tests: T1 stage-1 partition-perm invariance (§3.6a / §5.4 +
# A6); T2 intra-thread intern-order invariance (audit §1.4); T3
# OMP=1 vs OMP=N master equivalence (§5.4 thread-count variation).
# Iter-51 reviewer A4 (advisory; internal review).
add_executable(hyperflint-test-omp-determinism
               test/unit/test_omp_determinism.cpp)
target_link_libraries(hyperflint-test-omp-determinism
                      PRIVATE hyperflint)

# Phase 0 task 0-2 (2026-05-09): RssSample helper — per-step RSS
# sampler used by Phase 0 tasks 0-3/0-4/0-5 to instrument step-level
# and integration-node-level memory footprints.  Verifies current_kib
# > 0, peak_kib > 0, and that a 64-MB allocation is visible in current_kib.
add_executable(hyperflint-test-step-trace-rss
               test/unit/test_step_trace_rss.cpp)
target_link_libraries(hyperflint-test-step-trace-rss
                      PRIVATE hyperflint)

# Phase 0 task 0-4 (2026-05-09): IntegrationNodeRssSampler — per-
# integration-node RSS sampler.  Verifies that a 16-MiB allocation
# touched before exit_node produces rss_peak_kib_delta >= 16384 KiB.
add_executable(hyperflint-test-integration-node-rss
               test/unit/test_integration_node_rss.cpp)
target_link_libraries(hyperflint-test-integration-node-rss
                      PRIVATE hyperflint)

# Phase 0.5 Item U Stage 2 (2026-05-09): GCD dispatch_apply wrapper.
# Tests that dispatch_parallel_for iterates every entry in [begin, end),
# that slot indices are in [0, max_slots), and that gcd_dispatch_available()
# returns the expected value for the current platform.
add_executable(hyperflint-test-gcd-dispatch
               test/unit/test_gcd_dispatch.cpp)
target_link_libraries(hyperflint-test-gcd-dispatch
                      PRIVATE hyperflint)

# HF FF Phase 5 §A.1 iter-49: Probe A1 REQ-1 init unit test. Exercises the
# GMP-allocator wrap layer added in src/instrumentation/dag_hashcons_probe.cpp.
# See test/unit/test_dag_hashcons_probe_init.cpp.
add_executable(hyperflint-test-dag-hashcons-probe-init
               test/unit/test_dag_hashcons_probe_init.cpp)
target_link_libraries(hyperflint-test-dag-hashcons-probe-init
                      PRIVATE hyperflint)

# HF FF Phase 5 §A.1 iter-50: REQ-2 BINDING CI test (OMP=1 vs OMP=13
# construction-path dedup-rate invariance, ±0.5 pp gate). Exercises the
# sharded shared_mutex seen-set in src/instrumentation/dag_hashcons_probe.cpp.
add_executable(hyperflint-test-construction-path-dedup-rate-omp-invariance
               test/unit/test_construction_path_dedup_rate_omp_invariance.cpp)
target_link_libraries(hyperflint-test-construction-path-dedup-rate-omp-invariance
                      PRIVATE hyperflint)

# HF FF Phase 5 §A.1 iter-50: REQ-3 prerequisite (OFF-path structural no-op).
# Asserts hf_probe_init is a no-op when HF_DAG_HASHCONS_PROBE is unset;
# verifies GMP wrap is NOT installed and dedup counters stay at zero.
add_executable(hyperflint-test-dag-hashcons-probe-off-no-op
               test/unit/test_dag_hashcons_probe_off_no_op.cpp)
target_link_libraries(hyperflint-test-dag-hashcons-probe-off-no-op
                      PRIVATE hyperflint)

# Track 4.2 iter-27 §F row 14 deliverable: predicate-false no-op unit test
# for hf::phase_timer.  Validates (a) storage invariance: 10^6 predicate-
# false ticks leave timer_wall_s bit-identically 0.0; (b) latency contract:
# <=50 ns/tick (spec target 5 ns); (c) positive control: predicate-TRUE
# tick under both parent scopes does increment.  Uses
# gcd_cofactors_narrow whose parents = {omp_parallel_for_integration_step,
# rat_reduce_inplace_narrow_branch} — neither scope is entered by the
# test loop, so the predicate is structurally false.
add_executable(hyperflint-test-phase-timer-predicate-false-no-op
               test/unit/test_phase_timer.cpp)
target_link_libraries(hyperflint-test-phase-timer-predicate-false-no-op
                      PRIVATE hyperflint)

# Track 6.3 iter-35: StepStrategy dispatch falsifier.  Reads
# test/data/step_strategy_truth_table.json, calls pick_step_strategy on
# each of 6 rows, asserts enum match.  HF_TEST_DATA_DIR macro injects the
# source-side data path so the test binary works under any preset
# binary dir.  STRUCTURAL_TAUTOLOGY_ROUND_TRIP defence:
# step_strategy.cpp does not read the JSON; this test is the only
# consumer; three independent transcriptions (header table / JSON /
# .cpp impl) gate the decision rule.
add_executable(hyperflint-test-step-strategy-dispatch
               test/unit/test_step_strategy_dispatch.cpp)
target_link_libraries(hyperflint-test-step-strategy-dispatch
                      PRIVATE hyperflint)
target_compile_definitions(hyperflint-test-step-strategy-dispatch
    PRIVATE
        HF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/data")

# Track 8.1b chunk-2 (iter-47): C ABI smoke for hf_partial_fractions +
# hf_free_string + chunk-3 stubs.  Tier-1 ctest, sub-second wall.
# Linked against libhyperflint.a so we exercise the same TU that the
# LibraryLink shared library carries; this gates the C ABI surface
# itself, not the LibraryLink wrapper (a chunk-3+ ctest will gate the
# dlopen path once the c_abi.cpp impl is wrapped by hf_library.cpp).
add_executable(hyperflint-test-c-abi-pfrac
               test/unit/test_c_abi_pfrac.cpp)
target_link_libraries(hyperflint-test-c-abi-pfrac
                      PRIVATE hyperflint)

# Track 8.1b chunk-2b (iter-48): CLI vs C ABI byte-identical snapshot
# ctest.  Locks the transport-neutral contract introduced by iter-48
# chunk-2b (handler factored into hyperflint::handlers::partial_fractions)
# by running the same `partial_fractions` request through both transports
# and asserting they are byte-identical modulo the envelope strip.
# Linked against libhyperflint.a; CLI binary path passed via add_test
# generator expression.
add_executable(hyperflint-test-c-abi-pfrac-cli-snapshot
               test/unit/test_c_abi_pfrac_cli_snapshot.cpp)
target_link_libraries(hyperflint-test-c-abi-pfrac-cli-snapshot
                      PRIVATE hyperflint)

# Track 8.1b chunk-3b (iter-51): CLI vs C ABI byte-identical snapshot
# ctest for linear_factors.  Mirrors chunk-2b's contract for the
# iter-50 chunk-3 LAND (handler factored into
# hyperflint::handlers::linear_factors): same request through both
# transports, byte-identical modulo the schema_version + hf_version
# envelope strip.
add_executable(hyperflint-test-c-abi-linear-factors-cli-snapshot
               test/unit/test_c_abi_linear_factors_cli_snapshot.cpp)
target_link_libraries(hyperflint-test-c-abi-linear-factors-cli-snapshot
                      PRIVATE hyperflint)

# Track 8.4 (iter-58): CLI vs C ABI byte-identical snapshot for
# find_lr_orders, locking the iter-58 promotion of hf_find_lr_orders
# from its iter-56 chunk_3_pending stub to a real delegation.  Unlike
# the partial_fractions / linear_factors snapshots, the handler emits
# the schema_version + hf_version envelope inline (handlers.cpp:700-
# 702) so the test strips `timing_compute_s` (the only non-deterministic
# field) rather than the envelope.
add_executable(hyperflint-test-c-abi-find-lr-orders-cli-snapshot
               test/unit/test_c_abi_find_lr_orders_cli_snapshot.cpp)
target_link_libraries(hyperflint-test-c-abi-find-lr-orders-cli-snapshot
                      PRIVATE hyperflint)

# Track 8.4-real-op-1 (iter-61): CLI vs C ABI byte-identical snapshot
# for hyperflint_sym, locking the iter-61 promotion of hf_hyperflint_sym
# from "not yet declared in c_abi.h" to a real delegating entry point
# routing through hyperflint::handlers::hyperflint_sym. Hybrid of the
# pfrac and find_lr_orders snapshot patterns: the handler's happy path
# emits a CLI-form body WITHOUT the {schema_version, hf_version}
# envelope (so the ABI side has it spliced in by c_abi.cpp::
# splice_envelope, branch 2, and this test strips it — pfrac pattern),
# AND emits a wall-clock-dependent `timing_compute_s` field (so this
# test additionally strips that field on BOTH sides — find_lr_orders
# pattern, using the iter-59-widened char class). The fixture is the
# findroots21_b W-non-empty integrator request that iter-60's
# cross-process determinism probe pre-verified is process-stable
# modulo `timing_compute_s` (8/8 PASS at commit 231a063e4). The
# mzv_reductions.json path is injected at compile time so the test
# is hermetic w.r.t. CWD.
add_executable(hyperflint-test-c-abi-hyperflint-sym-cli-snapshot
               test/unit/test_c_abi_hyperflint_sym_cli_snapshot.cpp)
target_link_libraries(hyperflint-test-c-abi-hyperflint-sym-cli-snapshot
                      PRIVATE hyperflint)
target_compile_definitions(hyperflint-test-c-abi-hyperflint-sym-cli-snapshot
                           PRIVATE
                           HF_TEST_MZV_DATA=${CMAKE_CURRENT_SOURCE_DIR}/data/mzv_reductions.json)

# Track 8.4-probe (iter-60): CLI cross-process determinism probe for
# hyperflint_sym. Standalone smoke binary (no libhyperflint link, no
# mimalloc weak externs) that invokes hyperflint-cli twice in
# independent processes on the findroots21_b W-non-empty integrator
# fixture and asserts byte-identical stdout modulo timing_compute_s.
# This is the feasibility gate iter-61 reads before promoting
# hf_hyperflint_sym from "not declared" to a real C ABI op with a
# CLI-vs-ABI snapshot ctest (same pattern as iter-58 hf_find_lr_orders).
# The mzv_reductions.json path is injected at compile time so the
# test is hermetic w.r.t. CWD.
add_executable(hyperflint-test-hyperflint-sym-response-determinism
               test/unit/test_hyperflint_sym_response_determinism.cpp)
target_compile_definitions(hyperflint-test-hyperflint-sym-response-determinism
                           PRIVATE
                           HF_TEST_MZV_DATA=${CMAKE_CURRENT_SOURCE_DIR}/data/mzv_reductions.json)

# Track 8.2 (iter-56) B3-rec-1 paired refactor: c_abi.cpp now delegates
# to hyperflint::handlers::partial_fractions / ::linear_factors, which
# pulls in handlers.cpp.o at link time.  handlers.cpp packs
# hyperflint_sym alongside the JSON entry points, transitively dragging
# in hyper_int.cpp's mi_* weak externs.  Same precedent as
# hyperflint-test-find-lr-orders-strategy-roundtrip below (lines
# ~747-760): the symbols are guarded at runtime in hyper_int.cpp, so
# the test binary doesn't need libmimalloc.a force_load; we just mark
# them as allowed-undefined.  Mirrors the CLI's !HF_MIMALLOC path (line
# 299).  Applies to all three c_abi test executables.
if(APPLE)
    foreach(_hf_c_abi_test
            hyperflint-test-c-abi-pfrac
            hyperflint-test-c-abi-pfrac-cli-snapshot
            hyperflint-test-c-abi-linear-factors-cli-snapshot
            hyperflint-test-c-abi-find-lr-orders-cli-snapshot
            hyperflint-test-c-abi-hyperflint-sym-cli-snapshot)
        target_link_options(${_hf_c_abi_test}
                            PRIVATE
            "-Wl,-U,_mi_collect"
            "-Wl,-U,_mi_heap_collect"
            "-Wl,-U,_mi_heap_main"
            "-Wl,-U,_mi_heap_visit_abandoned_blocks"
            "-Wl,-U,_mi_heap_visit_blocks"
            "-Wl,-U,_mi_option_get"
            "-Wl,-U,_mi_option_set"
            "-Wl,-U,_mi_process_info"
            "-Wl,-U,_mi_subproc_main"
            "-Wl,-U,_mi_subproc_visit_heaps")
    endforeach()
endif()

# Track 6.5 iter-41 full PASS: JSON-boundary strategy-roundtrip ctest.
# Posts JSON request bodies to hyperflint::handlers::find_lr_orders and
# asserts the response's "strategy":"<name>" field matches the expected
# enum.  Distinct in coverage from hf-step-strategy-dispatch (which
# bypasses the JSON layer entirely): this gate fires on JSON-serialization
# regressions (typo in switch case, dropped field on conditional branch,
# nlohmann/json migration mishap, etc.) that the StepInputs-level test
# would miss.  Locks the JSON contract Mma callers depend on
# (STFindLROrdersHF in SubTropica.wl reads resp["strategy"] via Lookup).
add_executable(hyperflint-test-find-lr-orders-strategy-roundtrip
               test/unit/test_find_lr_orders_strategy_roundtrip.cpp)
target_link_libraries(hyperflint-test-find-lr-orders-strategy-roundtrip
                      PRIVATE hyperflint)
# handlers.cpp pulls in hyperflint_sym alongside find_lr_orders (same TU),
# which transitively references the mi_* weak externs from hyper_int.cpp.
# These symbols are guarded at runtime (`if (!mi_collect) return;` etc.)
# so the test does NOT need to force_load libmimalloc.a; we just mark
# them as allowed-undefined.  Mirrors the precedent at line 299 for the
# CLI's !HF_MIMALLOC path; here applied unconditionally because the test
# is small and we never want it to pull mimalloc in.
if(APPLE)
    target_link_options(hyperflint-test-find-lr-orders-strategy-roundtrip
                        PRIVATE
        "-Wl,-U,_mi_collect"
        "-Wl,-U,_mi_heap_collect"
        "-Wl,-U,_mi_heap_main"
        "-Wl,-U,_mi_heap_visit_abandoned_blocks"
        "-Wl,-U,_mi_heap_visit_blocks"
        "-Wl,-U,_mi_option_get"
        "-Wl,-U,_mi_option_set"
        "-Wl,-U,_mi_process_info"
        "-Wl,-U,_mi_subproc_main"
        "-Wl,-U,_mi_subproc_visit_heaps")
endif()

# Track 6.5b iter-42: LibraryLink-boundary roundtrip test
# add_executable + ctest registration live AFTER the HF_LIBRARYLINK
# target definition block (line ~1108) so that TARGET hyperflint_librarylink
# is resolvable. See line ~1181-ish (post-LibraryLink block).

# Track 7.1 iter-37 Step 5: env-flag registry coverage gate. Falsifier
# asserts S_md == S_src where S_md = names declared in docs/env_flags.md
# §2 (parser-A: backticked markdown row), S_src = "HF_..." C-string
# literals in src/, bridge/, include/ (parser-B; .bak excluded). Distinct
# parsers + distinct regexes preempt STRUCTURAL_TAUTOLOGY_ROUND_TRIP
# (iter-29 binding; iter-36 audit §F refinement: literal-string union,
# not getenv form, captures wrapper-based env vars too — iter-36
# Finding B HF_OPERATOR_MEMO family + OpDumper). HF_ENV_FLAGS_MD_PATH
# and HF_SRC_ROOT injected via target_compile_definitions so the test
# binary works under any preset binary dir.
add_executable(hyperflint-test-env-flag-registry
               test/unit/test_env_flag_registry.cpp)
target_compile_definitions(hyperflint-test-env-flag-registry
    PRIVATE
        HF_ENV_FLAGS_MD_PATH="${CMAKE_CURRENT_SOURCE_DIR}/docs/env_flags.md"
        HF_SRC_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

# HF basis-ctx campaign (2026-05-28), PHASE_1 iter 4: MzvExpansionTable loader
# unit tests. Test 1a: production-table flatness + load-time invariants +
# expansion-map shape + 8 divergent zeros + basis-block contiguity.
# Iter 5 adds tests 1b-benign + 1b-adversarial against synthetic chained
# fixtures under HyperFLINT/test/data/.
# Design: notes/hf_mzv_weight_cap_2026-05-28/design.md §7.2 gate 1.
add_executable(hyperflint-test-mzv-expansion
               test/unit/test_mzv_expansion.cpp)
target_link_libraries(hyperflint-test-mzv-expansion PRIVATE hyperflint)
target_compile_definitions(hyperflint-test-mzv-expansion
    PRIVATE
        HF_PROD_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
        HF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/data")

# Track 7.2 iter-39: cache-registry coverage gate.
# Falsifier on docs/cache_registry.md §2 vs 5-pattern source scan across src/,
# bridge/, include/. PASS iff S_md == S_src (10 cache identifiers); FAIL with
# diff dump otherwise. Tier-1 (sub-second wall). See test_cache_registry.cpp
# comment block for the STRUCTURAL_TAUTOLOGY_ROUND_TRIP defence axes
# (parser-A backtick markdown rows vs parser-B 5-pattern C++ regex set:
# thread_local + sharded vector<unique_ptr<Shard>> + accessor returning
# unordered_map& + namespace-scope unordered_map + singleton-member
# unordered_map + OperatorMemo<...>& accessor).
add_executable(hyperflint-test-cache-registry
               test/unit/test_cache_registry.cpp)
target_compile_definitions(hyperflint-test-cache-registry
    PRIVATE
        HF_CACHE_REGISTRY_MD_PATH="${CMAKE_CURRENT_SOURCE_DIR}/docs/cache_registry.md"
        HF_SRC_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

# iter-39 negative-control harness: configure-time write a stripped copy of
# docs/cache_registry.md with one §2 row deleted (`g_rhs_cache`). The negative
# control ctest passes the stripped path via argv[1] to the same binary and
# asserts WILL_FAIL. Without this, the coverage gate could be vacuous (always
# PASS). Mirrors iter-31 wall-regression-negative-control discipline. See
# cache_registry.md §4.d.
# docs/cache_registry.md is dev-only (docs/ is stripped from the public source
# tree), so this negative-control harness is skipped when the doc is absent
# (public / CI builds). The cache-registry coverage test then runs without its
# negative control, which is acceptable for a release build.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/docs/cache_registry.md")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/docs/cache_registry.md" _hf_cr_full)
    string(REGEX REPLACE
        "\n\\| `g_rhs_cache` \\|[^\n]*\n"
        "\n"
        _hf_cr_stripped "${_hf_cr_full}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cache_registry.stripped.md"
         "${_hf_cr_stripped}")
endif()

# HF FF Phase 5 §A.1 iter-53: REQ-3 BINDING (closure-trigger reviewer
# agentId `a643bd44c8210297c`). CI coverage for the
# HF_DAG_HASHCONS_PROBE_NDJSON=1 ndjson-emit path, which was missing
# after the iter-52 Class A fix. Without this test, a future iteration
# that re-enables NDJSON=1 silently re-introduces the iter-51 disk-flood
# failure mode.
add_executable(hyperflint-test-dag-hashcons-probe-ndjson-emit
               test/unit/test_dag_hashcons_probe_ndjson_emit.cpp)
target_link_libraries(hyperflint-test-dag-hashcons-probe-ndjson-emit
                      PRIVATE hyperflint)

# HF FF Phase 5 REC-1 Step REC-1-impl-1 TDD (iter-83): mpz-pool tracker
# (FLINT fmpz_single + GMP allocator partition attribution). Four binding
# test cases: EnvGateOff / EnvGateOn_FlintMallocChain /
# LabellingDriftOnSyntheticBlocks / PerSlotCounterUnderOMP.
# See notes/.../rec1_mpz_pool_tracker/design.md §6.1 + §iter-79 fold appendix.
add_executable(hyperflint-test-rec1-mpz-pool-probe
               test/test_rec1_mpz_pool_probe.cpp)
target_link_libraries(hyperflint-test-rec1-mpz-pool-probe
                      PRIVATE hyperflint)
# mpz_pool_probe.cpp references three weak externs that the test binary does
# not link statically:
#   - mi_process_info: resolved into hyperflint-cli via force_load libmimalloc.a
#     (line 399-400) but the test binary does not get the same force-load
#     treatment.
#   - hf_phase05_retrofit_done: defined in bridge/cli/gmp_mimalloc_init.cpp
#     which is linked ONLY into hyperflint-cli, not into libhyperflint.a or
#     the test binary.
# Use -Wl,-U on macOS so the linker accepts the weak-undefined symbols and
# resolves them to nullptr at runtime; the cpp code already guards against
# null function-pointer / false data-flag values per REQ-1 + REQ-4 folds.
if(APPLE)
    target_link_options(hyperflint-test-rec1-mpz-pool-probe PRIVATE
        "-Wl,-U,_mi_process_info"
        "-Wl,-U,_hf_phase05_retrofit_done")
endif()

# HF FF Phase 5 §C.a Step C.a-impl-1 TDD scaffolding (iter-59-γ):
# forward-scaffolding for the GMP allocator-hook with custom slab/arena.
# 7 SKIP placeholders mirroring §C.a design.md §6/§7/§9 test specs;
# bodies authored at iter-N+ Step C.a-impl-2 alongside production source.
# See notes/.../lever_c_a_gmp_allocator_hook/design.md.
add_executable(hyperflint-test-gmp-slab
               test/test_gmp_slab.cpp)
target_link_libraries(hyperflint-test-gmp-slab
                      PRIVATE hyperflint)

# HF FF Phase 5 §C.b Step C.b-impl-1 TDD scaffolding (iter-59-γ):
# forward-scaffolding for mimalloc per-arena heap with arena-bulk-free at
# libdispatch/OMP-team-teardown. 6 SKIP placeholders mirroring §C.b
# design.md §5 falsification-gate sub-tests; bodies authored at iter-N+
# Step C.b-impl-2.
# See notes/.../lever_c_b_mimalloc_per_arena/design.md.
if(HF_MIMALLOC)
add_executable(hyperflint-test-mimalloc-per-arena
               test/test_mimalloc_per_arena.cpp)
target_link_libraries(hyperflint-test-mimalloc-per-arena
                      PRIVATE hyperflint)
endif()

# HF FF Phase 6 §A.M iter-4 Option M.c OMP-safety unit test.
# BINDING adversarial-reviewer agentId a80aa1876bf43e818 CONCERNS-FOLD
# REQ-3.  Verifies: T1 determinism — 10 consecutive mi_subproc_visit_heaps
# walks return identical theaps_visited count (expected = 1 in HF/OMP per
# iter-39 §6 empirical refutation); T2 per-heap collect safety —
# mi_heap_collect(heap, true) per visited heap from main thread (after
# #pragma omp parallel allocations + frees) does not abort/segfault.
# See notes/.../probe_a_m_pool_force/verdict.md (iter-3) for the
# load-bearing iter-39 §6 reconciliation citation.
if(HF_MIMALLOC)
add_executable(hyperflint-test-pool-force-collect-omp-safety
               test/unit/test_pool_force_collect_omp_safety.cpp)
target_link_libraries(hyperflint-test-pool-force-collect-omp-safety
                      PRIVATE hyperflint)
endif()

# HF FF Phase 5 §E Step E.2-impl-1 TDD scaffolding (iter-59-γ):
# forward-scaffolding for Lever 5.4 operator-call memoization across 5
# boundaries (Rat::add, reduce_inplace, linear_factors, partial_fractions,
# transform_shuffle). 7 SKIP placeholders mirroring §E
# implementation_memo.md §4 test specs (§4.1-§4.7); bodies authored at
# iter-N+ Step E.2-impl-2 alongside production source.
# See notes/.../lever_5_4_op_call_memoization/implementation/implementation_memo.md.
add_executable(hyperflint-test-operator-memo
               test/test_operator_memo.cpp)
target_link_libraries(hyperflint-test-operator-memo
                      PRIVATE hyperflint)

# HF FF Phase 6 REVISED §E iter-7 — RSS-pressure-driven LRU eviction tests.
# Exercises evict_lru_batch (LRU/FIFO strategies), evict_post_step_hook
# threshold gate, and the HF_MI_COLLECT_OPTION_M_C ⊕ HF_OP_MEMO_EVICT_ON_RSS
# composability mutual-exclusion SIGABRT (forked child path).
# See notes/.../lever_e_op_memo_eviction_rss_pressure/{design.md,
# determinism_audit.md} + verdict_iter6_reviewer.md.
add_executable(hyperflint-test-op-memo-evict
               test/test_op_memo_evict.cpp)
target_link_libraries(hyperflint-test-op-memo-evict
                      PRIVATE hyperflint)

# HF FF Phase 6 REVISED §E iter-9 — REQ-11 dispatch-wiring per-cache test.
# Validates evict_lru_batch_all_caches dispatches to all 5 production caches.
# See notes/.../lever_e_op_memo_eviction_rss_pressure/runner_iter9_design.md §1.1.
add_executable(hyperflint-test-evict-dispatch-wiring
               test/test_evict_dispatch_wiring.cpp)
target_link_libraries(hyperflint-test-evict-dispatch-wiring
                      PRIVATE hyperflint)

# HF FF Phase 6 REVISED §E iter-9 — REQ-13 size-cap × RSS-pressure compose
# test. Validates the orthogonal-composability claim from iter-6 REQ-8.
# See runner_iter9_design.md §1.2.
add_executable(hyperflint-test-evict-compose-with-size-cap
               test/test_evict_compose_with_size_cap.cpp)
target_link_libraries(hyperflint-test-evict-compose-with-size-cap
                      PRIVATE hyperflint)

# HF FF Phase 6 REVISED §6.D iter-12 — TDD red-state scaffolding for the
# 4-column NodeRssRecord extension (REQ-C from iter-11 advisory pre-build
# reviewer agentId ab6fa5cda36908669 CONCERNS-FOLD, folded into
# notes/.../probe_a_d_integration_tree/design.md §10).
# Single binary, four subtests (one per new column), four `add_test`
# entries below. All four FAIL at iter-12 close (intentional TDD
# red-state); iter-13 production source change flips the local
# probe_*_present() definitions to call into hyperflint:: accessors and
# the subtests turn green.
add_executable(hyperflint-test-integration-node-rss-4col
               test/test_integration_node_rss_4col.cpp)
target_link_libraries(hyperflint-test-integration-node-rss-4col
                      PRIVATE hyperflint)

# HF FF Phase 6 REVISED §6.P iter-15: TDD red-state scaffolding for the
# structural-sharing-potential probe (§A.P / Lever 6.P; REVIEWER P6'
# BINDING byte-weighted activation gate). One executable, four
# `add_test` entries below (registered near the §6.D iter-12 entries).
# All four FAIL at iter-15 close (intentional TDD red-state); iter-17
# production source change wires env-gated probe hooks at the 4 entry
# points (Rat::reduce_inplace, Rat::add, linear_factors,
# partial_fractions) and flips the local probe_*_instrumented()
# definitions to call into hyperflint::structural_sharing:: accessors;
# the subtests then turn green. Mirror of iter-12 §6.D pattern (commit
# d640fa0ee).
#
# Compile-only fixture against current headers (no
# structural_sharing_probe.hpp exists yet — iter-17 will add it).
# Subtests are strictly self-contained (no link against libhyperflint
# needed at iter-15 because the test does not reference any
# hyperflint:: symbol; iter-17 production source change will add the
# link dependency when the probe accessors land).
add_executable(hyperflint-test-integration-structural-sharing-probe
               test/test_integration_structural_sharing_probe.cpp)
target_include_directories(hyperflint-test-integration-structural-sharing-probe
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# iter-17 link dependency: the test's `probe_*_instrumented()` stubs at
# the bottom of test_integration_structural_sharing_probe.cpp delegate
# to hyperflint::structural_sharing::probe_*_instrumented() which lives
# in libhyperflint (src/diagnostics/structural_sharing_probe.cpp).
target_link_libraries(hyperflint-test-integration-structural-sharing-probe
                      PRIVATE hyperflint)

# HF FF Phase 6 REVISED §6.D iter-22 — TDD red-state scaffolding for the
# §6.D engineering pilot (BFS-to-DFS dispatcher rewrite with env-var-
# gated hybrid cap; ship target Strategy (c) per design.md §3.4).
# Three new test files (~340 LOC total post-folds), three new
# add_test entries below. All three FAIL at iter-22a close (intentional
# TDD red-state); iter-22b production source change adds parse_dfs_cap_env
# + dispatch_cap_active + env_var_read_count_per_dispatch probes and
# flips the local probe_*_present() definitions; the subtests then
# turn green. Mirror of iter-12 §6.D pattern (commit d640fa0ee) and
# iter-15 §6.P pattern (commit 46e92302d).
#
# Self-contained at iter-22a: probe stubs return false locally; tests
# compile against the current headers only (no section_6d_dfs_cap.hpp
# exists yet — iter-22b will add it). The tests do NOT reference any
# hyperflint:: symbol at iter-22a, so no link against libhyperflint
# is needed YET; iter-22b's source change will add the link dependency
# when the probe accessors land.
add_executable(hyperflint-test-integration-section-6d-dfs-cap
               test/test_integration_section_6d_dfs_cap.cpp)
target_include_directories(hyperflint-test-integration-section-6d-dfs-cap
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hyperflint-test-integration-section-6d-dfs-cap
                      PRIVATE hyperflint)

# HF FF Phase 6 REVISED §6.D iter-22 REQ-21.5 R21.1 mitigation:
# defense-in-depth regression guard against LF cache-key thread-id
# dependence. Second BINDING reviewer (NAMED agentId b3f24a9c7d1e5680f)
# audit of src/algebra/linear_factors.cpp:761-766 confirms cache_key
# is purely structural at HEAD; this test is kept as a regression
# guard so a future refactor introducing thread-id state in the key
# path is caught at ctest.
add_executable(hyperflint-test-linear-factors-cache-key-thread-invariant
               test/test_linear_factors_cache_key_thread_invariant.cpp)
target_include_directories(hyperflint-test-linear-factors-cache-key-thread-invariant
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hyperflint-test-linear-factors-cache-key-thread-invariant
                      PRIVATE hyperflint)

# HF FF Phase 6 REVISED §6.D iter-22 REQ-21.5 R21.6 mitigation:
# defense-in-depth regression guard against zw_tab write-path under
# cap. iter-65/iter-66 source-changes (commits 0883aeb66, 7dd21d981,
# 12f1e8cc0) widened zw_tab via outer-scope shared_ptr<ZWTable> with
# read-only-inside-parallel-region contract; this test asserts the
# read-only invariance holds under cap=3 (representative non-trivial
# cap value per design.md §7 R21.6).
add_executable(hyperflint-test-transform-zw-tab-readonly-under-cap
               test/test_transform_zw_tab_readonly_under_cap.cpp)
target_include_directories(hyperflint-test-transform-zw-tab-readonly-under-cap
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hyperflint-test-transform-zw-tab-readonly-under-cap
                      PRIVATE hyperflint)

# HF FF Phase 5 §E iter-65 Phase 63-γ-bis: TSan-instrumented stress test
# for OperatorMemo<KeyT,ValueT> targeting the iter-62-δ S2 hazard
# (concurrent insert + try_lookup raw-pointer UAF), iter-63 REQ-1
# resolved via std::optional<ValueT> deep-copy under shared_lock.
# The iter-62-δ reviewer (agentId a0dd0c1c009d169d4) flagged §4.3
# omp-determinism-cache-hit as insufficient for active falsification
# of the post-REQ-1 fix (passive lookup-only after serial pre-pop);
# this target adds the active falsification under TSan.
#
# Self-contained: header-only template exercise, NO link against
# libhyperflint. Gated on HF_TSAN=ON so the default build-mcpu-tuned
# does not attempt to build it (no TSan flag in that build).
if(HF_TSAN)
    add_executable(hyperflint-test-operator-memo-tsan
                   test/test_operator_memo_tsan.cpp)
    target_include_directories(hyperflint-test-operator-memo-tsan PRIVATE
                               ${CMAKE_CURRENT_SOURCE_DIR}/include)
    # Threads::Threads is already required by the global toolchain;
    # pthread linkage is implicit through std::thread on libc++.
endif()

# mimalloc for the CLI binary. This session (2026-04-23) measured a
# clean 7-12% wall-time win on Smirnov tst1 and tst2 vs macOS 26's
# default xzm allocator when FLINT's `fmpq_mpoly_*` inits/clears
# dominate the allocation pattern. Linked via -force_load so the
# static library's malloc/free symbols aren't dead-stripped on
# Apple's linker (per mimalloc README for macOS).
#
# Turn off with `-DHF_MIMALLOC=OFF` if mimalloc isn't installed.
# (option(HF_MIMALLOC ...) is declared near the top of this file — hoisted
# 2026-06-05 so the if(HF_MIMALLOC)-gated test targets at ~line 1119 see it.)
if(HF_MIMALLOC)
    # HF FF Phase 5 §C.b iter-58 fold appendix REC-5 (landed iter-89):
    # require mimalloc >= 3.3 fail-fast.  mimalloc 3.3 is the first
    # release that includes the fix for issue #1055 (mi_heap_destroy
    # race in short-lived heap usage), which is load-bearing for §C.b's
    # per-region theap install/restore + bulk-free pattern at
    # libdispatch/OMP-team-teardown.  Brew currently ships 3.3.1.
    find_package(mimalloc 3.3 REQUIRED)
    if(TARGET mimalloc-static)
        if(APPLE)
            # Apple ld64 strips unused symbols from static archives by
            # default, which would skip mimalloc's malloc/free
            # overrides. -force_load keeps the whole archive.
            get_target_property(_mimalloc_static_path mimalloc-static
                                  IMPORTED_LOCATION_RELEASE)
            if(NOT _mimalloc_static_path)
                get_target_property(_mimalloc_static_path mimalloc-static
                                      IMPORTED_LOCATION)
            endif()
            if(_mimalloc_static_path)
                target_link_options(hyperflint-cli PRIVATE
                    "-Wl,-force_load,${_mimalloc_static_path}")
                message(STATUS "mimalloc (static, force_load): ${_mimalloc_static_path}")
            else()
                message(WARNING "mimalloc-static target has no IMPORTED_LOCATION; falling back to shared link")
                target_link_libraries(hyperflint-cli PRIVATE mimalloc)
            endif()
        else()
            target_link_libraries(hyperflint-cli PRIVATE mimalloc-static)
            message(STATUS "mimalloc (static) linked into CLI")
        endif()
    elseif(TARGET mimalloc)
        target_link_libraries(hyperflint-cli PRIVATE mimalloc)
        message(STATUS "mimalloc (shared) linked into CLI")
    else()
        message(WARNING "HF_MIMALLOC=ON but mimalloc not found; "
            "falling back to system allocator. "
            "On macOS: `brew install mimalloc`.")
    endif()

    # HF FF Phase 5 §C.b iter-89 path-default: link the
    # `hyperflint-test-mimalloc-per-arena` test target against mimalloc
    # so the 4 REAL primitives tests (version probe, mi_heap_t
    # lifecycle, mi_theap_set/get_default install/restore pairing,
    # mi_collect(false) no-crash) compile and link.  The test target
    # itself is defined earlier (around line 651) before find_package
    # runs; deferring the link wiring here is safe because
    # target_link_libraries / target_include_directories may be applied
    # to a previously-defined target at any point in the same
    # CMakeLists.txt.
    if(TARGET mimalloc-static)
        target_link_libraries(hyperflint-test-mimalloc-per-arena
                              PRIVATE mimalloc-static)
    elseif(TARGET mimalloc)
        target_link_libraries(hyperflint-test-mimalloc-per-arena
                              PRIVATE mimalloc)
    endif()

    # HF FF Phase 6 §A.M iter-4: also link the Option M.c OMP-safety
    # test against mimalloc so mi_subproc_main, mi_subproc_visit_heaps,
    # mi_heap_collect, mi_malloc, mi_free resolve at link time.
    if(TARGET mimalloc-static)
        target_link_libraries(hyperflint-test-pool-force-collect-omp-safety
                              PRIVATE mimalloc-static)
    elseif(TARGET mimalloc)
        target_link_libraries(hyperflint-test-pool-force-collect-omp-safety
                              PRIVATE mimalloc)
    endif()
endif()

# -------- LibraryLink shared library (Phase γ.1) --------
# In-process transport for Mma. Eliminates per-call RunProcess overhead.
# Skipped if Wolfram's LibraryLink header is not found — HF's CLI still
# builds and SubTropica falls back to the subprocess path.
# HF_LIBRARYLINK and HF_LIBRARYLINK_STATIC_DEPS option() declarations are
# hoisted to before the hyperflint_nomp_sd target (see that section for the
# rationale).  The set() cache variables below are only needed here.

# Static-link FLINT, GMP, MPFR into the LibraryLink dylib (instead of
# resolving them dynamically at load time).  Default ON because Mma's
# bundled libflint.17 / libgmp / libmpfr (in
# Wolfram.app/Contents/SystemFiles/Libraries/) collide with our
# @loader_path/libflint.22.0 / libgmp.10 / libmpfr.6 in dyld and trigger
# a SIGSEGV on hf_hyperflint_sym for nontrivial workloads (root-cause
# diagnosis: notes/benchmark_1massDB/segfault_diagnosis.md).  Static
# linking + -fvisibility=hidden + an exported_symbols_list eliminates
# the symbol-resolution race entirely; the dylib's FLINT calls all go
# to the embedded static archives.  Cost: dist size grows ~30 MB.
# HF_FLINT/GMP/MPFR_STATIC_ARCHIVE are defined near the top of this file (just
# after the HF_*_STATIC_DEPS options) so they are in scope for the CLI link
# (hyperflint-cli), which appears far above this point. Defining them here
# would leave them empty at that earlier link and drop GMP/MPFR from the CLI.
# HF_VERSION cache var moved to the top of CMakeLists.txt (Track 8.1
# iter-43); see the comment block there.  The hyperflint_librarylink
# PRIVATE definition below still stamps it into hf_library.cpp's TU.

if(HF_LIBRARYLINK)
    find_path(WOLFRAM_LIBRARY_INCLUDE_DIR WolframLibrary.h
        PATHS
            ENV WOLFRAM_LIBRARY_INCLUDE_DIR
            "/Applications/Wolfram.app/Contents/SystemFiles/IncludeFiles/C"
            "/Applications/Mathematica.app/Contents/SystemFiles/IncludeFiles/C"
            "/usr/local/Wolfram/Mathematica/*/SystemFiles/IncludeFiles/C"
            "/opt/Wolfram/WolframEngine/*/SystemFiles/IncludeFiles/C"
            "$ENV{HOME}/Wolfram/Mathematica/*/SystemFiles/IncludeFiles/C"
        DOC "Path to WolframLibrary.h (usually \
$InstallationDirectory/SystemFiles/IncludeFiles/C)")

    if(NOT WOLFRAM_LIBRARY_INCLUDE_DIR)
        message(STATUS
            "WolframLibrary.h not found — skipping LibraryLink build. "
            "Pass -DWOLFRAM_LIBRARY_INCLUDE_DIR=/path/to/IncludeFiles/C "
            "or set the WOLFRAM_LIBRARY_INCLUDE_DIR env var if your "
            "Mathematica install is in a non-standard location.")
    else()
        message(STATUS "LibraryLink header: ${WOLFRAM_LIBRARY_INCLUDE_DIR}")
        add_library(hyperflint_librarylink SHARED
            bridge/librarylink/hf_library.cpp)
        target_include_directories(hyperflint_librarylink PRIVATE
            ${WOLFRAM_LIBRARY_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${FLINT_INCLUDE_DIRS})
        # Must link the no-OpenMP variant (see comments on
        # `hyperflint_nomp` target above) so that brew's libomp does
        # not get dragged in and race with Wolfram's own libomp.
        # When HF_LIBRARYLINK_STATIC_DEPS=ON, we use the
        # `hyperflint_nomp_sd` variant (FLINT linked PRIVATE-only,
        # built with -fvisibility=hidden) and pull the FLINT/GMP/MPFR
        # archives directly so their symbols stay private to the
        # dylib and don't race with Mma's bundled FLINT/GMP/MPFR.
        if(HF_LIBRARYLINK_STATIC_DEPS)
            if(NOT EXISTS "${HF_FLINT_STATIC_ARCHIVE}")
                message(FATAL_ERROR
                    "HF_LIBRARYLINK_STATIC_DEPS=ON but static FLINT archive "
                    "not found at ${HF_FLINT_STATIC_ARCHIVE}.\n"
                    "Build it with the bundled script:\n"
                    "  ./scripts/build_static_flint.sh $PWD/flint-static\n"
                    "then re-run cmake with:\n"
                    "  -DHF_FLINT_STATIC_ARCHIVE=$PWD/flint-static/lib/libflint.a\n"
                    "Or pass -DHF_LIBRARYLINK_STATIC_DEPS=OFF to disable "
                    "(reverts to shared FLINT — known to crash Mma kernel "
                    "on heavy fixtures).")
            endif()
            # Force-load ALL objects from the static archives so the
            # FLINT/GMP/MPFR symbols are embedded in the library instead of
            # being deferred to runtime, where the loader would resolve them
            # against the host's bundled libflint/libgmp/libmpfr -- exactly
            # the collision we avoid. Symbol privacy is then enforced via the
            # exported-symbols list (macOS) / version script (Linux) below.
            if(APPLE)
                # macOS/ld64 syntax. -force_load is also required because the
                # dylib uses -undefined dynamic_lookup for the
                # WolframLibraryData callbacks (set in the APPLE block below).
                target_link_libraries(hyperflint_librarylink PRIVATE
                    hyperflint_nomp_sd
                    "-Wl,-force_load,${HF_FLINT_STATIC_ARCHIVE}"
                    "-Wl,-force_load,${HF_MPFR_STATIC_ARCHIVE}"
                    "-Wl,-force_load,${HF_GMP_STATIC_ARCHIVE}")
            else()
                # GNU ld (Linux): --whole-archive is the -force_load
                # equivalent; --no-whole-archive resets for any later libs.
                target_link_libraries(hyperflint_librarylink PRIVATE
                    hyperflint_nomp_sd
                    -Wl,--whole-archive
                    "${HF_FLINT_STATIC_ARCHIVE}"
                    "${HF_MPFR_STATIC_ARCHIVE}"
                    "${HF_GMP_STATIC_ARCHIVE}"
                    -Wl,--no-whole-archive)
            endif()
            target_compile_options(hyperflint_librarylink PRIVATE
                -fvisibility=hidden)
        else()
            target_link_libraries(hyperflint_librarylink PRIVATE
                hyperflint_nomp)
        endif()
        target_compile_definitions(hyperflint_librarylink PRIVATE
            HF_VERSION_STRING="${HF_VERSION}")
        set_target_properties(hyperflint_librarylink PROPERTIES
            CXX_STANDARD 17
            POSITION_INDEPENDENT_CODE ON
            OUTPUT_NAME "hyperflint_librarylink")
        if(APPLE)
            # Let the library reference WolframLibraryData callbacks
            # that the kernel provides at runtime.
            set(_hf_ll_link_flags "-undefined dynamic_lookup")
            if(HF_LIBRARYLINK_STATIC_DEPS)
                # Restrict exported symbols to the LibraryLink
                # entry points (hf_* + WolframLibrary_*).  Without
                # this, the embedded FLINT/GMP/MPFR symbols would
                # still be globally visible and dyld could
                # cross-resolve them with Mma's bundled copies.
                set(_hf_ll_link_flags
                    "${_hf_ll_link_flags} -Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/bridge/librarylink/exports.txt")
            endif()
            set_target_properties(hyperflint_librarylink PROPERTIES
                SUFFIX ".dylib"
                LINK_FLAGS "${_hf_ll_link_flags}")
        elseif(UNIX)
            # Linux/ELF: the undefined Wolfram callback symbols resolve at
            # load time by default (no -undefined dynamic_lookup needed). The
            # SHARED default already yields libhyperflint_librarylink.so. When
            # static-linking the deps, restrict the dynamic symbol table to
            # the LibraryLink entry points via the version script (the ELF
            # mirror of exports.txt) and hide all archive symbols, so the
            # embedded FLINT/GMP/MPFR cannot cross-resolve with the Engine's
            # bundled copies.
            if(HF_LIBRARYLINK_STATIC_DEPS)
                set_target_properties(hyperflint_librarylink PROPERTIES
                    LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/bridge/librarylink/exports.map -Wl,--exclude-libs,ALL")
            endif()
        endif()
    endif()
endif()

# Track 6.5b iter-42 (iter-40 binding rec B4): LibraryLink-boundary
# strategy-roundtrip test. Built only when the LibraryLink dylib was
# built (HF_LIBRARYLINK ON + WolframLibrary.h found). Independence axis
# vs the in-process iter-41 ctest: dlopen at runtime + dlsym, no
# compile-time link against the dylib (so a future
# -Wl,-exported_symbols_list change that drops hf_find_lr_orders would
# fail at dlsym, not at link). The test needs ONLY WolframLibrary.h for
# the MArgument layout and the LIBRARY_NO_ERROR constants; it does not
# need to link against the HyperFLINT static library. Placed AFTER the
# HF_LIBRARYLINK block above so that TARGET hyperflint_librarylink is
# defined at this point.
if(TARGET hyperflint_librarylink)
    add_executable(hyperflint-test-find-lr-orders-strategy-roundtrip-librarylink
                   test/unit/test_find_lr_orders_strategy_roundtrip_librarylink.cpp)
    target_include_directories(
        hyperflint-test-find-lr-orders-strategy-roundtrip-librarylink
        PRIVATE ${WOLFRAM_LIBRARY_INCLUDE_DIR})
    set_target_properties(
        hyperflint-test-find-lr-orders-strategy-roundtrip-librarylink
        PROPERTIES CXX_STANDARD 17)
    # The librarylink dylib is built with -undefined dynamic_lookup and
    # expects the host process (Mma kernel in production) to provide
    # libomp at load time. When dlopen'd from a bare test binary, the
    # OpenMP runtime symbols (___kmpc_dispatch_deinit etc.) are
    # unresolved. We pin libomp into the test process by linking
    # OpenMP::OpenMP_CXX so the runtime is in the process address space
    # before dlopen runs. The test itself does not USE OpenMP — this is
    # only to satisfy the dylib's deferred symbol resolution.
    if(OpenMP_CXX_FOUND)
        target_link_libraries(
            hyperflint-test-find-lr-orders-strategy-roundtrip-librarylink
            PRIVATE OpenMP::OpenMP_CXX)
    endif()
    # The librarylink dylib must exist before the test runs but is
    # NOT a compile-time link dependency (the test uses dlopen). A
    # ctest order dependency is what we need; add_dependencies at the
    # CMake-target level enforces it.
    add_dependencies(
        hyperflint-test-find-lr-orders-strategy-roundtrip-librarylink
        hyperflint_librarylink)
endif()

# -------- Install --------
install(TARGETS hyperflint-cli RUNTIME DESTINATION bin)
install(DIRECTORY include/hyperflint DESTINATION include)

# -------- Tests --------
# The unit-test executables above (hyperflint-test-*) are built
# unconditionally as ordinary CMake targets; this block registers
# them with ctest so the per-commit gate (design §5.2) can drive
# them via `ctest --test-dir HyperFLINT/build-release -R <pattern>`.
# The legacy `HF_BUILD_TESTS` option gated an `add_subdirectory(test/unit)`
# that was never populated (test/unit/CMakeLists.txt does not exist);
# removing the dead branch.
enable_testing()
# iter-29 Track 5.1 + iter-29 reviewer Rec 1 BINDING: verify the
# HF_BUILD_VARIANT stamp embedded in the binary matches the build-tree
# directory's leaf name. Per CMakePresets.json _base, binaryDir is
# ${sourceDir}/build-presets/${presetName}, so the leaf = preset name.
# The grep target is derived from ${CMAKE_BINARY_DIR} INDEPENDENTLY of
# ${HF_BUILD_VARIANT}, so an attacker who overrides
# `cmake -DHF_BUILD_VARIANT=foobar .` cannot also rewrite the grep
# target. Catches the structural-tautology trap from iter-29 reviewer
# Q4 attack test (the prior assertion sourced both sides from the same
# cache var and passed under -DHF_BUILD_VARIANT=foobar).
get_filename_component(_hf_build_variant_from_bindir "${CMAKE_BINARY_DIR}" NAME)
add_test(NAME hf-build-variant-stamp
         COMMAND bash -c "$<TARGET_FILE:hyperflint-cli> --version | grep -q 'HF_BUILD_VARIANT: ${_hf_build_variant_from_bindir}'")
set_tests_properties(hf-build-variant-stamp PROPERTIES TIMEOUT 10)

# iter-57 §T10: binary semver + ABI break detection. Two assertions:
#   (1) `hyperflint-cli --version` reports HF_VERSION matching the cmake
#       HF_VERSION cache var (which is the single source of truth, fed
#       into the eval-json envelope's `hf_version` field via
#       -DHF_VERSION_STRING=${HF_VERSION} in HyperFLINT/CMakeLists.txt
#       :181-185). A divergence here would mean the CLI's reported
#       version drifted from the JSON envelope's reported version.
#   (2) The public C-ABI symbol surface of libhyperflint.a matches the
#       checked-in golden file at test/abi/symbols_golden.txt. Detects
#       unintended ABI changes (symbol removal, rename, or accidental
#       addition without updating the golden) before downstream
#       consumers (Julia HJ.jl, Mma LibraryLink, Python cffi) silently
#       break. The extractor uses byte-level grep -a on the .a archive
#       (portable across BSD nm / GNU nm / llvm-nm flag dialects).
add_test(NAME hf-cli-version-string
         COMMAND bash -c "$<TARGET_FILE:hyperflint-cli> --version | grep -q 'HF_VERSION: ${HF_VERSION}'")
set_tests_properties(hf-cli-version-string PROPERTIES TIMEOUT 10)

add_test(NAME hf-abi-break-detection
         COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test/abi/check_abi_symbols.sh"
                      "$<TARGET_FILE:hyperflint>"
                      "${CMAKE_CURRENT_SOURCE_DIR}/test/abi/symbols_golden.txt")
set_tests_properties(hf-abi-break-detection PROPERTIES TIMEOUT 10)

add_test(NAME merge-sorted-canonical
         COMMAND hyperflint-test-merge-sorted-canonical)
# hyperflint-test-rat-add-equivalence is a fixture-driven harness
# (requires a path to rat_quads.txt at runtime); not a self-contained
# unit test, so it is intentionally NOT registered here. The
# executable is still built unconditionally and is run manually by
# fixture-bound benchmarks (see notes/3l3pt_branch_I_fmpz_q_poc/).
add_test(NAME sharded-flat-map
         COMMAND hyperflint-test-sharded-flat-map)
add_test(NAME poles-bucket-flush
         COMMAND hyperflint-test-poles-bucket-flush)
add_test(NAME operand-sparsity
         COMMAND hyperflint-test-operand-sparsity)
add_test(NAME rat-split-roundtrip
         COMMAND hyperflint-test-rat-split-roundtrip)
add_test(NAME zw-table-basic
         COMMAND hyperflint-test-zw-table-basic)
add_test(NAME sym-coef-split-roundtrip
         COMMAND hyperflint-test-sym-coef-split-roundtrip)
add_test(NAME rat-split-roundtrip-synthetic
         COMMAND hyperflint-test-rat-split-roundtrip-synthetic)
add_test(NAME sym-coef-canonical-string
         COMMAND hyperflint-test-sym-coef-canonical-string)
add_test(NAME rat-content-invariance
         COMMAND hyperflint-test-rat-content-invariance)
add_test(NAME omp-determinism
         COMMAND hyperflint-test-omp-determinism)
add_test(NAME step-trace-rss
         COMMAND hyperflint-test-step-trace-rss)
add_test(NAME integration-node-rss
         COMMAND hyperflint-test-integration-node-rss)
add_test(NAME gcd-dispatch
         COMMAND hyperflint-test-gcd-dispatch)
# HF FF Phase 5 §A.1 iter-49: Probe A1 REQ-1 init unit test. Verifies the
# GMP-allocator wrap registers the probe wrappers AFTER hf_init_mimalloc_for_gmp_flint,
# that g_prev_gmp_alloc != nullptr (Phase 0.5 retrofit was captured), and that
# the labelled-bytes counter increments/decrements correctly on a synthetic
# mpz_init+mpz_clear cycle. See test/unit/test_dag_hashcons_probe_init.cpp.
add_test(NAME dag-hashcons-probe-init
         COMMAND hyperflint-test-dag-hashcons-probe-init)
set_tests_properties(dag-hashcons-probe-init PROPERTIES
    ENVIRONMENT "HF_DAG_HASHCONS_PROBE=1;HF_DAG_HASHCONS_PROBE_OUT_DIR=${CMAKE_CURRENT_BINARY_DIR}")
# HF FF Phase 5 §A.1 iter-50: Probe A1 REQ-2 BINDING CI test (OMP=1 vs
# OMP=13 construction-path dedup-rate invariance ±0.5 pp). Exercises the
# sharded shared_mutex seen-set under concurrency.
add_test(NAME construction-path-dedup-rate-omp-invariance
         COMMAND hyperflint-test-construction-path-dedup-rate-omp-invariance)
set_tests_properties(construction-path-dedup-rate-omp-invariance PROPERTIES
    ENVIRONMENT "HF_DAG_HASHCONS_PROBE=1;HF_DAG_HASHCONS_PROBE_OUT_DIR=${CMAKE_CURRENT_BINARY_DIR}")
# HF FF Phase 5 §A.1 iter-50: Probe A1 REQ-3 prerequisite (OFF-path
# structural no-op). NOTE: ENVIRONMENT explicitly DOES NOT set
# HF_DAG_HASHCONS_PROBE so the probe is INACTIVE for the test.
add_test(NAME dag-hashcons-probe-off-no-op
         COMMAND hyperflint-test-dag-hashcons-probe-off-no-op)
# (no set_tests_properties — leave probe env unset to test OFF path)
# Track 4.2 iter-27 §F row 14: phase_timer predicate-false no-op + latency.
add_test(NAME phase-timer-predicate-false-no-op
         COMMAND hyperflint-test-phase-timer-predicate-false-no-op)
# Track 6.3 iter-35: StepStrategy dispatch decision-rule falsifier.
# Six-row truth-table check; no env (the test reads its data path from
# the HF_TEST_DATA_DIR compile-time macro).
add_test(NAME hf-step-strategy-dispatch
         COMMAND hyperflint-test-step-strategy-dispatch)
# Track 8.1b chunk-2 (iter-47): C ABI smoke for hf_partial_fractions
# success path + error envelopes + chunk-3 stubs.  Tier-1; sub-second.
# Linked against libhyperflint.a; exercises the same TU that the
# LibraryLink dylib carries.  A future chunk (iter-48+) adds the
# dlopen-path variant; this gate locks the in-process C ABI contract.
add_test(NAME hf-c-abi-pfrac-smoke
         COMMAND hyperflint-test-c-abi-pfrac)
# Track 8.1b chunk-2b (iter-48): byte-identical CLI vs C ABI snapshot.
# Spawns hyperflint-cli with a fixed partial_fractions request and
# compares its stdout to the in-process hf_partial_fractions output
# with the envelope stripped.  Locks the transport-neutral contract
# established in iter-48 (handle_partial_fractions drained into
# hyperflint::handlers::partial_fractions).  Tier-1; ~30 ms wall.
add_test(NAME hf-c-abi-pfrac-cli-snapshot
         COMMAND hyperflint-test-c-abi-pfrac-cli-snapshot
                 $<TARGET_FILE:hyperflint-cli>)
# Track 8.1b chunk-3b (iter-51): byte-identical CLI vs C ABI snapshot
# for linear_factors.  Same shape as hf-c-abi-pfrac-cli-snapshot but
# locks the iter-50 chunk-3 LAND of hyperflint::handlers::linear_factors.
add_test(NAME hf-c-abi-linear-factors-cli-snapshot
         COMMAND hyperflint-test-c-abi-linear-factors-cli-snapshot
                 $<TARGET_FILE:hyperflint-cli>)
# Track 8.4 (iter-58): byte-identical CLI vs C ABI snapshot for
# find_lr_orders, locking the iter-58 promotion of hf_find_lr_orders
# from the iter-56 chunk_3_pending stub to a real delegating entry
# point.  The handler emits the envelope inline so this test strips
# `timing_compute_s` (the only wall-clock-dependent field) instead of
# the envelope. Tier-1; ~30 ms wall.
add_test(NAME hf-c-abi-find-lr-orders-cli-snapshot
         COMMAND hyperflint-test-c-abi-find-lr-orders-cli-snapshot
                 $<TARGET_FILE:hyperflint-cli>)
# Track 8.4-real-op-1 (iter-61): byte-identical CLI vs C ABI snapshot
# for hyperflint_sym, locking the iter-61 promotion of hf_hyperflint_sym
# to a real delegating entry point. Hybrid of the pfrac (envelope strip)
# and find_lr_orders (timing strip) snapshot patterns: the handler emits
# the CLI-form body WITHOUT envelope on its happy path (so the ABI side
# has the envelope spliced in and this test strips it) AND emits a
# wall-clock `timing_compute_s` field (so this test strips it on both
# sides). Fixture: findroots21_b W-non-empty integrator request (the
# same one iter-60's cross-process determinism probe verified is
# process-stable modulo `timing_compute_s`). Tier-1; ~80 ms wall
# (CLI invocation + ~3 ms compute + tempfile / fork overhead).
add_test(NAME hf-c-abi-hyperflint-sym-cli-snapshot
         COMMAND hyperflint-test-c-abi-hyperflint-sym-cli-snapshot
                 $<TARGET_FILE:hyperflint-cli>)
# Track 8.4-probe (iter-60): cross-process determinism probe for the
# hyperflint_sym handler. Two independent CLI invocations on the
# findroots21_b W-non-empty integrator fixture; stripped-stdout
# byte-identity gate. Feasibility evidence for iter-61's planned
# hf_hyperflint_sym ABI op + CLI-vs-ABI snapshot ctest. Tier-1; ~80 ms
# wall (2 cells × ~35 ms compute + tempfile + fork overhead).
add_test(NAME hf-hyperflint-sym-response-determinism
         COMMAND hyperflint-test-hyperflint-sym-response-determinism
                 $<TARGET_FILE:hyperflint-cli>)
# Track 6.5 iter-41 full PASS: JSON-boundary strategy-roundtrip gate.
# 6 truth-table rows posted as JSON request bodies to
# hyperflint::handlers::find_lr_orders; response "strategy" field
# asserted against expected enum.  Tier-1 (sub-second wall, ~few-ms
# per row).  Locks the iter-40 wire-in's JSON-serialization contract
# that Mma callers consume via SubTropica.wl STFindLROrdersHF.
add_test(NAME hf-find-lr-orders-strategy-roundtrip
         COMMAND hyperflint-test-find-lr-orders-strategy-roundtrip)
# Track 6.5b iter-42 (iter-40 binding rec B4): LibraryLink-boundary
# roundtrip gate. Same 6 truth-table rows, but the entry path is
# dlopen + dlsym of libhyperflint_librarylink.dylib instead of the
# direct C++ handler call. Exercises the C ABI boundary (mint,
# MArgument, WolframLibraryData), the UTF8String unpack
# (MArgument_getUTF8String char** macro), and the set_utf8_result
# allocator path. Catches LibraryLink-wrapper regressions
# (mis-dispatch, ABI breakage, exports.txt allowlist drops) that the
# in-process iter-41 ctest cannot see. Registered only when the
# LibraryLink dylib was built (HF_LIBRARYLINK + WOLFRAM_LIBRARY_INCLUDE_DIR
# resolved); see CMakeLists.txt:1088 for the gate.
if(TARGET hyperflint_librarylink)
    add_test(NAME hf-find-lr-orders-strategy-roundtrip-librarylink
             COMMAND hyperflint-test-find-lr-orders-strategy-roundtrip-librarylink
                     $<TARGET_FILE:hyperflint_librarylink>)
endif()
# Track 7.1 iter-37 Step 5: env-flag registry coverage gate.
# Falsifier on env_flags.md §2 vs literal-string union across src/, bridge/,
# include/. PASS iff S_md == S_src; FAIL with diff dump otherwise. Tier-1
# (sub-second wall). See test_env_flag_registry.cpp comment block for the
# STRUCTURAL_TAUTOLOGY_ROUND_TRIP defence axes (distinct parsers, distinct
# regexes, literal-string union pattern).
add_test(NAME hf-env-flag-registry-coverage
         COMMAND hyperflint-test-env-flag-registry)
# HF basis-ctx campaign (2026-05-28), PHASE_1 iter 4: MzvExpansionTable
# load-time invariants on production mzv_reductions.json. Tier-1 (sub-second).
add_test(NAME hf-mzv-expansion
         COMMAND hyperflint-test-mzv-expansion)
# Track 7.2 iter-39: cache-registry coverage gate (positive control).
# PASS iff |S_md| == |S_src| == 10. Tier-1 (sub-second wall).
add_test(NAME hf-cache-registry-coverage
         COMMAND hyperflint-test-cache-registry)
# Track 7.2 iter-39: cache-registry negative control.
# Same binary, but argv[1] points at the configure-time stripped copy
# (`g_rhs_cache` row removed). Coverage gate MUST report S_src \ S_md = {g_rhs_cache}
# and exit non-zero; ctest WILL_FAIL inverts that to a PASS. Without this, the
# positive ctest could be vacuous. Mirrors iter-31 wall-regression-negative-control.
add_test(NAME hf-cache-registry-negative-control
         COMMAND hyperflint-test-cache-registry
                 "${CMAKE_CURRENT_BINARY_DIR}/cache_registry.stripped.md")
set_tests_properties(hf-cache-registry-negative-control
                     PROPERTIES WILL_FAIL TRUE)
# HF FF Phase 5 §A.1 iter-53: REQ-3 BINDING CI coverage for the
# HF_DAG_HASHCONS_PROBE_NDJSON=1 path (closure-trigger reviewer
# agentId `a643bd44c8210297c`). Sets the NDJSON sub-flag ON; the
# test creates probe_a1_values.ndjson + probe_a1_aggregate.json in a
# dedicated per-test subdir of the build tree (to avoid colliding
# with the OFF-path / counter-only tests which point at the build
# dir root). Asserts header line + at least one value-create record +
# size bound + aggregate.json records "ndjson_enabled": true. Prevents
# silent regression of the iter-51 disk-flood failure mode if NDJSON=1
# is re-enabled in a future iteration.
add_test(NAME dag-hashcons-probe-ndjson-emit
         COMMAND hyperflint-test-dag-hashcons-probe-ndjson-emit)
set_tests_properties(dag-hashcons-probe-ndjson-emit PROPERTIES
    ENVIRONMENT "HF_DAG_HASHCONS_PROBE=1;HF_DAG_HASHCONS_PROBE_NDJSON=1;HF_DAG_HASHCONS_PROBE_OUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/probe_ndjson_emit_test_out")
# HF FF Phase 5 REC-1 Step REC-1-impl-1 TDD (iter-83): REC-1 mpz-pool tracker
# unit test. Master env-gate HF_REC1_TRACK_MPZ_POOL is NOT set here because
# the test toggles it internally via setenv/unsetenv across cases (case 1
# tests OFF path; cases 2-4 test ON path). OUT_DIR is set so the atexit
# aggregate snapshot lands in the build tree.
add_test(NAME rec1-mpz-pool-probe
         COMMAND hyperflint-test-rec1-mpz-pool-probe)
set_tests_properties(rec1-mpz-pool-probe PROPERTIES
    ENVIRONMENT "HF_REC1_OUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/rec1_mpz_pool_test_out")
# HF FF Phase 5 §C.a Step C.a-impl-1 TDD scaffolding (iter-59-γ).
# Forward-scaffolded; all 7 sub-tests are SKIP placeholders at iter-59-γ;
# iter-N+ Step C.a-impl-2 fills bodies alongside the §C.a production source.
add_test(NAME gmp-slab
         COMMAND hyperflint-test-gmp-slab)
# HF FF Phase 5 §C.b Step C.b-impl-1 TDD scaffolding (iter-59-γ).
# Forward-scaffolded; all 6 sub-tests are SKIP placeholders at iter-59-γ;
# iter-N+ Step C.b-impl-2 fills bodies alongside the §C.b production source.
if(HF_MIMALLOC)
add_test(NAME mimalloc-per-arena
         COMMAND hyperflint-test-mimalloc-per-arena)
endif()
# HF FF Phase 6 §A.M iter-4 Option M.c OMP-safety unit test.
# BINDING adversarial-reviewer agentId a80aa1876bf43e818 CONCERNS-FOLD
# REQ-3.  Verifies: T1 determinism (10 consecutive mi_subproc_visit_heaps
# walks return identical theaps_visited count); T2 per-heap collect
# safety (mi_heap_collect(heap, true) from main thread does not
# abort/segfault after #pragma omp parallel allocations).
# OMP_NUM_THREADS=13 is set in-test via omp_set_num_threads (matches
# HF production OMP=13 default; see auto-memory
# feedback_hyperflint_threads.md).
if(HF_MIMALLOC)
add_test(NAME pool-force-collect-omp-safety
         COMMAND hyperflint-test-pool-force-collect-omp-safety)
set_tests_properties(pool-force-collect-omp-safety PROPERTIES
    TIMEOUT 60)
endif()
# HF FF Phase 5 §E Step E.2-impl-1 TDD scaffolding (iter-59-γ).
# Forward-scaffolded; all 7 sub-tests are SKIP placeholders at iter-59-γ;
# iter-N+ Step E.2-impl-2 fills bodies alongside the §E production source
# (per implementation_memo.md §4 + iter-59 fold appendix).
add_test(NAME op-memo-evict
         COMMAND hyperflint-test-op-memo-evict)
set_tests_properties(op-memo-evict PROPERTIES TIMEOUT 60)
# HF FF Phase 6 REVISED §E iter-9 (REQ-11 + REQ-13).
add_test(NAME evict-dispatch-wiring
         COMMAND hyperflint-test-evict-dispatch-wiring)
set_tests_properties(evict-dispatch-wiring PROPERTIES TIMEOUT 30)
add_test(NAME evict-compose-with-size-cap
         COMMAND hyperflint-test-evict-compose-with-size-cap)
set_tests_properties(evict-compose-with-size-cap PROPERTIES TIMEOUT 30)

# HF FF Phase 6 REVISED §6.D iter-12 — TDD red-state for 4-column
# NodeRssRecord extension (REQ-C).  Four `add_test` entries, all
# pointing at the same binary with a column-name argv, so `ctest -R
# node-rss-4col` matches all four.  Each subtest FAILs at iter-12
# (intentional); iter-13 production source change turns them green.
add_test(NAME node-rss-4col-thread-local
         COMMAND hyperflint-test-integration-node-rss-4col thread-local)
set_tests_properties(node-rss-4col-thread-local PROPERTIES TIMEOUT 30)
add_test(NAME node-rss-4col-aggregate-omp
         COMMAND hyperflint-test-integration-node-rss-4col aggregate-omp)
set_tests_properties(node-rss-4col-aggregate-omp PROPERTIES TIMEOUT 30)
add_test(NAME node-rss-4col-lf-cache-bytes
         COMMAND hyperflint-test-integration-node-rss-4col lf-cache-bytes)
set_tests_properties(node-rss-4col-lf-cache-bytes PROPERTIES TIMEOUT 30)
add_test(NAME node-rss-4col-reg-sym-bytes
         COMMAND hyperflint-test-integration-node-rss-4col reg-sym-bytes)
set_tests_properties(node-rss-4col-reg-sym-bytes PROPERTIES TIMEOUT 30)

# HF FF Phase 6 REVISED §6.P iter-15 — TDD red-state for the §A.P /
# Lever 6.P structural-sharing-potential probe. Four `add_test`
# entries, all pointing at the same binary with an entry-point-name
# argv, so `ctest -R structural-sharing-probe` matches all four. Each
# subtest FAILs at iter-15 close (intentional); iter-17 production
# source change wires the per-entry-point probe hooks and flips the
# local probe_*_instrumented() definitions; the subtests then turn
# green.
add_test(NAME structural-sharing-probe-reduce-inplace
         COMMAND hyperflint-test-integration-structural-sharing-probe
                 reduce-inplace)
set_tests_properties(structural-sharing-probe-reduce-inplace
                     PROPERTIES TIMEOUT 30)
add_test(NAME structural-sharing-probe-add
         COMMAND hyperflint-test-integration-structural-sharing-probe
                 add)
set_tests_properties(structural-sharing-probe-add
                     PROPERTIES TIMEOUT 30)
add_test(NAME structural-sharing-probe-linear-factors
         COMMAND hyperflint-test-integration-structural-sharing-probe
                 linear-factors)
set_tests_properties(structural-sharing-probe-linear-factors
                     PROPERTIES TIMEOUT 30)
add_test(NAME structural-sharing-probe-partial-fractions
         COMMAND hyperflint-test-integration-structural-sharing-probe
                 partial-fractions)
set_tests_properties(structural-sharing-probe-partial-fractions
                     PROPERTIES TIMEOUT 30)

# HF FF Phase 6 REVISED §6.D iter-22 — TDD red-state entries (3 new
# ctest entries; ctest count 35 → 38 per design.md §5 deliverables 1+2).
# All three FAIL at iter-22a close (intentional); iter-22b production
# source change flips the local probe_*() stubs to call into
# hyperflint:: accessors and the subtests turn green. The single
# `section-6d-dfs-cap` entry runs all four iter-22 subtests inline (no
# argv dispatch) per design.md §5 deliverable 1 ctest-count contract.
add_test(NAME section-6d-dfs-cap
         COMMAND hyperflint-test-integration-section-6d-dfs-cap)
set_tests_properties(section-6d-dfs-cap PROPERTIES TIMEOUT 30)
add_test(NAME lf-cache-key-thread-invariant
         COMMAND hyperflint-test-linear-factors-cache-key-thread-invariant)
set_tests_properties(lf-cache-key-thread-invariant PROPERTIES TIMEOUT 30)
add_test(NAME zw-tab-readonly-under-cap
         COMMAND hyperflint-test-transform-zw-tab-readonly-under-cap)
set_tests_properties(zw-tab-readonly-under-cap PROPERTIES TIMEOUT 30)

add_test(NAME operator-memo
         COMMAND hyperflint-test-operator-memo)
# iter-66 Phase 63-δ: pass the HF CLI binary path AND the MZV reductions
# data path to the unit test so its §4.7 smoke-heavy-fixture-tst2-mini
# subtest can subprocess the production CLI under HF_OPERATOR_MEMO={0,1}
# on the Smirnov tst0 fixture for byte-identity verification. The
# CLI's eval-json `op=hyperflint` path loads MZV reductions before
# invoking the integrator; without a valid HF_MZV_DATA_PATH the CLI
# returns an 88-byte error JSON that would byte-id "pass" trivially
# (silent-failure trap; root-caused by iter-66 author). If either env
# var is unset at runtime (the test binary is launched directly,
# bypassing ctest), §4.7 SKIPs gracefully. TIMEOUT 300 accommodates
# 3 x 2 subprocess invocations on tst0 (~1-2 s each including
# subprocess startup + MZV-data load).
set_tests_properties(operator-memo PROPERTIES
    ENVIRONMENT "HF_BIN_PATH=$<TARGET_FILE:hyperflint-cli>;HF_MZV_DATA_PATH=${CMAKE_CURRENT_SOURCE_DIR}/data/mzv_reductions.json"
    TIMEOUT 300)
# HF FF Phase 5 §E iter-65 Phase 63-γ-bis: TSan-instrumented stress test.
# Only registered when HF_TSAN=ON (i.e. in HyperFLINT/build-tsan/); the
# test runs under TSan instrumentation and asserts 0 data races plus
# counter invariants (insert_count = hit_count = expected; miss_count =
# collision_count = 0).
if(HF_TSAN)
    add_test(NAME operator-memo-tsan
             COMMAND hyperflint-test-operator-memo-tsan)
    set_tests_properties(operator-memo-tsan PROPERTIES TIMEOUT 600
                         ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1 abort_on_error=0 second_deadlock_stack=1")
endif()
# HF MZV-rewrite C0c.1 (iter-60 / B3 follow-up): T4 integrator-level
# OMP determinism end-to-end test. Closes the coverage gap flagged by
# iter-59 binding reviewer (agentId ac81c3dd674cb73a9) advisory-3:
# the existing T1+T2+T3 ctest target above exercises ZWTable layer
# only; T4 drives the CLI binary through op=hyperflint on the
# non-W-empty findroots21_a/b fixtures under OMP={1,2,13} +
# HF_USE_SCALAR_REP=1, asserting sha256 byte-identity of obj['result']
# across thread counts. Subprocess (Option B) because runtime/scalar_rep.cpp
# caches HF_USE_SCALAR_REP via std::atomic<int>{-1}; in-process env
# toggling is structurally impossible after the first
# scalar_rep_enabled() call.
# Fixture corpus: notes/hf_mzv_rewrite_design_2026-05-05/c_prep_2_fixtures
# (relative to CMAKE_CURRENT_SOURCE_DIR/.. which is the repo root).
add_test(NAME omp-e2e-determinism
         COMMAND ${CMAKE_COMMAND} -E env "Python3=python3" --
                 python3
                 ${CMAKE_CURRENT_SOURCE_DIR}/test/integration/test_omp_e2e_determinism.py
                 --hf-bin $<TARGET_FILE:hyperflint-cli>
                 --mzv-data ${CMAKE_CURRENT_SOURCE_DIR}/data/mzv_reductions.json
                 --fixture-dir ${CMAKE_CURRENT_SOURCE_DIR}/../notes/hf_mzv_rewrite_design_2026-05-05/c_prep_2_fixtures)
set_tests_properties(omp-e2e-determinism PROPERTIES TIMEOUT 300)
