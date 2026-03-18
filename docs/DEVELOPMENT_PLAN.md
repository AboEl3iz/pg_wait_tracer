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

## Sprint 3: Code Hardening

**Goal:** Fix fragile patterns identified in the review. These aren't bugs today
but prevent bugs tomorrow.

**Depends on:** Sprint 2 (tests catch any regressions from refactoring).

| # | Task | File(s) | Issue ref |
|---|------|---------|-----------|
| 3.1 | Add JSON escaping for all server output strings | `src/server.c`, `src/backend_meta.c` | Issue 9 |
| 3.2 | Fix `/proc/<pid>/stat` parsing: find last `)`, parse after | `src/backend.c`, `src/discovery.c` | Issue 7 |
| 3.3 | Add state_map garbage collection: periodic sweep every 60s | `src/daemon.c`, `src/backend.c` | Arch 5 |
| 3.4 | Move large stack arrays to `malloc` in `output.c` | `src/output.c` | Issue 12 |
| 3.5 | Add WebSocket origin check (localhost only) | `web/bridge.go` | Issue 10 |

**Deliverable:** All fragile patterns fixed. Re-run Sprint 2 tests to verify
no regressions.

---

## Sprint 4: Live Data Correctness Tests

**Goal:** Prove the full BPF → file → compute pipeline produces correct numbers
against real PostgreSQL workloads.

**Depends on:** Sprint 1 (bugs fixed), Sprint 2 (test harness patterns established).
**Requires:** Root + running PostgreSQL on test machine.

| # | Task | Test ref |
|---|------|----------|
| 4.1 | `test_percentage.py` — controlled 50/50 split (sleep + CPU) | 0A.1 |
| 4.2 | `test_aas_accuracy.py` — 4 backends sleeping → AAS ≈ 4.0 | 0A.2 |
| 4.3 | `test_session_accuracy.py` — per-session DB Time matches expectations | 0A.3 |
| 4.4 | `test_query_accuracy.py` — per-query attribution | 0A.4 |
| 4.5 | `test_partition.py` — CPU+IO+Lock+...= DB Time (< 0.1% tolerance) | 0A.5 |
| 4.6 | `test_idle_exclusion.py` — 5 idle + 1 active → AAS ≈ 1.0 | 0A.6 |
| 4.7 | `test_daemon_server.py` — daemon CLI ≈ pgwt-server on same traces | 0A.10 |
| 4.8 | Add all to `run_all.sh` | |

**Deliverable:** 7 live correctness tests. We can prove end-to-end that if we
say IO is 10%, it really is 10%.

---

## Sprint 5: Web UI Tests

**Goal:** Automate browser testing. Never manually click through the UI again.

**Depends on:** Sprint 2 (synthetic test data for canned responses).

| # | Task | Details |
|---|------|---------|
| 5.1 | Write `mock_server.py` | Python: serves `web/static/` over HTTP + WebSocket endpoint returning canned JSON responses keyed by command type. No SSH, no pgwt-server. |
| 5.2 | `test_web_ui.py` — Playwright test suite | Page load, tab navigation, drill-down flow, breadcrumb, chart zoom, column sorting, filter persistence, reconnection |
| 5.3 | Data display tests | Summary bar numbers, table rows, timeline bar positions match canned data |
| 5.4 | Regression tests | Bug 1 (timeline position), Bug 13 (double refresh), filter state across tabs |

**Deliverable:** Playwright test suite. `python3 tests/test_web_ui.py` exercises
the full UI without root, PG, or SSH.

---

## Sprint 6: Integrate cJSON

**Goal:** Replace hand-rolled JSON parsing and generation with cJSON. Eliminates
the substring-matching parser (Issue 8) and all escaping bugs (Issue 9 remainder).

**Depends on:** Sprint 2 (tests catch regressions), Sprint 3 (JSON escaping done
as a stopgap — cJSON replaces it entirely).

| # | Task | Details |
|---|------|---------|
| 6.1 | Vendor `cJSON.c` + `cJSON.h` into `src/` | Single .c/.h, MIT license, ~1000 lines |
| 6.2 | Replace `json_string`/`json_int64`/`json_uint64` with cJSON parsing | `src/server.c` request parsing |
| 6.3 | Replace `parse_filters` with cJSON | `src/server.c` filter parsing |
| 6.4 | Replace all `printf("{\"...` with cJSON output construction | `src/server.c` response generation |
| 6.5 | Replace JSON output in `backend_meta.c` | `src/backend_meta.c` |
| 6.6 | Update Makefile to compile cJSON | `Makefile` |
| 6.7 | Run all Sprint 2 + Sprint 5 tests | Verify no regressions |

