# pg_wait_tracer

Real-time PostgreSQL wait event tracer using BPF hardware watchpoints.

pg_wait_tracer attaches to a running PostgreSQL cluster and captures every
wait event transition across all backends with nanosecond precision. It
requires no PostgreSQL patches, no extensions, and no restarts. All
aggregation happens in-kernel via BPF maps — only summary statistics are
copied to userspace. Typical TPS overhead is under 5% in pgbench benchmarks
(up to ~13% under heavy write contention).

## Quick Start

```bash
# Build (see INSTALL.md for prerequisites)
make

# Run (auto-discovers single PG instance, must be root)
sudo ./pg_wait_tracer

# One-shot: collect one 10-second interval and exit
sudo ./pg_wait_tracer --count 1 --interval 10

# Pipe to file (auto-switches to text format with timestamps)
sudo ./pg_wait_tracer --count 5 > output.log

# Time windows (infrastructure for multi-window views, future phases)
sudo ./pg_wait_tracer --window 5s,1m,5m --count 3
```

## CLI Reference

| Flag | Short | Default | Description |
|------|-------|---------|-------------|
| `--pid <PID>` | `-p` | auto-detect | Postmaster PID (auto-discovered if omitted) |
| `--pgdata <DIR>` | `-D` | — | PGDATA directory (reads postmaster.pid) |
| `--interval <SEC>` | `-i` | 5 | Refresh interval in seconds (minimum 1) |
| `--duration <SEC>` | `-d` | unlimited | Stop after N seconds |
| `--count <N>` | `-n` | unlimited | Print N intervals then exit |
| `--window <W1,W2,W3>` | `-w` | — | Time windows, e.g. `5s,1m,5m` (first must equal interval) |
| `--view <VIEW>` | `-V` | `time_model` | Output view (see below) |
| `--format <FMT>` | `-f` | auto-detect | Output format: `tui` (terminal), `text` (pipe) |
| `--event <NAME>` | `-e` | — | Event filter (histogram: required; query_event: by event) |
| `--pid-filter <PID>` | `-P` | — | Show per-event detail for one backend (session_event view) |
| `--query-id <ID>` | `-Q` | — | Filter query_event to one query |
| `--verbose` | `-v` | off | Print diagnostic info to stderr |
| `--help` | `-h` | — | Show usage |

**Auto-discovery:** When neither `--pid` nor `--pgdata` is given, pg_wait_tracer
scans `/proc` for running PostgreSQL instances. If exactly one is found, it
attaches automatically. If multiple are found, it lists them and exits.

**Format auto-detect:** When stdout is a terminal, output uses TUI mode (screen
clearing). When piped or redirected, output switches to text mode (timestamps
per interval, no screen clearing).

## Views

### time_model (default)

System-wide time accounting with event hierarchy. Shows how backends spend their
time, broken down by CPU and each wait class, with the top individual events
shown as indented subcategories under each class. This is the starting point
for any investigation.

**Columns:**

| Column | Description |
|--------|-------------|
| Stat Name | Time category (class or individual event) |
| Time (ms) | Duration in the interval |
| % DB Time | Fraction of total DB Time |

**Event hierarchy:** Each wait class shows the top 3 individual events that
contribute >= 1% of DB Time, indented beneath the class total. CPU has no
sub-events (it is not a wait class). Classes with 0 time are hidden.

**Example:**

```
════════════════════════════════════════════════════════════════════════════════
pg_wait_tracer — Time Model    Backends: 12    Interval: 5s
════════════════════════════════════════════════════════════════════════════════

  Stat Name                           Time (ms) % DB Time
  ────────────────────────────────────────────────────────────────────────────
  DB Time                            25088.6     100.0%
    CPU                               5498.6      21.9%
    IO                                3262.8      13.0%
      IO:DataFileRead                 2534.1      10.1%
      IO:DataFileWrite                 312.4       1.2%
    Lock                              2104.3       8.4%
      Lock:Transaction                1980.1       7.9%
    LWLock                            1823.2       7.3%
      LWLock:WALInsert                1312.3       5.2%
      LWLock:BufferContent             410.8       1.6%
    Client                             882.1       3.5%
      Client:ClientRead                882.1       3.5%
    Timeout                            450.0       1.8%

  (Activity/Idle — excluded from DB Time)    12560.4       —
```

**Reading this output:**

- **DB Time** is the total non-idle time across all backends. With 12 backends
  over a 5-second interval, the maximum is 60,000 ms (12 x 5000).
- **CPU at 21.9%** means backends spent most of their active time waiting,
  not computing. The system is wait-bound.
- **IO at 13.0%** — and you can immediately see that **DataFileRead at 10.1%**
  is the dominant IO event. No need to switch to `system_event` view.
