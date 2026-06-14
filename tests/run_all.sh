#!/bin/bash
# run_all.sh — Master test runner for pg_wait_tracer
# Usage: sudo tests/run_all.sh [--pid POSTMASTER_PID] [--pg-version N]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/testutil.sh"

PM_PID=""
PG_VERSION=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --pg-version) PG_VERSION="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID] [--pg-version N]"; exit 1 ;;
    esac
done

# Auto-detect postmaster PID if not provided
if [[ -z "$PM_PID" ]] && pgrep -x postgres > /dev/null 2>&1; then
    if [[ -n "$PG_VERSION" ]]; then
        PM_PID=$(find_postmaster --pg-version "$PG_VERSION")
    else
        PM_PID=$(find_postmaster)
    fi
    if [[ -n "$PM_PID" ]]; then
        local_ver=$(readlink /proc/$PM_PID/exe 2>/dev/null | grep -oP 'postgresql/\K\d+(?=/)' || true)
        echo "Auto-detected postmaster PID $PM_PID (PostgreSQL ${local_ver:-unknown})"
    fi
fi

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
run_test "test_bucket" "$SCRIPT_DIR/test_bucket"
run_test "test_trace_v2" "$SCRIPT_DIR/test_trace_v2"
run_test "test_sampler" "$SCRIPT_DIR/test_sampler"

# Step 2.5: Synthetic data correctness tests (no root needed, needs pgwt-server)
if [[ -x "$PROJECT_DIR/pgwt-server" ]] && [[ -x "$SCRIPT_DIR/gen_test_traces" ]]; then
    run_test "test_data_time_model" python3 "$SCRIPT_DIR/test_data_time_model.py"
    run_test "test_data_aas" python3 "$SCRIPT_DIR/test_data_aas.py"
    run_test "test_data_events" python3 "$SCRIPT_DIR/test_data_events.py"
    run_test "test_data_sessions" python3 "$SCRIPT_DIR/test_data_sessions.py"
    run_test "test_data_queries" python3 "$SCRIPT_DIR/test_data_queries.py"
    run_test "test_data_filters" python3 "$SCRIPT_DIR/test_data_filters.py"
    run_test "test_data_timeline" python3 "$SCRIPT_DIR/test_data_timeline.py"
    run_test "test_data_idle" python3 "$SCRIPT_DIR/test_data_idle.py"
    run_test "test_data_edge" python3 "$SCRIPT_DIR/test_data_edge.py"
    run_test "test_data_transitions" python3 "$SCRIPT_DIR/test_data_transitions.py"
    run_test "test_data_lock_chains" python3 "$SCRIPT_DIR/test_data_lock_chains.py"
    run_test "test_current_trace" python3 "$SCRIPT_DIR/test_current_trace.py"
    run_test "test_protocol_drift" python3 "$SCRIPT_DIR/test_protocol_drift.py"
else
    skip_test "test_data_*" "pgwt-server or gen_test_traces not built"
fi

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
    run_test "test_client_wait" python3 "$SCRIPT_DIR/test_client_wait.py" $PID_ARG
    run_test "test_cpu_time" python3 "$SCRIPT_DIR/test_cpu_time.py" $PID_ARG
    run_test "test_lwlock" python3 "$SCRIPT_DIR/test_lwlock.py" $PID_ARG
    run_test "test_query_event" python3 "$SCRIPT_DIR/test_query_event.py" $PID_ARG
    run_test "test_cross_pg_wait_sampling" python3 "$SCRIPT_DIR/test_cross_pg_wait_sampling.py" $PID_ARG
    run_test "test_event_classes" python3 "$SCRIPT_DIR/test_event_classes.py" $PID_ARG
    run_test "test_multi_window" python3 "$SCRIPT_DIR/test_multi_window.py" $PID_ARG
    run_test "test_active" python3 "$SCRIPT_DIR/test_active.py" $PID_ARG

    # Step 4.5: Live data correctness tests (Sprint 4)
    run_test "test_percentage" python3 "$SCRIPT_DIR/test_percentage.py" $PID_ARG
    run_test "test_aas_accuracy" python3 "$SCRIPT_DIR/test_aas_accuracy.py" $PID_ARG
    run_test "test_session_accuracy" python3 "$SCRIPT_DIR/test_session_accuracy.py" $PID_ARG
    run_test "test_query_accuracy" python3 "$SCRIPT_DIR/test_query_accuracy.py" $PID_ARG
    run_test "test_partition" python3 "$SCRIPT_DIR/test_partition.py" $PID_ARG
    run_test "test_idle_exclusion" python3 "$SCRIPT_DIR/test_idle_exclusion.py" $PID_ARG
    run_test "test_daemon_server" python3 "$SCRIPT_DIR/test_daemon_server.py" $PID_ARG
    run_test "test_control" bash "$SCRIPT_DIR/test_control.sh" $PID_ARG
