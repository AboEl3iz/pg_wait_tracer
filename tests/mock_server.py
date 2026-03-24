#!/usr/bin/env python3
"""mock_server.py -- Serves web/static/ + WebSocket with canned JSON responses.

No SSH, no pgwt-server, no root needed.  Designed for Playwright testing.

Architecture:
  - HTTP on port P   (serves static files from web/static/)
  - WS   on port P+1 (WebSocket endpoint, same protocol as pgwt)

The test_web_ui.py injects a script to point the WS at the correct port.

Usage:
    python3 tests/mock_server.py              # HTTP :18765, WS :18766
    python3 tests/mock_server.py --port 9000  # HTTP :9000,  WS :9001
"""
import asyncio
import json
import os
import sys
import argparse
import signal
from http.server import SimpleHTTPRequestHandler, HTTPServer
import threading

try:
    import websockets
    import websockets.asyncio.server
except ImportError:
    print("ERROR: pip install websockets", file=sys.stderr)
    sys.exit(1)

STATIC_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "web", "static")

# ── Canned data ──────────────────────────────────────────────────────────────

_TO_NS   = 1_774_000_000_000_000_000
_FROM_NS = _TO_NS - 3600_000_000_000
_BUCKET_NS = 60_000_000_000

def _make_aas_buckets():
    buckets = []
    for i in range(60):
        t = _FROM_NS + i * _BUCKET_NS
        buckets.append({
            "t": t,
            "cpu": round(1.2 + 0.3 * (i % 5), 4),
            "io":  round(0.8 + 0.2 * (i % 3), 4),
            "lock": round(0.1 + 0.05 * (i % 7), 4),
            "lwlock": round(0.3 + 0.1 * (i % 4), 4),
            "ipc": 0.02,
            "client": 0.0,
            "timeout": round(0.5 + 0.1 * (i % 6), 4),
            "bufferpin": 0.0,
            "activity": 0.0,
            "extension": round(0.9 + 0.05 * (i % 3), 4),
            "unknown": 0.0,
        })
    return buckets

_AAS_BUCKETS = _make_aas_buckets()

def _aas_event_series():
    series = [
        {"name": "IO:DataFileRead", "event_id": 0x01000015},
        {"name": "IO:WalSync",      "event_id": 0x0100004e},
        {"name": "IO:WalWrite",     "event_id": 0x01000050},
    ]
    buckets = []
    for i in range(60):
        t = _FROM_NS + i * _BUCKET_NS
        buckets.append({
            "t": t,
            "aas": [
                round(0.4 + 0.1 * (i % 3), 4),
                round(0.2 + 0.05 * (i % 5), 4),
                round(0.2 + 0.05 * (i % 4), 4),
            ],
        })
    return series, buckets

_CANNED = {}

_CANNED["info"] = {
    "from_ns": _FROM_NS,
    "to_ns": _TO_NS,
    "num_cpus": 4,
    "num_events": 1_250_000,
}

_CANNED["time_model"] = {
    "wall_ms": 3600000,
    "rows": [
        {"indent": 0, "name": "DB Time",  "ms": 12500, "pct": 100.0, "aas": 3.47},
        {"indent": 1, "name": "CPU*",     "ms": 4800,  "pct": 38.4,  "aas": 1.33},
        {"indent": 1, "name": "IO",       "ms": 3200,  "pct": 25.6,  "aas": 0.89},
        {"indent": 2, "name": "IO:DataFileRead", "ms": 2100, "pct": 16.8, "aas": 0.58},
        {"indent": 2, "name": "IO:WalSync",      "ms": 800,  "pct": 6.4,  "aas": 0.22},
        {"indent": 2, "name": "IO:WalWrite",     "ms": 300,  "pct": 2.4,  "aas": 0.08},
        {"indent": 1, "name": "Lock",     "ms": 1500,  "pct": 12.0,  "aas": 0.42},
        {"indent": 1, "name": "LWLock",   "ms": 1200,  "pct": 9.6,   "aas": 0.33},
        {"indent": 1, "name": "Timeout",  "ms": 1000,  "pct": 8.0,   "aas": 0.28},
        {"indent": 1, "name": "Extension","ms": 800,   "pct": 6.4,   "aas": 0.22},
        {"indent": 0, "name": "Idle",     "ms": 45000, "pct": 0,     "aas": 0.0},
    ],
}

