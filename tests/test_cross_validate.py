#!/usr/bin/env python3
"""test_cross_validate.py — Compare pg_wait_tracer output against pg_stat_activity.

Requires: root, running PostgreSQL 18, pgbench workload.
Usage: sudo python3 tests/test_cross_validate.py [--pid POSTMASTER_PID]
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

VALID_CLASSES = {
    "CPU*", "IO", "LWLock", "Lock", "BufferPin",
    "Activity", "Client", "Extension", "IPC", "Timeout",
}

tests_run = 0
tests_passed = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")



def get_pg_backends():
    """Get active backends from pg_stat_activity."""
    result = subprocess.run(
        ["psql", "-U", "postgres", "-tAc",
         "SELECT pid, backend_type, wait_event_type, wait_event, usename, datname "
         "FROM pg_stat_activity WHERE pid != pg_backend_pid()"],
        capture_output=True, text=True, timeout=5
    )
    if result.returncode != 0:
        print(f"  psql failed: {result.stderr}")
        return []
    backends = []
    for line in result.stdout.strip().split('\n'):
        if not line:
            continue
        parts = line.split('|')
        if len(parts) >= 6:
            backends.append({
                'pid': int(parts[0]),
                'backend_type': parts[1],
                'wait_event_type': parts[2] if parts[2] else None,
                'wait_event': parts[3] if parts[3] else None,
                'usename': parts[4] if parts[4] else None,
                'datname': parts[5] if parts[5] else None,
            })
    return backends


def run_tracer_session_view(pm_pid, duration=6, interval=5):
    """Run tracer with session_event view and capture output."""
    cmd = [
        TRACER,
        "--mode", "full",
        "--pid", str(pm_pid),
        "--interval", str(interval),
        "--duration", str(duration),
        "--view", "session_event",
    ]
    result = subprocess.run(cmd, capture_output=True, timeout=30)
    output = result.stdout.decode('utf-8', errors='replace')
    output = STRIP_ANSI.sub('', output)
    return output


def run_tracer_system_view(pm_pid, duration=6, interval=5):
    """Run tracer with system_event view and capture output."""
    cmd = [
        TRACER,
        "--mode", "full",
        "--pid", str(pm_pid),
        "--interval", str(interval),
        "--duration", str(duration),
        "--view", "system_event",
    ]
    result = subprocess.run(cmd, capture_output=True, timeout=30)
    output = result.stdout.decode('utf-8', errors='replace')
    output = STRIP_ANSI.sub('', output)
    return output


def parse_session_pids(output):
    """Parse PIDs and types from session_event output."""
    sessions = []
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(r'^(\d+)\s+(\S+)\s+(\S+)\s+(\S+)', line)
        if m:
            sessions.append({
                'pid': int(m.group(1)),
                'type': m.group(2),
                'user': m.group(3),
                'db': m.group(4),
            })
    return sessions


def parse_system_events(output):
    """Parse event names from system_event output."""
    events = []
    for line in output.split('\n'):
        line = line.strip()
        # Match lines like: "IO:DataFileRead     1035      21.9 ..."
        m = re.match(r'^(\S+:\S+|CPU)\s+\d+\s+[\d.]+', line)
        if m:
            events.append(m.group(1))
    return events


def test_backend_coverage(pm_pid):
    """Test that tracer sees all backends that pg_stat_activity reports."""
    print("--- Backend Coverage ---")

    # Transient-PID race guard:
    #   The tracer takes its session snapshot at one instant, while
    #   pg_stat_activity is read separately. A short-lived client backend can
    #   appear in one source and vanish from the other purely because it
    #   connected or disconnected between the two reads — that is not a tracer
    #   bug, just timing.
    #
    #   To stay robust without weakening the real cross-check, we read
    #   pg_stat_activity BOTH before and after the tracer window and only
    #   require PIDs that are STABLY present (in both reads, i.e. alive for the
    #   whole tracer window) to show up in the tracer. PIDs present in only one
    #   read are in-flight/transient and are tolerated. The substantive
    #   assertion — that the tracer sees the backends pg_stat_activity reports —
    #   is still enforced for every stable backend.
    pg_backends_before = get_pg_backends()
    check(len(pg_backends_before) > 0, "pg_stat_activity should return backends")

    tracer_output = run_tracer_session_view(pm_pid)
    tracer_sessions = parse_session_pids(tracer_output)
    tracer_pids = {s['pid'] for s in tracer_sessions}

    pg_backends_after = get_pg_backends()

    check(len(tracer_sessions) > 0, "tracer should report at least one session")

    # Stable client backends = present in BOTH the before and after reads, so
    # they existed across the entire tracer window and must have been observed.
    client_pids_before = {b['pid'] for b in pg_backends_before
                          if b['backend_type'] == 'client backend'}
    client_pids_after = {b['pid'] for b in pg_backends_after
                         if b['backend_type'] == 'client backend'}
    stable_client_pids = client_pids_before & client_pids_after

    check(len(stable_client_pids) > 0,
          "at least one client backend should be stably present across the tracer window")

    for pid in stable_client_pids:
        check(pid in tracer_pids,
              f"stable client PID {pid} from pg_stat_activity missing in tracer")


def test_backend_types(pm_pid):
    """Test that backend types are correctly identified."""
    print("--- Backend Types ---")

    tracer_output = run_tracer_session_view(pm_pid)
    tracer_sessions = parse_session_pids(tracer_output)

    valid_types = {
        "client", "checkpointer", "bgwriter", "walwriter",
        "autovac_launcher", "autovac_worker", "walsender", "walreceiver",
        "startup", "logical_launcher", "logical_worker", "archiver",
        "logger", "parallel_worker", "io_worker", "bg_worker", "unknown",
    }

    for s in tracer_sessions:
        check(s['type'] in valid_types,
              f"PID {s['pid']} has invalid type '{s['type']}'")


def test_event_decode(pm_pid):
    """Test that all events are decoded (no 'unknown_*' in output)."""
    print("--- Event Decode Correctness ---")

    tracer_output = run_tracer_system_view(pm_pid)
    events = parse_system_events(tracer_output)

    check(len(events) > 0, "system_event view should report events")

    for ev in events:
        check("unknown" not in ev.lower(),
              f"event '{ev}' contains 'unknown' — decode table gap?")

    # Check all event classes are valid
    for ev in events:
        if ev == "CPU*":
            continue
        cls = ev.split(':')[0]
        check(cls in VALID_CLASSES,
              f"event '{ev}' has invalid class '{cls}'")


def test_wait_event_cross_check(pm_pid):
    """Cross-check: events seen by tracer should be in pg_wait_events catalog."""
    print("--- Cross-Check vs pg_wait_events ---")

    # Get all known wait events from PG catalog
    result = subprocess.run(
        ["psql", "-U", "postgres", "-tAc",
         "SELECT type || ':' || name FROM pg_wait_events ORDER BY 1"],
        capture_output=True, text=True, timeout=5
    )
    if result.returncode != 0:
        print("  SKIP: pg_wait_events not available")
        return

    pg_events = set(result.stdout.strip().split('\n'))

    tracer_output = run_tracer_system_view(pm_pid)
    events = parse_system_events(tracer_output)

    for ev in events:
        if ev == "CPU*":
            continue
        # Normalize: tracer uses "Lock:tuple" but PG catalog uses "Lock:tuple"
        check(ev in pg_events,
              f"tracer event '{ev}' not found in pg_wait_events catalog")


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

    print(f"=== test_cross_validate (postmaster PID {pm_pid}) ===")

    # Start a pgbench workload to generate events
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres", "-c", "4", "-T", "60"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    try:
        time.sleep(2)  # let pgbench warm up
        test_backend_coverage(pm_pid)
        test_backend_types(pm_pid)
        test_event_decode(pm_pid)
        test_wait_event_cross_check(pm_pid)
    finally:
        pgbench.terminate()
        pgbench.wait()

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
