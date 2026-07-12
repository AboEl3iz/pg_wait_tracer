#!/usr/bin/env python3
"""test_uprobe_fired.py — T2 item 0: the uprobes must actually FIRE.

The defect class this guards (T2 io_worker study, defect 1): uprobe file
offsets were computed as `va - 0x400000` (a non-PIE assumption). On PIE
builds (PGDG Ubuntu/EL9) the attach SUCCEEDED but the probe sat on a dead
byte and never fired — bpftool showed run_cnt = 0 for on_report_query_id
while USDT probes ran 61,869x in the same trace. Attribution was silently
zero; nothing went red. This test makes that class un-shippable:

  - kernel.bpf_stats_enabled=1 so the kernel counts per-program runs;
  - the tracer runs in --mode tiered (de-escalated: USDT probes are NOT
    attached, so the uprobes are the ONLY query/activity path);
  - a handful of psql statements execute (each one calls
    pgstat_report_activity and, on PG14+, pgstat_report_query_id);
  - `bpftool prog show` must report run_cnt > 0 for:
      * the query-id uprobe (on_report_query_id, or on_executor_start on
        PG13's pg_stat_statements route), and
      * the T2 command-open gate uprobe (on_report_activity).

Usage: sudo python3 tests/test_uprobe_fired.py [--pid PM_PID]
           [--pg-version N] [--bpftool PATH]
"""
import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACER = os.path.join(PROJECT_DIR, "pg_wait_tracer")

tests_run = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_failed
    tests_run += 1
    if cond:
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


def psql(sql):
    return subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=15).stdout.strip()


def prog_run_counts(bpftool):
    """Parse `bpftool prog show`: entries are contiguous lines (no blank
    separators) — a header line `<id>: <type>  name <name>  tag ...` with
    `run_time_ns X run_cnt Y` appended when bpf_stats is enabled, followed
    by indented detail lines. Parse statefully per line. BPF object names
    are truncated to 15 chars by the kernel, so callers match prefixes.
    Returns {name: run_cnt}."""
    out = subprocess.run([bpftool, "prog", "show"],
                         capture_output=True, text=True, timeout=15).stdout
    counts = {}
    current = None
    for line in out.splitlines():
        if re.match(r'^\d+:', line):
            m = re.search(r'\bname\s+(\S+)', line)
            current = m.group(1) if m else None
            if current is not None:
                counts.setdefault(current, 0)
        if current is None:
            continue
        r = re.search(r'\brun_cnt\s+(\d+)', line)
        if r:
            counts[current] = max(counts[current], int(r.group(1)))
    return counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int)
    parser.add_argument('--pg-version', type=int, default=0)
    parser.add_argument('--bpftool', default=os.environ.get('BPFTOOL',
                                                            'bpftool'))
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("ERROR: must run as root (sudo)")
        sys.exit(1)
    bpf_ok = subprocess.run([args.bpftool, "version"],
                            capture_output=True).returncode == 0
    if not bpf_ok:
        print(f"ERROR: bpftool not usable ({args.bpftool}) — pass --bpftool "
              "or set BPFTOOL; this assertion must not silently skip")
        sys.exit(1)

    pm_pid = args.pid or find_postmaster()
    if not pm_pid:
        print("ERROR: cannot find postmaster")
        sys.exit(1)

    print(f"=== test_uprobe_fired (postmaster {pm_pid}, "
          f"pg-version {args.pg_version or 'auto'}) ===")

    stats_path = "/proc/sys/kernel/bpf_stats_enabled"
    with open(stats_path) as f:
        prev_stats = f.read().strip()
    with open(stats_path, "w") as f:
        f.write("1\n")

    tracer = subprocess.Popen(
        [TRACER, "--mode", "tiered", "--pid", str(pm_pid),
         "--duration", "15", "--quiet", "--interval", "5"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        time.sleep(3.0)   # BPF load + attach + initial scan
        # Each statement drives pgstat_report_activity (RUNNING then IDLE)
        # and the per-version query-id path.
        for i in range(10):
            psql(f"SELECT {i} + count(*) FROM generate_series(1, 1000)")
        time.sleep(1.0)

        # Read run counts WHILE the tracer still holds the programs.
        check(tracer.poll() is None,
              "tracer still running when run counts are read")
        counts = prog_run_counts(args.bpftool)
        check(any(k.startswith("on_") for k in counts),
              f"tracer's BPF programs visible to bpftool "
              f"(saw {sorted(counts)[:12]})")
    finally:
        try:
            tracer.wait(timeout=15)
        except subprocess.TimeoutExpired:
            tracer.kill()
        with open(stats_path, "w") as f:
            f.write(prev_stats + "\n")

    err = tracer.stderr.read().decode('utf-8', errors='replace')

    def cnt(prefix):
        return sum(v for k, v in counts.items() if k.startswith(prefix))

    # Command-open gate uprobe (T2 item 1) — every PG version.
    activity = cnt("on_report_activ")
    check(activity > 0,
          f"on_report_activity uprobe FIRED (run_cnt={activity}) "
          f"[T2 gate probe; dead-offset class]"
          + ("" if activity else
             f" (progs seen: {sorted(counts)}; stderr tail: {err[-300:]!r})"))

    # Query-id uprobe — version-dependent symbol (PG13: pgss route).
    if args.pg_version == 13:
        qid = cnt("on_executor_sta")
        check(qid > 0,
              f"on_executor_start uprobe FIRED on PG13 (run_cnt={qid}) "
              f"[study defect 1: run_cnt was 0 on PIE builds]")
    else:
        qid = cnt("on_report_query")
        check(qid > 0,
              f"on_report_query_id uprobe FIRED (run_cnt={qid}) "
              f"[study defect 1: run_cnt was 0 on PIE builds]")

    print(f"\n{tests_run - tests_failed}/{tests_run} checks passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
