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

## PRIORITY: Phase F — Web Investigation Client

**Goal**: Oracle ASH / RDS Performance Insights-class interactive
investigation tool with a rich web UI, running on the DBA's laptop and
connecting to the DB server over SSH.

### Architecture

```
[DBA laptop]                              [DB server]
pgwt (Go binary)                          pgwt-server (C binary)
  ├─ spawns: ssh user@host pgwt-server    ├─ reads trace files (reuses event_reader.c)
  ├─ localhost HTTP server (net/http)     ├─ computes aggregates (reuses replay.c,
  ├─ WebSocket bridge: browser ↔ SSH      │   map_reader.c, wait_event.c)
  ├─ static assets (//go:embed)           └─ JSON lines on stdin/stdout
  └─ auto-opens browser
```

**Why this architecture**:
- Like git/rsync/mosh — spawns real `ssh`, inherits user's `~/.ssh/config`,
  agent, ProxyJump, known_hosts. Zero auth code.
- Server side is pure C — reuses existing trace reader and compute code,
  no new runtime dependencies on DB server
- Client side is Go — single static binary, stdlib HTTP/embed, trivial
  cross-compilation (macOS/Linux/Windows)
- Web frontend (ECharts + JS) gives full interactive charts, mouse
  interaction, tooltips — all the UX patterns that terminal can't do

**Usage**:
```bash
pgwt ssh root@db-server /var/lib/pgwt/traces
# Browser opens http://localhost:8384 automatically
```

### F.1: Server side — `pgwt-server` (C)

New binary: `src/server.c` (+ `src/server_compute.c`)

Reuses existing modules directly:
- `event_reader.c` — trace file reading, LZ4 decompression, block index
- `replay.c` — event iteration, time range filtering
- `wait_event.c` — event name resolution (PG17/PG18 tables)
- `map_reader.c` — accumulator logic

**Protocol** (JSON lines over stdin/stdout):

```
--> {"cmd":"info"}
<-- {"from_ns":1709000000,"to_ns":1709036000,"num_events":284000,"num_cpus":8}

--> {"cmd":"aas","from":1709000000,"to":1709003600,"buckets":120,"filters":{}}
<-- {"buckets":[{"t":1709000000,"cpu":2.1,"io":0.8,"lock":0.3,...},...],
     "max_aas":4.2}

--> {"cmd":"top_events","from":1709000000,"to":1709003600,"filters":{"class":"IO"}}
<-- {"rows":[{"event_id":167772161,"name":"IO:DataFileRead","count":4521,
     "total_ms":892.3,"avg_us":197.4,"max_us":12400,"pct":34.2,"aas":0.25},...],
     "db_time_ms":2608.1}

--> {"cmd":"top_sessions","from":...,"to":...,"filters":{}}
<-- {"rows":[{"pid":1234,"db_time_ms":450.2,"cpu_pct":62.1,"top_wait":"IO:DataFileRead"},...]}

--> {"cmd":"top_queries","from":...,"to":...,"filters":{}}
<-- {"rows":[{"query_id":123456789,"count":891,"total_ms":1200.5,"pct":46.0,
     "top_wait":"Lock:transactionid"},...]}

--> {"cmd":"time_model","from":...,"to":...,"filters":{}}
<-- {"db_time_ms":2608.1,"idle_time_ms":45000,"aas":2.61,
     "classes":[{"name":"CPU","ms":1200,"pct":46.0,"aas":1.2},...],"wall_ms":1000}
```

**Filters** (all optional, AND logic):
```json
{"class":"IO","event_id":167772161,"pid":1234,"query_id":123456789}
```

**Build**: compiled alongside the daemon by the existing Makefile.
Linked against same object files. No new dependencies.

### F.2: Client side — `pgwt` (Go)

New directory: `web/` (Go module)

```
web/
  ├── main.go          # CLI args, spawn ssh, HTTP server, open browser
  ├── bridge.go        # WebSocket ↔ SSH stdin/stdout bridge
  ├── static/          # Embedded web assets
  │   ├── index.html   # Single-page app
  │   ├── app.js       # Main application logic
  │   ├── chart.js     # ECharts stacked area chart
  │   └── style.css    # Layout and styling
  ├── go.mod
  └── go.sum
```

**Go dependencies** (minimal):
- `gorilla/websocket` — WebSocket support
- Everything else is stdlib (`net/http`, `os/exec`, `embed`, `encoding/json`)

