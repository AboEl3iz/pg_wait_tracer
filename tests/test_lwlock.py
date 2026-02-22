#!/usr/bin/env python3
"""test_lwlock.py — Verify LWLock events during concurrent write workload.

pgbench TPC-B with 8 clients generates WAL contention, producing LWLock events
such as WALInsert, WALWrite, BufferContent, BufferMapping.

Requires: root, running PostgreSQL 18, pg_wait_tracer built, pgbench initialized.
Usage: sudo python3 tests/test_lwlock.py [--pid POSTMASTER_PID]
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


def psql(sql):
    result = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=10
    )
    return result.stdout.strip()


def parse_system_events(output):
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


EXPECTED_LWLOCK_EVENTS = {
    "LWLock:WALInsert", "LWLock:WALWrite", "LWLock:BufferContent",
    "LWLock:BufferMapping", "LWLock:WALBufMapping", "LWLock:lock_manager",
    "LWLock:ProcArray", "LWLock:XidGen", "LWLock:CLogTruncation",
}


def test_lwlock_detection(pm_pid):
    """Run pgbench TPC-B, verify LWLock events appear."""
    print("--- Test 1: LWLock Event Detection ---")

    # Start pgbench with 8 clients for WAL contention
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "8", "-T", "20"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)  # warmup: let backends connect and start transactions

    # Start tracer
    tracer = subprocess.Popen(
        [TRACER, "--pid", str(pm_pid),
         "--interval", "12", "--duration", "15",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, stderr = tracer.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    pgbench.wait()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    events = parse_system_events(output)

    # Check for any LWLock event
    lwlock_events = [e for e in events if e['name'].startswith('LWLock:')]

    check(len(lwlock_events) > 0,
          f"At least one LWLock event detected "
          f"(found: {[e['name'] for e in lwlock_events]}, "
          f"all events: {[e['name'] for e in events]})")

    if lwlock_events:
        # Check count and duration
        top_lw = max(lwlock_events, key=lambda e: e['total_ms'])
        check(top_lw['count'] > 0,
              f"Top LWLock {top_lw['name']} count = {top_lw['count']}")
        check(top_lw['total_ms'] > 0,
              f"Top LWLock {top_lw['name']} total = {top_lw['total_ms']:.1f}ms")

        # Check for known expected events
        known = [e['name'] for e in lwlock_events
                 if e['name'] in EXPECTED_LWLOCK_EVENTS]
        check(len(known) > 0,
              f"Known LWLock events found: {known}")
    else:
        # Still register these as fails
        check(False, "Top LWLock event count > 0")
        check(False, "Known LWLock events found")


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

    print(f"=== test_lwlock (postmaster PID {pm_pid}) ===")

    test_lwlock_detection(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