- **Lock at 8.4%** — the sub-event shows it's almost entirely Transaction
  locks (7.9%), meaning queries are blocked by other uncommitted transactions.
- Small events below 1% of DB Time are hidden to avoid clutter. Use
  `system_event` view for the full event list.
- **Activity/Idle** is shown separately and excluded from DB Time. Idle backends
  contribute nothing to DB Time.

---

### system_event

Top wait events across all backends, ranked by total duration. Use this to
identify which specific events consume the most time.

**Columns:**

| Column | Description |
|--------|-------------|
| Wait Event | Event name as `Class:Event` (e.g. `IO:DataFileRead`) or `CPU` |
| Total Waits | Number of occurrences |
| Total (ms) | Cumulative duration |
| Avg (us) | Average duration per occurrence |
| Max (us) | Longest single occurrence |
| % DB | Fraction of DB Time |

**Example:**

```
════════════════════════════════════════════════════════════════════════════════
pg_wait_tracer — System Events (cumulative)    Backends: 12
════════════════════════════════════════════════════════════════════════════════

  Wait Event                 Total Waits     Total (ms)   Avg (us)     Max (us)    % DB
  ────────────────────────────────────────────────────────────────────────────────────────
  CPU                           43460        5498.6      126.5     10630.2   21.9%
  IO:DataFileRead                8234        3262.8      396.2     45623.1   13.0%
  Lock:Transaction                567        2104.3     3712.2    892100.5    8.4%
  LWLock:WALInsert               4812        1823.2      378.9      8934.2    7.3%
  Client:ClientRead              1203         882.1      733.1     25410.3    3.5%
  Timeout:PgSleep                   3         450.0   150000.0    200012.1    1.8%
  IO:DataFileWrite               1056         312.4      295.8      5612.0    1.2%
  LWLock:BufferContent            892         198.7      222.8      3451.0    0.8%
```

**Reading this output:**

- Events are sorted by `Total (ms)`, so the top rows are the biggest bottlenecks.
- **High Avg with low count** (like `Lock:Transaction`) means long individual waits
  — a few queries are blocking.
- **Low Avg with high count** (like `LWLock:WALInsert`) means frequent short waits
  — high concurrency on the WAL.
- **Large Max vs Avg gap** suggests outlier events. Use `histogram` view to see
  the full distribution.

---

### session_event

Per-backend summary. Shows each backend's DB Time, CPU/Wait ratio, and top
wait event. Use `--pid-filter` to drill into a specific backend.

**Columns (summary):**

| Column | Description |
|--------|-------------|
| PID | Backend process ID |
| Type | Backend type (client, bgwriter, checkpointer, walwriter, etc.) |
| User | Connected user (or `-`) |
| DB | Database (or `-`) |
| DB Time(ms) | Non-idle time for this backend |
| CPU% | Time spent on CPU |
| Wait% | Time spent waiting |
| Top Wait | Highest-duration wait event |

**Example:**

```
════════════════════════════════════════════════════════════════════════════════
pg_wait_tracer — Session Summary    Backends: 12
════════════════════════════════════════════════════════════════════════════════

  PID     Type           User       DB         DB Time(ms)  CPU% Wait%  Top Wait
  ──────────────────────────────────────────────────────────────────────────────────
  12345   client         app        mydb          4096.2  42.1% 57.9%  IO:DataFileRead
  12346   client         app        mydb          3820.5  38.4% 61.6%  Lock:Transaction
  12347   client         admin      mydb          1024.0  71.2% 28.8%  LWLock:WALInsert
  12400   bgwriter       -          -              512.3  62.1% 37.9%  IO:DataFileWrite
  12401   checkpointer   -          -              256.1  15.3% 84.7%  IO:DataFileSync
  12402   walwriter      -          -              128.0  44.2% 55.8%  IO:WALWrite
```

**With `--pid-filter 12345`**, an additional detail table appears below the
summary, showing the full event breakdown for that backend (same columns as
system_event).

**Reading this output:**

- **CPU% + Wait% = 100%** for each backend's active time.
- A backend with high Wait% and `Lock:Transaction` as top wait is blocked by
  another transaction — check for long-running queries or deadlocks.
- System processes (bgwriter, checkpointer, walwriter) normally show IO waits.

---

### histogram

Latency distribution for a single event, using 16 log2 buckets from <1 us
to >=16 ms. Requires the `--event` flag.

**Columns:**

| Column | Description |
|--------|-------------|
| Bucket(us) | Duration range in microseconds |
| Waits | Count of occurrences in this bucket |
| % Waits | Percentage of total occurrences |
| Cumulative | Running cumulative percentage |
| (bar) | ASCII visualization (each `#` ~ 2%) |