**SSH spawning**:
```go
cmd := exec.Command("ssh", host, "pgwt-server", traceDir)
stdin, _  := cmd.StdinPipe()   // send requests
stdout, _ := cmd.StdoutPipe()  // read responses
```

**WebSocket bridge**:
- Browser sends JSON request via WebSocket
- Go forwards to SSH stdin
- Go reads JSON response from SSH stdout
- Go forwards to browser via WebSocket
- Requests/responses matched by `"id"` field for concurrency

### F.3: Web frontend (HTML + JS + ECharts)

**UX design** (Oracle ASH / RDS Performance Insights inspired):

**Layout**:
```
┌──────────────────────────────────────────────┐
│ pgwt — db-server   14:00–14:30 (30m)  [8cpu] │  header
├──────────────────────────────────────────────┤
│ ▓▓▓▓▓▓▓▓▓▓████████▓▓▓▓▓▓░░░░░░░░░░░▓▓▓▓▓▓ │  AAS stacked
│ ▓▓▓▓▓▓████████████▓▓▓▓░░░░░░░░░░░░░░▓▓▓▓▓▓ │  area chart
│ ▓▓▓▓████████████▓▓▓▓░░░░░░░░░░░░░░░░░▓▓▓▓▓ │  (ECharts)
│ ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 8 cpu ━━━━━━━ │
│ ▓▓████████▓▓▓▓░░░░░░░░░░░░░░░░░░░░░░░░░▓▓▓ │
├──────────────────────────────────────────────┤
│ Slice by: [Waits ▼]   Filters: class=IO  ✕  │  controls
├──────────────────────────────────────────────┤
│ [Overview] [Events] [Sessions] [Queries]     │  tabs
├──────────────────────────────────────────────┤
│ #  Wait Event         Count  Total  %DB  AAS │  data table
│ 1  IO:DataFileRead    4521   892ms  34%  0.25│  (click = drill)
│ 2  Lock:transaction   1203   450ms  17%  0.13│
│ ...                                          │
│ 10 Other (42 events)         120ms   5%  0.03│
└──────────────────────────────────────────────┘
```

**Chart features** (ECharts provides all of these):
- Stacked area chart with 11 wait class colors
- Brush selection — drag on chart to select time range, all tables update
- Zoom — mouse wheel or toolbar buttons
- Max CPU reference line (markLine)
- Tooltip — hover shows AAS breakdown at that point
- Filtered context outline — dim unfiltered total behind filtered chart
- Legend — click to toggle wait classes

**Table features**:
- Click row to drill down (adds filter, pivots to next view)
- Breadcrumb trail: `Overview > IO > IO:DataFileRead > PID 1234`
- "Other" rollup row (top 9 + Other)
- Mini wait-breakdown color bars per row (inline `<div>` bars)
- Sortable columns (click header)
- "Slice by" dropdown: Waits / Events / SQL / Sessions

**Time navigation**:
- Chart brush select (drag) — primary method
- Zoom buttons / mouse wheel
- Reset button (full time range)
- Time range display in header

### F.4: Implementation order

| Step | What | Deliverable |
|------|------|-------------|
| 1 | `pgwt-server` — info + aas commands | C binary, reads traces, JSON stdout |
| 2 | `pgwt` Go skeleton — SSH + HTTP + WS bridge | Go binary, connects to server |
| 3 | AAS stacked area chart in browser | ECharts chart with time brush |
| 4 | `pgwt-server` — all 4 compute commands | time_model, top_events/sessions/queries |
| 5 | Data tables + drill-down | Click row → filter → next view |
| 6 | Filters, breadcrumbs, "slice by" | Full investigation workflow |
| 7 | Polish: CPU line, "Other" row, mini bars | RDS PI-quality UX |

---

## Planned: Phase G — Daemon Enhancements

### G.1: Query Text Capture

**Goal**: Show the actual SQL text alongside `query_id` in the investigation
client, without requiring pg_stat_statements lookups.

**How it works**: The daemon reads `PgBackendStatus.st_activity` from shared
memory via `process_vm_readv()` when it first sees a new `query_id`. This
gives the actual running SQL (not normalized — real parameter values), which
is more useful for debugging than `pg_stat_statements`' normalized form.

#### Daemon side

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

#### Server side

