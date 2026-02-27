# CLI Redesign: Multi-Window, Event Hierarchy, ASH-like Query Analysis

## Context

Current CLI is TUI-only (screen clearing), cumulative-only, shows wait classes but not
individual events in time_model, and has no way to investigate query↔event relationships
like Oracle ASH. Need to support real DBA investigation workflows.

## Decisions Made

- Auto-detect TTY: terminal → `tui`, pipe → `text`
- 3 configurable time windows (not just delta+cumulative)
- `--count N` flag for exact snapshot control
- Prometheus: skip for now, `--format json` first
- CLI-first, daemon mode later (future phase)
- Auto-discover PG instance when only one cluster is running
- Semi-interactive "top-like" active sessions view (--sort flag, no ncurses)
- No query text initially — cmdline parsing only for backend info
- Query text from shared memory: planned (Phase E.2 — `st_activity` via `process_vm_readv`)
- Plan identifier from shared memory: planned (Phase E.3 — `st_plan_id`, PG18+ only)

---

## 1. Time Windows (`--window`)

3 configurable time windows. The DBA sees NOW, RECENTLY, and OVERALL.

```
--window 5s,1m,5m       # default (first = --interval)
--window 10s,5m,30m     # custom
--window 5s,1m,1h       # longer history
```

First window always equals `--interval`. Format: `Ns`, `Nm`, `Nh`.

**Implementation**: Ring buffer of snapshots, one per tick. Size = largest_window / interval.
Each snapshot: time_model (88B) + compacted system_events (~5KB) + compacted query_events (~10KB).
For 1h at 5s: 720 * 15KB = 10.8MB. Acceptable.

Delta for window W = current_snapshot - snapshot_from_(W/interval)_ticks_ago.

---

## 2. Enhanced time_model: Event Hierarchy

Current time_model shows only class-level totals (Wait: IO, Wait: LWLock). The DBA
needs to see which specific events drive each class. Show top events per class as
indented subcategories:

```
════════════════════════════════════════════════════════════════════════════════
pg_wait_tracer — Time Model    Backends: 12    Uptime: 32m 15s
════════════════════════════════════════════════════════════════════════════════

  Stat Name                       Last 5s   % DB    Last 1m   % DB    Last 5m   % DB
  ──────────────────────────────────────────────────────────────────────────────────────
  DB Time                         5088.6  100.0%   62340.1  100.0%  312450.8  100.0%
    CPU                           1498.6   29.4%   13712.3   22.0%   72312.1   23.1%
    IO                             862.3   16.9%   12340.5   19.8%   98234.2   31.4%
      IO:DataFileRead              621.2   12.2%    9823.1   15.8%   82123.4   26.3%
      IO:DataFileWrite             198.4    3.9%    2012.3    3.2%   12345.6    4.0%
      IO:WALSync                    42.7    0.8%     505.1    0.8%    3765.2    1.2%
    LWLock                         423.1    8.3%    4923.4    7.9%   21234.5    6.8%
      LWLock:WALInsert             312.3    6.1%    3812.1    6.1%   16234.2    5.2%
      LWLock:BufferContent         110.8    2.2%    1111.3    1.8%    5000.3    1.6%
    Lock                           312.4    6.1%    2123.1    3.4%    8921.3    2.9%
      Lock:Transaction             301.2    5.9%    2023.4    3.2%    8512.1    2.7%
    Client                          88.2    1.7%    1512.3    2.4%    6234.1    2.0%
      Client:ClientRead             88.2    1.7%    1512.3    2.4%    6234.1    2.0%

  (Activity/Idle)                12560.4     —     62340.1     —    312450.8     —
```

This is the "one view to rule them all" — a DBA opens it and immediately knows:
- Which classes are hot (IO 16.9%)
- Which specific events within each class (DataFileRead 12.2%)
- How it compares across time windows (was 26.3% five minutes ago → improving)

