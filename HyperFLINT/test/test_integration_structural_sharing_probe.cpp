// HF FF Phase 6 REVISED §6.P iter-15 — TDD red-state scaffolding for the
// structural-sharing-potential probe (§A.P / Lever 6.P; REVIEWER P6'
// BINDING byte-weighted activation gate).
//
// Design memo:
//   notes/hf_finite_field_program/phase6_combined/probe_a_p_persistent_data/design.md
//   (~287 LOC at iter-15 cold-start; §1 hypothesis, §2 probe design,
//    §2.1 unchanged-bytes measurement protocol, §2.2 FOLD-DC5 stratified
//    sampling discipline, §4 alternatives audit, §7 13 load-bearing folds).
//
// Promotion rationale (iter-14 verdict §7.2 + handoff.md §6.P PROMOTED).
// iter-14 §6.D probe sweep DOWNGRADED §6.D to research-track: REQ-E
// ceiling formula misapplied on tst2 (6.22) and parity_1 (5.13), both
// > 1.0 which is nominally impossible; entry_bytes_aggregate_omp captures
// transient intra-step byte-flow absorbed by mimalloc pools, not
// BFS-level-resident state attackable by §6.D's BFS→DFS rewrite. §6.P
// (persistent-data / structural-sharing) is the next heavy lever: hypothesis
// is that LF_cache_bytes + RegulatorSym_bytes + Rat/SymCoef/Poly persistent
// inter-step state dominates RSS on tst2/parity_1; structural sharing via
// `shared_ptr<const T>` + path-copying could yield 3-5x transient peak
// reduction.
//
// Purpose.  Lock in the §6.P probe schema (4 entry points × per-call
// emission of {op, pre_bytes, post_bytes, unchanged_bytes}) BEFORE the
// production source change at iter-17. All four subtests assert the
// instrumentation exists and FAIL today; once iter-17's source change
// lands (env-gated probe hooks at the 4 entry points + drain API +
// per-entry-point `hf_probe_structural_sharing_<op>_instrumented()`
// accessors), each probe function below will be flipped to call into
// the production API and the subtests will turn green.
//
// Mirror of iter-12 §6.D pattern.  This file mirrors
// HyperFLINT/test/test_integration_node_rss_4col.cpp (iter-12
// commit d640fa0ee) — one executable, four `add_test` entries (one per
// entry point), each pinning the subtest with a single argv argument.
// Subtests are strictly independent; failure of one does not perturb the
// others.
//
// Probe convention.  Each subtest delegates the actual existence check
// to a local `probe_*_instrumented()` function, all defined at the
// bottom of this file in one block. These probes return `false` today
// (iter-15 red-state: §6.P instrumentation not yet present in production).
// iter-17's source change will:
//   (i)   Wire env-gated probe + byte-size friends at 4 entry points
//         (`Rat::reduce_inplace`, `Rat::add`, `linear_factors`,
//          `partial_fractions`);
//   (ii)  Add a `hyperflint::structural_sharing::probe_<op>_instrumented()`
//         (and companion accessors for record drain) in a new
//         include/hyperflint/diagnostics/structural_sharing_probe.hpp /
//         src/diagnostics/structural_sharing_probe.cpp pair;
//   (iii) Flip the four `probe_*_instrumented()` definitions below to
//         call the corresponding production accessors; the tests then
//         PASS.
//
// Compile-only fixture.  The test compiles against the current headers
// (no `structural_sharing_probe.hpp` exists yet — see §6 of the design
// memo for the planned iter-17 file list). The expected record schema
// is forward-declared here in a local `ExpectedStructuralSharingRecord`
// struct only as a documentation aid; we do NOT static_assert against
// the production schema (that would block the iter-17 ratification at
// compile-time, which is undesirable: we want the test to remain
// runnable across the iter-15 → iter-17 transition).
//
// REQ-G (iter-11 reviewer ab6fa5cda36908669) carry. This file is the
// FULL TDD red-state test (4 subtests, 4 ctest entries, 4 FAIL on first
// run). It is NOT a skeleton.
//
// REQ-13.4 carry (iter-13 BINDING reviewer aad9b0038dad42d4c). iter-12
// originally landed Part-1-only (schema-presence accessor); iter-13
// expanded each subtest to a 3-part anchor (Part 1 schema presence +
// Part 2 sentinel round-trip on directly constructed record + Part 3
// sampling-path observability). For iter-15 §6.P, we follow the
// iter-12 ORIGINAL pattern (Part 1 only) because (a) the production
// API (drain, sentinel-emit) does not yet exist at iter-15, (b) iter-17
// BINDING reviewer will likely demand the Parts 2-3 anchors as a
// REQ-17.* fold mirroring REQ-13.4, and (c) the iter-15 scaffolding's
// purpose is to lock in the per-entry-point dispatch + naming
// conventions, not to anchor sampling-path behavior. iter-17/iter-18
// will expand each subtest to Parts 2-3 after the production source
// change lands.
//
// FOLD-DC5 carry. The §6.P probe MUST honor stratified-sampling
// discipline (design.md §2.2). The iter-15 test does NOT exercise
// stratified sampling (that requires the production code path); iter-17
// BINDING reviewer pre-build scope is the gate for FOLD-DC5
// verification on the production source.

