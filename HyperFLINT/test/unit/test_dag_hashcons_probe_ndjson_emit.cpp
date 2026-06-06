// HF FF Phase 5 §A.1 iter-53: REQ-3 BINDING unit test (closure-trigger
// adversarial-reviewer agentId `a643bd44c8210297c`) — CI coverage for the
// `HF_DAG_HASHCONS_PROBE_NDJSON=1` ndjson-emit path.
//
// Reviewer rationale (iter-53 REQ-3 fold, BINDING):
//
//     The iter-52 Class A fix (NDJSON env-gate; default OFF) prevents the
//     iter-51 disk-flood failure mode (tst2 reached 60.4 GB of ndjson in
//     under 2 minutes). The fix is correct, but the existing ctest cluster
//     (`dag-hashcons-probe-init` + `dag-hashcons-probe-off-no-op` +
//     `construction-path-dedup-rate-omp-invariance`) covers only the
//     OFF-path and the counter-only ON-path; *none* of the existing tests
//     exercise `HF_DAG_HASHCONS_PROBE_NDJSON=1`. A future iteration that
//     re-enables NDJSON=1 (e.g., for tst3 follow-on per FOLD-M11) gets
//     zero CI coverage on:
//       1. `g_ndjson_values.is_open()` startup race
//       2. emit-path branch (lines 362, 380-383 / 404, 418 of
//          dag_hashcons_probe.cpp): writing the ndjson `vc` record
//       3. iter-51 disk-flood failure mode (silent regression)
//
//     This test sets HF_DAG_HASHCONS_PROBE_NDJSON=1, drives a tiny
//     synthetic workload (3 Poly creates + 1 Rat create), and asserts:
//       (a) the ndjson file is created at `<outdir>/probe_a1_values.ndjson`
//       (b) the file is non-empty
//       (c) the file contains the iter-49 `{"k":"hdr",...}` header line
//       (d) the file contains at least one `"k":"vc"` value-create record
//       (e) the file is bounded in size (< 1 MB) on the tiny workload —
//           sanity check that emission is not running away
//       (f) the aggregate snapshot `probe_a1_aggregate.json` records
//           `"ndjson_enabled": true`
//
// ctest registers this under name `dag-hashcons-probe-ndjson-emit` with
// `ENVIRONMENT HF_DAG_HASHCONS_PROBE=1;HF_DAG_HASHCONS_PROBE_NDJSON=1;
//  HF_DAG_HASHCONS_PROBE_OUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/probe_ndjson_emit_test_out`
// so each test invocation gets its own output directory (avoiding
// collision with the OFF-path / counter-only tests which point at the
// build dir directly).

#include "hyperflint/instrumentation/dag_hashcons_probe.hpp"

#include <flint/fmpq_mpoly.h>
#include <gmp.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

using namespace hyperflint;

static int g_failures = 0;

static void check(bool cond, const char* label) {
    if (cond) {
        std::printf("[PASS] %s\n", label);
    } else {
        std::printf("[FAIL] %s\n", label);
        ++g_failures;
    }
}

// Read entire file into a string. Returns empty string if the file is
// missing or unreadable (the caller distinguishes via a separate
// stat-based existence check).
static std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

static off_t file_size(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return -1;
    return st.st_size;
}

// Counts the number of lines in `content` whose first JSON key is
// exactly `key`. Approximate (does NOT do a full JSON parse) but
// sufficient for the iter-49 ndjson schema which begins every line
// with `{"k":"…",`.
static int count_lines_starting_with_key(const std::string& content,
                                         const char*        kind_value) {
    int    count   = 0;
    size_t pos     = 0;
    char   needle[32];
    std::snprintf(needle, sizeof(needle), "\"k\":\"%s\"", kind_value);
    const std::string nstr(needle);
    while (true) {
        size_t hit = content.find(nstr, pos);
        if (hit == std::string::npos) break;
        ++count;
        pos = hit + nstr.size();
    }
    return count;
}

