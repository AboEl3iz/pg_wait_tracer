#!/usr/bin/env python3
"""test_data_sessions.py — Per-session attribution.

Verifies that top_sessions correctly attributes time to each PID.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LOCK_RELATION, ACTIVITY_IDLE,
)

BASE_TS = 10_000_000_000_000


def build_scenario():
    """3 PIDs with known DB Time.

    PID 1000: CPU 3ms + IO 7ms = 10ms DB Time
    PID 1001: Lock 5ms = 5ms DB Time
    PID 1002: Activity(idle) 10ms = 0 DB Time (excluded)
    """
    events = []
    ts = BASE_TS

    # PID 1000
    ts += 3_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 3_000_000,
                    "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100})
    ts += 7_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 7_000_000,
                    "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})

    # PID 1001
    ts2 = BASE_TS + 500_000
    ts2 += 5_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 5_000_000,
                    "old": LOCK_RELATION, "new": CPU, "qid": 200})

    # PID 1002 (idle — excluded)
    ts3 = BASE_TS + 200_000
    ts3 += 10_000_000
    events.append({"pid": 1002, "ts": ts3, "dur": 10_000_000,
                    "old": ACTIVITY_IDLE, "new": CPU, "qid": 0})

    return {
        "backends": [
            {"pid": 1000, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1001, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1002, "type": "bgwriter"},
        ],
        "queries": [
            {"id": 100, "text": "SELECT 1"},
            {"id": 200, "text": "SELECT 2"},
        ],
        "events": events,
    }


def main():
    t = TestRunner("test_data_sessions")
    print(f"=== {t.name} ===")

    scenario = build_scenario()
    trace_dir = generate_traces(scenario)

    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("top_sessions")
            rows = resp.get("rows", [])
            by_pid = {r["pid"]: r for r in rows}

            print("--- PID 1000: 10ms DB Time ---")
            s1 = by_pid.get(1000, {})
            t.check_approx(s1.get("db_time_ms", -1), 10.0, 0.001,
                           "PID 1000 DB Time = 10ms")

            print("--- PID 1001: 5ms DB Time ---")
            s2 = by_pid.get(1001, {})
            t.check_approx(s2.get("db_time_ms", -1), 5.0, 0.001,
                           "PID 1001 DB Time = 5ms")

            print("--- PID 1002: idle, excluded ---")
            s3 = by_pid.get(1002, {})
            # Idle backend may or may not appear; if it does, DB Time should be 0
            if s3:
                t.check_approx(s3.get("db_time_ms", 0), 0.0, 0.001,
                               "PID 1002 DB Time = 0ms (idle)")
            else:
                t.check(True, "PID 1002 not in output (idle excluded)")

            print("--- Ordering ---")
            if len(rows) >= 2:
                t.check(rows[0]["db_time_ms"] >= rows[1]["db_time_ms"],
                        "Sessions sorted by DB Time descending")

    finally:
        cleanup_traces(trace_dir)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
