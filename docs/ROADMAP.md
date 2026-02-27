# pg_wait_tracer — Development Roadmap

This document tracks completed work and planned features. Each phase builds
on the previous one. Items marked **DONE** are merged to master.

## Completed Phases

### Phase 1–8: Core Tracer (DONE, tag v0.2)

CLI redesign, 6 diagnostic views, multi-window support, auto-discovery.

- Hardware watchpoint on `my_wait_event_info` per backend
- BPF ring buffer with raw event streaming
- Accumulator with 6 views: time_model, system_event, session_event,
  histogram, query_event, active
- Multi-window time comparisons (`--window 5s,1m,5m`)
- Auto-detect single PG instance
- TUI and text output modes (auto-detect TTY)

### Phase A: BPF Ring Buffer Dual-Path (DONE)

Raw event streaming alongside aggregated maps. Foundation for trace recording.

### Phase B: Accumulator + 6 Views (DONE)

In-memory accumulation of ring buffer events into the 6 diagnostic views.

### Phase C: Event File Writer + Reader + Replay (DONE)

- Columnar LZ4-compressed trace files (4096 events/block, ~36x compression)
- Block index footer for time-range binary search
- Hourly rotation, configurable retention (default 24h)
- Offline replay mode (`--replay`)

### Phase D: Daemon Mode (DONE)

- Persistent monitoring with `--daemon` flag
- Automatic PostgreSQL restart detection (postmaster health check)
- BPF destroy+reload on PG restart (rodata is immutable after load)
- PGDATA inference from `/proc/<pid>/cwd`

### Phase E.0: Investigation Client — Foundation (DONE)

Rust TUI (`pgwt-cli`) for interactive trace file analysis.

- **Trace file reader** (Rust): reads `.trace.lz4` files (header, footer,
  block index, LZ4 decompression, columnar decode)
- **Streaming reader** for `current.trace` (no footer — scans blocks
  sequentially, handles truncated blocks gracefully)
- **4 table views**: Overview (time_model), Top Events, Top Sessions,
  Top Queries
- **Wait event name resolution**: full PG18 event name tables ported to Rust
- **`--dump` mode**: non-interactive summary to stdout
- **`--generate-test`**: synthetic test data generator with realistic
  OLTP workload patterns (10 sessions, 6 roles, time phases, burst/idle)

### Phase E.1: AAS Stacked Bar Chart (DONE)

The centerpiece visual — Average Active Sessions over time.

- **AAS bucketing**: per-bucket, per-wait-class accumulation with
  overlap-aware duration splitting
- **Half-block rendering**: `'▄'` technique for 2x vertical resolution,
  works on any terminal
- **Pixel-perfect rendering**: via `ratatui-image` (iTerm2, Sixel, Kitty
  graphics protocols), auto-detected at startup
- **Transparent background**: RGBA images blend with terminal theme
- **Chart height**: 20 rows (40 virtual rows in half-block mode)
- **Color palette**: 11 wait classes with distinct colors
  (CPU=green, IO=blue, Lock=red, LwLock=pink, IPC=cyan, etc.)
- **Decorations**: auto-scaled Y-axis, HH:MM:SS X-axis, inline legend

---

## Planned: Phase E.2 — Query Text Capture

**Goal**: Show the actual SQL text alongside `query_id` in the investigation
client, without requiring pg_stat_statements lookups.

**How it works**: The daemon reads `PgBackendStatus.st_activity` from shared
memory via `process_vm_readv()` when it first sees a new `query_id`. This
gives the actual running SQL (not normalized — real parameter values), which
is more useful for debugging than `pg_stat_statements`' normalized form.

### Daemon side

New module: `src/query_text.c`

1. **Discover `st_activity` offset** in `PgBackendStatus`:
   - DWARF debug symbols (preferred): parse `readelf` output
   - Known offset table: PG17/18 x86_64 fallback
   - Same approach as existing `st_query_id` offset discovery

2. **Discover `BackendStatusArray` base address**:
   - ELF symbol resolution (already in `src/discovery.c`)
   - `BackendStatusArray` is a global pointer to an array of
     `PgBackendStatus` structs in shared memory

3. **Track seen query_ids**: hash set in daemon memory

4. **Capture logic** (on each timer tick):
   - For each backend with a `query_id` not in the seen set:
     - `process_vm_readv()` to read `st_activity` (1024 bytes,
       `PGBE_ACTIVITY_SIZE`, null-terminated)
     - Write `{query_id, text, timestamp}` to sidecar file
     - Add `query_id` to seen set

5. **Sidecar file**: `query_texts.jsonl` in trace directory
   ```json
   {"query_id":1234567890,"text":"SELECT * FROM accounts WHERE aid = 42837","ts":1709012345000000000}
   ```
   - One line per unique `query_id`
   - Rotated alongside trace files (same retention policy)

### Client side

New module: `client/src/query_text.rs`

1. Load `query_texts.jsonl` from trace directory
2. Build `HashMap<u64, String>` (query_id -> text)
3. Display truncated text (first 80 chars) in queries table
4. Full text on drill-down or popup

### Offset discovery

