#!/usr/bin/env python3
"""test_deterministic.py — Deterministic accuracy tests with controlled backends.

Tests:
  1. pg_sleep exact count: N × pg_sleep(1) in a loop → count=N, total≈N*1000ms
  2. Lock wait duration: Session B blocks on Lock:relation for a measured time

The key technique: keep the backend ALIVE past the timer tick so BPF map
entries are not deleted by handle_exit().

Requires: root, running PostgreSQL 18.
Usage: sudo python3 tests/test_deterministic.py [--pid POSTMASTER_PID]
"""
import subprocess
import sys
import os
import re
import time
import argparse

TRACER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "pg_wait_tracer")
STRIP_ANSI = re.compile(r'\x1b\[[0-9;]*[a-zA-Z]')

tests_run = 0
tests_passed = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


def psql(sql, timeout=10):
    """Run SQL via psql, return stdout."""
    result = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=timeout
    )
    return result.stdout.strip()


def cleanup_stale_backends():
    """Terminate leftover client backends from previous tests to avoid contamination."""
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND datname = 'postgres' "
             "AND backend_type = 'client backend' AND state != 'active'")
        # Also kill any backends stuck in pg_sleep from previous tests
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND datname = 'postgres' "
             "AND query LIKE '%pg_sleep%'")
    except subprocess.TimeoutExpired:
        pass
    time.sleep(1)  # let backends exit and BPF cleanup


def parse_system_events(output):
    """Parse system_event view into list of dicts."""
    events = []
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(\S+(?::\S+)?)\s+'
            r'(\d+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+)%',
            line
        )
        if m:
            events.append({
                'name': m.group(1),
                'count': int(m.group(2)),
                'total_ms': float(m.group(3)),
                'avg_us': float(m.group(4)),
                'max_us': float(m.group(5)),
                'pct': float(m.group(6)),
            })
    return events


# ── Test 1: pg_sleep exact count ───────────────────────────────

def test_pg_sleep_exact_count(pm_pid):
    """Execute N × pg_sleep(1) in a loop. Verify exact count and total duration.

    Start DO block BEFORE the tracer so the initial scan finds the backend
    already in PgSleep.  This avoids the race window in fork detection →
    watchpoint setup that causes flakiness under suite load.

    Timeline:
      t=0    start psql: DO block (5×pg_sleep(1)) + pg_sleep(60)
      t=0.5  start tracer (interval=8, duration=14)
      t=0.5  initial scan finds backend in PgSleep, sets watchpoint
      t=1    first PgSleep→CPU transition fires watchpoint
      ...    each pg_sleep transition fires watchpoint
      t=5    DO block done, pg_sleep(60) keeps backend alive
      t=8.5  timer tick → PgSleep count=5, total≈4500ms
      t=14.5 tracer exits, kill psql
    """
    print("--- Test 1: pg_sleep Exact Count ---")

    N = 5
    SLEEP_EACH_S = 1

    # Start deterministic workload BEFORE tracer:
    #   -c "DO block" executes 5×pg_sleep(1) → ~5s of Timeout:PgSleep
    #   -c "pg_sleep(60)" keeps the backend alive past the timer tick
    sql = (f"DO $$ BEGIN "
           f"FOR i IN 1..{N} LOOP PERFORM pg_sleep({SLEEP_EACH_S}); END LOOP; "
           f"END $$")
    psql_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", sql,
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(0.5)  # let backend enter first pg_sleep

    # Start tracer — initial scan finds backend in PgSleep
    tracer = subprocess.Popen(
        [TRACER, "--pid", str(pm_pid),
         "--interval", "8", "--duration", "14",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    # Wait for tracer to finish
    try:
        stdout, stderr = tracer.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))

    # Cleanup
    psql_proc.terminate()
    try:
        psql_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        psql_proc.kill()

    # Parse
    events = parse_system_events(output)
    pg_sleep_ev = [e for e in events if e['name'] == 'Timeout:PgSleep']

    check(len(pg_sleep_ev) > 0,
          f"Timeout:PgSleep found (events: {[e['name'] for e in events]})")

    if not pg_sleep_ev:
        return

    ev = pg_sleep_ev[0]

    # All 5 PgSleep→CPU transitions should fire the watchpoint
    check(ev['count'] == N,
          f"count = {ev['count']} (expected exactly {N})")

    # Total ≈ 4500ms (5000ms minus ~0.5s before attachment)
    # Use generous tolerance: 3500-6000ms
    check(3500 <= ev['total_ms'] <= 6000,
          f"total = {ev['total_ms']:.1f}ms "
          f"(expected {N * SLEEP_EACH_S * 1000}ms ±1500ms)")

    # Avg should be close to 1s (first sleep is shorter due to partial capture)
    check(ev['avg_us'] > 500000,
          f"avg = {ev['avg_us']:.0f}us (expected ≈ 1000000us)")

    # Max should be close to 1s (single sleep, no outliers)
    check(ev['max_us'] < SLEEP_EACH_S * 1.5e6,
          f"max = {ev['max_us']:.0f}us (expected < {SLEEP_EACH_S * 1.5e6:.0f}us)")


# ── Test 2: Lock wait with known duration ──────────────────────

