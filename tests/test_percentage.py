#!/usr/bin/env python3
"""test_percentage.py — Controlled 50/50 workload split (sleep + CPU).

Spec 0A.1: One backend does pg_sleep (Timeout), another does CPU burn.
Verify each accounts for ~50% of DB Time.

Requires: root, running PostgreSQL 18.
Usage: sudo python3 tests/test_percentage.py [--pid POSTMASTER_PID]
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


def parse_time_model(output):
    model = {}
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(r'^(.+?)\s{2,}([\d.]+)\s+[\d.]+%', line)
        if m:
            model[m.group(1).strip()] = float(m.group(2))
    for line in output.split('\n'):
        m = re.match(r'.*Activity.*?\s+([\d.]+)\s+', line.strip())
        if m:
            model['Activity'] = float(m.group(1))
    return model


def parse_system_events(output):
    events = []
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(
            r'^(\S+(?::\S+)?)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)%',
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


def test_percentage_split(pm_pid):
    """Backend A: pg_sleep(10) → Timeout:PgSleep.
    Backend B: CPU burn ~10s.
    Each should be ~50% of DB Time.

    Both start BEFORE the tracer so initial scan finds them already active.
    """
    print("--- Test 1: Controlled 50/50 split ---")

    DURATION = 10

    # Backend A: sleep
    sleep_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", f"SELECT pg_sleep({DURATION})",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    # Backend B: CPU burn (~10s of generate_series)
    cpu_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT count(*) FROM generate_series(1, 500000000)",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(1)  # let both backends start their work

    # Start tracer — initial scan finds both active
    INTERVAL = 8
    tracer = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, _ = tracer.communicate(timeout=INTERVAL + 20)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))

    # Cleanup
    for p in [sleep_proc, cpu_proc]:
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

    model = parse_time_model(output)

    check('DB Time' in model,
          f"DB Time found (keys: {list(model.keys())})")
    if 'DB Time' not in model:
        return

    db_time = model['DB Time']
    cpu_time = model.get('CPU*', 0)
    timeout_time = model.get('Timeout', 0)

    check(db_time > 1000,
          f"DB Time = {db_time:.0f}ms (expected > 1000ms)")

    # Each component should be roughly half. ±15% tolerance.
    if db_time > 0:
        cpu_pct = cpu_time / db_time * 100
        timeout_pct = timeout_time / db_time * 100

        check(15 <= cpu_pct <= 85,
              f"CPU = {cpu_pct:.1f}% of DB Time (expected 20-80%)")

        check(15 <= timeout_pct <= 85,
              f"Timeout = {timeout_pct:.1f}% of DB Time (expected 20-80%)")

        # Together they should account for a significant portion of DB Time
        # (other classes like Extension, IO, Client may also contribute)
        combined = cpu_pct + timeout_pct
        check(combined > 50,
              f"CPU + Timeout = {combined:.1f}% (expected > 50% of DB Time)")

    # Also check system_event view
    tracer2 = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", "1", "--duration", "1",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    # Rerun with workloads for system_event verification
    sleep_proc2 = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", f"SELECT pg_sleep({DURATION})",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    cpu_proc2 = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres",
         "-c", "SELECT count(*) FROM generate_series(1, 500000000)",
         "-c", "SELECT pg_sleep(60)"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(1)

    tracer2 = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout2, _ = tracer2.communicate(timeout=INTERVAL + 20)
    except subprocess.TimeoutExpired:
        tracer2.kill()
        stdout2, _ = tracer2.communicate()

    output2 = STRIP_ANSI.sub('', stdout2.decode('utf-8', errors='replace'))
    events = parse_system_events(output2)

    for p in [sleep_proc2, cpu_proc2]:
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

    pg_sleep_ev = [e for e in events if e['name'] == 'Timeout:PgSleep']
    cpu_ev = [e for e in events if e['name'] == 'CPU*']

    if pg_sleep_ev:
        check(pg_sleep_ev[0]['total_ms'] > 2000,
              f"system_event: PgSleep total = {pg_sleep_ev[0]['total_ms']:.0f}ms (> 2000ms)")
    else:
        check(False, f"PgSleep not in system_event (events: {[e['name'] for e in events]})")

    if cpu_ev:
        check(cpu_ev[0]['total_ms'] > 2000,
              f"system_event: CPU total = {cpu_ev[0]['total_ms']:.0f}ms (> 2000ms)")
    else:
        check(False, f"CPU not in system_event (events: {[e['name'] for e in events]})")


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

    print(f"=== test_percentage (postmaster PID {pm_pid}) ===")

    cleanup_stale_backends()
    test_percentage_split(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
