#!/usr/bin/env python3
"""test_query_event.py — Verify query-level wait event attribution.

Two tests:
  1. Basic: pgbench workload produces query_ids in query_event view,
     cross-referenced against pg_stat_statements.
  2. Specific: a known query's query_id from pg_stat_statements
     appears in tracer output.

Requires: root, running PostgreSQL 18, pg_wait_tracer built, pgbench initialized,
          compute_query_id = on/auto.
Usage: sudo python3 tests/test_query_event.py [--pid POSTMASTER_PID]
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
    result = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=timeout
    )
    return result.stdout.strip()


def parse_query_events(output):
    """Parse query_event view output."""
    events = []
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(-?\d+)\s+'           # query_id (signed int64)
            r'(\S+(?::\S+)?)\s+'     # event name
            r'(\d+)\s+'              # total waits
            r'([\d.]+)\s+'           # total (ms)
            r'([\d.]+)\s+'           # avg (us)
            r'([\d.]+)\s+'           # max (us)
            r'([\d.]+)%',            # % DB
            line
        )
        if m:
            events.append({
                'query_id': int(m.group(1)),
                'name': m.group(2),
                'count': int(m.group(3)),
                'total_ms': float(m.group(4)),
                'avg_us': float(m.group(5)),
                'max_us': float(m.group(6)),
                'pct': float(m.group(7)),
            })
    return events


def cleanup():
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pgbench_accounts%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)


def test_query_event_basic(pm_pid):
    """Run pgbench, verify query_ids appear and match pg_stat_statements."""
    print("--- Test 1: Query Event Basic ---")

    # Ensure pg_stat_statements is available
    psql("CREATE EXTENSION IF NOT EXISTS pg_stat_statements")
    psql("SELECT pg_stat_statements_reset()")

    # Start tracer
    tracer = subprocess.Popen(
        [TRACER, "--pid", str(pm_pid),
         "--interval", "12", "--duration", "16",
         "--view", "query_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    time.sleep(2)  # let tracer attach

    # Run pgbench workload
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "10"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    pgbench.wait()

    try:
        stdout, stderr = tracer.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    events = parse_query_events(output)

    # Query events should exist
    check(len(events) > 0,
          f"Query events found: {len(events)} entries")

    # Non-zero query_ids
    nonzero_qids = [e for e in events if e['query_id'] != 0]
    check(len(nonzero_qids) > 0,
          f"Non-zero query_ids present: {len(nonzero_qids)}")

    # All events have count > 0
    if events:
        all_valid = all(e['count'] > 0 for e in events)
        check(all_valid,
              f"All events have count > 0")

    # Cross-check with pg_stat_statements
    pss_raw = psql(
        "SELECT queryid FROM pg_stat_statements "
        "WHERE dbid = (SELECT oid FROM pg_database WHERE datname = 'postgres') "
        "AND queryid IS NOT NULL AND queryid != 0"
    )
    pss_qids = set()
    for line in pss_raw.strip().split('\n'):
        line = line.strip()
        if line:
            try:
                pss_qids.add(int(line))
            except ValueError:
                pass

    tracer_qids = set(e['query_id'] for e in events if e['query_id'] != 0)

    if pss_qids and tracer_qids:
        overlap = tracer_qids & pss_qids
        check(len(overlap) > 0,
              f"query_id overlap with pg_stat_statements: "
              f"{len(overlap)} shared out of "
              f"tracer={len(tracer_qids)}, pss={len(pss_qids)}")
    else:
        check(False,
              f"Cross-check: tracer_qids={len(tracer_qids)}, "
              f"pss_qids={len(pss_qids)} (need both non-empty)")


def test_specific_query_id(pm_pid):
    """Run a specific identifiable query, verify its query_id appears."""
    print("--- Test 2: Specific Query Identification ---")

    psql("CREATE EXTENSION IF NOT EXISTS pg_stat_statements")
    psql("SELECT pg_stat_statements_reset()")

    # Start tracer
    tracer = subprocess.Popen(
        [TRACER, "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "query_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    time.sleep(2)

    # Run a distinctive query that generates IO events on pgbench_accounts
    # The sequential scan on 138MB table produces IO:DataFileRead
    psql_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT count(*) FROM pgbench_accounts WHERE aid > 0",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        stdout, stderr = tracer.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    psql_proc.terminate()
    try:
        psql_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        psql_proc.kill()
    cleanup()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    events = parse_query_events(output)

    # Get the query_id from pg_stat_statements
    specific_raw = psql(
        "SELECT queryid FROM pg_stat_statements "
        "WHERE query LIKE '%pgbench_accounts%aid%' "
        "AND queryid IS NOT NULL LIMIT 1"
    )
    specific_qid = None
    if specific_raw.strip():
        try:
            specific_qid = int(specific_raw.strip())
        except ValueError:
            pass

    if specific_qid:
        tracer_qids = set(e['query_id'] for e in events)
        check(specific_qid in tracer_qids,
              f"Specific query_id {specific_qid} found in tracer output "
              f"(tracer has {len(tracer_qids)} unique query_ids)")
    else:
        check(False,
              f"Could not find query_id in pg_stat_statements "
              f"(raw: '{specific_raw}')")


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

    # Check compute_query_id is enabled
    cqid = psql("SHOW compute_query_id")
    if cqid not in ('on', 'auto'):
        print(f"ERROR: compute_query_id = '{cqid}' (need 'on' or 'auto')")
        sys.exit(1)

    print(f"=== test_query_event (postmaster PID {pm_pid}) ===")

    test_query_event_basic(pm_pid)
    test_specific_query_id(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
