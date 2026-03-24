#!/usr/bin/env python3
"""bench_server.py — Measure pgwt-server compute throughput.

Generates trace files of varying sizes, then measures response time for
each server command. Reports latency and throughput.

Usage:
    python3 tests/bench_server.py [--events 100000,1000000,10000000]
"""
import json
import os
import subprocess
import sys
import tempfile
import time

PGWT_SERVER = os.path.join(os.path.dirname(__file__), '..', 'pgwt-server')
GEN_BENCH = os.path.join(os.path.dirname(__file__), 'gen_bench_traces')

COMMANDS = [
    {"cmd": "time_model", "from_ns": 0, "to_ns": 999999999999999},
    {"cmd": "top_events", "from_ns": 0, "to_ns": 999999999999999},
    {"cmd": "top_sessions", "from_ns": 0, "to_ns": 999999999999999},
    {"cmd": "top_queries", "from_ns": 0, "to_ns": 999999999999999},
    {"cmd": "aas", "from_ns": 0, "to_ns": 999999999999999, "num_buckets": 60},
]


def run_server_bench(trace_dir, num_events):
    """Start pgwt-server, send commands, measure latency."""
    proc = subprocess.Popen(
        [PGWT_SERVER, trace_dir],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    results = []
    for cmd in COMMANDS:
        req = json.dumps(cmd)

        t0 = time.monotonic()
        proc.stdin.write(req + '\n')
        proc.stdin.flush()
        resp = proc.stdout.readline()
        t1 = time.monotonic()

        latency_ms = (t1 - t0) * 1000
        results.append((cmd['cmd'], latency_ms, len(resp)))

    proc.stdin.close()
    proc.wait()
    return results


def main():
    event_sizes = [100_000, 1_000_000]
    if len(sys.argv) > 1 and sys.argv[1].startswith('--events'):
        sizes_str = sys.argv[1].split('=')[1] if '=' in sys.argv[1] else sys.argv[2]
        event_sizes = [int(x) for x in sizes_str.split(',')]

    if not os.path.exists(GEN_BENCH):
        print(f"ERROR: {GEN_BENCH} not found. Run: make -C tests", file=sys.stderr)
        sys.exit(1)

    print(f"{'Events':>12} {'Command':>15} {'Latency (ms)':>14} {'Response KB':>12} {'Events/sec':>12}")
    print(f"{'─'*12} {'─'*15} {'─'*14} {'─'*12} {'─'*12}")

    for n in event_sizes:
        with tempfile.TemporaryDirectory() as tmpdir:
            # Generate trace files
            subprocess.run(
                [GEN_BENCH, '-o', tmpdir, '-n', str(n), '-p', '8'],
                capture_output=True, check=True,
            )

            results = run_server_bench(tmpdir, n)
            for cmd, latency_ms, resp_bytes in results:
                throughput = n / (latency_ms / 1000) if latency_ms > 0 else 0
                print(f"{n:>12,} {cmd:>15} {latency_ms:>13.1f} {resp_bytes/1024:>11.1f} {throughput:>12,.0f}")
            print()


if __name__ == '__main__':
    main()