**Example:**

```bash
sudo ./pg_wait_tracer --pid 12345 --view histogram --event IO:DataFileRead
```

```
════════════════════════════════════════════════════════════════════════════════
pg_wait_tracer — Event Histogram
════════════════════════════════════════════════════════════════════════════════

  Event: IO:DataFileRead | Total Waits: 8234 | Total: 3262.8 ms

  Bucket(us)        Waits    % Waits   Cumulative
  ────────────────────────────────────────────────────────────────────────────
       <1            123      1.5%        1.5%  #
    1-  2            456      5.5%        7.0%  ###
    2-  4           1832     22.3%       29.3%  ###########
    4-  8           2105     25.6%       54.8%  #############
    8- 16           1543     18.7%       73.6%  #########
   16- 32            987     12.0%       85.5%  ######
   32- 64            534      6.5%       92.0%  ###
   64-128            312      3.8%       95.8%  ##
  128-256            178      2.2%       98.0%  #
  256-512             89      1.1%       99.0%
  512-1K              42      0.5%       99.5%
   1K- 2K             18      0.2%       99.7%
   2K- 4K              8      0.1%       99.8%
   4K- 8K              4      0.0%       99.9%
   8K-16K              2      0.0%       99.9%
  >=16K                1      0.0%      100.0%
```

**Reading this output:**

- A **tight peak** in low buckets (2-16 us) means reads are hitting the OS page
  cache — fast.
- A **bimodal distribution** with peaks at both low and high buckets means some
  reads hit cache while others go to disk.
- A **heavy tail** (significant counts in 1K+ us buckets) indicates storage
  latency spikes — check disk I/O saturation.

---

### query_event

Wait events grouped by PostgreSQL query ID. Shows which queries cause which
waits. Requires `compute_query_id = on` (or `auto`) in `postgresql.conf`.

**Columns:**

| Column | Description |
|--------|-------------|
| query_id | PostgreSQL query fingerprint (matches `pg_stat_statements.queryid`) |
| Wait Event | Event name |
| Total Waits | Number of occurrences |
| Total (ms) | Cumulative duration |
| Avg (us) | Average per occurrence |
| Max (us) | Longest occurrence |
| % DB | Fraction of DB Time |

**Example:**

```
════════════════════════════════════════════════════════════════════════════════
pg_wait_tracer — Query Events (cumulative)    Backends: 12
════════════════════════════════════════════════════════════════════════════════

  query_id             Wait Event                 Total Waits     Total (ms)   Avg (us)     Max (us)    % DB
  ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
   5678234567890123     IO:DataFileRead                2340        1024.5      437.8     45623.1    4.1%
   1234567890123456     Lock:Transaction                145         892.3     6153.8    892100.5    3.6%
   5678234567890123     LWLock:WALInsert                890         334.2      375.5      8934.2    1.3%
   9876543210987654     IO:DataFileRead                 456         201.8      442.5      5432.1    0.8%
```

**Reading this output:**

- Cross-reference `query_id` with `pg_stat_statements` to find the actual SQL:
  ```sql
  SELECT query FROM pg_stat_statements WHERE queryid = 5678234567890123;
  ```
- The same query can appear multiple times with different wait events.
- A query with high `Lock:Transaction` time is waiting for row locks held by
  other transactions.

## Wait Event Classes

| Class | Description | Included in DB Time |
|-------|-------------|:---:|
| **CPU** | Backend running on CPU (not waiting) | Yes |
| **IO** | Storage I/O: reads, writes, syncs, extends | Yes |
| **LWLock** | Lightweight locks: WAL, buffers, proc array, etc. | Yes |
| **Lock** | Heavy locks: row, table, transaction, advisory | Yes |
| **BufferPin** | Waiting to pin a shared buffer | Yes |
| **Client** | Waiting for client: reading query, sending results | Yes |
| **IPC** | Inter-process communication: parallel workers, replication | Yes |
| **Timeout** | Timed waits: `pg_sleep()`, statement timeouts | Yes |
| **Extension** | Extension-generated events (e.g. pg_wait_sampling) | Yes |
| **Activity** | Idle states: waiting for next query | **No** |

## Understanding DB Time

**DB Time** is the total non-idle wall-clock time across all backends:

```
DB Time = CPU
        + IO
        + LWLock
        + Lock
        + BufferPin
        + Client
        + IPC
        + Timeout
        + Extension
```

Activity (idle) events are explicitly excluded. A backend sitting idle between
queries does not contribute to DB Time.

