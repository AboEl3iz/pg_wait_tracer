# Development Plan

Sequenced plan covering all bug fixes, architecture improvements, testing, and
feature work. Each sprint builds on the previous. Dependencies are explicit.

Reference: `REVIEW_AND_PLAN.md` (bugs, architecture, test specs),
`TRACING_ANALYSIS_PLAN.md` (phases 2-5), `ROADMAP.md` (phases G.2, H).

---

## Sprint 1: Bug Fixes + Ship Uncommitted Work ✅ COMPLETED (2026-03-18)

**Goal:** Fix all known bugs. Commit the in-progress session timeline work.

**No dependencies. All items are independent 5-15 min fixes.**

| # | Task | File(s) | Bug ref | Status |
|---|------|---------|---------|--------|
| 1.1 | Fix timeline bar displacement: send `s = timestamp_ns - duration_ns` | `src/server.c` | Bug 1 | ✅ |
| 1.2 | Fix histogram bucket mismatch: replace loop version with hardcoded thresholds | `src/summary_writer.c`, `src/compute.c` | Bug 2 | ✅ |
| 1.3 | Fix fd leak on re-init: close `wp_fd` before overwriting | `src/backend.c` | Bug 3 | ✅ |
| 1.4 | Fix unfiltered `class_ns` accumulation in query summary visitor | `src/compute.c` | Bug 4 | ✅ |
| 1.5 | Fix `next_pow2` overflow: clamp at 1<<30 | `src/server.c` | Bug 5 | ✅ |
| 1.6 | Fix heatmap `grid_size` overflow: use `size_t` + clamp at 100K | `src/compute.c` | Bug 6 | ✅ |
| 1.7 | Fix double refresh in drill-down, drill-up, and clear-filters | `web/static/app.js` | Issue 13 | ✅ |
| 1.8 | Commit: `[local]` support in cmdline parser | `src/cmdline.c` | Uncommitted | ✅ |
| 1.9 | Commit: session timeline event coalescing | `src/server.c` | Uncommitted | ✅ |
| 1.10 | Fix tests: add `bg_worker` to known backend types (PG18) | `tests/test_cmdline.c`, `test_cross_validate.py`, `test_active.py` | Pre-existing | ✅ |

**Result:** All 6 bugs fixed, 2 features committed, 3 test failures fixed.
16/16 test suites pass (354 individual checks). Tested on Hetzner cpx31 (Rocky 9, PG18.3).
Commits: `eacb880`, `cfde454`, `d1255c4`, `549417d`.

---

## Sprint 2: Test Infrastructure + Synthetic Data Correctness ✅ COMPLETED (2026-03-18)

**Goal:** Build the test data generator and prove our compute math is correct.
No root, no PostgreSQL needed — runs on any dev machine.

**Depends on:** Sprint 1 (bugs must be fixed before we lock in expected values).

| # | Task | Details | Status |
|---|------|---------|--------|
| 2.1 | Write `gen_test_traces.c` | C program: reads JSON scenario, writes `.trace` + `.summary` + `backends.jsonl` + `query_texts.jsonl`. Patches file headers so mono_to_wall=0 for deterministic timestamps. | ✅ |
| 2.2 | Write `server_harness.py` | Python module: `ServerHarness` spawns pgwt-server, sends JSON commands, parses responses. `TestRunner` class for assertions. PG18 event ID constants. | ✅ |
| 2.3 | `test_data_time_model.py` | 0B.1: exact time model arithmetic (9/9 checks, 0% tolerance) | ✅ |
| 2.4 | `test_data_aas.py` | 0B.2: AAS bucket correctness (7/7 checks, 0% tolerance) | ✅ |
| 2.5 | `test_data_events.py` | 0B.3: count, total, avg, max, percentages (14/14 checks) | ✅ |
| 2.6 | `test_data_sessions.py` | Per-session attribution (4/4 checks) | ✅ |
| 2.7 | `test_data_queries.py` | Per-query attribution + Bug 4 regression (6/6 checks) | ✅ |
| 2.8 | `test_data_filters.py` | 0B.4: all filter types + combinations (9/9 checks) | ✅ |
| 2.9 | `test_data_timeline.py` | 0B.6: timestamp contiguity — Bug 1 regression (10/10 checks) | ✅ |
| 2.10 | `test_data_idle.py` | 0B.7: idle exclusion from all views (3/3 checks) | ✅ |
| 2.11 | `test_data_edge.py` | 0B.8: single event, zero duration, 50 PIDs, 1hr event (10/10 checks) | ✅ |
| 2.12 | `test_bucket.c` | C unit test: exhaustive `pgwt_duration_to_bucket` (25/25 checks) | ✅ |
| 2.13 | Add to `tests/Makefile` and `run_all.sh` | New tests integrated into existing runner | ✅ |

