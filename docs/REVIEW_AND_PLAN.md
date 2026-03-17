# pg_wait_tracer — Review & Plan

Comprehensive review of the codebase: bugs found, architectural assessment,
and testing plan. 2026-03-17.

---

# Part I: Code Review Findings

Comprehensive code review of the entire pg_wait_tracer codebase. Prioritized by impact.

## Real Bugs

### Bug 1. Timeline bars shifted forward by one duration

**Files:** `src/server.c` (handle_session_timeline), `web/static/app.js` (line 1349)

The BPF emits `timestamp_ns = now` (end of old state) and `duration_ns = now - last_ts`.
So the old event ran from `(timestamp_ns - duration_ns)` to `timestamp_ns`.

But the server sends `"s": timestamp_ns` and app.js draws bars from `ev.s` to
`ev.s + ev.d`. **Every timeline bar is displaced forward by its own duration.**

**Fix:** Send `s = ev->timestamp_ns - ev->duration_ns` instead of `ev->timestamp_ns`.

### Bug 2. Histogram bucket mismatch between server and daemon builds

**Files:** `src/summary_writer.c:12-24` vs `src/map_reader.c:131-150`

Two different `pgwt_duration_to_bucket` implementations produce different results.
The server build's loop version is off-by-one from the daemon's hardcoded thresholds:

| Duration | Server build | Daemon build |
|----------|:-----------:|:-----------:|
| 1us | bucket 0 | bucket 1 |
| 2us | bucket 1 | bucket 2 |
| 4us | bucket 2 | bucket 3 |

Histograms from summary files (server path) vs raw events (daemon path) show different
distributions for the same data.

**Fix:** Replace the loop version in `summary_writer.c` with the same hardcoded
thresholds, or extract a shared implementation.

### Bug 3. `pgwt_handle_init` leaks watchpoint fd on re-init

**File:** `src/backend.c:239`

If `pgwt_scan_existing_backends` already set `be->wp_fd` and a queued BPF lifecycle
INIT event arrives for the same PID, `handle_init` overwrites `wp_fd` without closing
the old one. Leaks one `perf_event` fd per occurrence.

**Fix:** Add `pgwt_close_watchpoint(be->wp_fd)` before line 239.

### Bug 4. `tq_summary_visitor` accumulates unfiltered `class_ns`

**File:** `src/compute.c` (~line 1501)

When a class or event filter is active, `effective_ns` is correctly filtered, but
`class_ns[c]` accumulation is unconditional. The per-class breakdown in query results
includes all classes regardless of filter. Total is correct but class breakdown is wrong.

**Fix:** Only accumulate `class_ns[c]` for classes matching the filter, or compute
the breakdown from filtered events only.

### Bug 5. `next_pow2` infinite loop on large input

**File:** `src/server.c:35-41`

If `n > 2^30`, the signed `int p` overflows to negative on left shift, the
`while (p < n)` loop never terminates. Can happen with a very large
`query_texts.jsonl` file (> ~500M lines).

**Fix:** Use `unsigned int` or clamp `n` to a maximum.

### Bug 6. Integer overflow in heatmap `grid_size`

**File:** `src/compute.c` (~line 820)

`int grid_size = actual_buckets * HISTOGRAM_BUCKETS` — if a client sends a huge
`num_buckets`, this overflows `int`, leading to a tiny `calloc` and subsequent
out-of-bounds writes.

**Fix:** Use `size_t` for `grid_size`, or clamp `actual_buckets` to a reasonable
maximum.

## Suboptimal / Fragile

### Issue 7. `/proc/<pid>/stat` parsing with `%s`

**Files:** `src/backend.c:100`, `src/discovery.c:459`

`fscanf(f, "%d %255s %c %d", ...)` breaks for process names with spaces. For
PostgreSQL, `/proc/pid/stat` typically shows `(postgres)` so this works in practice,
but it's fragile. The correct pattern is to find the last `)` and parse fields
after it.