**Deliverable:** All JSON parsing/generation uses cJSON. `server.c` shrinks
significantly. Entire class of JSON bugs eliminated.

---

## Sprint 7: Dynamic Event Name Resolution

**Goal:** Stop hardcoding PG version-specific wait event tables. Forward-compatible
with PG19+ without code changes.

**Depends on:** Sprint 6 (cJSON makes it easy to serialize name maps to JSON).

| # | Task | Details |
|---|------|---------|
| 7.1 | At daemon startup: run `psql -tAc "SELECT type, name FROM pg_wait_events"` | Capture all event names from the running PG instance |
| 7.2 | Store name mapping in trace file header (or sidecar `.names.json`) | Format: `{"event_id": "Class:Name", ...}` |
| 7.3 | `pgwt-server` reads name mapping from trace file/sidecar | Falls back to hardcoded tables if missing (backward compat) |
| 7.4 | Web client displays names from server response (already does this) | No change needed |
| 7.5 | Keep hardcoded tables as fallback for old trace files | `src/wait_event.c` stays but is only used when no name file exists |
| 7.6 | Update tests to verify dynamic names | |

**Deliverable:** Daemon discovers event names at startup. New trace files include
name mapping. PG19 works out of the box.

---

## Sprint 8: Drop Rust TUI Client

**Goal:** Remove duplicated code, halve maintenance surface.

**Depends on:** Sprint 7 (before dropping, ensure pgwt-server has a text dump mode).

| # | Task | Details |
|---|------|---------|
| 8.1 | Add `--dump` mode to `pgwt-server` | Reads trace files, prints text summary to stdout (time_model + top events + top sessions). Replaces `pgwt-cli --dump`. |
| 8.2 | Remove `client/` directory | Delete Rust TUI client (~75K lines including target/) |
| 8.3 | Update README, INSTALL.md, ROADMAP.md | Remove references to pgwt-cli |

**Deliverable:** Single codebase for all analysis. `pgwt-server --dump` covers
the "quick SSH check" use case.

---

## Sprint 9: Performance Optimization + Benchmarks

**Goal:** Replace O(n²) hot paths with hash tables. Establish performance baselines.

**Depends on:** Sprint 2 (test data generator for benchmarks).

| # | Task | Details |
|---|------|---------|
| 9.1 | Hash tables for `pgwt_compute_time_model` event accumulation | Replace linear scan with open-addressing hash (Issue 11) |
| 9.2 | Hash tables for session/query per-wait tracking | `compute.c:~599, ~715` (Issue 11) |
| 9.3 | Hash lookup for session timeline PID index | `server.c` (Issue 11) |
| 9.4 | Write `gen_bench_traces.c` | Generate 1M/10M event trace files |
| 9.5 | Write `bench_server.py` | Measure compute throughput and server response latency |
| 9.6 | Establish performance baselines | Save results, track across commits |
| 9.7 | Add ASan/Valgrind targets to Makefile | `make test-asan`, `make test-valgrind` |

**Deliverable:** O(n) compute for all hot paths. Performance baselines documented.
Memory safety verified under ASan.

---

## Sprint 10: Tracing Analysis Phase 2 — Transitions + Fingerprinting

**Goal:** Build the features that differentiate tracing from sampling.

**Depends on:** Sprint 1 (timeline bug fixed), Sprint 6 (cJSON for new endpoints).

| # | Task | Details |
|---|------|---------|
| 10.1 | New server endpoint: `transitions` | Compute NxN state transition matrix for query_id/PID. Returns top transitions with probabilities. |
| 10.2 | Web UI: Sankey/chord diagram for transitions | New tab or drill-down from Queries tab. ECharts Sankey. |
| 10.3 | Wait pattern fingerprinting | Per-query_id transition signature. Compare across time windows. |
| 10.4 | Web UI: "pattern changed" indicator in Queries tab | Click to see before/after fingerprint diff. |
| 10.5 | Tests for transition computation | Synthetic events → verify transition matrix is correct |

**Deliverable:** Wait transition Sankey diagrams. Plan change detection from wait
event shapes. Features no sampling tool can offer.

---

## Sprint 11: Merge Daemon + Server

**Goal:** Single process for capture + serving. Enables live mode.

**Depends on:** Sprint 6 (cJSON), Sprint 9 (performance work done). This is the
biggest architectural change — all stabilization should be done first.