_CANNED["top_events"] = {
    "rows": [
        {"name": "CPU*",             "event_id": 0,          "class": "CPU",
         "count": 250000, "total_ms": 4800, "avg_us": 19.2, "p50_us": 12,
         "p95_us": 45, "p99_us": 120, "max_us": 5000, "pct": 38.4, "aas": 1.33},
        {"name": "IO:DataFileRead",  "event_id": 0x01000015, "class": "IO",
         "count": 85000, "total_ms": 2100, "avg_us": 24.7, "p50_us": 15,
         "p95_us": 80, "p99_us": 250, "max_us": 12000, "pct": 16.8, "aas": 0.58},
        {"name": "Lock:relation",    "event_id": 0x03000000, "class": "Lock",
         "count": 12000, "total_ms": 1500, "avg_us": 125.0, "p50_us": 50,
         "p95_us": 500, "p99_us": 2000, "max_us": 50000, "pct": 12.0, "aas": 0.42},
        {"name": "LWLock:WALWrite",  "event_id": 0x04000008, "class": "LWLock",
         "count": 30000, "total_ms": 1200, "avg_us": 40.0, "p50_us": 25,
         "p95_us": 100, "p99_us": 400, "max_us": 8000, "pct": 9.6, "aas": 0.33},
        {"name": "Timeout:PgSleep",  "event_id": 0x09000002, "class": "Timeout",
         "count": 500, "total_ms": 1000, "avg_us": 2000000, "p50_us": 2000000,
         "p95_us": 2000000, "p99_us": 2000000, "max_us": 2001000, "pct": 8.0, "aas": 0.28},
        {"name": "IO:WalSync",       "event_id": 0x0100004e, "class": "IO",
         "count": 15000, "total_ms": 800, "avg_us": 53.3, "p50_us": 30,
         "p95_us": 150, "p99_us": 500, "max_us": 10000, "pct": 6.4, "aas": 0.22},
        {"name": "Extension:Extension", "event_id": 0x0a000000, "class": "Extension",
         "count": 800, "total_ms": 800, "avg_us": 1000000, "p50_us": 1000000,
         "p95_us": 1000000, "p99_us": 1000000, "max_us": 1001000, "pct": 6.4, "aas": 0.22},
        {"name": "IO:WalWrite",      "event_id": 0x01000050, "class": "IO",
         "count": 10000, "total_ms": 300, "avg_us": 30.0, "p50_us": 20,
         "p95_us": 80, "p99_us": 200, "max_us": 5000, "pct": 2.4, "aas": 0.08},
    ],
}

_CANNED["top_sessions"] = {
    "rows": [
        {"pid": 1001, "type": "client", "user": "postgres", "db": "testdb",
         "db_time_ms": 5200, "cpu_pct": 45.0, "wait_pct": 55.0, "top_wait": "IO:DataFileRead"},
        {"pid": 1002, "type": "client", "user": "postgres", "db": "testdb",
         "db_time_ms": 3800, "cpu_pct": 38.0, "wait_pct": 62.0, "top_wait": "Lock:relation"},
        {"pid": 1003, "type": "client", "user": "app", "db": "mydb",
         "db_time_ms": 2500, "cpu_pct": 52.0, "wait_pct": 48.0, "top_wait": "LWLock:WALWrite"},
        {"pid": 1004, "type": "client", "user": "app", "db": "mydb",
         "db_time_ms": 1000, "cpu_pct": 30.0, "wait_pct": 70.0, "top_wait": "Timeout:PgSleep"},
        {"pid": 4870, "type": "checkpointer", "user": "", "db": "",
         "db_time_ms": 800, "cpu_pct": 10.0, "wait_pct": 90.0, "top_wait": "Timeout:CheckpointWriteDelay"},
        {"pid": 4871, "type": "bgwriter", "user": "", "db": "",
         "db_time_ms": 200, "cpu_pct": 5.0, "wait_pct": 95.0, "top_wait": "IO:DataFileWrite"},
    ],
}