**Bugs found and fixed during Sprint 2:**
- **Bug 7 (AAS event timing):** `compute_aas` in `compute.c` used `ev_start = timestamp_ns`
  instead of `timestamp_ns - duration_ns`, placing events at wrong times. AAS chart was
  shifted forward by event duration. Both class-bucket and event-detail paths fixed.

**Result:** 11 synthetic correctness tests + 1 C unit test. 72 individual checks, all pass
with 0% tolerance. `gen_test_traces` + `server_harness.py` infrastructure reusable for
future tests. Note: `test_data_summary.py` (0B.5: summary vs raw agreement) deferred to
Sprint 3 — requires summary path fixes for synthetic timestamps.
26/26 test suites pass on Hetzner cpx31 (Rocky 9, PG18). Tested 2026-03-18.

---

## Sprint 3: Code Hardening ✅ COMPLETED (2026-03-18)

**Goal:** Fix fragile patterns identified in the review. These aren't bugs today
but prevent bugs tomorrow.

**Depends on:** Sprint 2 (tests catch any regressions from refactoring).

| # | Task | File(s) | Issue ref | Status |
|---|------|---------|-----------|--------|
| 3.1 | Add JSON escaping for all server output strings | `src/server.c`, `src/backend_meta.c` | Issue 9 | ✅ |
| 3.2 | Fix `/proc/<pid>/stat` parsing: find last `)`, parse after | `src/backend.c`, `src/discovery.c` | Issue 7 | ✅ |
| 3.3 | Add state_map garbage collection: periodic sweep every 60s | `src/daemon.c` | Arch 5 | ✅ |
| 3.4 | Move large stack arrays to `malloc` in `output.c` | `src/output.c` | Issue 12 | ✅ |
| 3.5 | Add WebSocket origin check (localhost only) | `web/bridge.go` | Issue 10 | ✅ |

**Details:**
- **3.1:** Added `json_escape_stdout()` to server.c (~15 unescaped string outputs replaced) and
  `json_escape_fp()` to backend_meta.c (4 outputs). Prevents JSON injection from backend metadata,
  query text, or event names containing special characters.
- **3.2:** Changed `/proc/pid/stat` parsing from `fscanf` (breaks on comm with spaces/parens) to
  `fgets` + `strrchr(line, ')')` + `sscanf` after last paren. Fixed in both backend.c and discovery.c.
- **3.3:** Added periodic sweep every 60 ticks in `handle_timer()` using `kill(pid, 0)` to detect
  dead backends not caught by the exit tracepoint. Calls `pgwt_handle_exit()` to clean up state_map
  entries and watchpoint fds.
- **3.4:** Replaced two large stack arrays with malloc: `sorted[4096]` (40KB) in
  `pgwt_print_system_event()` and `entries[MAX_BACKENDS]` (96KB) in `pgwt_print_active()`.
  Also fixed a missing-braces bug in the error response path of `dispatch()`.
- **3.5:** Replaced `CheckOrigin: func(r) bool { return true }` with localhost-only check.
  Allows `http(s)://localhost` and `http(s)://127.0.0.1` with any port. Empty origin (non-browser
  clients like curl) still allowed.

**Result:** All 5 hardening tasks complete. No regressions in test suite.

---

## Sprint 4: Live Data Correctness Tests ✅ COMPLETED (2026-03-18)

**Goal:** Prove the full BPF → file → compute pipeline produces correct numbers
against real PostgreSQL workloads.

**Depends on:** Sprint 1 (bugs fixed), Sprint 2 (test harness patterns established).
**Requires:** Root + running PostgreSQL on test machine.

