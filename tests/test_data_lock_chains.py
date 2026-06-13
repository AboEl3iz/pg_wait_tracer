#!/usr/bin/env python3
"""test_data_lock_chains.py — Verify lock chain detection and interference scoring."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU,
)

LOCK_TXN = 0x03000008   # Lock:transactionid
LOCK_TUPLE = 0x03000009 # Lock:tuple
IO_READ = 0x0A000015    # IO:DataFileRead

# Scenario: PID 1000 holds lock (on CPU), PID 1001 waits on Lock:transactionid
# PID 1002 and 1003 both wait on IO:DataFileRead simultaneously (interference)
BASE = 10000000000000

SCENARIO = {
    "backends": [
        {"pid": 1000, "type": "client", "user": "u", "db": "d"},
        {"pid": 1001, "type": "client", "user": "u", "db": "d"},
        {"pid": 1002, "type": "client", "user": "u", "db": "d"},
        {"pid": 1003, "type": "client", "user": "u", "db": "d"},
    ],
    "queries": [{"id": 100, "text": "SELECT 1"}],
    "events": [
        # PID 1000 on CPU from BASE to BASE+10ms (blocker)
        {"pid": 1000, "ts": BASE + 10000000, "dur": 10000000, "old": CPU, "new": LOCK_TXN, "qid": 100},
        # PID 1000 continues on CPU from BASE+10ms to BASE+20ms
        {"pid": 1000, "ts": BASE + 20000000, "dur": 10000000, "old": CPU, "new": IO_READ, "qid": 100},

        # PID 1001 waiting on Lock:transactionid from BASE+1ms to BASE+8ms (7ms wait)
        {"pid": 1001, "ts": BASE + 8000000, "dur": 7000000, "old": LOCK_TXN, "new": CPU, "qid": 100},
        # PID 1001 then on CPU
        {"pid": 1001, "ts": BASE + 10000000, "dur": 2000000, "old": CPU, "new": IO_READ, "qid": 100},

        # PID 1002 waiting on IO:DataFileRead from BASE to BASE+5ms
        {"pid": 1002, "ts": BASE + 5000000, "dur": 5000000, "old": IO_READ, "new": CPU, "qid": 100},
        # PID 1002 then CPU
        {"pid": 1002, "ts": BASE + 7000000, "dur": 2000000, "old": CPU, "new": IO_READ, "qid": 100},

        # PID 1003 waiting on IO:DataFileRead from BASE+2ms to BASE+6ms (overlaps with 1002)
        {"pid": 1003, "ts": BASE + 6000000, "dur": 4000000, "old": IO_READ, "new": CPU, "qid": 100},
        # PID 1003 then CPU
        {"pid": 1003, "ts": BASE + 8000000, "dur": 2000000, "old": CPU, "new": IO_READ, "qid": 100},
    ],
}

t = TestRunner("test_data_lock_chains")
trace_dir = generate_traces(SCENARIO)

try:
    with ServerHarness(trace_dir) as srv:
        # Test lock_chains endpoint
        data = srv.query("lock_chains")
        chains = data.get("chains", [])

        t.check(len(chains) >= 1,
                f"at least 1 lock chain detected (got {len(chains)})")

        # The Lock wait by PID 1001 should identify PID 1000 as blocker
        txn_chain = None
        for c in chains:
            if c["waiter"] == 1001:
                txn_chain = c
                break

        t.check(txn_chain is not None,
                "PID 1001 lock chain found")
        if txn_chain:
            t.check(txn_chain["blocker"] == 1000,
                    "blocker is PID 1000 (was on CPU)")
            t.check(abs(txn_chain["wait_ms"] - 7.0) < 0.1,
                    f"wait_ms ~ 7ms (got {txn_chain['wait_ms']:.1f})")

        # Test interference endpoint
        data = srv.query("interference")
        rows = data.get("rows", [])

        t.check(len(rows) >= 1, "at least 1 interference pair")

        # PID 1002 ↔ 1003 should have high interference (both on IO:DataFileRead)
        pair = None
        for r in rows:
            pids = {r["pid_a"], r["pid_b"]}
            if pids == {1002, 1003}:
                pair = r
                break

        t.check(pair is not None, "PID 1002↔1003 interference detected")
        if pair:
            t.check("DataFileRead" in pair["top_event"],
                    "interference on IO:DataFileRead")
            t.check(pair["overlap_ms"] > 0, "overlap_ms > 0")
            t.check(pair["score"] > 0, "score > 0")

finally:
    cleanup_traces(trace_dir)

sys.exit(0 if t.summary() else 1)
