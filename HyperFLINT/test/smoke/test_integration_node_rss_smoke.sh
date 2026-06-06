#!/usr/bin/env bash
# Phase 0 task 0-5 smoke test: verify integ_node records appear in HF output.
#
# Runs hyperflint on tst1 with HF_STEP_TRACE=1 and HF_INTEG_NODE_RSS=2 and
# checks that at least 5 integ_node JSON records appear on stderr.
#
# PASS: prints "OK: N node records emitted (>= 5)"
# FAIL: prints "FAIL: ..." and exits 1
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BINARY="${HF_BINARY:-$REPO_ROOT/HyperFLINT/build-release/hyperflint}"
FIXTURE="$REPO_ROOT/notes/benchmark_smirnov/fixtures/tst1.json"
TRACE_OUT="$(mktemp /tmp/tst1_node_trace_XXXXXX)"
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
# Mirror the format from test_step_trace_rss_smoke.sh exactly.
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
    HF_STEP_TRACE=1 HF_INTEG_NODE_RSS=2 OMP_NUM_THREADS=1 \
    "$BINARY" eval-json <<< "$REQUEST") > /dev/null 2> "$TRACE_OUT"

python3 - "$TRACE_OUT" <<'PYEOF'
import json, sys

trace_file = sys.argv[1]
n = 0

with open(trace_file) as f:
    for line in f:
        line = line.strip()
        if not line.startswith('{'):
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        if d.get('record_type') == 'integ_node':
            n += 1

if n >= 5:
    print(f'OK: {n} node records emitted (>= 5)')
    sys.exit(0)
else:
    print(f'FAIL: only {n} node records emitted (expected >= 5)')
    sys.exit(1)
PYEOF