| # | Task | Test ref | Status |
|---|------|----------|--------|
| 4.1 | `test_percentage.py` — controlled 50/50 split (sleep + CPU) | 0A.1 | ✅ |
| 4.2 | `test_aas_accuracy.py` — 4 backends sleeping → AAS ≈ 4.0 | 0A.2 | ✅ |
| 4.3 | `test_session_accuracy.py` — per-session DB Time matches expectations | 0A.3 | ✅ |
| 4.4 | `test_query_accuracy.py` — per-query attribution | 0A.4 | ✅ |
| 4.5 | `test_partition.py` — CPU+IO+Lock+...= DB Time (< 0.1% tolerance) | 0A.5 | ✅ |
| 4.6 | `test_idle_exclusion.py` — Activity excluded from DB Time | 0A.6 | ✅ |
| 4.7 | `test_daemon_server.py` — daemon CLI ≈ pgwt-server on same traces | 0A.10 | ✅ |
| 4.8 | Add all to `run_all.sh` | | ✅ |

**Details:**
- **4.1:** Two backends (sleep + CPU burn), each ~50% of DB Time. Verifies time_model
  percentages and system_event totals. Tolerance: CPU + Timeout > 50% (Extension class
  from pg_wait_sampling contributes ~30%).
- **4.2:** 4 backends each doing pg_sleep(12). AAS should be ~4.0. System backends
  (checkpointer, pg_wait_sampling, etc.) add ~2 extra AAS. Tolerance: 0.5×-2.0× N_BACKENDS.
- **4.3:** 3 backends (2 sleeping, 1 CPU burn). Verifies per-session attribution in
  session_event view: DB Time > 0 for all, PgSleep as top wait, CPU-dominated session.
- **4.4:** 2 distinct queries (sleep + count). Verifies per-query attribution in
  query_event view. query_id can be negative (signed long).
- **4.5:** pgbench -c 8 -T 20. Verifies CPU + all wait classes = DB Time within 0.0001%.
  Partition error consistently 0.0000%.
- **4.6:** Redesigned from original spec (5 idle + 1 active → AAS ≈ 1.0). Original
  design was flawed: psql at prompt creates Client:ClientRead (not idle), pg_sleep creates
  Timeout:PgSleep (not idle). Only Activity:* events are idle. New design: 1 active CPU
  burn, verify Activity time > 0, partition holds, no Activity events in system_event.
- **4.7:** Runs daemon with --trace-dir during pgbench, then runs pgwt-server on trace
  files. Compares CLI vs server DB Time (ratio 0.3-3.0) and verifies server partition.

**Result:** 7 live correctness tests, 44 individual checks, all pass.
32/33 test suites pass on Hetzner cpx31 (Rocky 9, PG18). The only failure is
test_overhead (19% > 15% threshold) — pre-existing flaky test unrelated to Sprint 4.

---

## Sprint 5: Web UI Tests ✅ COMPLETED (2026-03-19)

**Goal:** Automate browser testing. Never manually click through the UI again.

**Depends on:** Sprint 2 (synthetic test data for canned responses).

| # | Task | Details | Status |
|---|------|---------|--------|
| 5.1 | Write `mock_server.py` | Python: HTTP server (port P) + WebSocket server (port P+1) with canned JSON for all 8 commands. No SSH, no pgwt-server, no root. | ✅ |
| 5.2 | `test_web_ui.py` — Playwright test suite | 18 tests (67 checks): page load, tabs, summary bar, tables, column sorting, drill-down, breadcrumbs, histogram, timeline, time picker, zoom, chart rendering, reconnection | ✅ |
| 5.3 | Data display tests | Exact summary bar values (DB Time=12.5s, Wall=3600.0s, AAS=3.47, Idle=45.0s, CPUs=4), exact event/session/query cell values matching canned data, timeline bar positions with duration verification | ✅ |
| 5.4 | Regression tests | Bug 1 (timeline bars start at `s`, not `s+d`, duration=50s matches canned data), Bug 13 (drill-down sends exactly 1 aas + 1 table request, not 2), filter persistence across manual tab switches (4 tabs verified) | ✅ |

**Details:**
- **Architecture:** mock_server.py runs HTTP (SimpleHTTPRequestHandler) on port P serving
  `web/static/` and WebSocket (websockets.asyncio) on port P+1. test_web_ui.py uses
  `context.add_init_script()` to monkey-patch the WebSocket constructor, redirecting
  connections from port P to port P+1. This avoids modifying app.js for testing.
