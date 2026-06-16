#!/usr/bin/env python3
"""test_active.py — Verify active sessions view output.

Tests:
  1. Format: header, column headers, Uptime, Backend Type column
  2. States: backends show one of 'on cpu', 'waiting', 'idle'
  3. Sort by wait_time (default): waiting backends first
  4. Sort by db_time: highest DB Time first
  5. Sort by pid: ascending PID order
  6. Backend types: known backend types appear in output

Requires: root, running PostgreSQL 18, pgbench initialized.
Usage: sudo python3 tests/test_active.py [--pid POSTMASTER_PID]
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


def run_tracer(pm_pid, interval=1, count=2, view="active",
               extra_args=None, timeout_extra=15):
    """Run tracer with --view active, return cleaned stdout."""
    cmd = [TRACER, "--mode", "full", "--pid", str(pm_pid),
           "--interval", str(interval),
           "--count", str(count),
           "--view", view]
    if extra_args:
        cmd += extra_args
    wall = interval * count + timeout_extra
    result = subprocess.run(cmd, capture_output=True, timeout=wall)
    return STRIP_ANSI.sub('', result.stdout.decode('utf-8', errors='replace'))


def split_ticks(output):
    """Split piped output into per-tick text blocks."""
    ticks = []
    current = []
    for line in output.split('\n'):
        if re.match(r'^--- \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} ---$', line.strip()):
            if current:
                text = '\n'.join(current)
                if text.strip():
                    ticks.append(text)
            current = []
        else:
            current.append(line)
    if current:
        text = '\n'.join(current)
        if text.strip():
            ticks.append(text)
    return ticks


def last_tick_rows(output):
    """Parse active rows from the last tick only."""
    ticks = split_ticks(output)
    if not ticks:
        return []
    return parse_active_rows(ticks[-1])


def parse_active_rows(output):
    """Parse active view data rows.

    Each row: PID  State  Wait Event  Wait (ms)  DB Time (ms)  Backend Type
    States: 'on cpu', 'waiting', 'idle'
    """
    rows = []
    for line in output.split('\n'):
        # Match: <PID> <state> <rest>
        m = re.match(
            r'^\s*(\d+)\s+'                              # PID
            r'(on cpu|waiting|idle)\s+'                   # State
            r'(\S+(?::\S+)?|\u2014)\s+'            # Wait Event or em-dash
            r'([\d.]+|\u2014)\s+'                  # Wait (ms) or em-dash
            r'([\d.]+|\u2014)\s+'                  # DB Time (ms) or em-dash
            r'(\S+)',                                     # Backend Type
            line
        )
        if m:
            wait_ms = None
            if m.group(4) != '\u2014':
                try:
                    wait_ms = float(m.group(4))
                except ValueError:
                    pass
            db_ms = None
            if m.group(5) != '\u2014':
                try:
                    db_ms = float(m.group(5))
                except ValueError:
                    pass
            rows.append({
                'pid': int(m.group(1)),
                'state': m.group(2),
                'event': m.group(3) if m.group(3) != '\u2014' else None,
                'wait_ms': wait_ms,
                'db_ms': db_ms,
                'type': m.group(6),
            })
    return rows


# ── Test 1: Format Validation ────────────────────────────────

def test_format(pm_pid):
    """Verify active view output has correct headers and structure."""
    print("--- Test 1: Format Validation ---")

    output = run_tracer(pm_pid, interval=1, count=2)

    # Title header
    check("Active Sessions" in output,
          "Output contains 'Active Sessions' header")

    # Uptime in header
    check("Uptime:" in output,
          "Output contains 'Uptime:' in header")

    # Backends count in header
    check("Backends:" in output,
          "Output contains 'Backends:' in header")

    # Column headers
    check("PID" in output,
          "Output contains 'PID' column")
    check("State" in output,
          "Output contains 'State' column")
    check("Wait Event" in output,
          "Output contains 'Wait Event' column")
    check("Wait (ms)" in output,
          "Output contains 'Wait (ms)' column")
    check("DB Time (ms)" in output,
          "Output contains 'DB Time (ms)' column")
    check("Backend Type" in output,
          "Output contains 'Backend Type' column")

    # Data rows present
    rows = parse_active_rows(output)
    check(len(rows) > 0,
          f"Parsed {len(rows)} data rows (expected > 0)")


# ── Test 2: States ────────────────────────────────────────────

def test_states(pm_pid):
    """Verify backends show valid states."""
    print("--- Test 2: States ---")

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "2", "-T", "15"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=1, count=2)

    pgbench.wait()

    rows = parse_active_rows(output)
    check(len(rows) > 0,
          f"Parsed {len(rows)} rows with workload")

    if not rows:
        return

    # All states should be valid
    valid_states = {'on cpu', 'waiting', 'idle'}
    all_valid = all(r['state'] in valid_states for r in rows)
    check(all_valid,
          "All backend states are valid (on cpu/waiting/idle)")

    # Waiting backends should have an event name
    for r in rows:
        if r['state'] == 'waiting':
            check(r['event'] is not None,
                  f"PID {r['pid']} waiting has event: {r['event']}")
            break
    else:
        # No waiting backend found — check for at least idle or on cpu
        has_state = any(r['state'] in ('idle', 'on cpu') for r in rows)
        check(has_state,
              "At least one backend has a known state")

    # Idle backends should not have wait_ms
    idle_rows = [r for r in rows if r['state'] == 'idle']
    if idle_rows:
        check(idle_rows[0]['wait_ms'] is None,
              "Idle backend has no Wait (ms) value")


# ── Test 3: Sort by wait_time (default) ──────────────────────

def test_sort_wait_time(pm_pid):
    """Verify default sort puts waiting backends first."""
    print("--- Test 3: Sort by wait_time ---")

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "15"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=1, count=2)

    pgbench.wait()

    rows = last_tick_rows(output)
    check(len(rows) >= 2,
          f"Got {len(rows)} rows for sort test")

    if len(rows) < 2:
        return

    # Find first non-waiting backend
    state_order = {'waiting': 0, 'on cpu': 1, 'idle': 2}
    states = [state_order.get(r['state'], 3) for r in rows]

    is_sorted = all(states[i] <= states[i+1] for i in range(len(states)-1))
    check(is_sorted,
          "Backends sorted: waiting first, then on cpu, then idle")


# ── Test 4: Sort by db_time ──────────────────────────────────

def test_sort_db_time(pm_pid):
    """Verify --sort db_time sorts by DB Time descending."""
    print("--- Test 4: Sort by db_time ---")

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "15"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=1, count=2,
                        extra_args=["--sort", "db_time"])

    pgbench.wait()

    rows = last_tick_rows(output)
    check(len(rows) >= 2,
          f"Got {len(rows)} rows for db_time sort test")

    if len(rows) < 2:
        return

    # DB times should be non-increasing (treating None as 0)
    db_times = [r['db_ms'] if r['db_ms'] is not None else 0 for r in rows]
    is_sorted = all(db_times[i] >= db_times[i+1]
                    for i in range(len(db_times)-1))
    check(is_sorted,
          f"Backends sorted by DB Time descending "
          f"(first={db_times[0]:.0f}, last={db_times[-1]:.0f})")


# ── Test 5: Sort by pid ──────────────────────────────────────

def test_sort_pid(pm_pid):
    """Verify --sort pid sorts by PID ascending."""
    print("--- Test 5: Sort by pid ---")

    output = run_tracer(pm_pid, interval=1, count=2,
                        extra_args=["--sort", "pid"])

    rows = last_tick_rows(output)
    check(len(rows) >= 2,
          f"Got {len(rows)} rows for pid sort test")

    if len(rows) < 2:
        return

    pids = [r['pid'] for r in rows]
    is_sorted = all(pids[i] <= pids[i+1] for i in range(len(pids)-1))
    check(is_sorted,
          f"Backends sorted by PID ascending "
          f"(first={pids[0]}, last={pids[-1]})")


# ── Test 6: Backend types ────────────────────────────────────

def test_backend_types(pm_pid):
    """Verify known backend types appear."""
    print("--- Test 6: Backend types ---")

    # Use longer count to ensure all backends are captured
    output = run_tracer(pm_pid, interval=1, count=3)

    rows = last_tick_rows(output)
    check(len(rows) > 0,
          f"Parsed {len(rows)} rows for type test")

    if not rows:
        return

    known_types = {'client', 'checkpointer', 'bgwriter', 'walwriter',
                   'autovac_launcher', 'autovac_worker', 'walsender',
                   'walreceiver', 'startup', 'logical_launcher',
                   'logical_worker', 'archiver', 'logger',
                   'parallel_worker', 'io_worker', 'bg_worker', 'unknown'}

    types_found = {r['type'] for r in rows}
    all_known = types_found.issubset(known_types)
    check(all_known,
          f"All backend types are known: {types_found}")

    # Should have at least one system backend (checkpointer, bgwriter, etc.)
    system_types = types_found - {'client', 'unknown'}
    check(len(system_types) > 0,
          f"Found system backend types: {system_types}")


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
        pm_pid = find_postmaster()
    if not pm_pid:
        print("ERROR: cannot find PostgreSQL postmaster PID")
        sys.exit(1)

    print(f"=== test_active (postmaster PID {pm_pid}) ===")

    test_format(pm_pid)
    test_states(pm_pid)
    test_sort_wait_time(pm_pid)
    test_sort_db_time(pm_pid)
    test_sort_pid(pm_pid)
    test_backend_types(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
