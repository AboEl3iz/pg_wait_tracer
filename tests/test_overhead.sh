#!/bin/bash
# test_overhead.sh — Measure TPS overhead of pg_wait_tracer using pgbench
# Usage: sudo tests/test_overhead.sh [--pid POSTMASTER_PID]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRACER="$SCRIPT_DIR/../pg_wait_tracer"
source "$SCRIPT_DIR/testutil.sh"
RUNS=3
DURATION=30
CLIENTS=4

# Parse args
PM_PID=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID]"; exit 1 ;;
    esac
done

# Auto-detect postmaster PID
if [[ -z "$PM_PID" ]]; then
    PM_PID=$(find_postmaster)
    if [[ -z "$PM_PID" ]]; then
        echo "ERROR: cannot find postmaster PID (pass --pid)"
        exit 1
    fi
fi

echo "=== test_overhead ==="
echo "Postmaster PID: $PM_PID"
echo "pgbench: -c $CLIENTS -T $DURATION ($RUNS runs each)"
echo ""

extract_tps() {
    # Extract "tps = 1234.56" from pgbench output
    grep -oP 'tps = \K[\d.]+' | head -1
}

# Baseline runs (no tracer)
echo "--- Baseline (no tracer) ---"
baseline_total=0
for i in $(seq 1 $RUNS); do
    tps=$(pgbench -U postgres -d postgres -c $CLIENTS -T $DURATION 2>&1 | extract_tps)
    echo "  Run $i: $tps TPS"
    baseline_total=$(echo "$baseline_total + $tps" | bc)
    sleep 2
done
baseline_avg=$(echo "scale=2; $baseline_total / $RUNS" | bc)
echo "  Average: $baseline_avg TPS"
echo ""

# Tracer runs
echo "--- With tracer ---"
tracer_total=0
for i in $(seq 1 $RUNS); do
    # Start tracer in background
    "$TRACER" --pid "$PM_PID" --interval 5 --duration $((DURATION + 10)) \
        --view time_model > /dev/null 2>&1 &
    tracer_pid=$!
    sleep 1  # let tracer attach

    tps=$(pgbench -U postgres -d postgres -c $CLIENTS -T $DURATION 2>&1 | extract_tps)
    echo "  Run $i: $tps TPS"
    tracer_total=$(echo "$tracer_total + $tps" | bc)

    kill "$tracer_pid" 2>/dev/null || true
    wait "$tracer_pid" 2>/dev/null || true
    sleep 2
done
tracer_avg=$(echo "scale=2; $tracer_total / $RUNS" | bc)
echo "  Average: $tracer_avg TPS"
echo ""

# Calculate overhead
overhead=$(echo "scale=2; ($baseline_avg - $tracer_avg) * 100 / $baseline_avg" | bc)
echo "--- Results ---"
echo "  Baseline:  $baseline_avg TPS"
echo "  With tracer: $tracer_avg TPS"
echo "  Overhead:  ${overhead}%"

# Verdict
result=$(echo "$overhead < 5" | bc)
if [[ "$result" -eq 1 ]]; then
    echo "  PASS (< 5%)"
    exit 0
fi
result=$(echo "$overhead < 15" | bc)
if [[ "$result" -eq 1 ]]; then
    echo "  WARN (5-15%)"
    exit 0
fi
echo "  FAIL (> 15%)"
exit 1