With N backends over an interval of T seconds, the theoretical maximum DB Time
is `N x T x 1000` ms. For example, 12 backends over 5 seconds = 60,000 ms max.
If DB Time is much lower than this maximum, most backends are idle.

**CPU** is the complement of wait time. It represents the time a backend was
actively running on a CPU core, not blocked on any resource.

```
CPU% = CPU / DB Time x 100
```

A healthy OLTP workload typically shows CPU% between 30-70%. Below 30% means
the system is wait-bound. Above 80% on a saturated system means CPU is the
bottleneck.

## Interpreting Results

### High IO%

Storage is the bottleneck. Check which IO events dominate:

- **DataFileRead** — Sequential or index scans reading from disk. Consider
  adding indexes, increasing `shared_buffers`, or using faster storage.
- **DataFileWrite** — Dirty page writes. May indicate checkpoint pressure
  (`checkpoint_completion_target`, `max_wal_size`).
- **DataFileSync** — fsync calls. Check `wal_sync_method` and storage write
  cache settings.
- **WALWrite/WALSync** — WAL is the bottleneck. Consider faster WAL storage
  or tuning `wal_buffers`.

### High Lock%

Transaction-level contention. Common causes:

- **Lock:Transaction** — Waiting for another transaction to commit/rollback.
  Look for long-running transactions holding row locks.
- **Lock:Relation** — Table-level lock conflicts (DDL vs DML).
- **Lock:Tuple** — Row-level lock contention. Multiple sessions updating the
  same rows.

### High LWLock%

Internal PostgreSQL contention:

- **WALInsert** — Many concurrent sessions writing WAL. Normal under heavy
  write load. Consider `wal_buffers` tuning.
- **BufferContent** — Contention on shared buffer pages. May indicate hot pages.
- **LockManager** — Lock table contention under very high concurrency.

### High Client%

The application is slow to consume results or send queries:

- **ClientRead** — Backend is waiting for the client to send the next query.
  If this is high during active workload, check network latency or connection
  pooler configuration.
- **ClientWrite** — Backend is waiting for the client to accept result data.
  The client may be processing results too slowly.

### High IPC%

Inter-process communication waits, typically from parallel query:

- **ParallelWorkerSync/ParallelQueryDSM** — Parallel workers synchronizing.
  Normal during parallel queries. If excessive, consider tuning
  `max_parallel_workers_per_gather`.

### Using Histogram

The histogram view reveals latency distribution patterns:

- **Unimodal peak at low values** (2-16 us for IO) — reads from OS page cache, healthy.
- **Bimodal** (peaks at both low and high buckets) — mix of cache hits and disk reads.
  The ratio shows effective cache hit rate.
- **Heavy right tail** (many events in ms+ buckets) — storage latency problems,
  possible I/O saturation.
- **Uniform spread** — unpredictable latency, possible noisy-neighbor or swap.

### Using query_event

Workflow for finding problematic queries:

1. Run `query_event` view to identify top query_id / event combinations
2. Look up the SQL in pg_stat_statements:
   ```sql
   SELECT queryid, calls, mean_exec_time, query
   FROM pg_stat_statements
   ORDER BY total_exec_time DESC;
   ```
3. Correlate: a query with high `IO:DataFileRead` time may need better indexes;
   a query with high `Lock:Transaction` time is hitting contention.

## How It Works

pg_wait_tracer uses a **CPU hardware debug register** (watchpoint) to trap
every write to `PGPROC->wait_event_info` in each PostgreSQL backend. PostgreSQL
updates this field on every wait event transition (entering a wait, leaving a
wait, changing wait type).

When the watchpoint fires, a BPF program runs in kernel context:

1. Reads the previous and new wait event values
2. Computes the duration of the previous state using `bpf_ktime_get_ns()`
3. Accumulates statistics (count, total, min, max, histogram bucket) into
   per-CPU BPF hash maps
4. Updates state for the next transition

Userspace reads the BPF maps at each `--interval` to produce the output views.
No data is copied per-event — only aggregated summaries are read.

This design means:
- **No PostgreSQL patches or extensions required** — works with stock binaries
- **No sampling** — every event transition is captured exactly
- **Low overhead** — BPF runs in nanoseconds, typically <5% TPS impact
- **No lock contention** — per-CPU maps avoid cache-line bouncing

## Requirements

- Linux kernel >= 5.8
- PostgreSQL 17 or 18 (PG14-16: limited, see [INSTALL.md](INSTALL.md))
- Root privileges (or CAP_SYS_ADMIN + CAP_SYS_PTRACE)
- x86_64 or aarch64

See [INSTALL.md](INSTALL.md) for build dependencies and setup instructions.

## License

pg_wait_tracer is released under the PostgreSQL License.