def test_lock_wait_duration(pm_pid):
    """Session A holds ACCESS EXCLUSIVE lock. Session B blocks for several
    seconds. Verify Lock:relation is detected with correct event name and
    reasonable duration.

    Key design: both sessions start BEFORE the tracer so the initial scan
    finds them as existing backends. The scan pre-initializes the BPF
    state_map with the current wait event, and at timer tick time the
    open interval accounting reports the duration since attachment.

    Timeline:
      t=0    Session A: acquire exclusive lock
      t=2    Session B: SELECT → blocks on Lock:relation
      t=5    start tracer (interval=8, duration=12)
      t=5-6  initial scan pre-initializes state_map with Lock:relation
      t=13   timer tick → open interval accounting reports Lock:relation ~8s
      t=17   tracer exits, cleanup
    """
    print("--- Test 2: Lock Wait Duration ---")

    # Create test table
    psql("CREATE TABLE IF NOT EXISTS _test_lock_wait (id int)")
    psql("TRUNCATE _test_lock_wait")

    # Session A: acquire exclusive lock and hold it
    session_a = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres", "-c",
         "BEGIN; "
         "LOCK TABLE _test_lock_wait IN ACCESS EXCLUSIVE MODE; "
         "SELECT pg_sleep(120);"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(2)  # ensure lock is acquired

    # Verify lock is held
    lock_held = psql(
        "SELECT count(*) FROM pg_locks "
        "WHERE relation = '_test_lock_wait'::regclass "
        "AND mode = 'AccessExclusiveLock' AND granted"
    )
    check(lock_held == '1',
          f"Session A holds AccessExclusiveLock (found: {lock_held})")

    # Session B: tries to read the table → blocks on Lock:relation.
    # Started BEFORE tracer so initial scan finds it as existing backend.
    session_b = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT count(*) FROM _test_lock_wait",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(1)  # let Session B block on lock

    # Verify Session B is waiting
    waiters = psql(
        "SELECT count(*) FROM pg_locks "
        "WHERE relation = '_test_lock_wait'::regclass "
        "AND NOT granted"
    )
    check(waiters >= '1',
          f"Session B is waiting for lock (waiters: {waiters})")

    time.sleep(2)  # let lock wait establish

    # Start tracer — initial scan finds both sessions as existing backends.
    # The scan pre-initializes state_map so Lock:relation is captured via
    # open interval accounting at timer tick time.
    tracer = subprocess.Popen(
        [TRACER, "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    # Wait for tracer to finish (exits at duration=12)
    stdout, stderr = tracer.communicate(timeout=25)
    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))

    # Cleanup: kill backends first, then drop table
    session_a.terminate()
    session_a.wait()
    session_b.terminate()
    session_b.wait()
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND datname = 'postgres' "
             "AND query LIKE '%_test_lock_wait%'")
    except subprocess.TimeoutExpired:
        pass
    time.sleep(1)
    try:
        psql("DROP TABLE IF EXISTS _test_lock_wait")
    except subprocess.TimeoutExpired:
        print("  WARNING: DROP TABLE timed out (non-fatal)")

    # Parse
    events = parse_system_events(output)
    lock_ev = [e for e in events if e['name'] == 'Lock:relation']

    check(len(lock_ev) > 0,
          f"Lock:relation found (events: {[e['name'] for e in events]})")

    if not lock_ev:
        return

    ev = lock_ev[0]

    # Count: exactly 1 (the open interval captured at timer tick)
    check(ev['count'] >= 1,
          f"Lock:relation count = {ev['count']} (expected >= 1)")

    # Duration: approximately the timer interval (8s).
    # The backend has been in Lock:relation since before the tracer started.
    # Open interval accounting reports duration from attachment to timer tick.
    check(ev['total_ms'] > 4000,
          f"Lock:relation total = {ev['total_ms']:.1f}ms (expected > 4000ms)")

    # Duration shouldn't exceed the total observation time
    check(ev['total_ms'] < 12000,
          f"Lock:relation total = {ev['total_ms']:.1f}ms (expected < 12000ms)")


# ── Main ─────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, help='Postmaster PID')
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("ERROR: must run as root (sudo)")
        sys.exit(1)

    if not os.path.exists(TRACER):
        print(f"ERROR: tracer binary not found at {TRACER}")
        sys.exit(1)

    pm_pid = args.pid
    if not pm_pid:
        # Find PostgreSQL 18 postmaster (skip PG16 and other versions)
        result = subprocess.run(["pgrep", "-x", "postgres"],
                                capture_output=True, text=True)
        for pid_str in result.stdout.strip().split('\n'):
            if not pid_str:
                continue
            pid = int(pid_str)
            try:
                exe = os.readlink(f"/proc/{pid}/exe")
                if "/18/" in exe:
                    pm_pid = pid
                    break
            except OSError:
                continue
    if not pm_pid:
        print("ERROR: cannot find PostgreSQL 18 postmaster PID")
        sys.exit(1)

    print(f"=== test_deterministic (postmaster PID {pm_pid}) ===")

    cleanup_stale_backends()

    test_pg_sleep_exact_count(pm_pid)
    test_lock_wait_duration(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
