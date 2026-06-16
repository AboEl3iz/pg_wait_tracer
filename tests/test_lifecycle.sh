#!/bin/bash
# test_lifecycle.sh — Test fork/init/exit detection via verbose mode
# Usage: sudo tests/test_lifecycle.sh [--pid POSTMASTER_PID]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRACER="$SCRIPT_DIR/../pg_wait_tracer"
source "$SCRIPT_DIR/testutil.sh"

PM_PID=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID]"; exit 1 ;;
    esac
done

if [[ -z "$PM_PID" ]]; then
    PM_PID=$(find_postmaster)
    if [[ -z "$PM_PID" ]]; then
        echo "ERROR: cannot find postmaster PID"
        exit 1
    fi
fi

echo "=== test_lifecycle ==="
echo "Postmaster PID: $PM_PID"

LOGFILE=$(mktemp /tmp/tracer_lifecycle_XXXXXX.log)
trap "rm -f $LOGFILE" EXIT

# Start tracer in verbose mode with short duration — let it exit naturally.
# Pinned to --mode full: this asserts watchpoint attach/init log lines, which
# only the full (exact) tier emits. The default is now tiered (sampled).
"$TRACER" --mode full --pid "$PM_PID" --interval 5 --duration 15 -v \
    > /dev/null 2>"$LOGFILE" &
TRACER_PID=$!

sleep 3

# Create a new backend and let it exit
echo "  Creating backend (pg_sleep 2)..."
psql -U postgres -c "SELECT pg_sleep(2);" > /dev/null 2>&1

# Wait for tracer to finish naturally (duration=15, so ~10s remaining)
wait "$TRACER_PID" 2>/dev/null || true

# Small delay for log flush
sleep 1

# Check for lifecycle events in log
echo ""
passed=0
failed=0

if grep -qi "fork" "$LOGFILE"; then
    echo "  PASS: fork detected"
    passed=$((passed + 1))
else
    echo "  FAIL: fork not detected"
    failed=$((failed + 1))
fi

if grep -qi "init\|bootstrap\|attached" "$LOGFILE"; then
    echo "  PASS: init/attach detected"
    passed=$((passed + 1))
else
    echo "  FAIL: init/attach not detected"
    failed=$((failed + 1))
fi

if grep -qi "exit\|cleanup\|dead" "$LOGFILE"; then
    echo "  PASS: exit detected"
    passed=$((passed + 1))
else
    echo "  FAIL: exit not detected"
    failed=$((failed + 1))
fi

echo ""
echo "$passed/$((passed+failed)) tests passed"
[[ $failed -eq 0 ]]