| # | Task | Details |
|---|------|---------|
| 11.1 | Add Unix socket listener to daemon | Accept client connections on `/var/run/pgwt/pgwt.sock` |
| 11.2 | Handle JSON requests in daemon event loop | Reuse `compute.c` functions. Serve from in-memory events + trace files. |
| 11.3 | Go client: connect to Unix socket (via SSH) instead of spawning pgwt-server | `web/bridge.go`: `ssh host "socat UNIX:/var/run/pgwt/pgwt.sock -"` or direct stdin/stdout forwarding |
| 11.4 | Keep `pgwt-server` standalone for offline replay | Compile separately, reads trace files without daemon |
| 11.5 | Event decoded cache: decode trace blocks once, reuse across requests | LRU cache keyed by (file, block_offset) |
| 11.6 | Tests: verify daemon-served responses match pgwt-server responses | Same synthetic data, both paths, compare output |

**Deliverable:** Web client connects to running daemon. No startup delay.
Historical + live data in same process. Foundation for live streaming.

---

## Sprint 12: Live Mode (Phase H)

**Goal:** Real-time streaming from daemon to web UI.

**Depends on:** Sprint 11 (daemon serves client connections).

| # | Task | Details |
|---|------|---------|
| 12.1 | Live AAS push: daemon sends AAS update every second | On timer tick, compute AAS for last N seconds, push to connected clients |
| 12.2 | Web UI: live/historical toggle | Switch between live streaming and historical trace file analysis |
| 12.3 | Active sessions view in web UI | Reads BPF state_map via daemon, shows real-time backend states |
| 12.4 | Live event stream for session timeline | New events appear in real-time as colored bars extending right |

**Deliverable:** `pgwt root@server` shows real-time AAS chart + active sessions.
Historical drill-down works seamlessly alongside live view.

---

## Sprint 13: Tracing Analysis Phase 3 — Concurrency

**Goal:** Detect thundering herds and micro-bursts.

**Depends on:** Sprint 10 (transitions), Sprint 11 (daemon-served for live detection).

| # | Task | Details |
|---|------|---------|
| 13.1 | Peak concurrency per AAS bucket | Track max simultaneous sessions per wait state within each time bucket |
| 13.2 | Web UI: peak concurrency markers on AAS chart | Secondary axis or annotation markers |
| 13.3 | Burst detection algorithm | Sliding window: N sessions enter same wait within <10ms |
| 13.4 | Web UI: burst annotations on AAS timeline | Clickable → session timeline for affected PIDs |

**Deliverable:** "43 sessions hit buffer_mapping simultaneously at 10:04:05" —
detected automatically, visible on the chart.

---

## Sprint 14: Advanced Analysis (Phases 4-5) + Phase G.2

**Goal:** Lock chains, interference scoring, plan_id capture.

**Depends on:** Sprint 13 (concurrency analysis foundation).

| # | Task | Details |
|---|------|---------|
| 14.1 | Lock chain detection | Scan overlapping Lock waits, reconstruct A→B→C chains |
| 14.2 | Web UI: lock chain visualization | New Locks tab or overlay in session timeline |
| 14.3 | Cross-session interference scoring | Temporal correlation between PIDs |
| 14.4 | Plan identifier capture (PG18+) | Read `st_plan_id` from shared memory, store in trace events |
| 14.5 | Plan change detection in web UI | Highlight when same query_id has multiple plan_ids |
| 14.6 | HMM anomaly detection (research/evaluate) | Train on "normal" transitions, alert on deviation |

**Deliverable:** Lock chain visualization, noisy-neighbor detection, plan
regression identification. Research verdict on HMM approach.

---

## Summary Timeline

```
Sprint 1:  Bug fixes + uncommitted work                    ──── Foundation
Sprint 2:  Test infrastructure + synthetic correctness      ──── Prove math
Sprint 3:  Code hardening                                   ──── Robustness
Sprint 4:  Live data correctness tests                      ──── Prove end-to-end
Sprint 5:  Web UI tests (Playwright)                        ──── Stop clicking
Sprint 6:  cJSON integration                                ──── Clean JSON
Sprint 7:  Dynamic event names                              ──── Forward compat
Sprint 8:  Drop Rust TUI                                    ──── Simplify
Sprint 9:  Performance optimization + benchmarks            ──── Speed
Sprint 10: Tracing analysis Phase 2 (transitions)           ──── Differentiate
Sprint 11: Merge daemon + server                            ──── Architecture
Sprint 12: Live mode (Phase H)                              ──── Real-time
Sprint 13: Tracing analysis Phase 3 (concurrency)           ──── Advanced
Sprint 14: Advanced analysis (Phases 4-5) + G.2             ──── Research
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