_CANNED["top_queries"] = {
    "rows": [
        {"query_id": "3886912043147135675", "text": "UPDATE pgbench_accounts SET abalance = abalance + $1 WHERE aid = $2",
         "total_ms": 4200, "pct": 33.6, "count": 45000, "avg_us": 93.3,
         "top_wait": "IO:DataFileRead",
         "classes": [2000, 1200, 500, 300, 0, 0, 100, 0, 0, 100, 0],
         "events": [
            {"name": "CPU*", "ms": 2000},
            {"name": "IO:DataFileRead", "ms": 1200},
            {"name": "Lock:relation", "ms": 500},
            {"name": "LWLock:WALWrite", "ms": 300},
            {"name": "Extension:Extension", "ms": 100},
            {"name": "Timeout:PgSleep", "ms": 100},
         ]},
        {"query_id": "5371305355164922084", "text": "SELECT abalance FROM pgbench_accounts WHERE aid = $1",
         "total_ms": 3100, "pct": 24.8, "count": 45000, "avg_us": 68.9,
         "top_wait": "IO:DataFileRead",
         "classes": [1500, 900, 200, 300, 0, 0, 100, 0, 0, 100, 0],
         "events": [
            {"name": "CPU*", "ms": 1500},
            {"name": "IO:DataFileRead", "ms": 900},
            {"name": "Lock:relation", "ms": 200},
            {"name": "LWLock:WALWrite", "ms": 300},
            {"name": "Extension:Extension", "ms": 100},
            {"name": "Timeout:PgSleep", "ms": 100},
         ]},
        {"query_id": "-2312456789012345678", "text": "INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES ($1, $2, $3, $4, CURRENT_TIMESTAMP)",
         "total_ms": 2800, "pct": 22.4, "count": 45000, "avg_us": 62.2,
         "top_wait": "LWLock:WALWrite",
         "classes": [1000, 800, 300, 500, 0, 0, 100, 0, 0, 100, 0],
         "events": [
            {"name": "CPU*", "ms": 1000},
            {"name": "IO:WalSync", "ms": 800},
            {"name": "LWLock:WALWrite", "ms": 500},
            {"name": "Lock:relation", "ms": 300},
            {"name": "Extension:Extension", "ms": 100},
            {"name": "Timeout:PgSleep", "ms": 100},
         ]},
    ],
}

_CANNED["heatmap"] = {
    "bucket_ns": _BUCKET_NS,
    "max_count": 5000,
    "times": [_FROM_NS + i * _BUCKET_NS for i in range(60)],
    "labels": ["<1", "1-2", "2-4", "4-8", "8-16", "16-32", "32-64",
               "64-128", "128-256", "256-512", "512-1K", "1K-2K",
               "2K-4K", "4K-8K", "8K-16K", ">=16K"],
    "cells": [
        [i, j, max(0, 5000 - abs(j - 3) * 800 - abs(i - 30) * 50)]
        for i in range(0, 60, 3)
        for j in range(16)
        if max(0, 5000 - abs(j - 3) * 800 - abs(i - 30) * 50) > 0
    ],
}

_CANNED["session_timeline"] = {
    "truncated": False,
    "total_count": 8,
    "pids": [1001, 1002],
    "events": [
        {"s": _FROM_NS + 100_000_000_000, "d": 50_000_000_000, "p": 1001,
         "n": "CPU*", "c": 0, "q": "3886912043147135675"},
        {"s": _FROM_NS + 150_000_000_000, "d": 30_000_000_000, "p": 1001,
         "n": "IO:DataFileRead", "c": 1, "q": "3886912043147135675"},
        {"s": _FROM_NS + 180_000_000_000, "d": 20_000_000_000, "p": 1001,
         "n": "Lock:relation", "c": 2, "q": "3886912043147135675"},
        {"s": _FROM_NS + 200_000_000_000, "d": 40_000_000_000, "p": 1001,
         "n": "CPU*", "c": 0, "q": "3886912043147135675"},
        {"s": _FROM_NS + 100_000_000_000, "d": 80_000_000_000, "p": 1002,
         "n": "Lock:relation", "c": 2, "q": "5371305355164922084"},
        {"s": _FROM_NS + 180_000_000_000, "d": 25_000_000_000, "p": 1002,
         "n": "CPU*", "c": 0, "q": "5371305355164922084"},
        {"s": _FROM_NS + 205_000_000_000, "d": 35_000_000_000, "p": 1002,
         "n": "IO:DataFileRead", "c": 1, "q": "5371305355164922084"},
        {"s": _FROM_NS + 240_000_000_000, "d": 15_000_000_000, "p": 1002,
         "n": "LWLock:WALWrite", "c": 3, "q": "5371305355164922084"},
    ],
}


