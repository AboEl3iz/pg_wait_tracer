#!/usr/bin/env bash
# workload.sh — Drives a 60-second mixed-wait workload on PG.
#
# Runs ON the DB server (or wherever PG is reachable). Generates a varied
# wait-event mix: pgbench TPC-B (Lock, WAL, Client), injected LOCK TABLE
# (Lock:relation), and a few large reads (CPU + buffer activity).
#
# Used by demos/capture-trace.sh after the tracer is started.
#
# Env:
#   PSQL    — psql binary (default: psql)
#   PGBENCH — pgbench binary (default: pgbench)
#   DB      — target database (default: postgres)
#   USER    — DB role (default: postgres)
#   CLIENTS — pgbench concurrency (default: 8)
#   DURATION_SEC — pgbench duration (default: 60)

set -euo pipefail

PSQL=${PSQL:-psql}
PGBENCH=${PGBENCH:-pgbench}
DB=${DB:-postgres}
USER=${USER:-postgres}
CLIENTS=${CLIENTS:-8}
DURATION_SEC=${DURATION_SEC:-60}

echo "=== Warmup ==="
$PSQL -U "$USER" -d "$DB" -c "SELECT pg_prewarm('pgbench_accounts');" 2>/dev/null || true

echo "=== Starting mixed workload (${DURATION_SEC}s, ${CLIENTS} clients) ==="

# Background: 5 transient LOCK TABLE bursts to seed Lock:relation waits
(
  for i in 1 2 3 4 5; do
    sleep 8
    $PSQL -U "$USER" -d "$DB" \
      -c "BEGIN; LOCK TABLE pgbench_branches IN EXCLUSIVE MODE; SELECT pg_sleep(2); COMMIT;" \
      >/dev/null 2>&1 &
  done
  wait
) &
LOCK_PID=$!

# Background: heavy reads to surface IO + buffer activity
(
  for i in 1 2 3; do
    $PSQL -U "$USER" -d "$DB" \
      -c "SELECT count(*) FROM pgbench_accounts WHERE aid % 7 = 0;" \
      >/dev/null 2>&1
  done
) &

# Foreground: pgbench TPC-B
$PGBENCH -U "$USER" -c "$CLIENTS" -j 2 -T "$DURATION_SEC" -P 10 -r "$DB" 2>&1

wait $LOCK_PID 2>/dev/null || true
echo "=== Workload done ==="
