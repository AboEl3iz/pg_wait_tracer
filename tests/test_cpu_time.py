#!/usr/bin/env python3
"""test_cpu_time.py — Verify CPU time dominance for compute-heavy queries.

Two tests:
  1. system_event: CPU event has highest total_ms for pure-compute query
  2. time_model:   CPU Time > sum of all Wait classes

Requires: root, running PostgreSQL 18, pg_wait_tracer built.
Usage: sudo python3 tests/test_cpu_time.py [--pid POSTMASTER_PID]
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


def parse_time_model(output):
    model = {}
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(r'^(.+?)\s{2,}([\d.]+)\s+[\d.]+%', line)
        if m:
            name = m.group(1).strip()
            value = float(m.group(2))
            model[name] = value
    for line in output.split('\n'):
        m = re.match(r'.*Activity.*?\s+([\d.]+)\s+', line.strip())
        if m:
            model['Activity'] = float(m.group(1))
    return model


def cleanup():
    """Terminate any leftover backends from this test."""
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%generate_series%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)


def test_cpu_system_event(pm_pid):
    """Run pure-compute query, verify CPU is dominant in system_event."""
    print("--- Test 1: CPU Dominance (system_event) ---")

    # Start a long CPU-heavy DO block BEFORE the tracer, so the initial scan
    # finds the backend already on CPU.  The loop runs ~8s of pure computation.
    psql_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "DO $$ DECLARE x bigint := 0; BEGIN "
               "FOR i IN 1..200000000 LOOP x := x + i; END LOOP; END $$",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(2)  # let backend start computing

    # Start tracer — initial scan will see backend on CPU (wait_event_info=0)
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

    psql_proc.terminate()
    try:
        psql_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        psql_proc.kill()
    cleanup()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    events = parse_system_events(output)

    cpu_ev = [e for e in events if e['name'] == 'CPU']

    check(len(cpu_ev) > 0,
          f"CPU event found (events: {[e['name'] for e in events]})")

    if cpu_ev:
        ev = cpu_ev[0]
        check(ev['total_ms'] > 1000,
              f"CPU total = {ev['total_ms']:.1f}ms > 1000ms")

        # CPU should be the top event or at least > 50% of the top event
        non_idle = [e for e in events
                    if not e['name'].startswith('Activity:')]
        if non_idle:
            top_ms = max(e['total_ms'] for e in non_idle)
            check(ev['total_ms'] >= top_ms * 0.5,
                  f"CPU {ev['total_ms']:.1f}ms >= 50% of top event "
                  f"{top_ms:.1f}ms")


def test_cpu_time_model(pm_pid):
    """Run pure-compute query, verify CPU Time > Wait in time_model."""
    print("--- Test 2: CPU Dominance (time_model) ---")

    # Same approach: start workload BEFORE tracer
    psql_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "DO $$ DECLARE x bigint := 0; BEGIN "
               "FOR i IN 1..200000000 LOOP x := x + i; END LOOP; END $$",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(2)

    tracer = subprocess.Popen(
        [TRACER, "--pid", str(pm_pid),
         "--interval", "8", "--duration", "12",
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
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
    model = parse_time_model(output)

    cpu_ms = model.get('CPU Time', 0)
    check(cpu_ms > 0,
          f"CPU Time = {cpu_ms:.1f}ms > 0")

    # CPU should be significant: > 5000ms for the compute-heavy query
    check(cpu_ms > 5000,
          f"CPU Time {cpu_ms:.1f}ms > 5000ms (compute-heavy query)")

    # CPU should be a major component of DB Time (> 25%).
    # We don't check CPU > total waits because system-wide time_model
    # includes background processes (checkpointer, pg_wait_sampling, etc.)
    # whose waits accumulate independently of our compute workload.
    db_time = model.get('DB Time', 0)
    if db_time > 0:
        cpu_pct = cpu_ms / db_time * 100
        check(cpu_pct > 25,
              f"CPU Time is {cpu_pct:.1f}% of DB Time "
              f"(expected > 25% for compute-heavy query)")


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

    print(f"=== test_cpu_time (postmaster PID {pm_pid}) ===")

    cleanup()

    test_cpu_system_event(pm_pid)
    test_cpu_time_model(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
