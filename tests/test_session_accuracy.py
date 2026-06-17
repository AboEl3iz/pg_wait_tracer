#!/usr/bin/env python3
"""test_session_accuracy.py — Per-session DB Time matches expectations.

Spec 0A.3: Three isolated backends with known workloads.
Verify per-session attribution via session_event view.

Requires: root, running PostgreSQL 18.
Usage: sudo python3 tests/test_session_accuracy.py [--pid POSTMASTER_PID]
"""
import subprocess
import sys
import os
import re
import time
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster

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
    result = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=timeout
    )
    return result.stdout.strip()


def cleanup_stale_backends():
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND datname = 'postgres' "
             "AND backend_type = 'client backend' AND state != 'active'")
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%generate_series%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)


def parse_session_events(output):
    """Parse session_event view into list of dicts.
    Format: PID  Type  User  DB  DB_Time(ms)  CPU%  Wait%  Top_Wait
    """
    sessions = []
    for line in output.split('\n'):
        line = line.strip()
        # Match: "12345  client  user  db  5000.0  30.0%  70.0%  Timeout:PgSleep"
        # Type/User/DB can have spaces within but are left-aligned fields
        m = re.match(
            r'^(\d+)\s+'                  # PID
            r'(\S+)\s+'                   # Type
            r'(\S+)\s+'                   # User (or -)
            r'(\S+)\s+'                   # DB (or -)
            r'([\d.]+)\s+'               # DB Time (ms)
            r'([\d.]+)%\s+'             # CPU %
            r'([\d.]+)%\s+'             # Wait %
            r'(\S+(?::\S+)?)',           # Top Wait Event
            line
        )
        if m:
            sessions.append({
                'pid': int(m.group(1)),
                'type': m.group(2),
                'user': m.group(3),
                'db': m.group(4),
                'db_time_ms': float(m.group(5)),
                'cpu_pct': float(m.group(6)),
                'wait_pct': float(m.group(7)),
                'top_wait': m.group(8),
            })
    return sessions


def parse_time_model(output):
    model = {}
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(r'^(.+?)\s{2,}([\d.]+)\s+[\d.]+%', line)
        if m:
            model[m.group(1).strip()] = float(m.group(2))
    return model


def test_session_accuracy(pm_pid):
    """Three backends with known workloads. Verify per-session attribution."""
    print("--- Test 1: Per-session DB Time ---")

    # Backend A: pg_sleep(5) — ~5s of Timeout:PgSleep
    proc_a = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT pg_sleep(5)",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    # Backend B: pg_sleep(3) — ~3s of Timeout:PgSleep
    proc_b = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT pg_sleep(3)",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    # Backend C: CPU burn — ~5s of CPU time
    proc_c = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT count(*) FROM generate_series(1, 300000000)",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(1)  # let all backends start

    # Start tracer for session_event view
    INTERVAL = 8
    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "session_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, _ = tracer.communicate(timeout=INTERVAL + 20)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))

    # Also get time_model for system-wide comparison
    tracer2 = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "1", "--duration", "1",
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    # (We won't use this since backends may have finished by now)

    # Cleanup
    for p in [proc_a, proc_b, proc_c]:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%generate_series%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)

    sessions = parse_session_events(output)

    # Filter to client backends only
    clients = [s for s in sessions if s['type'] == 'client']

    check(len(clients) >= 3,
          f"At least 3 client backends in session_event (got {len(clients)})")

    if len(clients) < 2:
        return

    # All client sessions should have positive DB Time
    all_positive = all(s['db_time_ms'] > 0 for s in clients)
    check(all_positive,
          f"All client sessions have DB Time > 0")

    # Total per-session DB Time should be reasonable
    total_session_db = sum(s['db_time_ms'] for s in clients)
    check(total_session_db > 5000,
          f"Sum of session DB Times = {total_session_db:.0f}ms (expected > 5000ms)")

    # At least one session should have Timeout:PgSleep as top wait
    sleep_sessions = [s for s in clients if 'PgSleep' in s['top_wait']]
    check(len(sleep_sessions) >= 1,
          f"At least 1 session has PgSleep as top wait (found {len(sleep_sessions)})")

    # At least one session should be CPU-dominated
    cpu_sessions = [s for s in clients if s['cpu_pct'] > 50]
    check(len(cpu_sessions) >= 1,
          f"At least 1 CPU-dominated session (cpu_pct > 50%): {len(cpu_sessions)} found")

    # Highest DB Time should be >= the shortest sleep (3s)
    max_db = max(s['db_time_ms'] for s in clients)
    check(max_db > 2000,
          f"Highest session DB Time = {max_db:.0f}ms (expected > 2000ms)")


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
        pm_pid = find_postmaster()
    if not pm_pid:
        print("ERROR: cannot find PostgreSQL postmaster PID")
        sys.exit(1)

    print(f"=== test_session_accuracy (postmaster PID {pm_pid}) ===")

    cleanup_stale_backends()
    test_session_accuracy(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
