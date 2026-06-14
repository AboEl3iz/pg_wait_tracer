#!/usr/bin/env python3
"""test_data_idle.py — 0B.7: idle exclusion.

Verifies that Activity (idle) events are excluded from DB Time, AAS,
top_events, top_sessions, and top_queries.

Client:ClientRead is treated as IDLE for load accounting (see the
load-vs-visibility split in src/wait_event.c): like Oracle's "SQL*Net
message from client", it is EXCLUDED from DB Time / AAS. But it is NOT
hidden — it must still appear in top_events and the timeline.
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
    PID 1001: Activity:Idle 50ms (idle + hidden — excluded everywhere)
    PID 1002: CPU 3ms (active)
    PID 1003: Client:ClientRead 20ms (idle — excluded from DB Time,
              but still VISIBLE in top_events/timeline)

    Expected DB Time = 5 + 3 = 8ms (Activity AND ClientRead excluded)
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
            t.check_approx(rows["DB Time"]["ms"], 8.0, 0.001,
                           "DB Time = 8ms (Activity AND ClientRead excluded)")

            # top_events must hide Activity events, but Client:ClientRead
            # stays VISIBLE (idle for load, not hidden from views)
            print("--- top_events hides Activity, keeps ClientRead ---")
            resp_ev = srv.query("top_events")
            idle_rows = [r for r in resp_ev.get("rows", [])
                         if "Activity" in r.get("name", "")
                         or "Idle" in r.get("name", "")]
            t.check(len(idle_rows) == 0,
                    f"No Activity events in top_events (found {len(idle_rows)})")
            client_rows = [r for r in resp_ev.get("rows", [])
                           if r.get("name", "") == "Client:ClientRead"]
            t.check(len(client_rows) == 1,
                    "Client:ClientRead present in top_events (visible)")

            # session_timeline must hide Activity bars but KEEP ClientRead
            print("--- timeline hides Activity, keeps ClientRead ---")
            resp_tl = srv.query("session_timeline")
            tl_events = resp_tl.get("events", [])
            idle_bars = [e for e in tl_events
                         if "Activity" in e.get("n", "")
                         or "Idle" in e.get("n", "")]
            t.check(len(idle_bars) == 0,
                    f"No Activity bars in timeline (found {len(idle_bars)})")
            client_bars = [e for e in tl_events
                           if "ClientRead" in e.get("n", "")]
            t.check(len(client_bars) >= 1,
                    f"Client:ClientRead bar present in timeline "
                    f"(found {len(client_bars)})")

    finally:
        cleanup_traces(trace_dir)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