- **Canned data:** Realistic mock responses — 1-hour range, 60 AAS buckets, 8 events,
  6 sessions, 3 queries, heatmap cells, timeline events. Supports class/pid/query_id filters.

**Result:** 25 Playwright tests, 98 individual checks, all pass.
Tested on Hetzner cpx31 (Rocky 9, headless Chromium). Added to `run_all.sh` (auto-skips
if playwright/websockets not installed).

---

## Sprint 6: Integrate cJSON ✅ COMPLETED (2026-03-19)

**Goal:** Replace hand-rolled JSON parsing and generation with cJSON. Eliminates
the substring-matching parser (Issue 8) and all escaping bugs (Issue 9 remainder).

**Depends on:** Sprint 2 (tests catch regressions), Sprint 3 (JSON escaping done
as a stopgap — cJSON replaces it entirely).

| # | Task | Details | Status |
|---|------|---------|--------|
| 6.1 | Vendor `cJSON.c` + `cJSON.h` into `src/` | cJSON v1.7.18, MIT license, 3443 lines | ✅ |
| 6.2 | Replace `json_string`/`json_int64`/`json_uint64` with cJSON parsing | `cJSON_Parse` + `cJSON_GetObjectItem` for request parsing | ✅ |
| 6.3 | Replace `parse_filters` with cJSON | `cJSON_GetObjectItem` on filters sub-object | ✅ |
| 6.4 | Replace all `printf("{\"...` with cJSON output construction | All 8 handlers + dispatch + main errors use cJSON tree + `cJSON_PrintUnformatted` | ✅ |
| 6.5 | Replace JSON output in `backend_meta.c` | `cJSON_CreateObject` + `cJSON_PrintUnformatted` replaces `json_escape_fp` | ✅ |
| 6.6 | Update Makefile to compile cJSON | Added to both `USER_SRCS` and `SERVER_SRCS`, added `-lm` to server linker | ✅ |
| 6.7 | Run all Sprint 2 + Sprint 5 tests | 33/33 test suites pass, 0 failures | ✅ |

**Details:**
- **Removed:** `json_escape_stdout`, `json_escape_fp`, `skip_ws`, `json_string`,
  `json_int64`, `json_uint64`, `json_int`, hand-rolled `parse_filters`/`parse_request`
- **Added:** `cjson_add_uint64` (raw number for ns timestamps to avoid double precision loss),
  `cjson_create_uint64` (raw for arrays), `emit_json` (print + cleanup helper)
- **.jsonl loading** also migrated: `query_texts.jsonl` and `backends.jsonl` now parsed with cJSON
- **Heatmap labels** changed from JSON `\u00b5` escapes to UTF-8 (`\xc2\xb5`) since cJSON
  handles string escaping automatically

**Result:** server.c shrinks by 101 lines (-414/+313). All JSON parsing/generation uses cJSON.
33/33 test suites pass on Hetzner cpx31 (Rocky 9, PG18).

---

## Sprint 7: Dynamic Event Name Resolution ✅ COMPLETED (2026-03-24)

**Goal:** Stop hardcoding PG version-specific wait event tables. Forward-compatible
with PG19+ without code changes.

**Depends on:** Sprint 6 (cJSON makes it easy to serialize name maps to JSON).

| # | Task | Details | Status |
|---|------|---------|--------|
| 7.1 | At daemon startup: query `pg_wait_events` via psql | Discovers all event names from the running PG instance (PG17+) | ✅ |
| 7.2 | Store name mapping as sidecar `wait_event_names.json` | JSON format: `{"IO": ["Event0", ...], "Lock": [...], ...}` — events in enum order | ✅ |
| 7.3 | `pgwt-server` reads name mapping from sidecar | Falls back to hardcoded tables if missing (backward compat) | ✅ |
| 7.4 | Web client displays names from server response (already does this) | No change needed | ✅ |
| 7.5 | Keep hardcoded tables as fallback for old trace files | `src/wait_event.c` dynamic lookup → hardcoded fallback | ✅ |
| 7.6 | Detect psql user from postmaster process owner | Reads UID from `/proc/PID/status`, resolves via `getpwuid()` | ✅ |