### Issue 8. JSON key substring matching

**File:** `src/server.c:101-135`

`json_int64`/`json_string` use `strstr(json, "\"key\"")`. Currently safe because no
key is a suffix of another (e.g., `"id"` won't match inside `"query_id"`), but adding
a field like `"from_ns"` would silently break `"from"` lookups. Fragile.

### Issue 9. No JSON escaping in server output

**Files:** `src/server.c`, `src/backend_meta.c`

User-controlled strings (usename, datname, event names, cmd) are injected into JSON
without escaping `"` or `\`. A PostgreSQL username containing `"` would corrupt the
JSON protocol stream. Low risk but a correctness issue.

### Issue 10. WebSocket accepts any origin

**File:** `web/bridge.go:19`

`CheckOrigin: func(r *http.Request) bool { return true }` — any website visited in
the browser can open a WebSocket to `ws://localhost:8384/ws` and read trace data
(query text, PIDs, database names). Low practical risk for a localhost tool but
real CSRF.

### Issue 11. O(n²) linear scans in hot paths

- `pgwt_compute_time_model`: linear scan of event accumulators per trace event
  (`compute.c:~334`)
- Session/query per-wait tracking: linear scan per event (`compute.c:~599, ~715`)
- Session timeline PID lookup: O(count × num_pids) (`server.c`)

All could use hash tables. Fine for typical workloads but becomes a bottleneck
with large trace files.

### Issue 12. Large stack allocations in output.c

`struct pgwt_snapshot deltas[3]` ≈ 312KB on stack.
`struct pgwt_event_stats sorted[4096]` ≈ 640KB on stack.
Combined risk of stack overflow with deep call stacks.

**Fix:** Use `malloc` for these large arrays.

### Issue 13. Double `refresh()` in drill-down

**File:** `web/static/app.js` (~lines 1008-1024)

`drillDown` calls `switchTab` (which refreshes the table) then `refresh()` (which
refreshes chart + table again). Every drill-down fires 2 redundant server requests.

**Fix:** Skip `refresh()` when `switchTab` already triggered a refresh, or have
`switchTab` return a flag indicating it refreshed.

## Bug Fix Priority

1. **Bug 1** — Timeline displacement (visible to users, incorrect visualization)
2. **Bug 2** — Histogram mismatch (incorrect data on summary path)
3. **Bug 3** — fd leak (resource exhaustion over time on long-running daemons)

---

# Part II: Architecture Review

Critical assessment of architectural decisions. What's right, what should change.

## What's right (would keep exactly as-is)

### Hardware watchpoints on `wait_event_info`

The killer insight. No sampling, no PG patches, no extensions, nanosecond precision.
uprobes on PG wait functions would be more fragile (function signatures change).
ptrace is too slow. Sampling loses short events entirely. The watchpoint approach
is correct and unique.

### SSH exec transport (stdin/stdout JSON lines)

Like git over SSH. Zero auth code, zero TLS, zero firewall rules. Every DBA already
has SSH access. The alternatives (gRPC server, REST API, opening ports) are all
worse for this use case.

### Compute-on-read with tiered storage

Store raw events (24h), pre-aggregate summaries (1yr). Full flexibility for recent
data and long-range trends. The right tradeoff.

### Columnar LZ4 trace files

Custom format is justified. Parquet/Arrow would add massive dependencies for a
simple append-only workload. The 36x compression ratio speaks for itself.

### Go for the web client

Single static binary, stdlib HTTP, embed, trivial cross-compilation. Perfect fit
for a tool that ships to DBA laptops.

### C for the daemon

Required by libbpf, and the overhead requirements demand it.

## What I'd change

### Arch 1. Dynamic event name resolution (biggest architectural miss)

Hardcoded PG17/PG18 wait event tables in `wait_event.c` are a maintenance trap.
Every new PG version needs manual table updates. The LWLock tranche table is
already PG18-only, producing wrong names on PG17.

**Alternative:** At daemon startup, read event names from PostgreSQL's
`pg_wait_events` view (PG17+) via `process_vm_readv` of shared memory, or a
one-shot `psql` query. Store the name mapping in the trace file header.
Forward-compatible with PG19+ without any code changes. Clients just read names
from the trace file.

### Arch 2. Drop the Rust TUI client

Three separate implementations of trace reading, event name resolution, and
aggregation: C (daemon+server), Rust (TUI), JS (web UI). Each is 15-25K lines.
The Rust TUI duplicates everything from the C server — and the web client is
strictly better for investigation.

**Alternative:** Drop the Rust client. For "quick SSH check" use cases,
`pgwt-server` could have a `--dump` mode that prints a text summary to stdout.
One codebase, one set of bugs.

### Arch 3. Use a real JSON library in C

`server.c` is 42K lines — a huge chunk is manual `printf`-based JSON construction
and hand-rolled `strstr`-based parsing. This directly caused several bugs (no
escaping, substring key matching, no nested object support).

**Alternative:** Use cJSON (single .c/.h, ~1000 lines, MIT license). Eliminates
an entire class of bugs. The protocol itself is fine — just the implementation
is error-prone.

### Arch 4. Merge daemon and server into one process

Currently: daemon captures events and writes files. pgwt-server reads files and
computes. Separate processes, no communication. Consequences:
- Every web session re-scans the trace directory from scratch
- No caching of computed aggregates between requests
- No live streaming (Phase H requires bridging the two)
- pgwt-server can't show currently-active backends (needs BPF state_map)

**Alternative:** Daemon accepts client connections via Unix socket. It already
has events in memory, trace files open, live backend state. Adding a request
handler is a small addition. Live mode comes for free. Historical queries use
the same trace reader. `pgwt-server` remains for offline replay (no daemon
needed), but the primary path is daemon-direct.

This is the single change that would most simplify the overall architecture.

### Arch 5. Garbage-collect BPF state_map

The `state_map` has 4096 fixed entries. Daemon cleans them on EXIT lifecycle
events, but if a lifecycle event is lost (ringbuf overflow under extreme load),
entries leak permanently. With enough PID churn, the map fills and new backends
silently stop being traced.

**Alternative:** Periodic sweep (every 60s) walks state_map from userspace,
checks each PID against `/proc/<pid>/status`, deletes entries for dead processes.
Simple, robust, prevents silent failure.

### Arch 6. Cache computed results in the server

Every zoom, filter, or tab switch re-reads and re-processes all matching trace
events. For a 1-hour trace with millions of events, noticeable latency on every
interaction.

**Alternative:** Cache last N computed results keyed by (command, from, to,
filters). Most interactions change only one parameter, so adjacent queries share
most work. Even a simple LRU of decoded event arrays (before aggregation) would
help — decode once, aggregate many times with different filters.

## What I explicitly would NOT change

- The overall capture approach (BPF watchpoints)
- The file format (columnar LZ4 blocks)
- The transport (SSH exec)
- The client technology (Go + ECharts)
- The compute-on-read philosophy
- The tiered storage (raw events + summaries)

## Architecture Summary

The architecture is fundamentally sound. The core decisions (watchpoints, SSH
transport, columnar files, compute-on-read) are all correct. The changes are
about **consolidation** — fewer moving parts, fewer duplicated implementations,
dynamic instead of hardcoded data. The biggest single win would be merging
daemon+server, which simplifies the system and makes live mode trivial.

---

# Part III: Testing Plan

The #1 job of this tool is to tell the truth. If we say IO:DataFileRead is 10%
of DB Time, it must really be 10%. Everything else (UI, performance, memory
safety) is secondary to data correctness.

## Current Test State

**What exists (6,200 lines):**
- 4 C unit tests (varint, columnar encoding, file format, wait event names)
- 3 shell integration tests (CLI validation, lifecycle, overhead)
- 11 Python functional tests (accuracy, cross-validation, deterministic counts)

**What the existing tests cover:**
- pg_sleep duration accuracy (±30% tolerance)
- Deterministic event count (N pg_sleep calls → N events)
- DB Time sanity check (> 0, < theoretical max)
- IO count cross-check against pg_stat_io (0.1-3x ratio)
- Backend coverage vs pg_stat_activity
- Event names vs pg_wait_events catalog

**What the existing tests do NOT cover:**
- Are percentages correct? (we say IO=10%, is it 10%?)
- Are per-session numbers correct? (PID 100 did 3s of IO — really?)
- Are per-query numbers correct? (query Q did 5s of Lock — really?)
- Does the server compute the same results as the CLI?
- Do summary files produce the same results as raw events?
- Is the AAS chart showing correct values?
- Is the timeline showing events at the correct timestamps?
- Does filtering actually work correctly?
- Do the numbers hold up over longer durations (hours, not seconds)?

## Layer 0: Data Correctness (PRIORITY #1)

### Philosophy

We test correctness by creating **controlled workloads where we know the answer
in advance**, then verify the tracer reports match. Every test has a mathematical
expectation that we can derive independently.

Two complementary approaches:
- **Live tests** (root + PostgreSQL): real BPF capture of controlled PG workloads
- **Synthetic tests** (no root): known events written to trace files, verify
  pgwt-server computes correct aggregates

### 0A. Live Data Correctness Tests (root + PostgreSQL)

These test the full pipeline: BPF capture → trace file → compute → output.

#### 0A.1 Percentage accuracy — controlled workload split

Create a workload where we KNOW what the percentage split should be:

```
Setup:
  - Backend A: DO $$ FOR i IN 1..5 LOOP PERFORM pg_sleep(1); END LOOP; END $$
  - Backend B: SELECT count(*) FROM generate_series(1, 500000000)  -- pure CPU ~10s
  - Both run simultaneously for ~10s

Expected:
  - Timeout:PgSleep ≈ 50% of DB Time (5s out of ~10s total)
  - CPU ≈ 50% of DB Time
  - Tolerance: ±15% (due to timing, CPU fluctuations)

Verify:
  - time_model: PgSleep% + CPU% ≈ 100%
  - system_event: PgSleep total_ms ≈ 5000ms
  - system_event: CPU total_ms ≈ 5000ms
```

#### 0A.2 AAS accuracy — known concurrency

```
Setup:
  - Start exactly 4 backends, each doing pg_sleep(10)
  - Measure for 8 seconds (after all are attached)

Expected:
  - AAS ≈ 4.0 (4 backends active simultaneously)
  - DB Time ≈ 4 × 8000ms = 32000ms
  - Timeout:PgSleep AAS ≈ 4.0

Verify:
  - time_model: AAS between 3.5 and 4.5
  - time_model: DB Time between 28000ms and 36000ms
```

#### 0A.3 Per-session correctness — isolated backends

```
Setup:
  - Backend A: pg_sleep(5) — should accumulate ~5s of Timeout:PgSleep
  - Backend B: pg_sleep(3) — should accumulate ~3s of Timeout:PgSleep
  - Backend C: generate_series CPU burn ~5s
  - Run tracer with session_event view

Verify:
  - session_event shows 3 client backends
  - PID A: DB Time ≈ 5000ms, top wait = Timeout:PgSleep
  - PID B: DB Time ≈ 3000ms, top wait = Timeout:PgSleep
  - PID C: DB Time ≈ 5000ms, top wait = CPU
  - Sum of per-session DB Time ≈ system DB Time
```

#### 0A.4 Per-query correctness — query_id attribution

```
Setup:
  - Enable pg_stat_statements (for query_id generation)
  - Backend A: SELECT pg_sleep(3)  -- gets query_id Q1
  - Backend B: SELECT count(*) FROM pgbench_accounts  -- gets query_id Q2
  - Run tracer with query_event view

Verify:
  - query_event shows Q1 with ~3000ms Timeout:PgSleep
  - query_event shows Q2 with IO + CPU time
  - Q1 time + Q2 time ≈ total DB Time
```

#### 0A.5 Time model partition — exact accounting

```
Setup:
  - pgbench -c 8 -T 20 (generates mixed CPU/IO/Lock/LWLock)

Verify (strict):
  - CPU_ms + IO_ms + Lock_ms + LWLock_ms + Client_ms + IPC_ms +
    Timeout_ms + BufferPin_ms + Extension_ms + Activity_ms = DB_Time_ms
  - Tolerance: < 0.1% (these are computed from the same events,
    so any discrepancy is a bug, not measurement noise)
  - Each class% = class_ms / DB_Time_ms × 100 (verify independently)
  - All percentages sum to 100% ± 0.1%
```

#### 0A.6 Idle exclusion — idle backends don't inflate DB Time

```
Setup:
  - 1 active backend: generate_series CPU burn for 10s
  - 5 idle backends: connected but doing nothing (psql sitting at prompt)
  - Tracer measures for 8s

Verify:
  - DB Time ≈ 8000ms (only the 1 active backend)
  - DB Time is NOT 48000ms (6 backends × 8s would be wrong)
  - AAS ≈ 1.0 (not 6.0)
  - Activity class is excluded from DB Time
```

#### 0A.7 IO percentage — cross-check with pg_stat_io

Stronger version of the existing IO count test:

```
Setup:
  - Drop caches
  - pgbench -S -c 4 -T 15 (read-heavy, cold cache → lots of IO)

Verify:
  - IO:DataFileRead count matches pg_stat_io reads ±20%
  - IO:DataFileRead total_ms is reasonable (count × avg should match total)
  - IO:DataFileRead % of DB Time is plausible (for cold cache, IO > 30%)
  - IO:DataFileRead avg latency < 10ms (sanity check for storage)
```

#### 0A.8 Lock time accuracy — timed lock contention

```
Setup:
  - Session A: BEGIN; LOCK TABLE t IN ACCESS EXCLUSIVE MODE; pg_sleep(5);
  - Session B: (1s later) SELECT * FROM t;  -- blocks ~4s on Lock:relation
  - Session B continues with pg_sleep(60) to stay alive

Verify:
  - Lock:relation total_ms between 3000 and 5500
  - Lock:relation count = 1
  - Lock:relation max_us ≈ total_us (single event)
  - Per-session: Session B shows Lock:relation as top wait
```

#### 0A.9 Duration consistency — avg, max, percentiles make sense

```
Setup:
  - 10 × pg_sleep(0.1) + 1 × pg_sleep(1.0) in sequence

Verify:
  - count = 11
  - total ≈ 2000ms (10×100ms + 1×1000ms)
  - avg ≈ 182ms (2000/11)
  - max ≈ 1000ms (the long sleep)
  - P50 ≈ 100ms (median is one of the short sleeps)
  - P99 ≈ 1000ms (the long sleep)
  - Invariant: P50 ≤ avg ≤ max
  - Invariant: P50 ≤ P95 ≤ P99 ≤ max
```

#### 0A.10 Daemon trace file → server agreement

The critical end-to-end test: daemon captures events, writes trace files,
pgwt-server reads them and computes — do the numbers match?

```
Setup:
  - Run daemon for 30s with pgbench -c 4 -T 25
  - Daemon captures to trace files
  - Run pgwt-server on the same trace files
  - Also capture CLI output from the daemon itself

Verify:
  - pgwt-server time_model ≈ CLI time_model (same events, same numbers)
  - pgwt-server top_events count/total ≈ CLI system_event count/total
  - pgwt-server AAS ≈ CLI AAS
  - Tolerance: < 5% (timing differences between CLI snapshot and server query)
```

### 0B. Synthetic Data Correctness Tests (no root, no PostgreSQL)

Test the compute layer in isolation: generate trace files with mathematically
exact event data, feed to pgwt-server, verify computed results are exactly correct.

**Infrastructure:** A C program `gen_test_traces` that writes trace files with
caller-specified events. Each test scenario creates its own trace dir.

#### 0B.1 Time model — exact arithmetic

```
Input: 100 events, all from PID 1000:
  - 60 events: old_event=CPU (0), duration_ns=1000000 (1ms each) → 60ms CPU
  - 30 events: old_event=IO:DataFileRead, duration_ns=2000000 (2ms) → 60ms IO
  - 10 events: old_event=Lock:relation, duration_ns=3000000 (3ms) → 30ms Lock

Expected time_model response:
  - db_time_ms = 150.0 (60 + 60 + 30)
  - CPU: 60ms, 40.0%
  - IO: 60ms, 40.0%
  - Lock: 30ms, 20.0%
  - Tolerance: 0% (exact integers, no measurement noise)
```

#### 0B.2 AAS — bucket correctness

```
Input: events spanning 10 seconds, 3 PIDs always active:
  - PID 1000: continuous CPU events, 10ms each, covers full 10s
  - PID 1001: continuous IO events, 10ms each, covers full 10s
  - PID 1002: continuous Lock events, 10ms each, covers full 10s

Request: aas with 10 buckets (1s each)

Expected:
  - Each bucket: CPU AAS=1.0, IO AAS=1.0, Lock AAS=1.0, total AAS=3.0
  - max_aas = 3.0
  - Tolerance: 0%
```

#### 0B.3 Per-event stats — count, total, avg, max, percentiles

```
Input: 1000 IO:DataFileRead events with durations:
  - 500 events: 100us
  - 400 events: 1ms
  - 100 events: 10ms

Expected top_events:
  - count = 1000
  - total = 500×0.1 + 400×1 + 100×10 = 1450ms
  - avg = 1450us
  - max = 10000us
  - P50 = 100us (event #500 is still in the 100us group)
  - P95 = 1000us (event #950 is in the 1ms group)
  - P99 = 10000us (event #990 is in the 10ms group)
  - pct = 100% (only one event type)
```

#### 0B.4 Filtering correctness

```
Input: mixed events from multiple PIDs, classes, query_ids

Test each filter independently and in combination:
  - class="IO" → only IO events in result, count matches
  - event_id=<specific> → only that event
  - pid=1000 → only events from PID 1000
  - query_id=999 → only events with that query_id
  - class="IO" + pid=1000 → intersection
  - Verify: filtered DB Time = sum of matching events only
  - Verify: filtered percentages are relative to filtered DB Time
```

#### 0B.5 Summary vs raw agreement

```
Input: same trace files processed via:
  a) Raw events path (pgwt-server reads .trace.lz4 directly)
  b) Summary path (generate .summary.lz4, pgwt-server reads summaries)

Verify:
  - time_model: same DB Time, same class breakdown
  - top_events: same counts, same totals
  - top_sessions: same per-PID totals
  - AAS chart: same bucket values
  - Known exception: percentiles NOT available on summary path
  - REGRESSION TEST for bug #2: histogram buckets must match between paths
```

#### 0B.6 Timeline timestamp correctness

```
Input: 5 events with known timestamps and durations:
  Event 1: timestamp_ns=10000, duration_ns=3000  → bar: [7000, 10000]
  Event 2: timestamp_ns=15000, duration_ns=5000  → bar: [10000, 15000]
  Event 3: timestamp_ns=20000, duration_ns=2000  → bar: [18000, 20000]

Request: session_timeline

Verify:
  - Event 1: s=7000, d=3000  (NOT s=10000, d=3000)
  - Event 2: s=10000, d=5000
  - Event 3: s=18000, d=2000
  - REGRESSION TEST for bug #1
```

#### 0B.7 Idle exclusion in compute

```
Input:
  - 50 events: old_event=Activity:LogicalLauncherMain (idle), duration=1s each
  - 10 events: old_event=CPU, duration=100ms each

Verify:
  - DB Time = 1000ms (only CPU events)
  - DB Time is NOT 51000ms
  - Activity is excluded from all percentage calculations
  - AAS computed from DB Time only
```

#### 0B.8 Edge cases — zero, one, overflow

```
Scenarios:
  - Empty trace file (header only, no blocks) → all zeros, no errors
  - Single event → count=1, total=duration, avg=duration, P50=P99=max=duration
  - Event with duration_ns=0 → count=1, total=0, avg=0
  - Event with duration_ns=UINT64_MAX → no overflow in total
  - 100K events from 1 PID → session total = system total
  - Events at exact bucket boundaries → land in correct bucket
```

## Layer 1: Server Protocol Tests

Test pgwt-server as a black box: spawn process, send JSON on stdin, verify JSON
on stdout. Uses the same synthetic trace files from Layer 0B.

Covers all commands: info, aas, top_events, top_sessions, top_queries,
session_timeline, time_model, heatmap.

Key difference from Layer 0B: Layer 0B tests data correctness (are the numbers
right?). Layer 1 tests protocol correctness (is the JSON well-formed? are all
fields present? does error handling work?).

Additional protocol tests:
- Unknown command → error response with correct id
- Missing required fields → error response
- Concurrent requests with different ids → responses matched correctly
- Very large response (10K events in timeline) → no truncation of JSON
- Request with all filters set simultaneously

## Layer 2: Web UI Tests (Playwright)

Automated browser testing. No root, no PostgreSQL, no SSH.

**Infrastructure:** Python mock server that serves static assets from
`web/static/` and provides a WebSocket endpoint returning canned JSON responses.

### Functional
- Page load, connection, initial chart render
- Tab navigation (Overview, Events, Sessions, Queries, Timeline)
- Table drill-down flow: Events → click row → Sessions → click PID → Timeline
- Breadcrumb navigation: click crumb → pops back
- Clear filters → resets to unfiltered view
- Chart zoom: drag to select time range → tables refresh
- Column sorting: click header → rows reorder

### Data display correctness
- Summary bar numbers match canned response (DB Time, Wall, AAS)
- Event table rows match canned top_events response
- Session table rows match canned top_sessions response
- Query table rows match canned top_queries response
- Timeline bars positioned at correct timestamps (regression for bug #1)
- Percentage bars width proportional to actual percentage

### Regression
- Drill-down sends exactly 1 refresh request, not 2 (bug #13)
- Filter state persists across tab switches
- Reconnection after WebSocket drop

## Layer 3: Compute Unit Tests (C)

Direct C function calls with in-memory event arrays. Fastest feedback.

- `pgwt_duration_to_bucket`: exhaustive test for all microsecond values 0-20000
  **REGRESSION for bug #2**: both implementations produce identical results
- `pgwt_compute_aas`: verify per-bucket, per-class values
- `pgwt_compute_top_events`: count, total, avg, max, percentiles
- `pgwt_compute_top_sessions`: per-PID totals, top wait
- `pgwt_compute_top_queries`: per-query grouping, class breakdown
- Filter matching: class, event_id, pid, query_id, combined
- Summary serialize → deserialize roundtrip

## Layer 4: Performance Benchmarks

- Compute throughput: 1M/10M/100M events × each command → measure time
- Server latency: p50/p95/p99 response time for typical queries
- File read throughput: events/sec for LZ4 decode + columnar parse
- Memory stability: 1000 sequential requests → RSS stays flat

## Layer 5: Memory Safety

- AddressSanitizer build (Makefile target)
- Run all unit tests + server protocol tests under ASan
- Valgrind for leak detection on server process

---

# Part IV: Implementation Priority

## Bug fixes (do first)

| # | What | Impact | Effort |
|---|------|--------|--------|
| Bug 1 | Timeline bar displacement | Users see wrong timestamps | 5 min |
| Bug 2 | Histogram bucket mismatch | Wrong histogram data on summary path | 5 min |
| Bug 3 | Watchpoint fd leak on re-init | fd exhaustion on long-running daemons | 5 min |
| Bug 4 | Unfiltered class_ns in queries | Wrong class breakdown with filters | 15 min |
| Bug 5 | next_pow2 infinite loop | Hang on huge query_texts.jsonl | 5 min |
| Bug 6 | Heatmap grid_size overflow | Memory corruption on malicious input | 5 min |

## Architecture changes (plan for next phase)

| # | What | Impact | Effort |
|---|------|--------|--------|
| Arch 1 | Dynamic event name resolution | Forward-compatible with PG19+ | Medium |
| Arch 2 | Drop Rust TUI client | Halve maintenance surface | Low |
| Arch 3 | Use cJSON library | Eliminate JSON bug class | Medium |
| Arch 4 | Merge daemon + server | Simplify architecture, enable live mode | High |
| Arch 5 | Garbage-collect state_map | Prevent silent tracing loss | Low |
| Arch 6 | Cache computed results | Interactive-speed queries on large traces | Medium |

## Testing (build incrementally)

| # | Layer | What | Needs | Value |
|---|-------|------|-------|-------|
| 1 | 0B | Synthetic data correctness | No root, no PG | **Highest** — proves math is right |
| 2 | 0A | Live data correctness | Root + PG | **Highest** — proves end-to-end is right |
| 3 | 2 | Web UI (Playwright) | No root, no PG | **High** — eliminates manual clicking |
| 4 | 3 | Compute unit tests (C) | No root, no PG | Medium — fast feedback loop |
| 5 | 1 | Server protocol tests | No root, no PG | Medium — protocol correctness |
| 6 | 4 | Performance benchmarks | No root, no PG | Medium — catch regressions |
| 7 | 5 | Memory safety | No root, no PG | Medium — one-time setup |

## Test File Structure

```
tests/
  # Existing (keep all)
  test_wait_event.c, test_cmdline.c, test_event_writer.c, test_event_reader.c
  test_accuracy.py, test_deterministic.py, test_cross_validate.py, ...
  run_all.sh

  # New: Layer 0B — Synthetic data correctness
  gen_test_traces.c          # C: generate trace files with exact known events
  test_data_time_model.py    # verify time model math
  test_data_aas.py           # verify AAS computation
  test_data_events.py        # verify event stats + percentiles
  test_data_sessions.py      # verify per-session attribution
  test_data_queries.py       # verify per-query attribution
  test_data_filters.py       # verify filtering correctness
  test_data_timeline.py      # verify timeline timestamps (bug #1)
  test_data_summary.py       # verify summary vs raw agreement (bug #2)
  test_data_idle.py          # verify idle exclusion
  test_data_edge.py          # edge cases (empty, single, overflow)

  # New: Layer 0A — Live data correctness
  test_percentage.py         # controlled workload split
  test_aas_accuracy.py       # known concurrency → known AAS
  test_session_accuracy.py   # isolated backends → per-session numbers
  test_query_accuracy.py     # query_id → per-query numbers
  test_partition.py          # DB Time = sum of all classes (exact)
  test_idle_exclusion.py     # idle backends don't inflate DB Time
  test_daemon_server.py      # daemon CLI output = pgwt-server output

  # New: Layer 2 — Web UI tests
  mock_server.py             # mock WebSocket + HTTP server
  test_web_ui.py             # Playwright browser tests

  # New: Layer 3 — Compute unit tests
  test_compute.c             # C unit tests for compute functions
  test_bucket.c              # histogram bucket (bug #2 regression)

  # New: Layer 4 — Performance
  gen_bench_traces.c         # generate large trace files
  bench_server.py            # latency/throughput benchmarks
```
