#!/bin/bash
# run_all.sh — Master test runner for pg_wait_tracer
# Usage: sudo tests/run_all.sh [--pid POSTMASTER_PID]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

PM_PID=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID]"; exit 1 ;;
    esac
done

PID_ARG=""
if [[ -n "$PM_PID" ]]; then
    PID_ARG="--pid $PM_PID"
fi

passed=0
failed=0
skipped=0

run_test() {
    local name="$1"
    shift
    echo ""
    echo "════════════════════════════════════════"
    echo "  $name"
    echo "════════════════════════════════════════"
    if "$@"; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
    fi
}

skip_test() {
    local name="$1"
    local reason="$2"
    echo ""
    echo "════════════════════════════════════════"
    echo "  $name — SKIPPED ($reason)"
    echo "════════════════════════════════════════"
    skipped=$((skipped + 1))
}

# Step 0: Build main project if needed
echo "Building main project..."
make -C "$PROJECT_DIR" -q 2>/dev/null || make -C "$PROJECT_DIR"

# Step 1: Build C unit tests
echo "Building C unit tests..."
make -C "$SCRIPT_DIR"

# Step 2: C unit tests (no root needed)
run_test "test_wait_event" "$SCRIPT_DIR/test_wait_event"
run_test "test_cmdline" "$SCRIPT_DIR/test_cmdline"

# Step 3: CLI tests (needs root)
if [[ $(id -u) -eq 0 ]]; then
    run_test "test_cli" bash "$SCRIPT_DIR/test_cli.sh" $PID_ARG
else
    skip_test "test_cli" "requires root"
fi

# Step 4: Integration tests (need root + running PG)
if [[ $(id -u) -eq 0 ]] && pgrep -x postgres > /dev/null 2>&1; then
    run_test "test_lifecycle" bash "$SCRIPT_DIR/test_lifecycle.sh" $PID_ARG
    run_test "test_cross_validate" python3 "$SCRIPT_DIR/test_cross_validate.py" $PID_ARG
    run_test "test_accuracy" python3 "$SCRIPT_DIR/test_accuracy.py" $PID_ARG
    run_test "test_deterministic" python3 "$SCRIPT_DIR/test_deterministic.py" $PID_ARG
    run_test "test_overhead" bash "$SCRIPT_DIR/test_overhead.sh" $PID_ARG
else
    if [[ $(id -u) -ne 0 ]]; then
        skip_test "test_lifecycle" "requires root"
        skip_test "test_cross_validate" "requires root"
        skip_test "test_accuracy" "requires root"
        skip_test "test_deterministic" "requires root"
        skip_test "test_overhead" "requires root"
    else
        skip_test "test_lifecycle" "PostgreSQL not running"
        skip_test "test_cross_validate" "PostgreSQL not running"
        skip_test "test_accuracy" "PostgreSQL not running"
        skip_test "test_deterministic" "PostgreSQL not running"
        skip_test "test_overhead" "PostgreSQL not running"
    fi
fi

# Summary
echo ""
echo "════════════════════════════════════════"
echo "  SUMMARY"
echo "════════════════════════════════════════"
total=$((passed + failed + skipped))
echo "  Passed:  $passed"
echo "  Failed:  $failed"
echo "  Skipped: $skipped"
echo "  Total:   $total"
echo ""

[[ $failed -eq 0 ]]