`pgwt-server` loads `query_texts.jsonl`, serves via `top_queries` response:
```json
{"query_id":123,"text":"SELECT * FROM accounts WHERE...","count":891,...}
```

#### Offset discovery

- `BackendStatusArray + backend_index * sizeof(PgBackendStatus) + st_activity_offset`
- Backend index = `MyBackendId - 1`
- `st_activity` is `char[1024]` — null-terminated C string

### G.2: Plan Identifier Capture (PG18+)

**Goal**: Capture the execution plan hash per-backend to detect plan changes
correlated with wait event spikes (plan regression detection).

**Background**: PostgreSQL 18 added `int64 st_plan_id` to `PgBackendStatus`
in shared memory (right after `st_query_id`). Core PG does **not** compute
it automatically — it stays 0 unless a `planner_hook` extension populates it.
There is no `compute_plan_id` GUC. On PG17, `st_plan_id` does not exist.

#### PG version matrix

| PG Version | `st_query_id` | `st_plan_id` | Notes |
|------------|:---:|:---:|---|
| 14–16 | Exists but unreliable | N/A | `my_wait_event_info` not writable |
| 17 | `uint64` | Does not exist | Full wait tracing works |
| 18+ | `int64` | `int64` (new) | Plan ID requires planner_hook extension |

#### Daemon side

1. **Discover `st_plan_id` offset** in `PgBackendStatus`:
   - DWARF: same approach as `st_query_id` / `st_activity`
   - Known offset: immediately after `st_query_id` (offset + 8)
   - PG17: skip (field doesn't exist)

2. **Read alongside `st_query_id`**: extend existing `process_vm_readv`
   call by 8 bytes — essentially free, no extra syscall

3. **Store in trace events**: add `plan_id` column to columnar block format
   - Bump trace file version (v1 -> v2)
   - Reader handles both versions gracefully

#### Server + client side

1. Show `plan_id` column in queries view (when non-zero)
2. Plan change detection: highlight when same `query_id` has multiple
   `plan_id` values over time
3. Investigation flow: wait time spike -> drill to query -> see `plan_id`
   changed at the spike -> plan regression confirmed

#### Requirements for non-zero plan_id

Users must install a `planner_hook` extension:
- **pg_stat_sql_plans** — computes plan hash, stores plan-level stats
- **pg_store_plans** — stores actual plan text indexed by (queryid, planid)
- Custom extension using `planner_hook` + `result->planId = hash`

Without such an extension, `st_plan_id` is always 0 and the feature is
silently inactive. Document this clearly.

---

## Planned: Phase H — Live Mode

**Goal**: Connect the web client to a running daemon for real-time streaming.

- `pgwt-server` connects to daemon via Unix socket on the DB server
- Pushes live AAS updates to the web client over the SSH channel
- Active view works only in live mode (requires BPF state_map)
- Dual mode: historical (trace files) + live (daemon) in same web UI

---

## Legacy: Rust TUI Client

The Rust TUI client (`client/` directory, Phases E.0–E.1) remains as a
lightweight terminal-based option for quick checks over SSH. It works
standalone on the DB server with no daemon needed — reads trace files
directly. The web client (Phase F) supersedes it for interactive
investigation workflows.

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

### Client-server protocol

- **Transport**: SSH exec channel (stdin/stdout of `pgwt-server`)
- **Format**: JSON lines (newline-delimited JSON)
- **Request**: `{"id":1,"cmd":"aas","from":...,"to":...,"filters":{}}`
- **Response**: `{"id":1,"buckets":[...],"max_aas":4.2}`
- **Matching**: `id` field for concurrent request/response pairing
- **Same model as**: git over SSH, rsync --server, ParaView pvserver

### UX reference sources

- **Oracle EM Performance Hub / ASH Analytics**: stacked area chart,
  two-tier time slider, click-to-filter, dimensional pivot dropdown,
  filter tags bar, CPU cores line, load map treemap view
- **AWS RDS Performance Insights**: "slice by" dropdown, mini wait bars
  per SQL row, Max vCPU line, top 25 items per dimension tab
- **PASH-Viewer**: time range selection on chart, Top SQL drill-down,
  execution plan display
- **pg_ash**: ASCII stacked bar charts, Unicode block characters for
  rendering, "Other" rollup in top-N, per-query wait profile,
  semantic color mapping (green=CPU, blue=IO, red=Lock)
