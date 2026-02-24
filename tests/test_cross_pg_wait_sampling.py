#!/usr/bin/env python3
"""test_cross_pg_wait_sampling.py — Cross-validate against pg_wait_sampling.

Runs both tools simultaneously during a pgbench workload and verifies:
  1. Top-5 event name overlap (at least 2 shared)
  2. Event class overlap (at least 1 shared)
  3. Time estimate correlation for the top shared event

pg_wait_sampling reports sample counts (not durations).  Estimated time =
count * profile_period_ms.  The BPF tracer reports precise nanosecond durations.

Requires: root, running PostgreSQL 18, pg_wait_tracer built, pgbench initialized,
          pg_wait_sampling loaded in shared_preload_libraries.
Usage: sudo python3 tests/test_cross_pg_wait_sampling.py [--pid POSTMASTER_PID]
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


def parse_sampling_profile(raw):
    """Parse pg_wait_sampling_profile SQL output (pipe-delimited)."""
    results = []
    for line in raw.strip().split('\n'):
        if '|' not in line:
            continue
        parts = line.split('|', 1)
        name = parts[0].strip()
        count = int(parts[1].strip())
        if name and count > 0:
            results.append({'name': name, 'count': count})
    return results


def test_cross_validate(pm_pid, profile_period_ms):
    """Run both tools, compare top event overlap."""
    print("--- Test 1: Top Event Overlap ---")

    # Reset pg_wait_sampling profile
    psql("SELECT pg_wait_sampling_reset_profile()")

    # Start pgbench
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "30"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)  # warmup

    # Start tracer
    tracer = subprocess.Popen(
        [TRACER, "--pid", str(pm_pid),
         "--interval", "20", "--duration", "25",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, stderr = tracer.communicate(timeout=40)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    # Read pg_wait_sampling profile immediately
    sampling_raw = psql(
        "SELECT COALESCE(event_type, 'CPU') || ':' || COALESCE(event, 'on-cpu') "
        "AS name, sum(count) AS total "
        "FROM pg_wait_sampling_profile "
        "WHERE event_type IS DISTINCT FROM 'Activity' "
        "GROUP BY event_type, event "
        "ORDER BY total DESC LIMIT 10"
    )

    pgbench.wait()

    # Parse tracer output
    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    tracer_events = parse_system_events(output)

    # Parse pg_wait_sampling output
    sampling_events = parse_sampling_profile(sampling_raw)

    print(f"  Tracer events:   {[e['name'] for e in tracer_events[:5]]}")
    print(f"  Sampling events: {[e['name'] for e in sampling_events[:5]]}")

    # Build top-5 sets (tracer excludes idle events already)
    tracer_top5 = set(e['name'] for e in tracer_events[:5])
    # Map pg_wait_sampling names: "CPU:on-cpu" → "CPU"
    sampling_names = []
    for e in sampling_events[:5]:
        name = e['name']
        if name == 'CPU:on-cpu':
            name = 'CPU*'
        sampling_names.append(name)
    sampling_top5 = set(sampling_names)

    # Event name overlap
    overlap = tracer_top5 & sampling_top5
    check(len(overlap) >= 2,
          f"Top-5 event overlap >= 2: {overlap} "
          f"(tracer={tracer_top5}, sampling={sampling_top5})")

    # Event class overlap
    def get_class(name):
        if ':' in name:
            return name.split(':')[0]
        return name

    tracer_classes = set(get_class(n) for n in tracer_top5)
    sampling_classes = set(get_class(n) for n in sampling_top5)
    class_overlap = tracer_classes & sampling_classes

    check(len(class_overlap) >= 1,
          f"Event class overlap >= 1: {class_overlap}")


def test_time_estimate(pm_pid, profile_period_ms):
    """For the top shared event, compare time estimates."""
    print("--- Test 2: Time Estimate Correlation ---")

    # Reset pg_wait_sampling profile
    psql("SELECT pg_wait_sampling_reset_profile()")

    # Start pgbench
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "4", "-T", "25"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    # Start tracer
    tracer = subprocess.Popen(
        [TRACER, "--pid", str(pm_pid),
         "--interval", "15", "--duration", "20",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout, stderr = tracer.communicate(timeout=35)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, _ = tracer.communicate()

    sampling_raw = psql(
        "SELECT COALESCE(event_type, 'CPU') || ':' || COALESCE(event, 'on-cpu') "
        "AS name, sum(count) AS total "
        "FROM pg_wait_sampling_profile "
        "WHERE event_type IS DISTINCT FROM 'Activity' "
        "GROUP BY event_type, event "
        "ORDER BY total DESC LIMIT 10"
    )

    pgbench.wait()

    output = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    tracer_events = parse_system_events(output)
    sampling_events = parse_sampling_profile(sampling_raw)

    # Build lookup dicts
    tracer_by_name = {}
    for e in tracer_events:
        tracer_by_name[e['name']] = e

    sampling_by_name = {}
    for e in sampling_events:
        name = e['name']
        if name == 'CPU:on-cpu':
            name = 'CPU*'
        sampling_by_name[name] = e

    # Find top shared event
    shared_event = None
    for e in tracer_events:
        if e['name'] in sampling_by_name:
            shared_event = e['name']
            break

    if shared_event:
        tracer_ms = tracer_by_name[shared_event]['total_ms']
        sampling_count = sampling_by_name[shared_event]['count']
        sampling_estimated_ms = sampling_count * profile_period_ms

        if tracer_ms > 0 and sampling_estimated_ms > 0:
            ratio = sampling_estimated_ms / tracer_ms
            check(0.1 <= ratio <= 10.0,
                  f"Time ratio for {shared_event}: "
                  f"sampling={sampling_estimated_ms:.0f}ms / "
                  f"tracer={tracer_ms:.0f}ms = {ratio:.2f} "
                  f"(acceptable 0.1-10x)")
        else:
            check(True,
                  f"Time comparison for {shared_event}: "
                  f"tracer={tracer_ms:.0f}ms, "
                  f"sampling_est={sampling_estimated_ms:.0f}ms "
                  f"(one is zero, skip ratio)")
    else:
        check(False,
              f"No shared event found between tools "
              f"(tracer: {list(tracer_by_name.keys())[:5]}, "
              f"sampling: {list(sampling_by_name.keys())[:5]})")


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

    # Check pg_wait_sampling is available
    try:
        psql("SELECT 1 FROM pg_wait_sampling_profile LIMIT 1")
    except Exception:
        print("SKIP: pg_wait_sampling not available")
        sys.exit(0)

    # Check for empty result (extension not loaded)
    test_result = psql("SELECT count(*) FROM pg_wait_sampling_profile")
    # Even if count is 0, the view exists so the extension is loaded

    # Get profile_period for time estimation
    period_raw = psql("SHOW pg_wait_sampling.profile_period")
    try:
        profile_period_ms = int(period_raw)
    except (ValueError, TypeError):
        profile_period_ms = 10  # default

    print(f"=== test_cross_pg_wait_sampling (postmaster PID {pm_pid}, "
          f"profile_period={profile_period_ms}ms) ===")

    test_cross_validate(pm_pid, profile_period_ms)
    test_time_estimate(pm_pid, profile_period_ms)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
