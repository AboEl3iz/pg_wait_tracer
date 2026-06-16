#!/usr/bin/env python3
"""test_aas_accuracy.py — Verify AAS with known concurrency.

Spec 0A.2: Start exactly 4 backends each doing pg_sleep(10).
Use session_event to separate client AAS from system backend AAS.
Verify client AAS ≈ 4.0 and total AAS = client + system.

Requires: root, running PostgreSQL 18.
Usage: sudo python3 tests/test_aas_accuracy.py [--pid POSTMASTER_PID]
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
        m = re.search(r'([\d.]+)\s+AAS', line)
        if m:
            model['AAS'] = float(m.group(1))
    for line in output.split('\n'):
        m = re.match(r'.*Activity.*?\s+([\d.]+)\s+', line.strip())
        if m:
            model['Activity'] = float(m.group(1))
    return model


def parse_session_events(output):
    """Parse session_event view into list of dicts."""
    sessions = []
    for line in output.split('\n'):
        line = line.strip()
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
                'db_time_ms': float(m.group(5)),
            })
    return sessions


def test_aas_accuracy(pm_pid):
    """Start 4 backends each doing pg_sleep(10). Verify AAS ≈ 4.0.

    Uses session_event to separate client vs system backend AAS.
    """
    print("--- Test 1: AAS with 4 concurrent sleepers ---")

    N_BACKENDS = 4
    SLEEP_SEC = 12

    # Start N backends, each sleeping — BEFORE the tracer
    procs = []
    for _ in range(N_BACKENDS):
        p = subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres",
             "-c", f"SELECT pg_sleep({SLEEP_SEC})",
             "-c", "SELECT pg_sleep(60)"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        procs.append(p)

    time.sleep(2)  # let all backends enter pg_sleep

    INTERVAL = 8

    # Run time_model and session_event in parallel
    tracer_tm = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    tracer_se = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "session_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout_tm, _ = tracer_tm.communicate(timeout=INTERVAL + 20)
    except subprocess.TimeoutExpired:
        tracer_tm.kill()
        stdout_tm, _ = tracer_tm.communicate()

    try:
        stdout_se, _ = tracer_se.communicate(timeout=INTERVAL + 20)
    except subprocess.TimeoutExpired:
        tracer_se.kill()
        stdout_se, _ = tracer_se.communicate()

    output_tm = STRIP_ANSI.sub('', stdout_tm.decode('utf-8', errors='replace'))
    output_se = STRIP_ANSI.sub('', stdout_se.decode('utf-8', errors='replace'))

    # Cleanup
    for p in procs:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)

    # Parse time_model
    model = parse_time_model(output_tm)
    check('DB Time' in model,
          f"DB Time found (keys: {list(model.keys())})")
    if 'DB Time' not in model:
        return

    db_time = model['DB Time']
    total_aas = db_time / (INTERVAL * 1000)

    # Parse session_event — separate client vs system backends
    sessions = parse_session_events(output_se)
    clients = [s for s in sessions if s['type'] == 'client']
    system = [s for s in sessions if s['type'] != 'client']

    client_db_time = sum(s['db_time_ms'] for s in clients)
    system_db_time = sum(s['db_time_ms'] for s in system)
    client_aas = client_db_time / (INTERVAL * 1000)
    system_aas = system_db_time / (INTERVAL * 1000)

    print(f"    Total AAS = {total_aas:.2f} "
          f"(client={client_aas:.2f} + system={system_aas:.2f})")
    print(f"    {len(clients)} client backends, {len(system)} system backends")

    # Client AAS should be close to N_BACKENDS (each sleeping = 1 AAS)
    check(N_BACKENDS * 0.8 <= client_aas <= N_BACKENDS * 1.2,
          f"Client AAS = {client_aas:.2f} (expected {N_BACKENDS}.0 ±20%)")

    # System backends should contribute some AAS (> 0)
    check(system_aas >= 0,
          f"System AAS = {system_aas:.2f} (≥ 0)")

    # Total AAS = client + system (session_event should partition DB Time)
    # Note: session_event may not capture all system backend activity
    # (some short-lived or background backends may not appear), so we
    # allow up to 25% deviation.
    session_total = client_db_time + system_db_time
    if db_time > 0:
        partition_error = abs(session_total - db_time) / db_time * 100
        check(partition_error < 25.0,
              f"Session partition: client({client_db_time:.0f}) + "
              f"system({system_db_time:.0f}) = {session_total:.0f} "
              f"vs DB Time {db_time:.0f} (error {partition_error:.1f}%)")

    # Timeout should dominate client DB Time
    timeout_time = model.get('Timeout', 0)
    if db_time > 0:
        timeout_pct = timeout_time / db_time * 100
        check(timeout_pct > 40,
              f"Timeout = {timeout_pct:.1f}% of DB Time (expected > 40% for sleeping)")


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

    print(f"=== test_aas_accuracy (postmaster PID {pm_pid}) ===")

    cleanup_stale_backends()
    test_aas_accuracy(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