**Design choices:**
- Show top 3 events per class (configurable with `--top N`?)
- Only show events contributing >= 1% of DB Time (avoid clutter)
- Classes with 0 time are hidden (current behavior)
- CPU has no sub-events (it's not a wait class), shown as single line

---

## 3. ASH-like Query Event Analysis

Oracle ASH lets DBAs answer two questions:
1. "Which queries are causing this wait event?" (event → queries)
2. "What is this query waiting on?" (query → events)

### New `--query-id` filter flag

```
--query-id <ID>    Filter query_event view to one query
```

### query_event view modes

**Mode A: Default — top query-event combinations (current behavior)**
```bash
sudo pg_wait_tracer --pid 12345 --view query_event
```
```
  query_id             Wait Event            Waits     Total (ms)  Avg (us)  Max (us)  % DB
  5678234567890123     IO:DataFileRead        2340       1024.5     437.8   45623.1   4.1%
  1234567890123456     Lock:Transaction        145        892.3    6153.8  892100.5   3.6%
  5678234567890123     LWLock:WALInsert        890        334.2     375.5    8934.2   1.3%
```

**Mode B: Filter by event — top queries for a specific wait event**
```bash
sudo pg_wait_tracer --pid 12345 --view query_event --event IO:DataFileRead
```
```
════════════════════════════════════════════════════════════════════════════════
pg_wait_tracer — Top Queries for IO:DataFileRead    Backends: 12
════════════════════════════════════════════════════════════════════════════════

  query_id               Waits     Total (ms)  Avg (us)  Max (us) % Event   % DB
  5678234567890123        2340       1024.5     437.8   45623.1   31.4%    4.1%
  9876543210987654         456        201.8     442.5    5432.1    6.2%    0.8%
  1111222233334444         123         89.2     725.2    3214.5    2.7%    0.4%
```

Shows "% Event" = fraction of this event's total time. DBA sees: "query 5678... is responsible
for 31.4% of all DataFileRead time."

**Mode C: Filter by query_id — all events for one query**
```bash
sudo pg_wait_tracer --pid 12345 --view query_event --query-id 5678234567890123
```
```
════════════════════════════════════════════════════════════════════════════════
pg_wait_tracer — Wait Profile for query_id 5678234567890123    Backends: 12
════════════════════════════════════════════════════════════════════════════════

  Wait Event                 Waits     Total (ms)  Avg (us)  Max (us)  % Query  % DB
  CPU                         5432       1892.3     348.5    12340.1   58.2%   7.5%
  IO:DataFileRead             2340       1024.5     437.8    45623.1   31.5%   4.1%
  LWLock:WALInsert             890        334.2     375.5     8934.2   10.3%   1.3%
```

Shows "% Query" = fraction of this query's total time. DBA sees: "this query spends
58.2% of its time on CPU and 31.5% on DataFileRead."

### query_event with time windows

query_event should also support 3 windows when `--window` is set:
```
  ──── Last 5s ────────────────────────────────────────────────
  query_id             Wait Event            Waits   Total (ms)  % DB
  5678234567890123     IO:DataFileRead         23       10.2     0.2%

  ──── Last 1m ────────────────────────────────────────────────
  query_id             Wait Event            Waits   Total (ms)  % DB
  5678234567890123     IO:DataFileRead        234      102.5     0.2%
  1234567890123456     Lock:Transaction        12       89.3     0.1%
```

---

## 4. Histogram Windows

Show 3 latency distributions side-by-side (one per window):

```bash
sudo pg_wait_tracer --pid 12345 --view histogram --event IO:DataFileRead --window 5s,1m,5m
```
```
════════════════════════════════════════════════════════════════════════════════
pg_wait_tracer — Histogram: IO:DataFileRead    Uptime: 32m 15s
════════════════════════════════════════════════════════════════════════════════

  Bucket(us)    Last 5s   %      Last 1m   %      Last 5m   %
  ──────────────────────────────────────────────────────────────────────────
       <1          12   1.5%      123   1.3%      612   0.7%
    1-  2          45   5.5%      512   5.3%     4312   5.1%
    2-  4         183  22.3%     2132  22.0%    18234  21.5%
    4-  8         210  25.6%     2505  25.8%    22123  26.1%
    8- 16         153  18.6%     1843  19.0%    16234  19.2%
   16- 32          98  11.9%     1234  12.7%    10812  12.8%
   32- 64          54   6.6%      623   6.4%     5234   6.2%
   64-128          31   3.8%      378   3.9%     3123   3.7%
  128-256          17   2.1%      198   2.0%     1812   2.1%
  256-512           9   1.1%       95   1.0%      812   1.0%
  512-1K            5   0.6%       42   0.4%      412   0.5%
   1K- 2K           3   0.4%       18   0.2%      198   0.2%
  >=16K             1   0.1%        1   0.0%       12   0.0%
```

DBA reads: "Distribution is stable across windows — no latency shift."

---

## 5. Auto-Discovery of PostgreSQL Instance

Currently `--pid` or `--pgdata` is required. For ad-hoc DBA use, auto-detect the
PostgreSQL postmaster when neither is specified:

```
# Auto-detect: finds the single running postmaster
sudo pg_wait_tracer

# Explicit PID (unchanged)
sudo pg_wait_tracer --pid 12345

# Via PGDATA (unchanged)
sudo pg_wait_tracer --pgdata /var/lib/pgsql/17/data
```

**Algorithm** (port from `tests/testutil.sh:find_postmaster()`):
1. `pgrep -x postgres` → list candidate PIDs
2. Filter children: skip PIDs whose parent comm is also "postgres"
3. If exactly 1 postmaster → use it automatically
4. If multiple postmasters → list them with version + PGDATA and FATAL:
   ```
   Multiple PostgreSQL instances found:
     PID 1234  PG17  /var/lib/pgsql/17/data
     PID 5678  PG18  /var/lib/pgsql/18/data
   Use --pid <PID> or --pgdata <DIR> to select one.
   ```
5. If none found → FATAL: "No running PostgreSQL instance found"

**Version detection**: extract from exe path (`/usr/pgsql-17/bin/postgres` or
`/usr/lib/postgresql/17/bin/postgres`) via `readlink /proc/PID/exe`.

**PGDATA detection**: read `/proc/PID/environ` for `PGDATA=`, or parse cmdline
for `-D` flag.

**Do we need both `--pid` and `--pgdata`?** Keep both:
- `--pid` for multi-instance hosts (direct, unambiguous)
- `--pgdata` for systemd integration and scripts (reads `postmaster.pid`)
- Auto-detect for the common single-instance ad-hoc case

---

## 6. Active Sessions View (`--view active`)

A "top-like" refreshing view of currently active backends. This is what a DBA
opens first to see what's happening right now.

```bash
sudo pg_wait_tracer --view active
sudo pg_wait_tracer --view active --sort wait_time
sudo pg_wait_tracer --view active --sort db_time
```

```
════════════════════════════════════════════════════════════════════════════════
pg_wait_tracer — Active Sessions    Backends: 12/100    Uptime: 32m 15s
════════════════════════════════════════════════════════════════════════════════

  PID      State       Wait Event          Wait (ms)   DB Time (ms)  Backend Type
  ────────────────────────────────────────────────────────────────────────────────
  34521    waiting     Lock:Transaction     8923.1       12450.3      client backend
  34587    waiting     IO:DataFileRead         3.2        8234.1      client backend
  34602    on cpu      —                       —          5123.4      client backend
  34534    waiting     LWLock:WALInsert        0.8        4892.1      client backend
  34498    waiting     Client:ClientRead    1234.5        3421.2      client backend
  34612    idle        —                       —             —        client backend
  34701    active      —                       —             —        autovacuum worker
  34702    active      —                       —             —        wal writer
```

**Columns:**
- **PID**: Backend OS PID
- **State**: `on cpu` | `waiting` | `idle` (from BPF tracing state)
- **Wait Event**: Current wait event (if waiting), `—` otherwise
- **Wait (ms)**: How long in current wait state (from BPF timestamp delta)
- **DB Time (ms)**: Total DB Time for this backend (cumulative)
- **Backend Type**: From cmdline parsing (`client backend`, `autovacuum worker`,
  `wal writer`, `checkpointer`, `bgwriter`, `walreceiver`, etc.)

**Sorting** (`--sort` flag):
```
--sort wait_time     Sort by current wait duration (default)
--sort db_time       Sort by cumulative DB Time
--sort pid           Sort by PID
--sort event         Sort by wait event name
```

**Semi-interactive**: Not full ncurses. Refreshes with screen clear (TUI mode),
supports `--sort` flag on command line. No runtime key bindings for now.

**Backend info from cmdline**: PostgreSQL writes the backend type into
`/proc/PID/cmdline` (e.g., `postgres: autovacuum worker`). Parse this for the
Backend Type column. No query text — just type identification.

**Future**: Read query text from shared memory (`PgBackendStatus.st_activity`)
for an additional column showing the current SQL statement.

---

## 7. Session Event Windowing

**Summary mode** (default `--view session_event`): Shows only the latest interval.
No time windows — the per-backend snapshot is already interval-sized and useful as-is.

**Detail mode** (`--view session_event --pid-filter <PID>`): Shows time windows
for a single backend. This is for deep-diving one specific problematic session.

```bash
sudo pg_wait_tracer --view session_event --pid-filter 34521 --window 5s,1m,5m
```
```
════════════════════════════════════════════════════════════════════════════════
pg_wait_tracer — Session Detail: PID 34521    Backend: client backend
════════════════════════════════════════════════════════════════════════════════

  Wait Event              Last 5s    %     Last 1m    %     Last 5m    %
  ──────────────────────────────────────────────────────────────────────────
  Lock:Transaction         4921.3  96.8%    8923.1  51.2%   12450.3  28.1%
  IO:DataFileRead            98.2   1.9%    5234.1  30.0%   18234.1  41.2%
  CPU                        64.1   1.3%    3271.8  18.8%   13621.2  30.7%
```

DBA reads: "PID 34521 is currently stuck on Lock:Transaction (96.8% of last 5s),
but historically it was mostly doing DataFileRead (41.2% of last 5m)."

---

## 8. Filtering

### `--class` filter
```
--class IO             Show only IO events in system_event
--class IO,LWLock      Show only IO and LWLock events
```

### `--min-pct` threshold
```
--min-pct 1            Hide events below 1% of DB Time (reduce clutter)
```

### `--top N` for time_model subcategories
```
--top 5                Show top 5 events per class in time_model (default: 3)
```

---

## 9. `--format` flag

```
--format tui       Screen-clearing interactive (default for terminal)
--format text      No screen clear, timestamp per interval (default for pipes)
--format json      JSONL — one JSON object per interval
--format csv       Flat rows, one per event per interval
```

Auto-detect: `isatty(stdout)` → tui, else → text.

**JSON includes everything** — all windows, event hierarchy, query details.

---

## 10. `--count N` flag

```
--count 1    One-shot: collect for one interval, print, exit
--count 10   Print 10 intervals then exit
```

---

## 11. Full CLI Summary

```
Usage: pg_wait_tracer [OPTIONS]

Target (auto-detect if omitted, single instance):
  -p, --pid <PID>         Postmaster PID
  -D, --pgdata <DIR>      PGDATA directory (reads postmaster.pid)

Views:
  -V, --view <VIEW>       time_model | system_event | session_event |
                           histogram | query_event | active

Output control:
  -f, --format <FMT>      tui | text | json | csv  (default: auto-detect)
  -i, --interval <SEC>    Refresh interval (default: 5)
  -d, --duration <SEC>    Stop after N seconds
  -n, --count <N>         Stop after N intervals
  -w, --window <W1,W2,W3> Time windows (default: interval only)

Filters:
  -e, --event <NAME>      Event filter (histogram: required; query_event: filter by event)
  -P, --pid-filter <PID>  Show detail for specific backend (session_event)
  -Q, --query-id <ID>     Filter query_event to one query
  -C, --class <CLASS>     Filter by wait class (system_event)
      --min-pct <N>       Hide events below N% of DB Time
      --top <N>           Top events per class in time_model (default: 3)
      --sort <COL>        Sort column for active view (wait_time|db_time|pid|event)

Other:
  -v, --verbose           Verbose output to stderr
  -h, --help              Show this help
```

---

## 12. Implementation Order

| Phase | What | Effort |
|-------|------|--------|
| **Phase 1** | Auto-discovery + `--count` + `--format` infrastructure + TTY auto-detect | Medium |
| **Phase 2** | Snapshot ring buffer + `--window` parsing + delta computation | Medium |
| **Phase 3** | Enhanced time_model with event hierarchy (subcategories) | Medium |
| **Phase 4** | Multi-window time_model (side-by-side columns) | Medium |
| **Phase 5** | Multi-window system_event (3 sections) | Small |
| **Phase 6** | ASH-like query_event (--event filter, --query-id filter) | Medium |
| **Phase 7** | Multi-window histogram | Small |
| **Phase 8** | Active sessions view (--view active, --sort, cmdline parsing) | Medium |
| **Phase 9** | Session event windowing (--pid-filter with windows) | Small |
| **Phase 10** | Text format (no screen clear, timestamps) | Small |
| **Phase 11** | JSON format | Medium |
| **Phase 12** | CSV format | Small |
| **Phase 13** | Filtering (--class, --min-pct, --top) | Small |
| **Phase 14** | Update README + tests | Medium |
| **Phase 15** | Recording & replay (`--record`, `--replay`, `--from`, `--to`) | Medium |
| **Phase 16** | SQL query text exposure (shared memory or eBPF uprobe) | Medium-Large |

---

## 13. Files to Modify

| File | Changes |
|------|---------|
| `src/pg_wait_tracer.h` | Format/window/sort enums, new CLI field structs, auto-detect flag |
| `src/pg_wait_tracer.c` | Parse all new flags, TTY auto-detect, postmaster auto-discovery |
| `src/daemon.h` | format, count, windows[], ring_buffer, query_id filter, sort to pgwt_daemon |
| `src/daemon.c` | Snapshot per tick, count exit, format dispatch, active sessions collect |
| `src/map_reader.h` | Snapshot struct, ring buffer, window delta functions |
| `src/map_reader.c` | Snapshot save, window delta computation |
| `src/output.h` | Format-aware signatures, active sessions output |
| `src/output.c` | Event hierarchy in time_model, multi-window columns/sections, ASH query modes, histogram windows, active sessions view, session detail windows, conditional screen clear, text format |
| `src/cmdline.c` | **New** — Parse /proc/PID/cmdline for backend type |
| `src/cmdline.h` | **New** — Backend type enum and parser |
| `src/output_json.c` | **New** — JSON formatter |
| `src/output_csv.c` | **New** — CSV formatter |
| `src/recording.c` | **New** — Snapshot recording and replay |
| `src/recording.h` | **New** — Recording file format and API |
| `src/query_text.c` | **New** — Query text reader (shmem or BPF, Phase 16) |
| `src/query_text.h` | **New** — Query text API |
| `Makefile` | Add new .c files |

---

## 14. Future: Recording & Replay (SAR-like Time Travel)

Record snapshots to disk for offline analysis. Like `sar -o`/`sar -f`, the DBA can
capture a performance recording during a problem window, then analyze it later — or
share it with another DBA for review.

### Recording Mode

```bash
# Record snapshots to a binary file (runs like normal, also writes to disk)
sudo pg_wait_tracer --record perf_issue.pgwt --interval 5 --duration 3600

# Record with specific views' data (all snapshot data is always recorded)
sudo pg_wait_tracer --record overnight.pgwt --interval 5
```

Each tick writes one `pgwt_snapshot` (time_model + system_events + query_events) plus
a timestamp header to the file. File format:

```
[file header: magic, version, interval, PG version, start time]
[snapshot 0: timestamp + pgwt_snapshot]
[snapshot 1: timestamp + pgwt_snapshot]
...
```

File size: ~15KB per snapshot. At 5s interval: 720 snapshots/hour = ~10.8MB/hour.
24 hours = ~260MB. Acceptable for investigation recordings.

### Replay Mode

```bash
# Replay entire recording with default view
sudo pg_wait_tracer --replay perf_issue.pgwt

# Replay a specific time range
sudo pg_wait_tracer --replay perf_issue.pgwt --from "2025-01-15 14:00" --to "14:05"

# Replay with specific view and windows
sudo pg_wait_tracer --replay perf_issue.pgwt --from "14:00" --to "14:30" \
    --view query_event --window 5s,1m,5m

# One-shot summary of a time range
sudo pg_wait_tracer --replay perf_issue.pgwt --from "14:00" --to "14:05" --count 1
```

Replay loads snapshots from file into the ring buffer, then renders views using the
same delta logic as live mode. `--from`/`--to` select the time range. Without them,
replays from start to end.

### Key Design Points

- **Same views, same code**: Replay populates the ring buffer from file instead of
  from BPF maps. All output functions (`pgwt_print_*`) work unchanged.
- **No BPF needed for replay**: Replay is read-only, no root required for viewing.
- **Delta computation reuse**: `pgwt_ring_delta()` already computes arbitrary deltas.
  Replay just needs to load the right snapshots into the ring.
- **Active sessions not replayable**: The active view shows real-time per-backend state
  from BPF state_map, which is not captured in snapshots. Recording could optionally
  store per-backend state too (future extension).

### New CLI Flags

```
--record <FILE>     Write snapshots to file while tracing
--replay <FILE>     Replay from recorded file (no BPF, no root needed for viewing)
--from <TIME>       Start time for replay (ISO 8601 or HH:MM)
--to <TIME>         End time for replay
```

### Implementation Approach

1. Define binary file format with header (magic, version, interval, metadata)
2. In `handle_timer()`, after ring push, also write snapshot to file if recording
3. New `pgwt_replay_load()` that reads file, populates ring buffer for time range
4. Replay main loop: step through snapshots, render view per tick (or single summary)
5. File ~200-300 lines: `src/recording.c` / `src/recording.h`

---

## 15. SQL Query Text Exposure

**Decision made**: Option A (shared memory) — read `PgBackendStatus.st_activity`
via `process_vm_readv()`. Captures the actual running SQL (not normalized).
See [ROADMAP.md](ROADMAP.md) Phase E.2 for full implementation plan.

Show the currently executing SQL statement for each backend. Two implementation
approaches were evaluated:

### Where Query Text Appears

- **Active sessions view**: New `Query` column (truncated to ~60 chars) showing the
  current statement for each backend
- **query_event view**: Optionally map `query_id` → query text for display
- **session_event detail**: Show current query for the filtered PID

Example active view with query text:
```
  PID      State       Wait Event       Wait (ms)  DB Time (ms)  Query
  ────────────────────────────────────────────────────────────────────────────────
  34521    waiting     Lock:Transaction   8923.1      12450.3     UPDATE accounts SET ba...
  34587    waiting     IO:DataFileRead       3.2       8234.1     SELECT * FROM orders W...
  34602    on cpu      —                     —         5123.4     INSERT INTO logs (ts, ...
  34612    idle        —                     —            —       —
```

### Option A: Shared Memory (`PgBackendStatus.st_activity`)

PostgreSQL maintains `PgBackendStatus` in shared memory for each backend. The
`st_activity` field contains the current query text (up to `track_activity_query_size`,
default 1024 bytes). This is what `pg_stat_activity.query` reads.

**How it works:**
1. Find the `BackendStatusArray` symbol in the postgres binary (ELF lookup, like we
   do for `my_wait_event_info`)
2. For each backend PID, read `PgBackendStatus.st_activity` from `/proc/PID/mem`
   or via `process_vm_readv()`
3. Display truncated text in output

**Pros:**
- Simpler implementation (~100-150 lines)
- No additional BPF programs needed
- Reads the same data `pg_stat_activity` shows — well-understood semantics
- Works for all backend types that set `st_activity`

**Cons:**
- Shared memory layout is PG-version-dependent (struct offsets change between major
  versions). Need per-version offset tables or runtime discovery via DWARF/debuginfo.
- `BackendStatusArray` is a static variable — need to resolve its address per version.
  May require debuginfo packages on some distros.
- Read is a point-in-time snapshot — slight race with backend updating the field.
  Acceptable for display purposes (same race `pg_stat_activity` has).
- `track_activity_query_size` limits text length (default 1024, configurable).

**Implementation sketch:**
```c
/* Resolve BackendStatusArray address from ELF symbols */
uintptr_t status_array = pgwt_resolve_symbol("BackendStatusArray");

/* For each backend, read st_activity */
struct iovec local  = { .iov_base = buf, .iov_len = 1024 };
struct iovec remote = { .iov_base = (void *)(status_array + idx * sizeof_entry + activity_offset),
                        .iov_len = 1024 };
process_vm_readv(pid, &local, 1, &remote, 1, 0);
```

### Option B: eBPF Uprobe on Query Execution

Attach a uprobe to a query execution function (e.g., `exec_simple_query()`,
`PortalRun()`, or `pgstat_report_activity()`) and capture the query string via
`bpf_probe_read_user()`.

**How it works:**
1. Attach uprobe to `exec_simple_query(const char *query_string)` — called for
   every simple query protocol message
2. In the BPF program, read the query string argument and store it in a per-PID
   hash map (`query_text_map`)
3. Userspace reads `query_text_map` alongside other maps during each tick

**Pros:**
- No shared memory layout dependency — reads function arguments directly
- Version-resilient: `exec_simple_query` signature has been stable for 15+ years
- Natural fit with existing BPF infrastructure (same attach/read pattern)
- Can also hook extended query protocol (`exec_parse_message`, `exec_bind_message`)

**Cons:**
- `bpf_probe_read_user()` has a size limit in BPF programs (typically ~256 bytes per
  read, can do multiple reads but adds complexity). Long queries get truncated.
- BPF hash map value size must be fixed at compile time. Storing 1024 bytes per PID
  in a BPF map is expensive: 1024 * max_backends. With 1000 backends = ~1MB map.
- Extended query protocol: prepared statements execute via `PortalRun()` where the
  query text isn't a direct argument — need to hook `exec_parse_message` to capture
  the text at parse time and correlate with portal execution.
- `pgstat_report_activity()` is called more often but has the text as an argument —
  could be a simpler single hook point that covers both protocols.
- Additional uprobe = additional overhead (though likely negligible — one read per
  query start, not per wait event).

**Implementation sketch:**
```c
/* BPF program attached to exec_simple_query */
SEC("uprobe/exec_simple_query")
int handle_query(struct pt_regs *ctx)
{
    const char *query = (const char *)PT_REGS_PARM1(ctx);
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    char buf[256];
    bpf_probe_read_user_str(buf, sizeof(buf), query);
    bpf_map_update_elem(&query_text_map, &pid, buf, BPF_ANY);
    return 0;
}
```

### Decision Criteria

| Factor | Option A (shmem) | Option B (eBPF) |
|--------|-------------------|-----------------|
| Implementation complexity | Medium | Medium-Large |
| PG version sensitivity | High (struct layout) | Low (function signature stable) |
| Debuginfo dependency | Likely needed | Not needed |
| Text length | Full (1024 default) | Limited (~256 per read) |
| Overhead | Minimal (read on tick) | Minimal (one write per query) |
| Extended query protocol | Handled (always in st_activity) | Needs extra hooks |
| Consistency with tool | Different pattern (shmem) | Same pattern (BPF) |

**Decision**: Option A chosen. `st_activity` gives the actual running SQL with real
parameter values, which is more useful for debugging than normalized queries. We
deduplicate by `query_id` and keep the first-seen text as representative.

---

## 16. Future: Daemon Mode (not in Phase 1-16)

After the CLI is feature-complete, add a daemon mode for continuous monitoring:

**Architecture:**
```
pg_wait_tracer --daemon          # background process, BPF tracing + ring buffer
pg_wait_tracer --view time_model # CLI client, connects to daemon via Unix socket
```

- Daemon runs as a long-lived process, maintains the ring buffer and BPF programs
- CLI client connects to daemon, requests specific views with filters
- Avoids re-attaching BPF probes for every ad-hoc query
- Multiple CLI clients can connect simultaneously
- PG extension wrapper: a thin SQL interface that talks to the daemon

**Benefits:**
- Start with PostgreSQL (systemd companion service)
- No BPF attach/detach overhead for each investigation
- Foundation for Prometheus exporter (daemon exposes metrics endpoint)
- PG extension can expose views like `pg_wait_tracer_time_model()`

**Protocol**: Simple request/response over Unix domain socket. Client sends view
name + filters as JSON, daemon responds with JSON data. CLI client formats output.

**Not in scope for Phase 1-16** — this is a separate future project after the CLI
redesign is complete and validated.

---

## 17. Query Plan Identification

**Decision made**: Capture `st_plan_id` from `PgBackendStatus` in shared memory (PG18+).
See [ROADMAP.md](ROADMAP.md) Phase E.3 for full implementation plan.

**Key finding**: PostgreSQL 18 added `int64 st_plan_id` to `PgBackendStatus` right
after `st_query_id`. It's readable from shared memory via the same `process_vm_readv()`
technique — essentially free (extend existing read by 8 bytes).

**Limitation**: Core PG does **not** compute `planId` automatically. Users need a
`planner_hook` extension (e.g., `pg_store_plans`, `pg_stat_sql_plans`). Without one,
`st_plan_id` is always 0. On PG17, the field does not exist.

**Full execution plans are not feasible** to capture passively:
- Plan tree is in backend-local memory (complex pointer-chasing structure)
- Serializing from eBPF is impractical (dozens of node types)
- `auto_explain` captures actual plans but only to PG log (complex parsing)
- `pg_store_plans` stores plan text indexed by (queryid, planid) — best option
  if users want the actual plan text alongside the plan hash

**Practical investigation flow with plan_id**:
1. See wait time spike in AAS chart
2. Drill to query causing the spike
3. See `plan_id` changed at the spike time → plan regression confirmed
4. Use `pg_store_plans` or `EXPLAIN` to examine the specific plan

---

## 18. Verification

```bash
# Auto-detect single instance
sudo ./pg_wait_tracer

# Current behavior unchanged
sudo ./pg_wait_tracer --pid 12345

# Enhanced time_model with event subcategories
sudo ./pg_wait_tracer --pid 12345 --view time_model

# Multi-window
sudo ./pg_wait_tracer --pid 12345 --window 5s,1m,5m

# Active sessions (top-like)
sudo ./pg_wait_tracer --view active
sudo ./pg_wait_tracer --view active --sort db_time

# Session detail with windows
sudo ./pg_wait_tracer --view session_event --pid-filter 34521 --window 5s,1m,5m

# ASH-like: which queries cause DataFileRead?
sudo ./pg_wait_tracer --pid 12345 --view query_event --event IO:DataFileRead

# ASH-like: what does this query wait on?
sudo ./pg_wait_tracer --pid 12345 --view query_event --query-id 5678234567890123

# Histogram comparison across windows
sudo ./pg_wait_tracer --pid 12345 --view histogram --event IO:DataFileRead --window 5s,1m,5m

# One-shot JSON
sudo ./pg_wait_tracer --pid 12345 --format json --count 1 --interval 10

# Pipe to file (auto text format)
sudo ./pg_wait_tracer --pid 12345 --count 5 > output.log

# Multiple instances: auto-detect fails, shows list
sudo ./pg_wait_tracer
# → "Multiple PostgreSQL instances found: ..."

# Recording & replay (SAR-like)
sudo ./pg_wait_tracer --record /tmp/perf.pgwt --interval 5 --count 10
./pg_wait_tracer --replay /tmp/perf.pgwt
./pg_wait_tracer --replay /tmp/perf.pgwt --from "14:00" --to "14:05" --view query_event

# Active sessions with query text (Phase 16)
sudo ./pg_wait_tracer --view active --show-query

# Existing tests
sudo tests/run_all.sh
```
