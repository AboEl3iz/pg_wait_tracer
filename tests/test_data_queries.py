#!/usr/bin/env python3
"""test_data_queries.py — Per-query attribution + filtered class_ns (Bug 4 regression).

Verifies that top_queries correctly attributes time per query_id,
and that class filters only report the filtered class in class breakdown.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ, IO_WAL_SYNC, LOCK_RELATION,
)

BASE_TS = 10_000_000_000_000


def build_scenario():
    """2 queries with known wait profiles.

    Query 100: CPU 2ms + IO:Read 8ms = 10ms total
    Query 200: CPU 1ms + Lock 4ms + IO:WalSync 5ms = 10ms total
    """
    events = []
    ts = BASE_TS

    # Query 100: CPU 2ms
    ts += 2_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 2_000_000,
                    "old": CPU, "new": IO_DATA_FILE_READ, "qid": 100})
    # Query 100: IO 8ms
    ts += 8_000_000
    events.append({"pid": 1000, "ts": ts, "dur": 8_000_000,
                    "old": IO_DATA_FILE_READ, "new": CPU, "qid": 100})

    # Query 200: CPU 1ms
    ts2 = BASE_TS + 500_000
    ts2 += 1_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 1_000_000,
                    "old": CPU, "new": LOCK_RELATION, "qid": 200})
    # Query 200: Lock 4ms
    ts2 += 4_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 4_000_000,
                    "old": LOCK_RELATION, "new": IO_WAL_SYNC, "qid": 200})
    # Query 200: IO:WalSync 5ms
    ts2 += 5_000_000
    events.append({"pid": 1001, "ts": ts2, "dur": 5_000_000,
                    "old": IO_WAL_SYNC, "new": CPU, "qid": 200})

    return {
        "backends": [
            {"pid": 1000, "type": "client", "user": "test", "db": "testdb"},
            {"pid": 1001, "type": "client", "user": "test", "db": "testdb"},
        ],
        "queries": [
            {"id": 100, "text": "SELECT pg_sleep(0.01)"},
            {"id": 200, "text": "UPDATE t SET x = 1"},
        ],
        "events": events,
    }


def main():
    t = TestRunner("test_data_queries")
    print(f"=== {t.name} ===")

    scenario = build_scenario()
    trace_dir = generate_traces(scenario)

    try:
        with ServerHarness(trace_dir) as srv:
            # Unfiltered query list
            resp = srv.query("top_queries")
            rows = resp.get("rows", [])
            by_qid = {str(r["query_id"]): r for r in rows}

            print("--- Unfiltered ---")
            q100 = by_qid.get("100", {})
            t.check_approx(q100.get("total_ms", -1), 10.0, 0.001,
                           "Query 100 total = 10ms")

            q200 = by_qid.get("200", {})
            t.check_approx(q200.get("total_ms", -1), 10.0, 0.001,
                           "Query 200 total = 10ms")

            # Bug 4 regression: filter by IO class
            print("--- Filtered by IO class ---")
            resp_io = srv.query("top_queries", filters={"class": "io"})
            rows_io = resp_io.get("rows", [])
            by_qid_io = {str(r["query_id"]): r for r in rows_io}

            q100_io = by_qid_io.get("100", {})
            t.check_approx(q100_io.get("total_ms", -1), 8.0, 0.001,
                           "Query 100 IO-only = 8ms")

            q200_io = by_qid_io.get("200", {})
            t.check_approx(q200_io.get("total_ms", -1), 5.0, 0.001,
                           "Query 200 IO-only = 5ms")

            # Verify filtered by Lock class
            print("--- Filtered by Lock class ---")
            resp_lock = srv.query("top_queries", filters={"class": "lock"})
            rows_lock = resp_lock.get("rows", [])
            by_qid_lock = {str(r["query_id"]): r for r in rows_lock}

            # Query 100 has no Lock events — should not appear
            t.check("100" not in by_qid_lock,
                    "Query 100 absent when filtered by Lock")

            q200_lock = by_qid_lock.get("200", {})
            t.check_approx(q200_lock.get("total_ms", -1), 4.0, 0.001,
                           "Query 200 Lock-only = 4ms")

    finally:
        cleanup_traces(trace_dir)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