int main(int /*argc*/, char** /*argv*/) {
    // Defensive: refuse to run if the master gate is unset (the test would
    // mis-fire — it's asserting NDJSON-ON path behaviour).
    const char* env_master = std::getenv("HF_DAG_HASHCONS_PROBE");
    if (env_master == nullptr || env_master[0] == '\0' || env_master[0] == '0') {
        std::printf("[SKIP] HF_DAG_HASHCONS_PROBE is unset; the NDJSON-emit "
                    "test cannot exercise the ON+NDJSON path. Refusing.\n");
        return 2;
    }
    const char* env_ndjson = std::getenv("HF_DAG_HASHCONS_PROBE_NDJSON");
    if (env_ndjson == nullptr || env_ndjson[0] == '\0' || env_ndjson[0] == '0') {
        std::printf("[SKIP] HF_DAG_HASHCONS_PROBE_NDJSON is unset; the "
                    "NDJSON-emit test cannot exercise the ON+NDJSON path. "
                    "Refusing.\n");
        return 2;
    }
    const char* env_outdir = std::getenv("HF_DAG_HASHCONS_PROBE_OUT_DIR");
    if (env_outdir == nullptr || env_outdir[0] == '\0') {
        std::printf("[SKIP] HF_DAG_HASHCONS_PROBE_OUT_DIR is unset; the "
                    "NDJSON-emit test needs a writable output directory. "
                    "Refusing.\n");
        return 2;
    }

    // Create the output directory (idempotent). The CMake-registered test
    // points HF_DAG_HASHCONS_PROBE_OUT_DIR at a fresh per-test subdir of
    // the build tree, so we just need to mkdir -p it.
    {
        std::string mkcmd = std::string("mkdir -p '") + env_outdir + "'";
        int rc = std::system(mkcmd.c_str());
        if (rc != 0) {
            std::printf("[FAIL] mkdir -p %s failed (rc=%d)\n", env_outdir, rc);
            return 1;
        }
    }

    // Clean any stale ndjson + aggregate from a prior run, so we can be
    // sure the files we observe are produced by THIS run.
    std::string values_path    = std::string(env_outdir) + "/probe_a1_values.ndjson";
    std::string ops_path       = std::string(env_outdir) + "/probe_a1_ops.ndjson";
    std::string aggregate_path = std::string(env_outdir) + "/probe_a1_aggregate.json";
    std::remove(values_path.c_str());
    std::remove(ops_path.c_str());
    std::remove(aggregate_path.c_str());

    // Stage 1: probe init. Expect hf_probe_active=true AND the ndjson
    // stream to have been opened (we cannot directly observe the stream
    // state from outside the probe, but Stage 4 verifies its contents).
    hf_probe_init();
    check(hf_probe_active,
          "hf_probe_active is TRUE after hf_probe_init under "
          "HF_DAG_HASHCONS_PROBE=1, HF_DAG_HASHCONS_PROBE_NDJSON=1");

    // Stage 2: synthesize a tiny workload. 3 Poly creates + 1 Rat create.
    // Each emit fires one `vc` ndjson line; if the ndjson is open, all 4
    // lines plus the header will be in the file.
    fmpq_mpoly_ctx_t ctx;
    fmpq_mpoly_ctx_init(ctx, /*nvars=*/2, ORD_LEX);

    fmpq_mpoly_t p1, p2, p3;
    fmpq_mpoly_init(p1, ctx);
    fmpq_mpoly_init(p2, ctx);
    fmpq_mpoly_init(p3, ctx);
    fmpq_mpoly_set_si(p1, 17, ctx);                 // constant 17
    fmpq_mpoly_set_si(p2, 23, ctx);                 // constant 23
    {
        // x0^1 (a non-trivial monomial; gives p3 a single term with
        // n_terms = 1 so the emit records a non-zero payload_bytes_est).
        const char* var_names[] = {"x", "y"};
        fmpq_mpoly_set_str_pretty(p3, "x", var_names, ctx);
    }

    // Hand each Poly to the probe with a synthetic instance_id.
    hf_probe_emit_poly_create(reinterpret_cast<uintptr_t>(p1), p1, ctx);
    hf_probe_emit_poly_create(reinterpret_cast<uintptr_t>(p2), p2, ctx);
    hf_probe_emit_poly_create(reinterpret_cast<uintptr_t>(p3), p3, ctx);

    // One synthetic Rat create over (p1 / p2). The probe emits a `vc`
    // record at layer=Rat with the combined hash.
    uintptr_t rat_id = static_cast<uintptr_t>(0xdeadbeefULL);
    hf_probe_emit_rat_create(rat_id, p1, p2, ctx);

    // Drop dtor emits to balance the create counts (good ndjson hygiene).
    hf_probe_emit_poly_destroy(reinterpret_cast<uintptr_t>(p1));
    hf_probe_emit_poly_destroy(reinterpret_cast<uintptr_t>(p2));
    hf_probe_emit_poly_destroy(reinterpret_cast<uintptr_t>(p3));
    hf_probe_emit_rat_destroy(rat_id);

    fmpq_mpoly_clear(p1, ctx);
    fmpq_mpoly_clear(p2, ctx);
    fmpq_mpoly_clear(p3, ctx);
    fmpq_mpoly_ctx_clear(ctx);

    // Stage 3: flush + close. hf_probe_finalize is idempotent (CAS-guarded
    // per iter-52 Class C fix); calling it here forces the ndjson + aggregate
    // to disk so we can stat them BEFORE the std::atexit-registered second
    // call fires at process exit.
    hf_probe_finalize();

    // Stage 4: verify the ndjson + aggregate exist and contain the
    // expected records.
    off_t values_sz    = file_size(values_path);
    off_t aggregate_sz = file_size(aggregate_path);
    check(values_sz > 0,
          "probe_a1_values.ndjson exists and is non-empty");
    check(aggregate_sz > 0,
          "probe_a1_aggregate.json exists and is non-empty");

    // Bound check: even a tiny 4-create workload should fit comfortably
    // under 1 MB. The bound mirrors the iter-51 disk-flood failure mode
    // (~60 GB on a heavy fixture) — if this synthetic workload comes out
    // anywhere near 1 MB, something has regressed in the emit path.
    const off_t kMaxValuesBytes = 1 << 20;  // 1 MiB
    check(values_sz < kMaxValuesBytes,
          "probe_a1_values.ndjson is bounded (< 1 MiB on synthetic 4-create "
          "workload)");
    std::printf("       (values ndjson size: %lld bytes)\n",
                (long long)values_sz);

    std::string vcontent = slurp(values_path);
    int hdr_lines = count_lines_starting_with_key(vcontent, "hdr");
    int vc_lines  = count_lines_starting_with_key(vcontent, "vc");
    int vd_lines  = count_lines_starting_with_key(vcontent, "vd");
    check(hdr_lines == 1,
          "probe_a1_values.ndjson contains exactly 1 header line "
          "(\\\"k\\\":\\\"hdr\\\")");
    check(vc_lines >= 1,
          "probe_a1_values.ndjson contains at least 1 value-create line "
          "(\\\"k\\\":\\\"vc\\\")");
    std::printf("       (ndjson line counts: hdr=%d, vc=%d, vd=%d)\n",
                hdr_lines, vc_lines, vd_lines);

    // Verify the aggregate records ndjson_enabled=true. We do a substring
    // search rather than a full JSON parse to avoid a dependency on a
    // JSON library here; the iter-49 emit format pins this line shape.
    std::string acontent = slurp(aggregate_path);
    bool aggregate_has_ndjson_true =
        acontent.find("\"ndjson_enabled\": true") != std::string::npos;
    check(aggregate_has_ndjson_true,
          "probe_a1_aggregate.json records \\\"ndjson_enabled\\\": true");

    if (g_failures > 0) {
        std::printf("=== FAIL: %d check(s) failed ===\n", g_failures);
        return 1;
    }
    std::printf("=== PASS: all checks passed ===\n");
    return 0;
}
