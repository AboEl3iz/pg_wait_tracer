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

## 5. Filtering

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

## 6. `--format` flag

```
--format tui       Screen-clearing interactive (default for terminal)
--format text      No screen clear, timestamp per interval (default for pipes)
--format json      JSONL — one JSON object per interval
--format csv       Flat rows, one per event per interval
```

Auto-detect: `isatty(stdout)` → tui, else → text.

**JSON includes everything** — all windows, event hierarchy, query details.

---

## 7. `--count N` flag

```
--count 1    One-shot: collect for one interval, print, exit
--count 10   Print 10 intervals then exit
```

---

## 8. Full CLI Summary

```
Usage: pg_wait_tracer --pid <PID> [OPTIONS]

Target:
  -p, --pid <PID>         Postmaster PID
  -D, --pgdata <DIR>      PGDATA directory (reads postmaster.pid)

Output control:
  -V, --view <VIEW>       time_model | system_event | session_event | histogram | query_event
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

Other:
  -v, --verbose           Verbose output to stderr
  -h, --help              Show this help
```

---

## 9. Implementation Order

| Phase | What | Effort |
|-------|------|--------|
| **Phase 1** | Snapshot ring buffer + `--window` + `--count` + `--format` infrastructure | Medium |
| **Phase 2** | Enhanced time_model with event hierarchy (subcategories) | Medium |
| **Phase 3** | Multi-window time_model (side-by-side columns) | Medium |
| **Phase 4** | Multi-window system_event (3 sections) | Small |
| **Phase 5** | ASH-like query_event (--event filter, --query-id filter) | Medium |
| **Phase 6** | Multi-window histogram | Small |
| **Phase 7** | Text format (no screen clear, timestamps) | Small |
| **Phase 8** | JSON format | Medium |
| **Phase 9** | CSV format | Small |
| **Phase 10** | Filtering (--class, --min-pct, --top) | Small |
| **Phase 11** | Update README + tests | Medium |

---

## 10. Files to Modify

| File | Changes |
|------|---------|
| `src/pg_wait_tracer.h` | Format/window enums, new CLI field structs |
| `src/pg_wait_tracer.c` | Parse all new flags, TTY auto-detect |
| `src/daemon.h` | format, count, windows[], ring_buffer, query_id filter to pgwt_daemon |
| `src/daemon.c` | Snapshot per tick, count exit, format dispatch |
| `src/map_reader.h` | Snapshot struct, ring buffer, window delta functions |
| `src/map_reader.c` | Snapshot save, window delta computation |
| `src/output.h` | Format-aware signatures |
| `src/output.c` | Event hierarchy in time_model, multi-window columns/sections, ASH query modes, histogram windows, conditional screen clear, text format |
| `src/output_json.c` | **New** — JSON formatter |
| `src/output_csv.c` | **New** — CSV formatter |
| `Makefile` | Add new .c files |

---

## 11. Verification

```bash
# Current behavior unchanged
sudo ./pg_wait_tracer --pid 12345

# Enhanced time_model with event subcategories
sudo ./pg_wait_tracer --pid 12345 --view time_model

# Multi-window
sudo ./pg_wait_tracer --pid 12345 --window 5s,1m,5m

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

# Existing tests
sudo tests/run_all.sh
```
