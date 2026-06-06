#!/usr/bin/env python3
# HF MZV-rewrite C0c.1 (iter-60 / B3 follow-up): T4 integrator-level
# OMP determinism integration test.
#
# Design refs:
#   notes/hf_mzv_rewrite_design_2026-05-05/design.md
#     §3.6a (cross-merge protocol; canonical-content sort with
#            handle-relabel-deterministic master assignment),
#     §4.4a (bit-identity is the advisory nondeterminism canary;
#            value-equivalence is the correctness gate),
#     §5.4  (OMP determinism test).
#
# Iter-59 review: verdict OK; advisory-3 (deferred, B3): the existing
# T1+T2+T3 unit tests in HyperFLINT/test/unit/test_omp_determinism.cpp
# exercise the ZWTable layer ONLY (combined.merge_into / master.merge_into
# directly). They do NOT drive integration_step end-to-end with
# HF_USE_SCALAR_REP=1 + multi-thread OMP on a W-non-empty fixture.
# T4 closes that gap.
#
# Iter-57 review caught the B1 race in the OUTER OMP parallel-for
# (integration_step.cpp:~1520) under HF_USE_SCALAR_REP=1 + W-non-empty +
# size>=16. Iter-58 fix landed Protocol A on the outer parallel-for. T4
# is the integrator-level verification that the fix holds end-to-end.
#
# Why subprocess (Option B) and not in-process (Option A):
#   HF_USE_SCALAR_REP and HF_SCALAR_REP_REQUIRE_PERSISTENT are one-shot
#   cached via std::atomic<int>{-1} sentinels in src/runtime/scalar_rep.cpp
#   (g_scalar_rep_state, g_require_persistent_state). Once any code path
#   has called runtime::scalar_rep_enabled() in a process, subsequent
#   setenv calls do not propagate. The only way to vary
#   HF_USE_SCALAR_REP=1 across a single test run is to spawn the CLI as
#   a subprocess for each cell. OMP_NUM_THREADS is also most reliably
#   set via env at process start (libgomp/libomp read it once during
#   team-creation).
#
# Test mechanism:
#   For each fixture in {findroots21_a, findroots21_b}:
#     For each OMP_NUM_THREADS in {1, 2, 13}:
#       Run ${HF_BIN} eval-json with HF_USE_SCALAR_REP=1 +
#       HF_RAT_SPLIT_VERIFY=0; pipe the request envelope on stdin;
#       capture stdout JSON. Extract obj['result'] and compute sha256
#       of its canonical JSON form. Record wall.
#     Assert the three sha256 values match.
#   Print PASS/FAIL summary; exit 0/1.
#
# Exit codes:
#   0 = all fixtures byte-identical across OMP={1,2,13}.
#   1 = one or more fixtures produced different sha256 values across
#       thread counts (regression in OMP determinism under SCALAR_REP=1).
#   2 = subprocess-level error (binary missing, fixture missing, JSON
#       parse error, etc.); test inconclusive.
#
# OMP_NUM_THREADS=13 default per feedback_hyperflint_threads.md
# (14-core machine, one core reserved). The test exercises {1, 2, 13}
# to cover the full thread-count variation space (single-thread baseline
# + small multi-thread + production thread count).
#
# Wall budget: ~3 ms compute per cell + ~50-200 ms process-startup +
# subprocess RPC overhead = ~700-1500 ms total for 2 fixtures x 3 thread
# counts = 6 cells. Well within ctest's default 300 s timeout.

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path


def build_request_envelope(fixture_path: Path, mzv_data_path: Path) -> str:
    fix = json.loads(fixture_path.read_text())
    integ = fix.get("expr") or fix.get("integrand") or fix.get("f")
    if integ is None:
        raise ValueError(
            f"fixture {fixture_path} has no expr/integrand/f field")
    # Per run_smirnov_phase_b_gate.sh: key='expr' if 'Log[' in integ
    # else 'f'. The findroots21 fixtures do not contain Log[], so 'f'
    # would be canonical, but the iter-56 U5 probe used 'expr' on the
    # same fixtures and the binary accepts both. Mirror the iter-56
    # convention to keep request shape parity.
    req = {
        "op": "hyperflint",
        "expr": integ,
        "vars_int": fix["vars_int"],
        "vars": fix["vars"],
        "mzv_data_path": str(mzv_data_path),
        "algebraic_letters": fix.get("algebraic_letters", True),
    }
    return json.dumps(req)


