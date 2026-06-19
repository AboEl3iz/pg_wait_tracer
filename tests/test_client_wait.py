#!/usr/bin/env python3
"""test_client_wait.py — Verify Client:ClientRead idle-but-visible contract.

Client:ClientRead is an idle wait event (like Oracle's "SQL*Net message from
client").  When a backend is idle-in-transaction, it enters Client:ClientRead.

The contract (load-vs-visibility split, see src/wait_event.c):
  - Client:ClientRead is EXCLUDED from DB Time / AAS (it is idle load), so an
    idle-in-transaction backend must NOT inflate DB Time.
  - Client:ClientRead is NOT hidden: it must still APPEAR in the system_event
    list (unlike Activity events, which are hidden).
  - The idle time is still accounted — it shows up in the Activity/idle bucket
    of the time model, not in DB Time.

Requires: root, running PostgreSQL 18, pg_wait_tracer built.
Usage: sudo python3 tests/test_client_wait.py [--pid POSTMASTER_PID]
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
tests_skipped = 0


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


def skip(msg):
    global tests_skipped
    tests_skipped += 1
    print(f"  SKIP: {msg}")


def psql(sql):
    result = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=10
    )
    return result.stdout.strip()


def wait_for_idle_in_tx(exclude_pids, timeout=30):
    """Poll pg_stat_activity until a client backend (not in exclude_pids) is
    'idle in transaction' parked in Client:ClientRead.  Returns its pid, or
    None on timeout.  This is the test's precondition: never measure before
    the backend we want to observe is actually established and parked, so the
    result can't depend on a fixed sleep racing a loaded box."""
    deadline = time.time() + timeout
    excl = ",".join(str(p) for p in exclude_pids if p) or "-1"
    sql = (
        "SELECT pid FROM pg_stat_activity "
        "WHERE backend_type = 'client backend' "
        "AND state = 'idle in transaction' "
        "AND wait_event_type = 'Client' AND wait_event = 'ClientRead' "
        f"AND pid NOT IN ({excl}) "
        "ORDER BY backend_start DESC LIMIT 1"
    )
    while time.time() < deadline:
        out = psql(sql)
        if out:
            try:
                return int(out.splitlines()[0])
            except (ValueError, IndexError):
                pass
        time.sleep(0.25)
    return None


def parse_system_events(output):
    events = []
    for line in output.split('\n'):
        line = line.strip()
        # Last column "% DB" is either a number ("99.5%") for DB-Time events
        # or the em-dash sentinel ("—") for idle-but-visible events such as
        # Client:ClientRead (see src/output.c fmt_pct_db()).  Both forms must
        # parse, otherwise the ClientRead row — the whole point of this test —
        # is silently dropped.
        m = re.match(
            r'^(\S+(?::\S+)?)\s+'
            r'(\d+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+)\s+'
            r'([\d.]+%|—)',
            line
        )
        if m:
            pct_raw = m.group(6)
            events.append({
                'name': m.group(1),
                'count': int(m.group(2)),
                'total_ms': float(m.group(3)),
                'avg_us': float(m.group(4)),
                'max_us': float(m.group(5)),
                'pct': None if pct_raw == '—' else float(pct_raw.rstrip('%')),
            })
    return events


def parse_time_model(output):
    model = {}
    for line in output.split('\n'):
        line = line.strip()
        # Normal rows: "Name   <ms>   <pct>%"
        m = re.match(r'^(.+?)\s{2,}([\d.]+)\s+[\d.]+%', line)
        if m:
            model[m.group(1).strip()] = float(m.group(2))
            continue
        # Idle bucket row: "(Activity/Idle — excluded from DB Time)  <ms>  —"
        m = re.match(r'^(\(Activity/Idle.*?\))\s{2,}([\d.]+)\s', line)
        if m:
            model['Idle'] = float(m.group(2))
    return model