def handle_request(msg):
    """Dispatch a WebSocket JSON request and return canned response."""
    cmd = msg.get("cmd", "")
    req_id = msg.get("id", 0)

    if cmd == "info":
        return {"id": req_id, **_CANNED["info"]}

    if cmd == "aas":
        resp = {"id": req_id, "bucket_ns": _BUCKET_NS, "max_aas": 4.5}
        if msg.get("detail") == "events":
            series, buckets = _aas_event_series()
            resp["breakdown"] = "events"
            resp["series"] = series
            resp["buckets"] = buckets
        else:
            resp["buckets"] = _AAS_BUCKETS
        return resp

    if cmd == "time_model":
        return {"id": req_id, **_CANNED["time_model"]}

    if cmd == "top_events":
        rows = _CANNED["top_events"]["rows"]
        filters = msg.get("filters", {})
        if "class" in filters:
            cls = filters["class"]
            rows = [r for r in rows if r["class"].lower() == cls.lower()]
        return {"id": req_id, "rows": rows}

    if cmd == "top_sessions":
        return {"id": req_id, **_CANNED["top_sessions"]}

    if cmd == "top_queries":
        return {"id": req_id, **_CANNED["top_queries"]}

    if cmd == "heatmap":
        return {"id": req_id, **_CANNED["heatmap"]}

    if cmd == "session_timeline":
        filters = msg.get("filters", {})
        if "pid" in filters or "query_id" in filters:
            return {"id": req_id, **_CANNED["session_timeline"]}
        return {"id": req_id, "events": [], "pids": [], "truncated": False, "total_count": 0}

    if cmd == "transitions":
        return {"id": req_id, "total": 1500, "links": [
            {"source": "CPU*", "target": "IO:DataFileRead", "value": 500, "duration_ms": 2500.0},
            {"source": "IO:DataFileRead", "target": "CPU*", "value": 480, "duration_ms": 1920.0},
            {"source": "CPU*", "target": "LWLock:WALInsert", "value": 300, "duration_ms": 900.0},
            {"source": "LWLock:WALInsert", "target": "CPU*", "value": 290, "duration_ms": 870.0},
            {"source": "CPU*", "target": "IO:WalSync", "value": 100, "duration_ms": 3200.0},
        ]}

    if cmd == "concurrency":
        nb = msg.get("num_buckets", 60)
        peaks = [{"t": 1711936000000000000 + i * 60000000000, "max": 3 + (i % 5), "event": "LWLock:BufferMapping"} for i in range(nb)]
        bursts = [
            {"timestamp_ns": 1711936180000000000, "event": "LWLock:BufferMapping", "sessions": 8, "pids": [1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008]},
            {"timestamp_ns": 1711936300000000000, "event": "IO:DataFileRead", "sessions": 5, "pids": [1001, 1003, 1005, 1007, 1009]},
        ]
        return {"id": req_id, "peaks": peaks, "bursts": bursts, "bucket_ns": 60000000000}

    if cmd == "fingerprints":
        return {"id": req_id, "rows": [
            {"query_id": 123456, "transitions": 800, "signature": "IO:40%|CPU:35%|LWLock:25%",
             "class_pct": {"io": 40, "cpu": 35, "lwlock": 25}, "top_from": "CPU*", "top_to": "IO:DataFileRead"},
        ]}

    return {"id": req_id, "error": f"unknown command: {cmd}"}


# ── WebSocket server ─────────────────────────────────────────────────────────

async def ws_handler(websocket):
    async for raw in websocket:
        try:
            msg = json.loads(raw)
            resp = handle_request(msg)
            await websocket.send(json.dumps(resp))
        except Exception as e:
            err = {"id": 0, "error": str(e)}
            await websocket.send(json.dumps(err))


# ── HTTP server ──────────────────────────────────────────────────────────────

class StaticHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=STATIC_DIR, **kwargs)

    def log_message(self, format, *args):
        pass  # suppress logs


def run_http(port):
    httpd = HTTPServer(("127.0.0.1", port), StaticHandler)
    httpd.serve_forever()


# ── Main ─────────────────────────────────────────────────────────────────────

async def run_ws(port):
    async with websockets.asyncio.server.serve(
        ws_handler, "127.0.0.1", port,
        origins=None,
    ):
        await asyncio.Future()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18765,
                        help="HTTP port (WS = port+1)")
    args = parser.parse_args()

    http_port = args.port
    ws_port = args.port + 1

    # HTTP in thread
    http_thread = threading.Thread(target=run_http, args=(http_port,), daemon=True)
    http_thread.start()

    print(f"mock_server: HTTP http://127.0.0.1:{http_port}, "
          f"WS ws://127.0.0.1:{ws_port}", flush=True)

    # WS in asyncio
    try:
        asyncio.run(run_ws(ws_port))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