def run_one_cell(hf_bin: Path, req_str: str, omp_threads: int,
                 timeout_s: int) -> dict:
    env = dict(os.environ)
    env["HF_USE_SCALAR_REP"] = "1"
    env["HF_RAT_SPLIT_VERIFY"] = "0"
    env["OMP_NUM_THREADS"] = str(omp_threads)
    t0 = time.time()
    try:
        proc = subprocess.run(
            [str(hf_bin), "eval-json"],
            input=req_str,
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout_s,
        )
    except subprocess.TimeoutExpired as e:
        return {"ok": False, "error": f"timeout after {timeout_s}s",
                "wall_s": timeout_s, "stderr_tail": ""}
    wall = time.time() - t0

    if proc.returncode != 0:
        return {"ok": False,
                "error": f"exit={proc.returncode}",
                "wall_s": wall,
                "stderr_tail": "\n".join(proc.stderr.splitlines()[-10:])}
    try:
        obj = json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        return {"ok": False,
                "error": f"stdout not JSON: {e}",
                "wall_s": wall,
                "stdout_head": proc.stdout[:500],
                "stderr_tail": "\n".join(proc.stderr.splitlines()[-10:])}
    if "result" not in obj:
        return {"ok": False,
                "error": "no 'result' field in response",
                "wall_s": wall,
                "obj_keys": sorted(obj.keys()),
                "stderr_tail": "\n".join(proc.stderr.splitlines()[-10:])}
    # Canonical JSON form: sort_keys=True, no spaces. This collapses
    # any whitespace / key-order ambiguity in the JSON serialisation
    # of the result subtree, isolating sha256 differences to genuine
    # value-level divergence.
    canon = json.dumps(obj["result"], sort_keys=True, separators=(",", ":"))
    sha = hashlib.sha256(canon.encode("utf-8")).hexdigest()
    return {"ok": True,
            "sha256": sha,
            "wall_s": wall,
            "result_len": len(canon)}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--hf-bin", type=Path, required=True,
                   help="absolute path to hyperflint CLI binary")
    p.add_argument("--mzv-data", type=Path, required=True,
                   help="absolute path to mzv_reductions.json")
    p.add_argument("--fixture-dir", type=Path, required=True,
                   help="absolute path to c_prep_2_fixtures dir")
    p.add_argument("--threads", type=str, default="1,2,13",
                   help="comma-separated OMP_NUM_THREADS values "
                        "(default: 1,2,13)")
    p.add_argument("--fixtures", type=str,
                   default="findroots21_a,findroots21_b",
                   help="comma-separated fixture names "
                        "(default: findroots21_a,findroots21_b)")
    p.add_argument("--timeout", type=int, default=60,
                   help="per-cell subprocess timeout in seconds "
                        "(default: 60)")
    args = p.parse_args()

    if not args.hf_bin.is_file():
        print(f"[T4] ERROR: --hf-bin {args.hf_bin} is not a file",
              file=sys.stderr)
        return 2
    if not os.access(args.hf_bin, os.X_OK):
        print(f"[T4] ERROR: --hf-bin {args.hf_bin} is not executable",
              file=sys.stderr)
        return 2
    if not args.mzv_data.is_file():
        print(f"[T4] ERROR: --mzv-data {args.mzv_data} is not a file",
              file=sys.stderr)
        return 2
    if not args.fixture_dir.is_dir():
        print(f"[T4] ERROR: --fixture-dir {args.fixture_dir} is not a "
              "directory", file=sys.stderr)
        return 2

    threads = [int(t.strip()) for t in args.threads.split(",") if t.strip()]
    fixtures = [f.strip() for f in args.fixtures.split(",") if f.strip()]

    print(f"[T4] hf_bin={args.hf_bin}")
    print(f"[T4] mzv_data={args.mzv_data}")
    print(f"[T4] fixture_dir={args.fixture_dir}")
    print(f"[T4] threads={threads}")
    print(f"[T4] fixtures={fixtures}")
    print(f"[T4] HF_USE_SCALAR_REP=1 (forced)")
    print(f"[T4] HF_RAT_SPLIT_VERIFY=0 (forced)")
    print(f"[T4] timeout={args.timeout}s per cell")
    print()

    overall_pass = True
    n_inconclusive = 0
    per_fixture: dict = {}

    for fixture_name in fixtures:
        fixture_path = args.fixture_dir / f"{fixture_name}.json"
        if not fixture_path.is_file():
            print(f"[T4] ERROR: fixture {fixture_path} not found",
                  file=sys.stderr)
            n_inconclusive += 1
            continue
        try:
            req_str = build_request_envelope(fixture_path, args.mzv_data)
        except Exception as e:
            print(f"[T4] ERROR: fixture {fixture_name} envelope build "
                  f"failed: {e}", file=sys.stderr)
            n_inconclusive += 1
            continue

        cells: dict = {}
        per_fixture[fixture_name] = cells
        print(f"[T4] === fixture: {fixture_name} ===")
        for omp in threads:
            outcome = run_one_cell(args.hf_bin, req_str, omp, args.timeout)
            cells[omp] = outcome
            if not outcome["ok"]:
                print(f"[T4] {fixture_name} OMP={omp:>2}  "
                      f"INCONCLUSIVE wall={outcome['wall_s']:.3f}s  "
                      f"error={outcome['error']}")
                if "stderr_tail" in outcome and outcome["stderr_tail"]:
                    for line in outcome["stderr_tail"].splitlines():
                        print(f"      stderr: {line}")
                n_inconclusive += 1
            else:
                print(f"[T4] {fixture_name} OMP={omp:>2}  "
                      f"PASS wall={outcome['wall_s']:.3f}s  "
                      f"sha={outcome['sha256'][:16]}...  "
                      f"result_len={outcome['result_len']}")

        # Per-fixture cross-thread sha256 invariance assertion.
        oks = [(omp, c) for omp, c in cells.items() if c["ok"]]
        if not oks:
            print(f"[T4] {fixture_name}  ALL CELLS INCONCLUSIVE; cannot "
                  "compare", file=sys.stderr)
            overall_pass = False
            continue
        if len(oks) < len(threads):
            print(f"[T4] {fixture_name}  PARTIAL: {len(oks)}/{len(threads)} "
                  "cells succeeded; treating as test failure", file=sys.stderr)
            overall_pass = False
            continue
        shas = [c["sha256"] for _, c in oks]
        if len(set(shas)) == 1:
            print(f"[T4] {fixture_name}  byte-identity ACROSS OMP={threads}: "
                  f"PASS sha={shas[0][:16]}...")
        else:
            print(f"[T4] {fixture_name}  byte-identity ACROSS OMP={threads}: "
                  f"FAIL", file=sys.stderr)
            for omp, c in oks:
                print(f"      OMP={omp:>2}  sha={c['sha256']}",
                      file=sys.stderr)
            overall_pass = False
        print()

    print("[T4] === summary ===")
    if n_inconclusive > 0:
        print(f"[T4] WARNING: {n_inconclusive} cells were inconclusive")
    if overall_pass and n_inconclusive == 0:
        print("[T4] RESULT: PASS  (all fixtures byte-identical across OMP "
              f"thread counts {threads} under HF_USE_SCALAR_REP=1)")
        return 0
    elif overall_pass and n_inconclusive > 0:
        print("[T4] RESULT: INCONCLUSIVE  (no failures observed but some "
              "cells could not be compared)", file=sys.stderr)
        return 2
    else:
        print("[T4] RESULT: FAIL  (OMP-determinism regression detected; "
              "see diffs above)", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
