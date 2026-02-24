#!/usr/bin/env python3
"""test_multi_window.py — Verify multi-window time_model output.

Tests:
  1. Format validation: column headers, separator, data rows present
  2. Progressive population: shorter windows fill first, longer show '-'
  3. Internal consistency: DB Time = CPU + Wait classes (first window)

Requires: root, running PostgreSQL 18, pgbench initialized.
Usage: sudo python3 tests/test_multi_window.py [--pid POSTMASTER_PID]
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


def run_tracer(pm_pid, interval, count, windows, duration=None, timeout_extra=15):
    """Run tracer with --window, return cleaned stdout."""
    cmd = [TRACER, "--pid", str(pm_pid),
           "--interval", str(interval),
           "--window", windows,
           "--count", str(count),
           "--view", "time_model"]
    if duration:
        cmd += ["--duration", str(duration)]
    wall = interval * count + timeout_extra
    result = subprocess.run(cmd, capture_output=True, timeout=wall)
    return STRIP_ANSI.sub('', result.stdout.decode('utf-8', errors='replace'))


def split_ticks(output):
    """Split piped output into per-tick text blocks.

    In pipe mode each tick starts with '--- YYYY-MM-DD HH:MM:SS ---'.
    Returns only non-empty tick blocks (skips preamble before first separator).
    """
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


def parse_first_window(output):
    """Parse time_model multi-window output, extracting first window values.

    Returns dict of {name: value_ms}. Works because the existing regex
    captures the first numerical value on each row — which is the first
    window's value in multi-window mode.
    """
    model = {}
    for line in output.split('\n'):
        line = line.strip()
        m = re.match(r'^(.+?)\s{2,}([\d.]+)\s+[\d.]+%', line)
        if m:
            name = m.group(1).strip()
            value = float(m.group(2))
            model[name] = value
    return model


# ── Test 1: Format Validation ────────────────────────────────

def test_format(pm_pid):
    """Verify multi-window output has correct headers and structure."""
    print("--- Test 1: Format Validation ---")

    # Start pgbench for background workload
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "2", "-T", "20"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=1, count=4, windows="1s,3s")

    pgbench.wait()

    # Column headers present
    check("Last 1s" in output,
          f"Output contains 'Last 1s' header")
    check("Last 3s" in output,
          f"Output contains 'Last 3s' header")
    check("% DB" in output,
          f"Output contains '% DB' header")

    # Separator line (all dashes)
    has_separator = any(
        set(line.strip()) <= {'-', ' '} and len(line.strip()) > 30
        for line in output.split('\n')
        if line.strip()
    )
    check(has_separator, "Output contains dash separator line")

    # Data rows
    check("DB Time" in output,
          f"Output contains 'DB Time' row")
    check("CPU*" in output,
          f"Output contains 'CPU*' row")

    # At least one data row has numeric values (not just dashes)
    has_data = bool(re.search(r'DB Time\s+[\d.]+\s+[\d.]+%', output))
    check(has_data,
          "DB Time row has numeric values")


# ── Test 2: Progressive Population ───────────────────────────

def test_progressive_population(pm_pid):
    """Verify shorter windows fill before longer ones.

    With --window 1s,5s --interval 1:
    - Tick 1: "waiting for data" (ring needs >=2 pushes for 1-tick delta)
    - Tick 2: first window valid, second shows '-' (needs 5 pushes)
    - Tick 6+: both windows valid
    """
    print("--- Test 2: Progressive Population ---")

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", "2", "-T", "20"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    output = run_tracer(pm_pid, interval=1, count=7, windows="1s,5s")

    pgbench.wait()

    ticks = split_ticks(output)

    check(len(ticks) >= 5,
          f"Got {len(ticks)} ticks (expected >= 5)")

    if len(ticks) < 2:
        return

    # First tick should show "waiting for data"
    check("waiting for data" in ticks[0],
          "First tick shows 'waiting for data'")

    # Early ticks (2-4): first window valid, second may show '-'
    # Look for a tick that has "Last 1s" data but '-' for second window
    found_partial = False
    for tick in ticks[1:4]:
        if "Last 1s" in tick:
            lines = tick.split('\n')
            for line in lines:
                if "DB Time" in line:
                    # Check if line has a '-' after first window's value
                    # Pattern: number + % + ... + '-'
                    m = re.search(r'DB Time\s+[\d.]+\s+[\d.]+%.*\s+-\s+-', line)
                    if m:
                        found_partial = True
                    break

    check(found_partial,
          "Early tick has first window data but '-' for second window")

    # Later ticks: both windows should be valid
    if len(ticks) >= 6:
        last_tick = ticks[-1]
        # Should have "Last 5s" data (not just '-')
        has_both = bool(re.search(r'DB Time\s+[\d.]+\s+[\d.]+%\s+[\d.]+\s+[\d.]+%',
                                  last_tick))
        check(has_both,
              "Last tick has data for both windows")


# ── Test 3: Internal Consistency ─────────────────────────────

def test_internal_consistency(pm_pid):
    """Verify DB Time = CPU* + sum(Wait classes) in multi-window mode.

    Uses first window values parsed from multi-window output.
    """
    print("--- Test 3: Internal Consistency ---")

    CLIENTS = 4
    BENCH_SEC = 30

    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres",
         "-c", str(CLIENTS), "-T", str(BENCH_SEC)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    time.sleep(3)

    # Use a 5s window; count=3 because first tick is "waiting for data"
    # pgbench must outlive the measurement: 3 × 5s = 15s + 3s warmup = 18s < 30s
    output = run_tracer(pm_pid, interval=5, count=3,
                        windows="5s")

    pgbench.wait()

    model = parse_first_window(output)

    check('DB Time' in model,
          f"DB Time found in multi-window output (keys: {list(model.keys())})")

    if 'DB Time' not in model:
        return

    db_time_ms = model['DB Time']
    cpu_time_ms = model.get('CPU*', 0)

    check(db_time_ms > 100,
          f"DB Time = {db_time_ms:.0f}ms (expected > 100ms)")

    check(cpu_time_ms > 0,
          f"CPU* = {cpu_time_ms:.0f}ms (expected > 0)")

    # Sum wait classes (top-level names without ':')
    WAIT_CLASSES = {'IO', 'LWLock', 'Lock', 'Client', 'IPC',
                    'BufferPin', 'Timeout', 'Extension'}
    wait_sum = sum(v for k, v in model.items() if k in WAIT_CLASSES)

    reconstructed = cpu_time_ms + wait_sum

    if db_time_ms > 0:
        error_pct = abs(reconstructed - db_time_ms) / db_time_ms * 100
        check(error_pct < 2.0,
              f"DB Time consistency: CPU({cpu_time_ms:.0f}) + "
              f"Waits({wait_sum:.0f}) = {reconstructed:.0f}ms "
              f"vs DB Time {db_time_ms:.0f}ms (error {error_pct:.1f}%)")


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

    print(f"=== test_multi_window (postmaster PID {pm_pid}) ===")

    test_format(pm_pid)
    test_progressive_population(pm_pid)
    test_internal_consistency(pm_pid)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