- `BackendStatusArray + backend_index * sizeof(PgBackendStatus) + st_activity_offset`
- Backend index = `MyBackendId - 1`
- `st_activity` is `char[1024]` — null-terminated C string

---

## Planned: Phase E.3 — Plan Identifier Capture (PG18+)

**Goal**: Capture the execution plan hash per-backend to detect plan changes
correlated with wait event spikes (plan regression detection).

**Background**: PostgreSQL 18 added `int64 st_plan_id` to `PgBackendStatus`
in shared memory (right after `st_query_id`). Core PG does **not** compute
it automatically — it stays 0 unless a `planner_hook` extension populates it.
There is no `compute_plan_id` GUC. On PG17, `st_plan_id` does not exist.

### PG version matrix

| PG Version | `st_query_id` | `st_plan_id` | Notes |
|------------|:---:|:---:|---|
| 14–16 | Exists but unreliable | N/A | `my_wait_event_info` not writable |
| 17 | `uint64` | Does not exist | Full wait tracing works |
| 18+ | `int64` | `int64` (new) | Plan ID requires planner_hook extension |

### Daemon side

1. **Discover `st_plan_id` offset** in `PgBackendStatus`:
   - DWARF: same approach as `st_query_id` / `st_activity`
   - Known offset: immediately after `st_query_id` (offset + 8)
   - PG17: skip (field doesn't exist)

2. **Read alongside `st_query_id`**: extend existing `process_vm_readv`
   call by 8 bytes — essentially free, no extra syscall

3. **Store in trace events**: add `plan_id` column to columnar block format
   - Bump trace file version (v1 -> v2)
   - Reader handles both versions gracefully

### Client side

1. Show `plan_id` column in queries view (when non-zero)
2. Plan change detection: highlight when same `query_id` has multiple
   `plan_id` values over time
3. Investigation flow: wait time spike -> drill to query -> see `plan_id`
   changed at the spike -> plan regression confirmed

### Requirements for non-zero plan_id

Users must install a `planner_hook` extension:
- **pg_stat_sql_plans** — computes plan hash, stores plan-level stats
- **pg_store_plans** — stores actual plan text indexed by (queryid, planid)
- Custom extension using `planner_hook` + `result->planId = hash`

Without such an extension, `st_plan_id` is always 0 and the feature is
silently inactive. Document this clearly.

---

## Planned: Phase E.4 — Drill-Down & Filter System

**Goal**: Turn pgwt-cli from a static viewer into an interactive
investigation tool with Oracle ASH-style filter+pivot workflow.

### Drill-down chain

```
Overview (time_model)
  -> Enter on a wait class
  -> Events view filtered to that class
    -> Enter on an event
    -> Sessions view filtered to that event
      -> Enter on a session
      -> Queries view filtered to that session
        -> Enter on a query
        -> Histogram for that query+event combination
```

Esc pops back one level. Full history stack maintained.

### Filter system

- `/` opens filter input (text match on event name, PID, query_id)
- `\` clears all filters
- Filters are cumulative (AND logic)
- Active filters shown in status bar: `Filters: class=Lock > event=Lock:Transaction`

### Time navigation

- `[` / `]` shift time window left/right
- `+` / `-` zoom in/out
- Chart click to zoom to a specific time range

### Additional views

- **Activity Over Time**: split time range into 10 slots, show AAS + top 3
  events per slot (identify WHEN the problem happened)
- **Histogram**: latency distribution with 16 log2 buckets for current
  filter context

### Sort

- `Tab` cycles sort column in current view

---

## Planned: Phase E.5 — Live Mode

**Goal**: Connect pgwt-cli to a running daemon for real-time streaming.

- Unix socket protocol: JSON request -> length-prefixed response frames
- Active view works only in live mode (requires BPF state_map)
- Dual mode: historical (trace files) + live (socket) in same TUI
- Separate from investigation workflow — additive feature

---

## Architecture Notes

### Hardware watchpoint approach

pg_wait_tracer uses CPU debug registers to set a hardware watchpoint on
the `my_wait_event_info` global variable. PostgreSQL 17+ writes wait events
through `*my_wait_event_info` (pointer dereference), which the watchpoint
captures. This gives exact, non-sampled event transitions at nanosecond
precision with ~5% TPS overhead.

### BPF constraints

- BPF rodata is immutable after skeleton load — must destroy+reload on
  PG restart (daemon mode)
- Hardware watchpoints limited to ~4 per CPU — kill stale processes
  between test runs
- `pgwt_daemon` struct is ~27MB — heap-allocated

### Trace file design

- Columnar blocks (4096 events), LZ4 compressed (~36x ratio)
- Block index footer for binary search by timestamp
- `current.trace` has no footer while being written — streaming reader
  scans blocks sequentially
- Hourly rotation to `YYYY-MM-DD_HH.trace.lz4`
- Configurable retention (default 24h)

### Client architecture

- Client-side compute: reads trace files directly, no daemon needed
  for historical analysis
- In-memory event store with on-the-fly aggregation
- Dual rendering: pixel-perfect (iTerm2/Sixel/Kitty) with half-block
  Unicode fallback
- Tech stack: Rust + ratatui + crossterm + lz4_flex + chrono + clap +
  image + ratatui-image
