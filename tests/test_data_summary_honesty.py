#!/usr/bin/env python3
"""test_data_summary_honesty.py — T1/FID-2: the summary fast-path must be
coverage-aware and the fidelity label must reflect the actual data source.

Before the fix, should_use_summaries() selected the summary path on range
length alone (>= 120 s) and stamped "fidelity":"exact" — but summaries are
fed only by the exact (watchpoint) path. In the DEFAULT tiered mode a
"last 15 minutes" query therefore returned escalation slivers or nothing,
labeled exact: the worst possible violation of "never a silent empty
result" and of the standing rule that "last 15 min must mean NOW".

After the fix:
  * a >= 120 s window whose sampled data is not fully covered by exact data
    falls back to the raw (merge) path → correct ASH-estimated values,
    honest "sampled"/"mixed" label;
  * a >= 120 s window fully covered by exact data still uses the summary
    fast path and is honestly labeled "exact";
  * a window with no data at all is labeled "none" — never "exact".
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ,
)

BASE = 10_000_000_000_000
S = 1_000_000_000
PERIOD = 1 * S      # 1 Hz sampling keeps the sample count small


def build_tiered_15min():
    """Default-tiered 'last 15 minutes': sampled data only (no escalation
    ever fired). 900 IO samples at 1 Hz = 900 s of estimated IO time."""
    samples = []
    ts = BASE
    for _ in range(900):
        ts += PERIOD
        samples.append({"pid": 1000, "ts": ts, "event": IO_DATA_FILE_READ,
                        "qid": 100})
    return {
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "sample_period_ns": PERIOD,
        "samples": samples,
    }, BASE, ts


def test_tiered_15min(t):
    print("\n### 1. FID-2: 15-min tiered window returns the sampled data ###")
    scenario, lo, hi = build_tiered_15min()
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            frm, to = lo - 10 * S, hi + 10 * S   # ~920 s >= 120 s threshold
            expected_ms = 900 * (PERIOD / 1e6)   # 900 samples * 1 s

            tm = srv.query("time_model", from_=frm, to_=to)
            rows = {r["name"]: r for r in tm["rows"]}
            t.check_eq(tm.get("fidelity"), "sampled",
                       "time_model over sampled-only 15 min: fidelity=sampled")
            t.check_approx(rows.get("DB Time", {}).get("ms", -1),
                           expected_ms, 0.001,
                           f"DB Time = {expected_ms:.0f}ms (never silent-empty)")

            ev = srv.query("top_events", from_=frm, to_=to)
            t.check_eq(ev.get("fidelity"), "sampled",
                       "top_events fidelity=sampled")
            evrows = {r["name"]: r for r in ev["rows"]}
            t.check_approx(evrows.get("IO:DataFileRead", {}).get("total_ms", -1),
                           expected_ms, 0.001,
                           "top_events IO total from samples")
            # FID-3: latency columns are fabrications over sampled data —
            # they must be gated (null), not presented like measurements.
            io = evrows.get("IO:DataFileRead", {})
            for col in ("avg_us", "p50_us", "p95_us", "p99_us", "max_us"):
                t.check(io.get(col, "missing") is None,
                        f"sampled row gates latency column {col} (null)")

            qr = srv.query("top_queries", from_=frm, to_=to)
            t.check_eq(qr.get("fidelity"), "sampled",
                       "top_queries fidelity=sampled")
            qrows = {r["query_id"]: r for r in qr["rows"]}
            t.check_approx(qrows.get("100", {}).get("total_ms", -1),
                           expected_ms, 0.001, "top_queries qid 100 total")

            aas = srv.query("aas", from_=frm, to_=to, buckets=10)
            t.check_eq(aas.get("fidelity"), "sampled", "aas fidelity=sampled")
            active_ms = sum(
                v * aas["bucket_ns"] / 1e6
                for b in aas["buckets"]
                for k, v in b.items()
                if k != "t" and isinstance(v, (int, float)))
            t.check_approx(active_ms, expected_ms, 0.02,
                           "aas integrates to the sampled DB time")

            sess = srv.query("top_sessions", from_=frm, to_=to)
            t.check_eq(sess.get("fidelity"), "sampled",
                       "top_sessions fidelity=sampled")
            t.check(any(r["pid"] == 1000 for r in sess["rows"]),
                    "top_sessions shows the sampled session")
    finally:
        cleanup_traces(trace_dir)


def build_full_300s():
    """--mode full style trace: exact transitions only, spanning 300 s."""
    events = []
    ts = BASE
    for i in range(300):
        ts += S
        events.append({"pid": 1000, "ts": ts, "dur": S,
                       "old": IO_DATA_FILE_READ if i % 2 else CPU,
                       "new": CPU, "qid": 100})
    return {
        "backends": [{"pid": 1000, "type": "client", "user": "u", "db": "d"}],
        "queries": [{"id": 100, "text": "SELECT 1"}],
        "events": events,
    }, BASE, ts


def test_full_mode_still_fast(t):
    print("\n### 2. Exact-covered long window keeps the summary path ###")
    scenario, lo, hi = build_full_300s()
    trace_dir = generate_traces(scenario)
    try:
        with ServerHarness(trace_dir) as srv:
            frm, to = lo - 10 * S, hi + 10 * S
            tm = srv.query("time_model", from_=frm, to_=to)
            rows = {r["name"]: r for r in tm["rows"]}
            t.check_eq(tm.get("fidelity"), "exact",
                       "fully exact-covered window labeled exact")
            t.check_approx(rows.get("DB Time", {}).get("ms", -1),
                           300_000.0, 0.001, "DB Time = 300s from summaries")

            # A window with NO data must be labeled none — never exact.
            empty = srv.query("time_model",
                              from_=hi + 1000 * S, to_=hi + 1200 * S)
            t.check_eq(empty.get("fidelity"), "none",
                       "empty >=120s window labeled 'none', not 'exact'")
            erows = {r["name"]: r for r in empty["rows"]}
            t.check_approx(erows.get("DB Time", {}).get("ms", -1), 0.0, 0.001,
                           "empty window DB Time = 0")
    finally:
        cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_summary_honesty")
    print(f"=== {t.name} ===")
    test_tiered_15min(t)
    test_full_mode_still_fast(t)
    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