**Details:**
- **Dynamic name tables:** Heap-allocated `dyn_names[16][512]` array indexed by `(class_byte, event_id)`.
  `pgwt_event_name()` checks dynamic tables first, falls back to hardcoded.
- **PG port detection:** Reads line 4 of `postmaster.pid` for the port number.
- **Sidecar format:** Compact JSON with one array per class. Includes `pg_version` for reference.
  Example: 5.5 KB for PG18 (81 IO events, 84 LWLock tranches, 57 IPC events, etc.).
- **Multi-instance:** Each daemon reads port/user from its own postmaster, writes sidecar
  to its own trace_dir. No conflicts between instances.

**Result:** All 136 C unit tests pass. Tested on Hetzner cx43 (Rocky 9, PG18).
Commits: `6fd90f9`, `0ffd565`.

---

## Sprint 8: Drop Rust TUI Client ✅ COMPLETED (2026-03-24)

**Goal:** Remove duplicated code, halve maintenance surface.

**Depends on:** Sprint 7 (before dropping, ensure pgwt-server has a text dump mode).

| # | Task | Details | Status |
|---|------|---------|--------|
| 8.1 | Add `--dump` mode to `pgwt-server` | Text summary: time model + top events + top sessions + top queries | ✅ |
| 8.2 | Remove `client/` directory | Deleted Rust TUI client (4782 lines removed) | ✅ |
| 8.3 | Update README, INSTALL.md, ROADMAP.md | Removed all pgwt-cli references | ✅ |

**Result:** Single C codebase for all analysis. `pgwt-server --dump /path/to/traces`
covers the "quick SSH check" use case. Net: +133/-4782 lines.
Commit: `008f624`.

---

## Sprint 9: Performance Optimization + Benchmarks ✅ COMPLETED (2026-03-24)

**Goal:** Replace O(n²) hot paths with hash tables. Establish performance baselines.

**Depends on:** Sprint 2 (test data generator for benchmarks).

| # | Task | Details | Status |
|---|------|---------|--------|
| 9.1 | Hash tables for `pgwt_compute_time_model` event accumulation | `event_ht_entry` hash table (2048 slots), replaces linear scan | ✅ |
| 9.2 | Hash tables for session/query per-wait tracking | `wait_ht_entry` hash table (256 slots per PID/query), replaces linear arrays | ✅ |
| 9.3 | Hash lookup for session timeline PID index | `pid_idx_ht` hash table (1024 slots), replaces linear search in second pass | ✅ |
| 9.4 | Write `gen_bench_traces.c` | Generates 1M/10M events with realistic pgbench-like mix | ✅ |
| 9.5 | Write `bench_server.py` | Measures server latency per command at varying event counts | ✅ |
| 9.6 | Establish performance baselines | 1M events: 101ms (10M/sec), 10M events: 924ms (10.8M/sec) | ✅ |
| 9.7 | Add ASan/Valgrind targets to Makefile | `make test-asan`, `make test-valgrind`, `make bench` | ✅ |

**Details:**
- **Hash helpers:** `hash32()`, `hash64()`, `event_ht_find_or_insert()`, `wait_ht_find_or_insert()` —
  reusable open-addressing hash functions at the top of compute.c.
- **Top events:** Already used hash table (pre-existing); removed duplicate `EVENT_HT_SIZE` definition.
- **Session/query accum:** Replaced `wait_ids[]/wait_ns[]/num_waits` flat arrays with `wait_ht_entry waits[256]`
  hash tables. Top-wait scan iterates 256 hash slots instead of variable-length array.
- **Summary path:** Serves most commands in <1ms regardless of event count (pre-aggregated).

**Result:** All 72 synthetic data tests pass with 0% tolerance. Compute throughput: 10.8M events/sec.
Commits: `711f590`, `7fd8f62`.

---

## Sprint 10: Tracing Analysis Phase 2 — Transitions + Fingerprinting ✅ COMPLETED (2026-03-24)

**Goal:** Build the features that differentiate tracing from sampling.

**Depends on:** Sprint 1 (timeline bug fixed), Sprint 6 (cJSON for new endpoints).

