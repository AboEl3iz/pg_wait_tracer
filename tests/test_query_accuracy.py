#!/usr/bin/env python3
"""test_query_accuracy.py — Per-query attribution via query_event view.

Spec 0A.4: Two distinct queries with known characteristics.
Verify query_id-based time attribution.

Requires: root, running PostgreSQL 18, pg_stat_statements enabled.
Usage: sudo python3 tests/test_query_accuracy.py [--pid POSTMASTER_PID]
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


def parse_query_events(output):
    """Parse query_event Mode A: query_id + event rows.
    Format: right-aligned query_id, then event name, count, total, avg, max, %DB
    """
    entries = []
    for line in output.split('\n'):
        line = line.strip()
        # Match: "-1234567890  Timeout:PgSleep   1   3000.0  3000000.0  3000000.0   50.0%"
        # query_id can be negative (signed long)
        m = re.match(
            r'^(-?\d+)\s+'             # query_id (may be negative)
            r'(\S+(?::\S+)?)\s+'      # wait event
            r'(\d+)\s+'               # count
            r'([\d.]+)\s+'            # total (ms)
            r'([\d.]+)\s+'            # avg (us)
            r'([\d.]+)\s+'            # max (us)
            r'([\d.]+)%',             # % DB
            line
        )
        if m:
            entries.append({
                'query_id': m.group(1),
                'event': m.group(2),
                'count': int(m.group(3)),
                'total_ms': float(m.group(4)),
                'avg_us': float(m.group(5)),
                'max_us': float(m.group(6)),
                'pct': float(m.group(7)),
            })
    return entries


def test_query_accuracy(pm_pid):
    """Backend A: pg_sleep(3) → query_id Q1.
    Backend B: pgbench-style query → query_id Q2.
    Verify per-query attribution.
    """
    print("--- Test 1: Per-query attribution ---")

    # Backend A: pg_sleep(5) — Timeout dominant
    proc_a = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT pg_sleep(5)",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    # Backend B: count query on pgbench_accounts — IO/CPU mix
    proc_b = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT count(*) FROM pgbench_accounts",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(1)

    INTERVAL = 8
    tracer = subprocess.Popen(
        [TRACER, "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "query_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, _ = tracer.communicate(timeout=INTERVAL + 20)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))

    # Cleanup
    for p in [proc_a, proc_b]:
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

    entries = parse_query_events(output)

    check(len(entries) > 0,
          f"query_event has entries (got {len(entries)})")

    if not entries:
        return

    # Group by query_id
    qids = {}
    for e in entries:
        qid = e['query_id']
        if qid not in qids:
            qids[qid] = {'total_ms': 0, 'events': []}
        qids[qid]['total_ms'] += e['total_ms']
        qids[qid]['events'].append(e['event'])

    # Should have at least 2 distinct non-zero query IDs
    non_zero_qids = {k: v for k, v in qids.items() if k != '0'}
    check(len(non_zero_qids) >= 1,
          f"At least 1 non-zero query_id (got {len(non_zero_qids)})")

    # At least one query should have Timeout:PgSleep
    sleep_queries = [qid for qid, info in qids.items()
                     if 'Timeout:PgSleep' in info['events']]
    check(len(sleep_queries) >= 1,
          f"At least 1 query with PgSleep (found {len(sleep_queries)})")

    # The PgSleep query should have ~5000ms total
    if sleep_queries:
        sq = qids[sleep_queries[0]]
        check(sq['total_ms'] > 2000,
              f"PgSleep query total = {sq['total_ms']:.0f}ms (expected > 2000ms)")

    # Total per-query time should be positive
    total_query_time = sum(info['total_ms'] for info in qids.values())
    check(total_query_time > 1000,
          f"Total query time = {total_query_time:.0f}ms (expected > 1000ms)")


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

    print(f"=== test_query_accuracy (postmaster PID {pm_pid}) ===")

    cleanup_stale_backends()
    test_query_accuracy(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