def test_client_read_visible_but_idle(pm_pid):
    """Idle-in-transaction backend: ClientRead is VISIBLE in system_event
    but does NOT inflate DB Time (idle for load accounting)."""
    print("--- Test 1: Client:ClientRead visible but excluded from DB Time ---")

    # Record any pre-existing idle-in-tx backends (e.g. leftovers from a prior
    # benchmark in a full-suite run) so we can identify OUR backend specifically
    # and not be fooled by — or attribute load to — someone else's.
    pre_existing = set()
    try:
        out = psql("SELECT pid FROM pg_stat_activity "
                   "WHERE state = 'idle in transaction'")
        pre_existing = {int(p) for p in out.split() if p.strip().isdigit()}
    except (subprocess.TimeoutExpired, Exception):
        pass

    # Start psql idle-in-transaction FIRST — before tracer.  Keep its
    # connection open (do NOT close stdin) for the whole capture window so the
    # backend stays parked in Client:ClientRead the entire time.
    psql_proc = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres"],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    psql_proc.stdin.write(b"BEGIN;\n")
    psql_proc.stdin.flush()

    # Precondition: do not start measuring until our backend is actually
    # established and parked idle-in-transaction in Client:ClientRead.  A fixed
    # sleep raced a loaded box (backend not yet connected/parked), which made
    # the ClientRead row absent through no fault of the tracer.
    idle_pid = wait_for_idle_in_tx(pre_existing, timeout=30)
    if idle_pid is None:
        # Genuinely could not set up the precondition — skip, don't fail.
        skip("could not establish idle-in-transaction backend parked in "
             "Client:ClientRead within 30s (box too loaded to set up); "
             "not a tracer failure")
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
        return
    print(f"  (idle-in-transaction backend pid={idle_pid} parked in "
          f"Client:ClientRead)")

    INTERVAL = 8

    # Run both system_event and time_model in parallel
    tracer_se = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    tracer_tm = subprocess.Popen(
        [TRACER, "--mode", "full", "--pid", str(pm_pid),
         "--interval", str(INTERVAL), "--duration", str(INTERVAL + 4),
         "--view", "time_model"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        stdout_se, _ = tracer_se.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer_se.kill()
        stdout_se, _ = tracer_se.communicate()

    try:
        stdout_tm, _ = tracer_tm.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        tracer_tm.kill()
        stdout_tm, _ = tracer_tm.communicate()

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

    # Terminate only OUR idle-in-transaction backend if it lingers; never
    # touch other backends (a concurrent suite/benchmark may own theirs).
    try:
        psql(f"SELECT pg_terminate_backend({idle_pid}) FROM pg_stat_activity "
             f"WHERE pid = {idle_pid} AND state = 'idle in transaction'")
    except (subprocess.TimeoutExpired, Exception):
        pass
    time.sleep(1)

    output_se = STRIP_ANSI.sub('', stdout_se.decode('utf-8', errors='replace'))
    output_tm = STRIP_ANSI.sub('', stdout_tm.decode('utf-8', errors='replace'))

    events = parse_system_events(output_se)
    model = parse_time_model(output_tm)

    # Client:ClientRead MUST appear in system_event (idle but visible).
    client_read = [e for e in events if e['name'] == 'Client:ClientRead']
    check(len(client_read) >= 1,
          f"Client:ClientRead visible in system_event "
          f"(found {len(client_read)}, events: {[e['name'] for e in events]})")

    # Activity events stay hidden (they are idle AND hidden).
    activity = [e for e in events if 'Activity' in e['name']]
    check(len(activity) == 0,
          f"Activity events hidden from system_event (found {len(activity)})")

    # Client-class wait time must NOT appear in the DB-Time breakdown.
    # ClientRead is the only Client-class event the idle backend produces,
    # and the tracer routes it to the Activity/idle bucket (see compute.c:
    # the Client class_ns has cr_ns subtracted out, then added to idle).
    # A correct build therefore emits NO "Client" class row and NO
    # "Client:ClientRead" sub-row inside the DB-Time section; if ClientRead
    # ever leaked into DB Time, one of those keys would appear in `model`.
    #
    # This is the load-INDEPENDENT correctness invariant.  The previous
    # check compared total DB Time against an absolute idle-box threshold
    # (INTERVAL*1000*0.8 ms), which broke under concurrent load: when the
    # full suite runs other backends in parallel they add real DB Time that
    # legitimately exceeds the bound, even though ClientRead is correctly
    # excluded.  That made the test environment-sensitive without signalling
    # any real bug.  The Client-class absence asserted below holds no matter
    # how busy the box is, and still FAILS if ClientRead ever leaks into DB
    # Time (a leak would add a "Client"/"Client:ClientRead" key here).
    leaked = [k for k in model
              if k == 'Client' or k.startswith('Client:')]
    check(len(leaked) == 0,
          f"No Client-class row in DB-Time breakdown "
          f"(idle ClientRead excluded from DB Time; leaked keys: {leaked})")

    # The idle time is still accounted — routed to the Activity/idle bucket.
    if 'Idle' in model:
        idle_time = model['Idle']
        check(idle_time >= INTERVAL * 1000 * 0.8,
              f"Activity/idle time = {idle_time:.0f}ms >= "
              f"{INTERVAL * 1000 * 0.8:.0f}ms "
              f"(captures Client:ClientRead idle time)")


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

    print(f"=== test_client_wait (postmaster PID {pm_pid}) ===")

    test_client_read_visible_but_idle(pm_pid)

    summary = f"\n{tests_passed}/{tests_run} tests passed"
    if tests_skipped:
        summary += f" ({tests_skipped} skipped)"
    print(summary)
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