#include <cmath>     // std::fabs (REQ-16.4 fold; iter-17 Part-2 sentinel round-trip)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>    // drain_records() return type (REQ-16.4 fold; iter-17)

// iter-17 production source change: flip the local probe_*_instrumented()
// stubs (defined at the bottom of this file) to delegate to
// hyperflint::structural_sharing::probe_*_instrumented() which live in
// libhyperflint (src/diagnostics/structural_sharing_probe.cpp).
#include "hyperflint/diagnostics/structural_sharing_probe.hpp"

namespace {

// ---------------------------------------------------------------------------
// Local forward-declared mirror of the EXPECTED post-iter-17
// StructuralSharingRecord layout (per design.md §2 probe design). This is
// a documentation aid only; the test does NOT assume binary compatibility
// with the production struct.
//
// iter-17 production source change will install
// `hyperflint::structural_sharing::Record` with the four fields below;
// until then, this struct exists only in the test translation unit and
// serves to lock in field names + types.
// ---------------------------------------------------------------------------
struct ExpectedStructuralSharingRecord {
    // Entry-point identifier; one of {"reduce_inplace", "add",
    // "linear_factors", "partial_fractions"}.
    const char* op;

    // Total byte size of operands BEFORE the mutation/computation.
    //   - reduce_inplace: bytes(num) + bytes(den).
    //   - add:            bytes(a) + bytes(b).
    //   - linear_factors: bytes(p).
    //   - partial_fractions: bytes(f).
    int64_t pre_bytes;

    // Total byte size of result AFTER the mutation/computation.
    //   - reduce_inplace: bytes(num') + bytes(den')   (after GCD-cancel).
    //   - add:            bytes(a + b).
    //   - linear_factors: bytes(factorisation) summed over factors.
    //   - partial_fractions: bytes(fractions) summed over fractions.
    int64_t post_bytes;

    // Bytes that appear in both pre and post via structural equivalence
    // (design.md §2.1 measurement protocol). For monomial-level
    // operations: byte-size of the intersection set, with `fmpz`
    // indirect storage included. For factor-level operations
    // (linear_factors, partial_fractions): byte-size of factors /
    // fractions that appear in both pre and post lists.
    int64_t unchanged_bytes;

