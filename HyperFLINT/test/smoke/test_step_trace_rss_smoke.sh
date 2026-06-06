#!/usr/bin/env bash
# Phase 0 task 0-3 smoke test: verify RSS fields appear in HF_STEP_TRACE output.
#
# Runs hyperflint on tst0 with HF_STEP_TRACE=1 and checks that every JSON line
# with a "step" field also contains "rss_current_kib" and "rss_peak_kib".
#
# PASS: prints "OK: all step trace lines contain RSS fields"
# FAIL: prints "FAIL: ..." and exits 1
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BINARY="${HF_BINARY:-$REPO_ROOT/HyperFLINT/build-release/hyperflint}"
FIXTURE="$REPO_ROOT/notes/benchmark_smirnov/fixtures/tst0.json"
TRACE_OUT="$(mktemp /tmp/tst0_trace_XXXXXX)"
trap 'rm -f "$TRACE_OUT"' EXIT

if [[ ! -x "$BINARY" ]]; then
    echo "FAIL: binary not found or not executable: $BINARY"
    exit 1
fi
if [[ ! -f "$FIXTURE" ]]; then
    echo "FAIL: fixture not found: $FIXTURE"
    exit 1
fi

# Build the eval-json request from the fixture fields.
# hyperflint reads a single JSON object from stdin when invoked as "eval-json".
REQUEST="$(python3 -c "
import json, sys
fix = json.load(open('$FIXTURE'))
integ = fix['integrand']
key = 'expr' if 'Log[' in integ else 'f'
req = {'op': 'hyperflint', key: integ, 'vars_int': fix['vars_int'], 'vars': fix['vars']}
print(json.dumps(req))
")"

# hyperflint resolves data/mzv_reductions.json relative to cwd,
# so it must be invoked from $REPO_ROOT/HyperFLINT.
(cd "$REPO_ROOT/HyperFLINT" && \
    HF_STEP_TRACE=1 OMP_NUM_THREADS=1 \
    "$BINARY" eval-json <<< "$REQUEST") > /dev/null 2> "$TRACE_OUT"

python3 - "$TRACE_OUT" <<'PYEOF'
import json, sys

trace_file = sys.argv[1]
required_fields = ['rss_current_kib', 'rss_peak_kib']
step_lines_checked = 0
failures = []

with open(trace_file) as f:
    for lineno, line in enumerate(f, 1):
        line = line.strip()
        if not line.startswith('{'):
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        # Only check lines that are step-trace lines (have "step" key and hf_step_trace sentinel)
        if d.get('hf_step_trace') is not True:
            continue
        step_lines_checked += 1
        for field in required_fields:
            if field not in d:
                failures.append(f'line {lineno}: missing {field!r}')
            elif d[field] <= 0:
                failures.append(f'line {lineno}: {field!r} = {d[field]} (expected > 0; -1 is sentinel for measurement failure)')

if not step_lines_checked:
    print('FAIL: no hf_step_trace lines found in output')
    sys.exit(1)

if failures:
    for msg in failures:
        print(f'FAIL: {msg}')
    print(f'FAIL: {len(failures)} field(s) missing across {step_lines_checked} step-trace line(s)')
    sys.exit(1)

print(f'OK: all {step_lines_checked} step trace line(s) contain RSS fields (rss_current_kib + rss_peak_kib)')
PYEOF
