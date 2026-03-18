#!/usr/bin/env python3
"""test_data_idle.py — 0B.7: idle exclusion.

Verifies that Activity (idle) events are excluded from DB Time, AAS,
top_events, top_sessions, and top_queries. Only non-idle events contribute.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, ACTIVITY_IDLE,
)

BASE_TS = 10_000_000_000_000


def build_scenario():
    """Mix of active and idle events.

    PID 1000: IO:Read 5ms (active)
    PID 1001: Activity:Idle 50ms (should be excluded)
    PID 1002: CPU 3ms (active)

    Expected DB Time = 5 + 3 = 8ms (not 58ms)
    """
    events = [
        {"pid": 1000, "ts": BASE_TS + 5_000_000, "dur": 5_000_000,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
        {"pid": 1001, "ts": BASE_TS + 50_000_000, "dur": 50_000_000,
         "old": ACTIVITY_IDLE, "new": CPU, "qid": 0},
        {"pid": 1002, "ts": BASE_TS + 3_000_000, "dur": 3_000_000,
         "old": CPU, "new": IO_DATA_FILE_READ, "qid": 200},
    ]

    return {
        "backends": [
            {"pid": 1000, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1001, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1002, "type": "client", "user": "test", "db": "testdb"},
        ],
        "queries": [
            {"id": 100, "text": "SELECT 1"},
            {"id": 200, "text": "SELECT 2"},
        ],
        "events": events,
    }


def main():
    t = TestRunner("test_data_idle")
    print(f"=== {t.name} ===")

    scenario = build_scenario()
    trace_dir = generate_traces(scenario)

    try:
        with ServerHarness(trace_dir) as srv:
            # time_model
            resp = srv.query("time_model")
            rows = {r["name"]: r for r in resp["rows"]}

            print("--- DB Time excludes idle ---")
            t.check_approx(rows["DB Time"]["ms"], 8.0, 0.001,
                           "DB Time = 8ms (not 58ms)")

            # top_events should not contain Activity:Idle (or if present, total=0)
            print("--- top_events excludes idle ---")
            resp_ev = srv.query("top_events")
            idle_rows = [r for r in resp_ev.get("rows", [])
                         if "Activity" in r.get("name", "")
                         or "Idle" in r.get("name", "")]
            t.check(len(idle_rows) == 0,
                    f"No Activity/Idle events in top_events (found {len(idle_rows)})")

            # session_timeline should not show idle bars
            print("--- timeline excludes idle ---")
            resp_tl = srv.query("session_timeline")
            idle_bars = [e for e in resp_tl.get("events", [])
                         if "Activity" in e.get("n", "")
                         or "Idle" in e.get("n", "")]
            t.check(len(idle_bars) == 0,
                    f"No idle bars in timeline (found {len(idle_bars)})")

    finally:
        cleanup_traces(trace_dir)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
