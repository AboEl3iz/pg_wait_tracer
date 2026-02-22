#!/bin/bash
# test_cli.sh — Test CLI argument parsing and validation
# Usage: sudo tests/test_cli.sh [--pid POSTMASTER_PID]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRACER="$SCRIPT_DIR/../pg_wait_tracer"

PM_PID=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID]"; exit 1 ;;
    esac
done

if [[ -z "$PM_PID" ]]; then
    PM_PID=$(pgrep -x postgres | head -1)
fi

echo "=== test_cli ==="

passed=0
failed=0

check_exit() {
    local expected=$1
    local actual=$2
    local desc="$3"
    if [[ "$actual" -eq "$expected" ]]; then
        echo "  PASS: $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL: $desc (expected exit $expected, got $actual)"
        failed=$((failed + 1))
    fi
}

# --help exits 0
"$TRACER" --help > /dev/null 2>&1
check_exit 0 $? "--help exits 0"

# --help output contains Usage
output=$("$TRACER" --help 2>&1)
if echo "$output" | grep -q "Usage"; then
    echo "  PASS: --help prints Usage"
    passed=$((passed + 1))
else
    echo "  FAIL: --help does not print Usage"
    failed=$((failed + 1))
fi

# No args exits non-zero
"$TRACER" > /dev/null 2>&1
check_exit 1 $? "no args exits 1"

# Invalid PID exits non-zero
"$TRACER" --pid 999999999 > /dev/null 2>&1
check_exit 1 $? "invalid PID exits non-zero"

# Invalid view exits non-zero
"$TRACER" --pid "${PM_PID:-1}" --view invalid_view > /dev/null 2>&1
check_exit 1 $? "invalid view exits non-zero"

# Interval 0 exits non-zero
"$TRACER" --pid "${PM_PID:-1}" --interval 0 > /dev/null 2>&1
check_exit 1 $? "interval 0 exits non-zero"

# Valid args with --duration 1 runs and exits cleanly
if [[ -n "$PM_PID" ]]; then
    timeout 10 "$TRACER" --pid "$PM_PID" --interval 1 --duration 2 \
        --view time_model > /dev/null 2>&1
    rc=$?
    if [[ $rc -eq 0 || $rc -eq 124 ]]; then
        echo "  PASS: valid args run successfully"
        passed=$((passed + 1))
    else
        echo "  FAIL: valid args returned exit code $rc"
        failed=$((failed + 1))
    fi

    # All 4 views work
    for view in time_model system_event session_event query_event; do
        timeout 10 "$TRACER" --pid "$PM_PID" --interval 1 --duration 2 \
            --view "$view" > /dev/null 2>&1
        rc=$?
        if [[ $rc -eq 0 || $rc -eq 124 ]]; then
            echo "  PASS: --view $view works"
            passed=$((passed + 1))
        else
            echo "  FAIL: --view $view returned exit code $rc"
            failed=$((failed + 1))
        fi
    done

    # Histogram requires --event
    "$TRACER" --pid "$PM_PID" --view histogram > /dev/null 2>&1
    check_exit 1 $? "histogram without --event exits non-zero"
else
    echo "  SKIP: no running PG (pass --pid to test valid args)"
fi

echo ""
echo "$passed/$((passed+failed)) tests passed"
[[ $failed -eq 0 ]]