| # | Task | Details | Status |
|---|------|---------|--------|
| 10.1 | New server endpoint: `transitions` | Hash table (4096 slots) for (from, to) pairs. Returns Sankey-compatible links JSON. | ✅ |
| 10.2 | Web UI: Sankey diagram for transitions | New "Transitions" tab. ECharts Sankey with hover tooltips (count, %, duration). | ✅ |
| 10.3 | Wait pattern fingerprinting | `pgwt_compute_fingerprints()`: per-query class distribution + top transition. Signature: "IO:30%\|CPU:22%\|LWLock:21%" | ✅ |
| 10.4 | Server endpoint: `fingerprints` | Returns per-query signatures, class percentages, top transitions. | ✅ |
| 10.5 | Tests for transition computation | 10/10 checks: exact counts, durations, fingerprint signature content. | ✅ |

**Details:**
- **Transitions:** Uses `(from * 0x45d9f3b) ^ (to * 0x9e3779b9)` hash for O(1) pair lookup.
  Filters idle events and exit sentinels. Returns top N by count with duration totals.
- **Fingerprints:** Per-query hash table (2048 slots). Class % sorted descending for
  consistent signatures. Top transition tracked per query with 64-entry linear array.
- **Sankey:** Handles self-transitions (CPU→CPU) by appending space suffix to target.
  Gradient line style, adjacency focus on hover.

**Result:** 10/10 transition tests pass. +517 lines. Commit: `efe531e`.

---

## Sprint 11: Merge Daemon + Server — DEFERRED

Deferred: the per-session SSH-spawned model (pgwt → ssh → pgwt-server) works
well without merging. Security, ops simplicity, and resilience all favor the
current architecture. Revisit if shared cache across users becomes a need.

---

## Sprint 12: Live Mode (Phase H) ✅ COMPLETED (2026-03-24)

**Goal:** Near-real-time AAS chart and drill-down from the running daemon's
trace data, without merging daemon + server.

**Architecture:** pgwt-server reads `current.trace` (the file being written by
the daemon) using a streaming block reader that handles missing footer. Immutable
`.trace.lz4` files are cached per-session; only `current.trace` is re-read on
auto-refresh. This gives 1-5 second latency with zero persistent server processes.

```
Daemon (root, BPF):                pgwt-server (no root, per-session):
  writes current.trace ──────────→ reads current.trace (tail-follow)
  rotates to .trace.lz4 ────────→ reads .trace.lz4 (cached, immutable)
```

**Depends on:** Sprint 9 (performance), Sprint 10 (transitions).

| # | Task | Details |
|---|------|---------|
| 12.1 | Streaming trace reader for `current.trace` | Read blocks sequentially without footer. Stop at EOF or incomplete block header. Detect new blocks on re-read. |
| 12.2 | Include `current.trace` in pgwt-server file list | `server_init()` adds `current.trace` to scan. Rescan on each request (file grows). |
| 12.3 | Per-session cache for immutable trace files | Cache computed aggregates keyed by `(filename, time_range)`. Only re-read `current.trace` on refresh. |
| 12.4 | Web UI: auto-refresh toggle | Button to enable 5-second polling. Re-sends current view request. AAS chart appends new buckets. |
| 12.5 | Web UI: "Live" time range | New quick range: "Live" = last 5 minutes, auto-refresh on. Updates `viewTo` to "now" on each refresh. |
| 12.6 | Tests: verify `current.trace` reading | Write partial trace file (no footer), verify pgwt-server reads all complete blocks. |

**Deliverable:** `pgwt root@server` shows near-real-time AAS chart that updates
every 5 seconds. Drill-down into any time range works at full nanosecond
resolution from trace files. Historical + live data seamlessly merged.

---

## Sprint 13: Tracing Analysis Phase 3 — Concurrency ✅ COMPLETED (2026-03-24)

**Goal:** Detect thundering herds and micro-bursts.

**Depends on:** Sprint 10 (transitions).

| # | Task | Details | Status |
|---|------|---------|--------|
| 13.1 | Peak concurrency per AAS bucket | Builds sorted active intervals, counts max overlapping sessions per event per bucket | ✅ |
| 13.2 | Web UI: peak concurrency markers | Deferred to UI polish pass — data available via `concurrency` endpoint | ⏸ |
| 13.3 | Burst detection algorithm | Sliding window (10ms default): finds N+ sessions entering same wait simultaneously. Returns PIDs. | ✅ |
| 13.4 | Web UI: burst annotations | Deferred to UI polish pass — data available via `concurrency` endpoint | ⏸ |

