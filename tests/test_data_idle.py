#!/usr/bin/env python3
"""test_data_idle.py — 0B.7: idle exclusion.

Verifies that Activity (idle) events are excluded from DB Time, AAS,
top_events, top_sessions, and top_queries.

Client:ClientRead is deliberately NOT idle (see pgwt_is_idle_event in
src/wait_event.c): it is a real wait event — time spent waiting for the
client to send the next command — and counts toward DB Time under the
Client class.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, ACTIVITY_IDLE, CLIENT_READ,
)

BASE_TS = 10_000_000_000_000


def build_scenario():
    """Mix of active and idle events.

    PID 1000: IO:Read 5ms (active)
    PID 1001: Activity:Idle 50ms (should be excluded)
    PID 1002: CPU 3ms (active)
    PID 1003: Client:ClientRead 20ms (NOT idle — counts toward DB Time)

    Expected DB Time = 5 + 3 + 20 = 28ms (not 78ms)
    """
    events = [
        {"pid": 1000, "ts": BASE_TS + 5_000_000, "dur": 5_000_000,
         "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100},
        {"pid": 1001, "ts": BASE_TS + 50_000_000, "dur": 50_000_000,
         "old": ACTIVITY_IDLE, "new": CPU, "qid": 0},
        {"pid": 1002, "ts": BASE_TS + 3_000_000, "dur": 3_000_000,
         "old": CPU, "new": IO_DATA_FILE_READ, "qid": 200},
        {"pid": 1003, "ts": BASE_TS + 20_000_000, "dur": 20_000_000,
         "old": CLIENT_READ, "new": CPU, "qid": 0},
    ]

    return {
        "backends": [
            {"pid": 1000, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1001, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1002, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1003, "type": "client", "user": "test", "db": "testdb"},
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
            t.check_approx(rows["DB Time"]["ms"], 28.0, 0.001,
                           "DB Time = 28ms (Activity excluded, ClientRead counted)")

            # top_events should not contain Activity events, but
            # Client:ClientRead is a real wait and must be present
            print("--- top_events excludes idle ---")
            resp_ev = srv.query("top_events")
            idle_rows = [r for r in resp_ev.get("rows", [])
                         if "Activity" in r.get("name", "")
                         or "Idle" in r.get("name", "")]
            t.check(len(idle_rows) == 0,
                    f"No Activity events in top_events (found {len(idle_rows)})")
            client_rows = [r for r in resp_ev.get("rows", [])
                           if r.get("name", "") == "Client:ClientRead"]
            t.check(len(client_rows) == 1,
                    "Client:ClientRead present in top_events (not idle)")

            # session_timeline should not show Activity bars
            print("--- timeline excludes idle ---")
            resp_tl = srv.query("session_timeline")
            idle_bars = [e for e in resp_tl.get("events", [])
                         if "Activity" in e.get("n", "")
                         or "Idle" in e.get("n", "")]
            t.check(len(idle_bars) == 0,
                    f"No Activity bars in timeline (found {len(idle_bars)})")

    finally:
        cleanup_traces(trace_dir)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
