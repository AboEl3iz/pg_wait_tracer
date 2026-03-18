#!/usr/bin/env python3
"""test_aas_accuracy.py — Verify AAS with known concurrency.

Spec 0A.2: Start exactly 4 backends each doing pg_sleep(10).
Verify AAS ≈ 4.0 and DB Time ≈ 4 × measurement_window.

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
    # AAS line: "DB Time    25000.0    100.0%    3.12 AAS"
    for line in output.split('\n'):
        m = re.search(r'([\d.]+)\s+AAS', line)
        if m:
            model['AAS'] = float(m.group(1))
    for line in output.split('\n'):
        m = re.match(r'.*Activity.*?\s+([\d.]+)\s+', line.strip())
        if m:
            model['Activity'] = float(m.group(1))
    return model


def test_aas_accuracy(pm_pid):
    """Start 4 backends each doing pg_sleep(10). Verify AAS ≈ 4.0."""
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

    # Start tracer
    INTERVAL = 8
    tracer = subprocess.Popen(
        [TRACER, "--pid", str(pm_pid),
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

    model = parse_time_model(output)

    check('DB Time' in model,
          f"DB Time found (keys: {list(model.keys())})")
    if 'DB Time' not in model:
        return

    db_time = model['DB Time']

    # DB Time should be approximately N_BACKENDS × INTERVAL × 1000,
    # plus some from system backends (checkpointer, pg_wait_sampling, etc.)
    expected_min = N_BACKENDS * INTERVAL * 1000 * 0.5  # generous lower
    expected_max = (N_BACKENDS + 4) * INTERVAL * 1000  # allow ~4 extra system backends
    check(expected_min <= db_time <= expected_max,
          f"DB Time = {db_time:.0f}ms (expected {expected_min:.0f}-{expected_max:.0f}ms)")

    # AAS should be approximately N_BACKENDS (system backends add a bit more)
    if 'AAS' in model:
        aas = model['AAS']
        check(N_BACKENDS * 0.5 <= aas <= N_BACKENDS * 2.0,
              f"AAS = {aas:.2f} (expected ~{N_BACKENDS}.0)")
    else:
        # Compute AAS from DB Time / interval
        aas_computed = db_time / (INTERVAL * 1000)
        check(N_BACKENDS * 0.5 <= aas_computed <= N_BACKENDS * 2.0,
              f"Computed AAS = {aas_computed:.2f} (expected ~{N_BACKENDS}.0)")

    # Timeout should dominate (pg_sleep is Timeout:PgSleep)
    timeout_time = model.get('Timeout', 0)
    if db_time > 0:
        timeout_pct = timeout_time / db_time * 100
        check(timeout_pct > 50,
              f"Timeout = {timeout_pct:.1f}% of DB Time (expected > 50% for sleeping)")


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
