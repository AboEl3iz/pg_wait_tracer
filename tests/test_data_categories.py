#!/usr/bin/env python3
"""test_data_categories.py — T2 decomposed-AAS model over synthetic traces
(docs/AAS_SEMANTICS_DECISION.md).

Covers, end-to-end through pgwt-server:
  1. io_worker exclusion: an io_worker's waits enter neither the class AAS
     nor DB Time in any view; they appear ONLY in the "io_worker" category
     slot and drive io_worker_busy_pct.
  2. Category decomposition from backends.jsonl types
     (maintenance = autovac_worker, background = checkpointer, ...).
  3. CPU SAMPLES (event id 0 in a SAMPLES block — new in T2) are
     first-class active time: they raise sampled AAS and the CPU class.
  4. CMD_START/CMD_END markers classify exact we==0 intervals: the
     in-command portion is CPU, the out-of-command portion is idle
     (excluded from DB Time) — the AAS-1 "one definition, all paths" rule.
  5. Exit-record phantom backstop: an EXIT transition closing a huge
     "CPU" interval OUTSIDE exact coverage in a sampled trace (what
     pre-T2 daemons recorded at every disconnect — study defect 2) must
     not contribute to AAS/DB Time.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import (
    ServerHarness, generate_traces, cleanup_traces, TestRunner,
    CPU, IO_DATA_FILE_READ,
)

BASE_TS = 10_000_000_000_000
SEC = 1_000_000_000

# Markers (src/pg_wait_tracer.h)
MARKER_CMD_START = 0xFFFFFFF6
MARKER_CMD_END = 0xFFFFFFF7
EVENT_EXIT = 0xFFFFFFFF


def scenario_io_worker():
    """4 pids x 5 s of IO:DataFileRead: client, io_worker, autovac_worker,
    checkpointer. Expected: class io AAS = 3.0 (io_worker excluded);
    categories command/maintenance/background = 1.0 each, io_worker = 1.0
    in its own slot; db_time = 15 s; io_worker_busy_pct = 100 (1 worker,
    busy the whole window)."""
    events = []
    for pid in (100, 200, 300, 400):
        events.append({"pid": pid, "ts": BASE_TS + 5 * SEC,
                       "dur": 5 * SEC, "old": IO_DATA_FILE_READ,
                       "new": CPU, "qid": 0})
    return {
        "backends": [
            {"pid": 100, "type": "client", "user": "t", "db": "d"},
            {"pid": 200, "type": "io_worker"},
            {"pid": 300, "type": "autovac_worker"},
            {"pid": 400, "type": "checkpointer"},
        ],
        "events": events,
    }


def scenario_cpu_samples():
    """2 client pids sampled on-CPU (event 0) for 5 s at 10 Hz + 1
    io_worker sampled in IO. Sampled AAS must be ~2.0 CPU (the io_worker
    excluded), proving CPU samples are first-class active time."""
    samples = []
    for i in range(50):  # 5 s at 10 Hz
        ts = BASE_TS + i * 100_000_000
        samples.append({"pid": 500, "ts": ts, "event": CPU, "qid": 7})
        samples.append({"pid": 501, "ts": ts, "event": CPU, "qid": 0})
        samples.append({"pid": 502, "ts": ts, "event": IO_DATA_FILE_READ,
                        "qid": 0})
    return {
        "backends": [
            {"pid": 500, "type": "client", "user": "t", "db": "d"},
            {"pid": 501, "type": "client", "user": "t", "db": "d"},
            {"pid": 502, "type": "io_worker"},
        ],
        "queries": [{"id": 7, "text": "SELECT cpu"}],
        "sample_period_ns": 100_000_000,
        "samples": samples,
    }


def scenario_cmd_gate():
    """One client pid, exact tier, with CMD markers:
      0s   CMD_START
      0-2s we==0 interval (fully in-command)      -> CPU, counted
      2s   CMD_END
      2-5s we==0 interval (fully out-of-command)  -> idle, NOT counted
    Expected: CPU AAS over the 5 s window = 2/5 = 0.4; db_time = 2 s."""
    events = [
        {"pid": 600, "ts": BASE_TS, "dur": 0,
         "old": MARKER_CMD_START, "new": MARKER_CMD_START, "qid": 9},
        {"pid": 600, "ts": BASE_TS + 2 * SEC, "dur": 2 * SEC,
         "old": CPU, "new": IO_DATA_FILE_READ, "qid": 9},
        {"pid": 600, "ts": BASE_TS + 2 * SEC, "dur": 0,
         "old": MARKER_CMD_END, "new": MARKER_CMD_END, "qid": 9},
        {"pid": 600, "ts": BASE_TS + 5 * SEC, "dur": 3 * SEC,
         "old": CPU, "new": IO_DATA_FILE_READ, "qid": 0},
    ]
    return {
        "backends": [{"pid": 600, "type": "client", "user": "t", "db": "d"}],
        "queries": [{"id": 9, "text": "SELECT gated"}],
        "events": events,
    }


def scenario_exit_phantom():
    """The study defect-2 shape: a sampled trace (10 Hz samples of one
    genuinely active pid) plus an EXIT transition closing a phantom 5 s
    "CPU" interval for a pid the sampler barely saw. The phantom lies
    outside any exact coverage; the SAMPLES stream already covers the
    window. Expected: AAS ~ 1.0 (the sampled pid), NOT ~2.0."""
    samples = []
    for i in range(50):
        ts = BASE_TS + i * 100_000_000
        samples.append({"pid": 700, "ts": ts, "event": IO_DATA_FILE_READ,
                        "qid": 0})
    events = [
        # phantom: old=0 (CPU), new=EXIT, closing the whole 5 s window
        {"pid": 701, "ts": BASE_TS + 5 * SEC, "dur": 5 * SEC,
         "old": CPU, "new": EVENT_EXIT, "qid": 0},
    ]
    return {
        "backends": [
            {"pid": 700, "type": "client", "user": "t", "db": "d"},
            {"pid": 701, "type": "client", "user": "t", "db": "d"},
        ],
        "sample_period_ns": 100_000_000,
        "samples": samples,
        "events": events,
        "interleave": 1,
    }


def total_class_aas(bucket):
    return sum(v for k, v in bucket.items() if k not in ("t", "total", "cat"))


def run_io_worker(t):
    print("--- 1+2: io_worker exclusion + categories ---")
    trace_dir = generate_traces(scenario_io_worker())
    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("aas", buckets=1,
                             from_=BASE_TS, to_=BASE_TS + 5 * SEC)
            b = resp["buckets"][0]
            t.check_approx(total_class_aas(b), 3.0, 0.02,
                           "class AAS = 3.0 (io_worker excluded)")
            t.check_approx(resp.get("max_aas", -1), 3.0, 0.02,
                           "max_aas excludes io_worker")
            cat = b["cat"]
            t.check_approx(cat["io_worker"], 1.0, 0.02,
                           "io_worker category slot = 1.0")
            t.check_approx(cat["maintenance"], 1.0, 0.02,
                           "autovac_worker -> maintenance = 1.0")
            t.check_approx(cat["background"], 1.0, 0.02,
                           "checkpointer -> background = 1.0")
            t.check_approx(cat["command"] + cat["execution"] + cat["planning"],
                           1.0, 0.02, "client wait -> foreground = 1.0")

            tm = srv.query("time_model",
                           from_=BASE_TS, to_=BASE_TS + 5 * SEC)
            t.check_approx(tm["db_time_ms"], 15000, 0.02,
                           "DB Time = 15 s (io_worker excluded)")
            t.check_approx(tm.get("io_worker_busy_pct", -1), 100.0, 0.02,
                           "io_worker_busy_pct = 100 (1 worker, 5s/5s busy)")
            cats = {r["name"]: r for r in tm.get("categories", [])}
            t.check_approx(cats["io_worker"]["ms"], 5000, 0.02,
                           "io_worker category ms outside DB Time")
            t.check_approx(cats["maintenance"]["ms"], 5000, 0.02,
                           "maintenance category ms")

            te = srv.query("top_events",
                           from_=BASE_TS, to_=BASE_TS + 5 * SEC)
            io_rows = [r for r in te["rows"]
                       if r["name"] == "IO:DataFileRead"]
            t.check(len(io_rows) == 1, "one IO:DataFileRead row")
            t.check_approx(io_rows[0]["total_ms"], 15000, 0.02,
                           "top_events excludes the io_worker's 5 s")

            ts_rows = srv.query("top_sessions",
                                from_=BASE_TS, to_=BASE_TS + 5 * SEC)["rows"]
            t.check(all(r["pid"] != 200 for r in ts_rows),
                    "io_worker pid not listed as a session")
    finally:
        cleanup_traces(trace_dir)


def run_cpu_samples(t):
    print("--- 3: CPU samples are first-class active time ---")
    trace_dir = generate_traces(scenario_cpu_samples())
    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("aas", buckets=1,
                             from_=BASE_TS, to_=BASE_TS + 5 * SEC)
            t.check(resp.get("fidelity") == "sampled",
                    "window is sampled fidelity")
            b = resp["buckets"][0]
            t.check_approx(b.get("cpu", -1), 2.0, 0.05,
                           "sampled CPU AAS = 2.0 (two on-CPU clients)")
            t.check_approx(b["cat"]["io_worker"], 1.0, 0.05,
                           "sampled io_worker slot = 1.0")
            t.check_approx(b["cat"]["execution"], 1.0, 0.05,
                           "CPU sample with query_id -> execution")
            t.check_approx(b["cat"]["command"], 1.0, 0.05,
                           "CPU sample without query_id -> command")

            tm = srv.query("time_model",
                           from_=BASE_TS, to_=BASE_TS + 5 * SEC)
            t.check_approx(tm["db_time_ms"], 10000, 0.06,
                           "sampled DB Time = 10 s (2 pids x 5 s CPU)")

            tq = srv.query("top_queries",
                           from_=BASE_TS, to_=BASE_TS + 5 * SEC)["rows"]
            q7 = [r for r in tq if r["query_id"] == "7"]
            t.check(len(q7) == 1 and abs(q7[0]["total_ms"] - 5000) < 500,
                    "CPU samples attribute to their query")
    finally:
        cleanup_traces(trace_dir)


def run_cmd_gate(t):
    print("--- 4: CMD markers classify exact we==0 ---")
    trace_dir = generate_traces(scenario_cmd_gate())
    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("aas", buckets=1,
                             from_=BASE_TS, to_=BASE_TS + 5 * SEC)
            b = resp["buckets"][0]
            t.check_approx(b.get("cpu", -1), 0.4, 0.02,
                           "in-command CPU only: AAS = 2s/5s = 0.4")
            tm = srv.query("time_model",
                           from_=BASE_TS, to_=BASE_TS + 5 * SEC)
            t.check_approx(tm["db_time_ms"], 2000, 0.02,
                           "DB Time = 2 s (post-command CPU is idle)")
            t.check_approx(tm["idle_time_ms"], 3000, 0.02,
                           "idle = 3 s (the out-of-command interval)")
    finally:
        cleanup_traces(trace_dir)


def run_exit_phantom(t):
    print("--- 5: exit-record phantom CPU backstop (study defect 2) ---")
    trace_dir = generate_traces(scenario_exit_phantom())
    try:
        with ServerHarness(trace_dir) as srv:
            resp = srv.query("aas", buckets=1,
                             from_=BASE_TS, to_=BASE_TS + 5 * SEC)
            b = resp["buckets"][0]
            total = total_class_aas(b)
            t.check_approx(total, 1.0, 0.05,
                           "AAS = 1.0: the phantom 5 s exit-CPU interval "
                           "is dropped, the sampled pid remains")
            t.check_approx(b.get("cpu", 0), 0.0, 0.02,
                           "no phantom CPU class AAS")
            tm = srv.query("time_model",
                           from_=BASE_TS, to_=BASE_TS + 5 * SEC)
            t.check_approx(tm["db_time_ms"], 5000, 0.06,
                           "DB Time = 5 s (samples only, no phantom)")
    finally:
        cleanup_traces(trace_dir)


def main():
    t = TestRunner("test_data_categories")
    print(f"=== {t.name} ===")
    run_io_worker(t)
    run_cpu_samples(t)
    run_cmd_gate(t)
    run_exit_phantom(t)
    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