else
    if [[ $(id -u) -ne 0 ]]; then
        skip_test "test_lifecycle" "requires root"
        skip_test "test_cross_validate" "requires root"
        skip_test "test_accuracy" "requires root"
        skip_test "test_deterministic" "requires root"
        skip_test "test_overhead" "requires root"
        skip_test "test_client_wait" "requires root"
        skip_test "test_cpu_time" "requires root"
        skip_test "test_lwlock" "requires root"
        skip_test "test_query_event" "requires root"
        skip_test "test_cross_pg_wait_sampling" "requires root"
        skip_test "test_event_classes" "requires root"
        skip_test "test_multi_window" "requires root"
        skip_test "test_active" "requires root"
        skip_test "test_percentage" "requires root"
        skip_test "test_aas_accuracy" "requires root"
        skip_test "test_session_accuracy" "requires root"
        skip_test "test_query_accuracy" "requires root"
        skip_test "test_partition" "requires root"
        skip_test "test_idle_exclusion" "requires root"
        skip_test "test_daemon_server" "requires root"
        skip_test "test_control" "requires root"
    else
        skip_test "test_lifecycle" "PostgreSQL not running"
        skip_test "test_cross_validate" "PostgreSQL not running"
        skip_test "test_accuracy" "PostgreSQL not running"
        skip_test "test_deterministic" "PostgreSQL not running"
        skip_test "test_overhead" "PostgreSQL not running"
        skip_test "test_client_wait" "PostgreSQL not running"
        skip_test "test_cpu_time" "PostgreSQL not running"
        skip_test "test_lwlock" "PostgreSQL not running"
        skip_test "test_query_event" "PostgreSQL not running"
        skip_test "test_cross_pg_wait_sampling" "PostgreSQL not running"
        skip_test "test_event_classes" "PostgreSQL not running"
        skip_test "test_multi_window" "PostgreSQL not running"
        skip_test "test_active" "PostgreSQL not running"
        skip_test "test_percentage" "PostgreSQL not running"
        skip_test "test_aas_accuracy" "PostgreSQL not running"
        skip_test "test_session_accuracy" "PostgreSQL not running"
        skip_test "test_query_accuracy" "PostgreSQL not running"
        skip_test "test_partition" "PostgreSQL not running"
        skip_test "test_idle_exclusion" "PostgreSQL not running"
        skip_test "test_daemon_server" "PostgreSQL not running"
        skip_test "test_control" "PostgreSQL not running"
    fi
fi

# Step 5: Web UI tests (needs playwright + websockets, no root needed)
# Locally a missing dependency is a skip; in CI ($CI set) it is a FAILURE —
# the UI suite silently not running is how regressions slip through.
# test_web_ui_chaos runs the same UI against mock_server.py in CHAOS mode
# (latency jitter / out-of-order / late responses) — its race-exposing tests
# are classified gating vs xfail internally, so it stays CI-green either way.
if python3 -c "import playwright, websockets" 2>/dev/null; then
    run_test "test_web_ui" python3 "$SCRIPT_DIR/test_web_ui.py"
    run_test "test_web_ui_chaos" python3 "$SCRIPT_DIR/test_web_ui_chaos.py"
elif [[ -n "${CI:-}" ]]; then
    run_test "test_web_ui" bash -c \
        'echo "ERROR: playwright/websockets not installed — required in CI"; exit 1'
    run_test "test_web_ui_chaos" bash -c \
        'echo "ERROR: playwright/websockets not installed — required in CI"; exit 1'
else
    skip_test "test_web_ui" "playwright or websockets not installed"
    skip_test "test_web_ui_chaos" "playwright or websockets not installed"
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
