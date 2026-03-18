#!/usr/bin/env python3
"""test_data_timeline.py — 0B.6: timestamp correctness (Bug 1 regression).

Verifies that session_timeline events have correct start/end timestamps:
  start = timestamp_ns - duration_ns
  end = timestamp_ns
  Adjacent events for the same PID are contiguous (no gaps/overlaps).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, LOCK_RELATION,
)

BASE_TS = 10_000_000_000_000


def build_scenario():
    """3 contiguous events for PID 1000.

    Event 1: CPU     from BASE_TS to BASE_TS+2ms (dur=2ms)
    Event 2: IO:Read from BASE_TS+2ms to BASE_TS+7ms (dur=5ms)
    Event 3: Lock    from BASE_TS+7ms to BASE_TS+10ms (dur=3ms)
    """
    events = [
        {"pid": 1000, "ts": BASE_TS + 2_000_000, "dur": 2_000_000,
         "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100},
        {"pid": 1000, "ts": BASE_TS + 7_000_000, "dur": 5_000_000,
         "old": IO_DATA_FILE_READ, "new": LOCK_RELATION, "qid": 100},
        {"pid": 1000, "ts": BASE_TS + 10_000_000, "dur": 3_000_000,
         "old": LOCK_RELATION, "new": CPU, "qid": 100},
    ]

    return {
        "backends": [{"pid": 1000, "type": "client", "user": "test", "db": "testdb"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": events,
    }


def main():
    t = TestRunner("test_data_timeline")
    print(f"=== {t.name} ===")

    scenario = build_scenario()
    trace_dir = generate_traces(scenario)

    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("session_timeline")
            evts = resp.get("events", [])

            print("--- Event count ---")
            t.check_eq(len(evts), 3, "3 timeline events")

            if len(evts) >= 3:
                print("--- Event 1: CPU 2ms ---")
                e1 = evts[0]
                expected_start = BASE_TS
                t.check_eq(e1["s"], expected_start,
                           f"Event 1 start = {expected_start}")
                t.check_eq(e1["d"], 2_000_000, "Event 1 dur = 2ms")
                t.check(e1["s"] + e1["d"] == BASE_TS + 2_000_000,
                        "Event 1 end = BASE_TS + 2ms")

                print("--- Event 2: IO:Read 5ms ---")
                e2 = evts[1]
                t.check_eq(e2["s"], BASE_TS + 2_000_000,
                           "Event 2 start = Event 1 end (contiguous)")
                t.check_eq(e2["d"], 5_000_000, "Event 2 dur = 5ms")

                print("--- Event 3: Lock 3ms ---")
                e3 = evts[2]
                t.check_eq(e3["s"], BASE_TS + 7_000_000,
                           "Event 3 start = Event 2 end (contiguous)")
                t.check_eq(e3["d"], 3_000_000, "Event 3 dur = 3ms")

                print("--- Contiguity ---")
                for i in range(len(evts) - 1):
                    a = evts[i]
                    b = evts[i + 1]
                    if a["p"] == b["p"]:
                        t.check_eq(a["s"] + a["d"], b["s"],
                                   f"Events {i} → {i+1} contiguous")

    finally:
        cleanup_traces(trace_dir)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
