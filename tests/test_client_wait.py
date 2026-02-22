#!/usr/bin/env python3
"""test_client_wait.py — Verify Client:ClientRead detection.

A backend in "idle in transaction" (after BEGIN, waiting for next command)
enters Client:ClientRead.  This test verifies the tracer detects it via
open-interval accounting.

Requires: root, running PostgreSQL 18, pg_wait_tracer built.
Usage: sudo python3 tests/test_client_wait.py [--pid POSTMASTER_PID]
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


def test_client_read(pm_pid):
    """Start an idle-in-transaction backend BEFORE tracer, verify Client:ClientRead."""
    print("--- Test 1: Client:ClientRead Detection ---")

    # Start psql idle-in-transaction FIRST — before tracer
    # so the initial scan finds it already in Client:ClientRead
    psql_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres"],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    psql_proc.stdin.write(b"BEGIN;\n")
    psql_proc.stdin.flush()
    time.sleep(2)  # let backend enter Client:ClientRead

    # Now start tracer — initial scan will detect the ClientRead state
    tracer = subprocess.Popen(
        [TRACER, "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, stderr = tracer.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    # Cleanup psql
    try:
        psql_proc.stdin.write(b"END;\n")
        psql_proc.stdin.close()
    except (BrokenPipeError, OSError):
        pass
    psql_proc.terminate()
    try:
        psql_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        psql_proc.kill()

    # Kill any lingering idle-in-transaction backends
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND state = 'idle in transaction'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    events = parse_system_events(output)

    client_read = [e for e in events if e['name'] == 'Client:ClientRead']

    check(len(client_read) > 0,
          f"Client:ClientRead found (events: {[e['name'] for e in events]})")

    if client_read:
        ev = client_read[0]
        check(ev['total_ms'] > 4000,
              f"Client:ClientRead duration {ev['total_ms']:.1f}ms > 4000ms")
        check(ev['count'] >= 1,
              f"Client:ClientRead count = {ev['count']} >= 1")


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
        # Find PostgreSQL 18 postmaster (skip other versions)
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

    print(f"=== test_client_wait (postmaster PID {pm_pid}) ===")

    test_client_read(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
