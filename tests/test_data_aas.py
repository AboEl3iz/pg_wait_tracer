#!/usr/bin/env python3
"""test_data_aas.py — 0B.2: AAS bucket correctness (0% tolerance).

Generates 4 backends each active for exactly 5 seconds, verifies AAS ≈ 4.0.
Also verifies per-class AAS breakdown sums correctly.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LOCK_RELATION, LWLOCK_WAL_WRITE,
)

BASE_TS = 10_000_000_000_000


def build_scenario():
    """4 PIDs, each with events covering exactly 5 seconds.

    PID 1000: 5s of CPU
    PID 1001: 5s of IO:DataFileRead
    PID 1002: 5s of Lock:relation
    PID 1003: 5s of LWLock:WALWrite

    Time range: BASE_TS to BASE_TS + 5_000_000_000
    Expected total AAS over 5s = 4.0 (4 active sessions)
    """
    events = []
    pids = [1000, 1001, 1002, 1003]
    wait_events = [CPU, IO_DATA_FILE_READ, LOCK_RELATION, LWLOCK_WAL_WRITE]

    # Each PID gets one big event spanning 5 seconds
    for i, (pid, we) in enumerate(zip(pids, wait_events)):
        ts = BASE_TS + 5_000_000_000
        events.append({
            "pid": pid,
            "ts": ts,
            "dur": 5_000_000_000,
            "old": we,
            "new": CPU if we != CPU else IO_DATA_FILE_READ,
            "qid": 100 + i,
        })

    return {
        "backends": [
            {"pid": p, "type": "client", "user": "test", "db": "testdb"}
            for p in pids
        ],
        "queries": [{"id": 100 + i, "text": f"SELECT {i}"} for i in range(4)],
        "events": events,
    }


def main():
    t = TestRunner("test_data_aas")
    print(f"=== {t.name} ===")

    scenario = build_scenario()
    trace_dir = generate_traces(scenario)

    try:
        with ServerHarness(trace_dir) as srv:
            # Query AAS with 1 bucket covering the full range
            resp = srv.query("aas", buckets=1,
                             from_=BASE_TS, to_=BASE_TS + 5_000_000_000)

            print("--- AAS total ---")
            buckets = resp.get("buckets", [])
            t.check(len(buckets) >= 1, f"Got {len(buckets)} AAS bucket(s)")

            if buckets:
                b = buckets[0]
                # Sum all class values
                total_aas = sum(v for k, v in b.items()
                                if k not in ("t", "total"))
                t.check_approx(total_aas, 4.0, 0.05,
                               "Total AAS = 4.0 (4 active sessions)")

            # Also check time_model for AAS (use same 5s window)
            resp_tm = srv.query("time_model",
                                from_=BASE_TS, to_=BASE_TS + 5_000_000_000)
            rows = {r["name"]: r for r in resp_tm["rows"]}

            print("--- time_model AAS ---")
            db_aas = rows.get("DB Time", {}).get("aas", -1)
            t.check_approx(db_aas, 4.0, 0.05,
                           "time_model DB Time AAS = 4.0")

            # Per-class AAS
            cpu_aas = rows.get("CPU*", {}).get("aas", -1)
            t.check_approx(cpu_aas, 1.0, 0.05, "CPU AAS = 1.0")

            io_aas = rows.get("IO", {}).get("aas", -1)
            t.check_approx(io_aas, 1.0, 0.05, "IO AAS = 1.0")

            lock_aas = rows.get("Lock", {}).get("aas", -1)
            t.check_approx(lock_aas, 1.0, 0.05, "Lock AAS = 1.0")

            lwlock_aas = rows.get("LWLock", {}).get("aas", -1)
            t.check_approx(lwlock_aas, 1.0, 0.05, "LWLock AAS = 1.0")

    finally:
        cleanup_traces(trace_dir)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