    // Derived as (pre_bytes + post_bytes - 2*unchanged_bytes) /
    // (pre_bytes + post_bytes); the iter-17 aggregator computes this on
    // the receiver side, so the record may emit it as a convenience or
    // omit it. iter-15 forward-declares the field; iter-17 may move it
    // into the aggregator only.
    double frac_changed;
};

// ---------------------------------------------------------------------------
// Probe functions — local fallbacks (return false at iter-15).
//
// iter-17 production source change will flip each of these to delegate
// to a new production accessor in
// `hyperflint::structural_sharing::`, e.g.:
//
//   bool probe_reduce_inplace_instrumented() {
//       return hyperflint::structural_sharing::probe_reduce_inplace_instrumented();
//   }
//
// Until then, all four return false → the subtests FAIL.
//
// Production accessor signatures (planned at iter-17):
//
//   namespace hyperflint::structural_sharing {
//     bool probe_reduce_inplace_instrumented();
//     bool probe_add_instrumented();
//     bool probe_linear_factors_instrumented();
//     bool probe_partial_fractions_instrumented();
//
//     // Drain API (iter-17 + iter-18 Parts 2-3 anchor):
//     struct Record { /* see ExpectedStructuralSharingRecord above */ };
//     void reset_records();
//     std::vector<Record> drain_records();
//   }  // namespace hyperflint::structural_sharing
// ---------------------------------------------------------------------------
bool probe_reduce_inplace_instrumented();
bool probe_add_instrumented();
bool probe_linear_factors_instrumented();
bool probe_partial_fractions_instrumented();

// ---------------------------------------------------------------------------
// REQ-16.4 Part-2 sentinel round-trip helper (iter-17; mirror of
// REQ-13.4 carry on `test_integration_node_rss_4col.cpp`).
//
// For a given op_tag, the helper:
//   1. Clears the probe's drain aggregate via reset_records() (this
//      also re-reads the env var so the master gate stays enabled).
//   2. Re-asserts HF_STRUCTURAL_SHARING_PROBE=1 (reset_records()
//      re-reads env; the calling subtest already setenv'd before
//      Part 1).
//   3. Forces full emission by ensuring sample_rate <= 1 via
//      HF_STRUCTURAL_SHARING_PROBE_SAMPLE_RATE=0 (full emission).
//   4. Directly invokes
//      hyperflint::structural_sharing::emit(op_tag, 100, 80, 50).
//   5. Calls drain_records().
//   6. Asserts the returned vector contains exactly ONE record with
//      op == op_tag, pre_bytes == 100, post_bytes == 80,
//      unchanged_bytes == 50, and frac_changed == (100+80-2*50)/(100+80)
//      = 0.4444... (ε-tolerance 1e-9).
//
// Failure prints a [FAIL] line keyed by op_tag and returns 1.  On
// success returns 0.
// ---------------------------------------------------------------------------
int part2_sentinel_round_trip(const char* op_tag,
                              const char* subtest_pretty_name) {
    setenv("HF_STRUCTURAL_SHARING_PROBE", "1", /*overwrite=*/1);
    setenv("HF_STRUCTURAL_SHARING_PROBE_SAMPLE_RATE", "0", /*overwrite=*/1);
    hyperflint::structural_sharing::reset_records();

    // Sanity check: the master gate must still report enabled after the
    // reset (reset_records() re-reads env; we just setenv'd above).
    if (!hyperflint::structural_sharing::probe_reduce_inplace_instrumented()) {
        std::fprintf(stderr,
            "[FAIL] %s Part-2 sentinel: master gate disabled after "
            "reset_records()+setenv. Expected enabled.\n",
            subtest_pretty_name);
        return 1;
    }

    // Direct emit with sentinel byte values 100/80/50.
    hyperflint::structural_sharing::emit(op_tag, /*pre=*/100, /*post=*/80,
                                         /*unchanged=*/50);

    auto records = hyperflint::structural_sharing::drain_records();
    if (records.size() != 1) {
        std::fprintf(stderr,
            "[FAIL] %s Part-2 sentinel: expected exactly 1 record after "
            "emit(\"%s\", 100, 80, 50) + drain_records(); got %zu records.\n",
            subtest_pretty_name, op_tag, records.size());
        return 1;
    }
    const auto& r = records[0];
    if (r.op == nullptr || std::strcmp(r.op, op_tag) != 0) {
        std::fprintf(stderr,
            "[FAIL] %s Part-2 sentinel: expected op=\"%s\"; got op=\"%s\".\n",
            subtest_pretty_name, op_tag, r.op ? r.op : "(null)");
        return 1;
    }
    if (r.pre_bytes != 100 || r.post_bytes != 80 || r.unchanged_bytes != 50) {
        std::fprintf(stderr,
            "[FAIL] %s Part-2 sentinel: expected "
            "pre=100,post=80,unchanged=50; got pre=%lld,post=%lld,"
            "unchanged=%lld.\n",
            subtest_pretty_name,
            (long long)r.pre_bytes, (long long)r.post_bytes,
            (long long)r.unchanged_bytes);
        return 1;
    }
    // frac_changed = (100 + 80 - 2*50) / (100 + 80) = 80/180 = 0.4444444...
    const double expected_frac =
        (100.0 + 80.0 - 2.0 * 50.0) / (100.0 + 80.0);
    if (std::fabs(r.frac_changed - expected_frac) > 1e-9) {
        std::fprintf(stderr,
            "[FAIL] %s Part-2 sentinel: expected frac_changed≈%.12f; "
            "got %.12f (|delta|=%.3e > 1e-9 tolerance).\n",
            subtest_pretty_name, expected_frac, r.frac_changed,
            std::fabs(r.frac_changed - expected_frac));
        return 1;
    }
    // Clean up the env var we forced full-emission on.
    unsetenv("HF_STRUCTURAL_SHARING_PROBE_SAMPLE_RATE");
    hyperflint::structural_sharing::reset_records();
    std::fprintf(stdout,
        "[PASS] %s Part-2 sentinel round-trip\n", subtest_pretty_name);
    return 0;
}

// ---------------------------------------------------------------------------
// Subtest 1 — reduce_inplace instrumented under HF_STRUCTURAL_SHARING_PROBE=1.
//
// §1 hypothesis (design.md). `Rat::reduce_inplace` modifies only the
// GCD-cancellation portion, leaving most numerator/denominator
// coefficient bytes untouched. The probe emits, on each call:
//
//   op="reduce_inplace", pre_bytes=bytes(num)+bytes(den),
//   post_bytes=bytes(num')+bytes(den'),
//   unchanged_bytes=Σ bytes(monomials shared pre↔post).
//
// Hook site (design.md §6): src/core/rat.cpp (file-local
// `reduce_inplace(Poly& num, Poly& den)` at rat.cpp:1527 + the
// `reduce_inplace_impl` body at rat.cpp:1045). The probe wraps the
// public entry-point so both cache HIT and MISS paths emit (HIT path's
// pre/post-bytes are identical to the cached payload).
//
// Current state (iter-15): probe returns false → FAIL.
// After iter-17: production source installs the probe hook + flips
//                probe_reduce_inplace_instrumented() → PASS.
// ---------------------------------------------------------------------------
int test_emit_reduce_inplace() {
    setenv("HF_STRUCTURAL_SHARING_PROBE", "1", /*overwrite=*/1);

    if (!probe_reduce_inplace_instrumented()) {
        std::fprintf(stderr,
            "[FAIL] reduce_inplace NOT instrumented under "
            "HF_STRUCTURAL_SHARING_PROBE=1. §A.P / Lever 6.P (REVIEWER P6' "
            "BINDING byte-weighted activation gate) requires per-call "
            "emission of {op='reduce_inplace', pre_bytes, post_bytes, "
            "unchanged_bytes} from src/core/rat.cpp::reduce_inplace "
            "(rat.cpp:1527 public entry-point + rat.cpp:1045 impl body). "
            "iter-17 production source change will install the hook and "
            "flip probe_reduce_inplace_instrumented() to return true. "
            "Intentional iter-15 TDD red-state.\n");
        return 1;
    }
    std::fprintf(stdout, "[PASS] structural-sharing-probe/reduce-inplace\n");
    // REQ-16.4 Part-2 (iter-17 BINDING reviewer ae98902a768f7f242): sentinel
    // round-trip via emit() → drain_records() with byte values 100/80/50.
    if (int rc = part2_sentinel_round_trip(
                     "reduce_inplace",
                     "structural-sharing-probe/reduce-inplace");
        rc != 0) {
        return rc;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Subtest 2 — Rat::add instrumented under HF_STRUCTURAL_SHARING_PROBE=1.
//
// §1 hypothesis (design.md). `Rat::add(a, b)` produces
// `Rat(a.num*b.den + b.num*a.den, a.den*b.den)`; the `a.num` and `b.den`
// subtrees are shareable if the result is constructed via path-copying.
// The probe emits, on each call:
//
//   op="add", pre_bytes=bytes(a)+bytes(b),
//   post_bytes=bytes(a+b),
//   unchanged_bytes=Σ bytes(monomials in a+b matching a coefficient
//                  in a or b).
//
// Hook site (design.md §6): src/core/rat.cpp public `Rat::add(const Rat&)`
// (header at include/hyperflint/core/rat.hpp:161; impl at
// rat.cpp:2418). Per design.md §2 NOTE: the probe emits per `Rat::add`
// (operator+) call only — the `operator+=` in-place case has
// shareable-bytes well-defined only for non-in-place mutations, so the
// probe SKIPS operator+= per FOLD-DC5 stratification discipline.
//
// Current state (iter-15): probe returns false → FAIL.
// ---------------------------------------------------------------------------
int test_emit_add() {
    setenv("HF_STRUCTURAL_SHARING_PROBE", "1", /*overwrite=*/1);

    if (!probe_add_instrumented()) {
        std::fprintf(stderr,
            "[FAIL] Rat::add NOT instrumented under "
            "HF_STRUCTURAL_SHARING_PROBE=1. §A.P / Lever 6.P requires "
            "per-call emission of {op='add', pre_bytes, post_bytes, "
            "unchanged_bytes} from src/core/rat.cpp::Rat::add "
            "(rat.cpp:2418; operator+ only — operator+= skipped per "
            "FOLD-DC5 stratification). iter-17 production source change "
            "will install the hook and flip probe_add_instrumented(). "
            "Intentional iter-15 TDD red-state.\n");
        return 1;
    }
    std::fprintf(stdout, "[PASS] structural-sharing-probe/add\n");
    // REQ-16.4 Part-2 (iter-17 BINDING reviewer ae98902a768f7f242).
    if (int rc = part2_sentinel_round_trip(
                     "add", "structural-sharing-probe/add");
        rc != 0) {
        return rc;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Subtest 3 — linear_factors instrumented under HF_STRUCTURAL_SHARING_PROBE=1.
//
// §1 hypothesis (design.md). `linear_factors` results re-use many of the
// same factor polynomials across distinct call sites (the entire
// motivation for `$LinearFactorsCache`). The probe emits, on each call:
//
//   op="linear_factors", pre_bytes=bytes(p),
//   post_bytes=Σ bytes(factor_i) over the returned factorisation,
//   unchanged_bytes=Σ bytes(factor_i) where factor_i appears in BOTH
//                  the pre-call cache state AND the returned
//                  factorisation list (factor-level structural
//                  equivalence per design.md §2.1).
//
// Hook site (design.md §6): src/algebra/linear_factors.cpp public
// `linear_factors(const Poly&, size_t, ...)` (header at
// include/hyperflint/algebra/linear_factors.hpp:95; impl at
// linear_factors.cpp:1782). NOTE design memo §6 said
// `src/core/linear_factors.cpp` but the actual file lives in
// `src/algebra/` — iter-17 production source change will use the
// correct path.
//
// Current state (iter-15): probe returns false → FAIL.
// ---------------------------------------------------------------------------
int test_emit_linear_factors() {
    setenv("HF_STRUCTURAL_SHARING_PROBE", "1", /*overwrite=*/1);

    if (!probe_linear_factors_instrumented()) {
        std::fprintf(stderr,
            "[FAIL] linear_factors NOT instrumented under "
            "HF_STRUCTURAL_SHARING_PROBE=1. §A.P / Lever 6.P requires "
            "per-call emission of {op='linear_factors', pre_bytes, "
            "post_bytes, unchanged_bytes} from "
            "src/algebra/linear_factors.cpp::linear_factors "
            "(linear_factors.cpp:1782; factor-level structural "
            "equivalence per design.md §2.1). iter-17 production source "
            "change will install the hook and flip "
            "probe_linear_factors_instrumented(). Intentional iter-15 "
            "TDD red-state.\n");
        return 1;
    }
    std::fprintf(stdout, "[PASS] structural-sharing-probe/linear-factors\n");
    // REQ-16.4 Part-2 (iter-17 BINDING reviewer ae98902a768f7f242).
    if (int rc = part2_sentinel_round_trip(
                     "linear_factors",
                     "structural-sharing-probe/linear-factors");
        rc != 0) {
        return rc;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Subtest 4 — partial_fractions instrumented under HF_STRUCTURAL_SHARING_PROBE=1.
//
// §1 hypothesis (design.md). `partial_fractions` decomposition produces
// a sum of fractions where each fraction's denominator is a shared
// linear factor. The probe emits, on each call:
//
//   op="partial_fractions", pre_bytes=bytes(f),
//   post_bytes=Σ bytes(fraction_i) over the decomposition,
//   unchanged_bytes=Σ bytes(fraction_i) where fraction_i has a
//                  denominator already present in the LF cache AND a
//                  numerator that appears (structurally) in `f`.
//
// Hook site (design.md §6): src/algebra/partial_fractions.cpp public
// `partial_fractions(...)` (header at
// include/hyperflint/algebra/partial_fractions.hpp:64; impl at
// partial_fractions.cpp:444). NOTE design memo §6 said
// `src/core/partial_fractions.cpp` but the actual file lives in
// `src/algebra/` — iter-17 production source change will use the
// correct path.
//
// Current state (iter-15): probe returns false → FAIL.
// ---------------------------------------------------------------------------
int test_emit_partial_fractions() {
    setenv("HF_STRUCTURAL_SHARING_PROBE", "1", /*overwrite=*/1);

    if (!probe_partial_fractions_instrumented()) {
        std::fprintf(stderr,
            "[FAIL] partial_fractions NOT instrumented under "
            "HF_STRUCTURAL_SHARING_PROBE=1. §A.P / Lever 6.P requires "
            "per-call emission of {op='partial_fractions', pre_bytes, "
            "post_bytes, unchanged_bytes} from "
            "src/algebra/partial_fractions.cpp::partial_fractions "
            "(partial_fractions.cpp:444; fraction-level structural "
            "equivalence per design.md §2.1). iter-17 production source "
            "change will install the hook and flip "
            "probe_partial_fractions_instrumented(). Intentional iter-15 "
            "TDD red-state.\n");
        return 1;
    }
    std::fprintf(stdout,
                 "[PASS] structural-sharing-probe/partial-fractions\n");
    // REQ-16.4 Part-2 (iter-17 BINDING reviewer ae98902a768f7f242).
    if (int rc = part2_sentinel_round_trip(
                     "partial_fractions",
                     "structural-sharing-probe/partial-fractions");
        rc != 0) {
        return rc;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Probe definitions (LOCAL, iter-15 red-state).
//
// All four return false; iter-17 production source change will flip each
// to delegate to a production accessor in
// `hyperflint::structural_sharing::`. The accessor's return value at
// iter-17+ is the env-gated activation predicate
// (HF_STRUCTURAL_SHARING_PROBE=1 turns it true; default-OFF
// returns false).
//
// Anchor the expected schema's minimum size via a one-shot static_assert
// that the struct holds at least 4 documented columns (one int64_t each
// for pre_bytes, post_bytes, unchanged_bytes — `op` is a const char*,
// `frac_changed` a double). This both suppresses unused-symbol
// diagnostics on `ExpectedStructuralSharingRecord` and pins the
// documented schema's bottom layout-size against accidental shrinkage.
// ---------------------------------------------------------------------------
static_assert(
    sizeof(ExpectedStructuralSharingRecord) >= 3 * sizeof(int64_t),
    "ExpectedStructuralSharingRecord must hold at least pre_bytes, "
    "post_bytes, unchanged_bytes (3 int64_t columns) per design.md §2 "
    "probe design. iter-17 production source change will install the "
    "real hyperflint::structural_sharing::Record struct with the same "
    "schema.");

// iter-17: stubs flipped to delegate to production accessors in
// hyperflint::structural_sharing::.  setenv("HF_STRUCTURAL_SHARING_PROBE","1")
// performed by each subtest above causes the master gate to read true on the
// first probe_*_instrumented() call (which triggers the env-cache refresh).
bool probe_reduce_inplace_instrumented() {
    return hyperflint::structural_sharing::probe_reduce_inplace_instrumented();
}
bool probe_add_instrumented() {
    return hyperflint::structural_sharing::probe_add_instrumented();
}
bool probe_linear_factors_instrumented() {
    return hyperflint::structural_sharing::probe_linear_factors_instrumented();
}
bool probe_partial_fractions_instrumented() {
    return hyperflint::structural_sharing::probe_partial_fractions_instrumented();
}

}  // namespace

// ---------------------------------------------------------------------------
// main — single-subtest dispatch via argv[1].
//
// Usage:  test_integration_structural_sharing_probe
//             {reduce-inplace|add|linear-factors|partial-fractions}
//
// Each `add_test` entry in HyperFLINT/CMakeLists.txt passes exactly one
// subtest name; mismatched / missing argv is a usage error and returns 2.
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr,
            "usage: %s {reduce-inplace|add|linear-factors|partial-fractions}\n",
            argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "reduce-inplace")    return test_emit_reduce_inplace();
    if (mode == "add")               return test_emit_add();
    if (mode == "linear-factors")    return test_emit_linear_factors();
    if (mode == "partial-fractions") return test_emit_partial_fractions();

    std::fprintf(stderr, "unknown subtest mode: %s\n", mode.c_str());
    return 2;
}