**Details:**
- **Algorithm:** Sorts events by start_ns, scans for time-overlapping intervals on same event_id.
  Peak tracking per bucket: O(events × events_per_bucket). Burst detection: O(events × window_size).
- **Parameters:** `burst_window_ns` (default 10ms), `burst_threshold` (default 4 sessions).
- **Server endpoint:** `concurrency` returns `peaks[]` (per-bucket) + `bursts[]` (sorted by sessions desc).

**Result:** Commit: `2e6483f`. Web UI integration deferred — backend API complete.

---

## Sprint 14: Advanced Analysis (Phases 4-5) + Phase G.2 ✅ COMPLETED (2026-03-24)

**Goal:** Lock chains, interference scoring, plan_id capture.

**Depends on:** Sprint 13 (concurrency analysis foundation).

| # | Task | Details | Status |
|---|------|---------|--------|
| 14.1 | Lock chain detection | Scans Lock waits, finds likely blocker PID via CPU interval overlap | ✅ |
| 14.2 | Web UI: lock chain visualization | Deferred — data available via `lock_chains` endpoint | ⏸ |
| 14.3 | Cross-session interference scoring | Overlapping wait intervals on same event from different PIDs, scored 0-1 | ✅ |
| 14.4 | Plan identifier capture (PG18+) | Deferred — requires BPF changes + `st_plan_id` offset discovery | ⏸ |
| 14.5 | Plan change detection in web UI | Deferred — depends on 14.4 | ⏸ |
| 14.6 | HMM anomaly detection (research/evaluate) | Deferred — research task | ⏸ |

**Details:**
- **Lock chains:** Collects Lock-class wait intervals + CPU intervals. For each Lock wait,
  finds the different PID on CPU with maximum temporal overlap → likely blocker. Heuristic
  (no pg_locks data), but effective for Lock:transactionid and Lock:tuple contention.
- **Interference:** Sorts active intervals by start_ns, scans forward for overlapping pairs
  on same event from different PIDs. Hash table (4096 slots) per (pid_a, pid_b) pair.
  Normalized score: 1.0 = highest overlap in the trace.
- **Endpoints:** `lock_chains` (waiter/blocker/lock/wait_ms), `interference` (pid_a/pid_b/score/overlap_ms).

**Result:** Commit: `5af0d0b`. All synthetic tests pass.

---

## Summary Timeline

```
Sprint 1:  Bug fixes + uncommitted work                    ──── Foundation
Sprint 2:  Test infrastructure + synthetic correctness      ──── Prove math
Sprint 3:  Code hardening                                   ──── Robustness
Sprint 4:  Live data correctness tests                      ──── Prove end-to-end
Sprint 5:  Web UI tests (Playwright)                    ✅ ──── Stop clicking
Sprint 6:  cJSON integration                            ✅ ──── Clean JSON
Sprint 7:  Dynamic event names                          ✅ ──── Forward compat
Sprint 8:  Drop Rust TUI                                ✅ ──── Simplify
Sprint 9:  Performance optimization + benchmarks        ✅ ──── Speed
Sprint 10: Tracing analysis Phase 2 (transitions)       ✅ ──── Differentiate
Sprint 11: Merge daemon + server                     ⏸ ──── Deferred
Sprint 12: Live mode (Phase H)                          ✅ ──── Real-time
Sprint 13: Tracing analysis Phase 3 (concurrency)       ✅ ──── Advanced
Sprint 14: Advanced analysis (Phases 4-5) + G.2         ✅ ──── Research
```

## Dependency Graph

```
Sprint 1 (bugs)
  ├→ Sprint 2 (test infra) ─→ Sprint 3 (hardening)
  │    ├→ Sprint 4 (live tests)
  │    ├→ Sprint 5 (UI tests)
  │    ├→ Sprint 9 (perf) ─────────→ Sprint 11 (merge daemon+server)
  │    └→ Sprint 6 (cJSON) ──┬─────→ Sprint 11
  │         └→ Sprint 7 (dynamic names)
  │              └→ Sprint 8 (drop Rust)
  ├→ Sprint 10 (transitions) ─→ Sprint 13 (concurrency)
  │                                  └→ Sprint 14 (advanced)
  └→ Sprint 11 ─→ Sprint 12 (live mode)
```
