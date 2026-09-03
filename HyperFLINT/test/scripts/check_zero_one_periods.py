#!/usr/bin/env python3
"""Issue #52 round 5 regression: unsupported 0->1 boundary periods.

Runs the face-1 payload of GitHub issue #52 (round 5) in its recorded
integration order through `hyperflint eval-json`:
  mode "mint"    -> must exit 0 and report exactly the seven minted atoms in
                    "zero_one_periods" (the run used to die with SIGABRT).
  mode "exhaust" -> with HF_ZERO_ONE_ATOM_POOL=1 the second distinct word
                    exhausts the pool; the exception must cross the OpenMP
                    region as a structured {"failed":true,...} response
                    (exit 0), never as a crash.
"""
import json, os, subprocess, sys

binary, fixture, mode = sys.argv[1], sys.argv[2], sys.argv[3]
req = json.load(open(fixture))
# The MZV reduction table lives in the source tree; the fixture carries no
# absolute path (argv[4] is supplied by CMake as ${CMAKE_SOURCE_DIR}/data/...).
if len(sys.argv) > 4:
    req["mzv_data_path"] = sys.argv[4]
env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "OMP_NUM_THREADS": "2", "HF_MEM_BUDGET_MB": "8000"}
if mode == "exhaust":
    env["HF_ZERO_ONE_ATOM_POOL"] = "1"
if mode == "reserved":
    # A user variable that collides with a reserved atom name must be
    # rejected with a structured error, never silently merged with the atom.
    req["vars"] = list(req["vars"]) + ["zop_1"]
p = subprocess.run([binary, "eval-json"], input=json.dumps(req), capture_output=True, text=True, timeout=900, env=env)
if p.returncode != 0:
    print(f"FAIL: exit {p.returncode}; stderr tail: {p.stderr.strip().splitlines()[-1:] if p.stderr.strip() else ''}")
    sys.exit(1)
try:
    r = json.loads(p.stdout)
except Exception as e:
    print("FAIL: non-JSON response:", p.stdout[:200]); sys.exit(1)
if mode == "mint":
    zop = r.get("zero_one_periods")
    expected = {"zop_1": [0, 1, -1, -2, -1], "zop_2": [0, -1, 1, -2, -1], "zop_3": [0, -1, -2, 1, -1],
                "zop_4": [0, -1, -2, -1, 1], "zop_5": [0, -1, -2, -1], "zop_6": [-1, -2, -1], "zop_7": [-2, -1]}
    if "result" not in r or zop != expected:
        print("FAIL: unexpected response:", {k: (v if k != "result" else f"<{len(v)} terms>") for k, v in r.items()}); sys.exit(1)
    print("PASS: mint -> 7 atoms reported,", len(r["result"]), "terms")
elif mode == "exhaust":
    if not r.get("failed") or "unsupported 0->1" not in r.get("reason", ""):
        print("FAIL: expected structured failure, got:", str(r)[:300]); sys.exit(1)
    print("PASS: exhaustion -> structured failure:", r["reason"][:90])
elif mode == "reserved":
    msg = r.get("error", "") + r.get("reason", "")
    if "result" in r or "reserved" not in msg:
        print("FAIL: expected a structured reserved-name rejection, got:", str(r)[:300]); sys.exit(1)
    print("PASS: reserved name rejected:", msg[:90])
else:
    print("FAIL: unknown mode", mode); sys.exit(1)
