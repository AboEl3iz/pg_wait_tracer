# Tracing Analysis — Implementation Plan

Leverage the fact that pg_wait_tracer captures **every** state transition with
nanosecond precision (tracing, not sampling). Each phase builds on the previous.

## Data we have per event

```
{pid, old_event (wait_event_id), duration_ns, query_id, wall_timestamp}
```

---

## Phase 1 — Foundation (immediate value)

### 1. Session Timeline — Gantt chart for a single PID

Click a session row → see every wait event as a colored bar on a time axis.
The "EXPLAIN ANALYZE for wait events" — DBAs see exactly what happened during
a slow query.

- New server endpoint: `session_timeline` — returns raw events for a PID+time range
- UI: ECharts custom series (gantt-style horizontal bars)
- Color by wait class, tooltip shows event name + exact duration
- Builds on existing event_reader / raw events path

### 2. Wait Event Duration Percentiles — P50/P95/P99/max per event

Extend the Events tab with exact percentile columns. With tracing, these are
exact values, not estimates from sampling.

- Server: collect all durations per event_id, compute percentiles
- UI: add P50/P95/P99 columns to Events table
- Optionally show distribution shape indicator (normal / bimodal / long-tail)
- Pure computation on existing data, small change to `pgwt_compute_top_events`

---

## Phase 2 — Unique to tracing (differentiator)

### 3. Wait Transitions (Markov) — Per-query state flow diagram

For a selected query_id, compute the state transition probability matrix from
sequential events. Visualize as a Sankey diagram.

A DBA sees: "This query always goes CPU → DataFileRead (85%) → WALWrite (60%) → CPU"
and understands coupled bottlenecks — fixing IO won't help if WAL lock always follows.

- New server endpoint: `transitions` — returns transition matrix for query_id/PID
- Scan events sorted by (pid, timestamp), build NxN transition counts, normalize
- UI: Sankey or chord diagram showing top transitions with probabilities
- This is something **no sampling tool can do**

### 4. Wait Pattern Fingerprinting — Plan change detection

Build a transition signature per query_id per time window. Compare across windows:
if IO event count jumps 100x → seq scan replaced index scan → flag as plan change.

- Reuses transition computation from #3
- Compare fingerprints across time windows (e.g., hourly)
- UI: "pattern changed" indicator in Queries tab, click to see diff
- Detects plan changes without EXPLAIN, purely from wait event shapes:
  - Index scan: CPU → few IO:DataFileRead → CPU
  - Seq scan: CPU → many consecutive IO:DataFileRead → CPU
  - Hash join: CPU (long) → IO burst → CPU

---

## Phase 3 — Concurrency analysis

### 5. Peak Concurrency — Micro-burst detection

Add max_concurrent per class/event to each AAS bucket. Sampling averages these
away; tracing preserves them.

- Extend AAS compute: track max simultaneous sessions per wait state within bucket
- UI: overlay on AAS chart as markers or secondary axis
- Detect thundering herd: "43 sessions hit buffer_mapping simultaneously at 10:04:05"
- Small extension to `pgwt_compute_aas`

### 6. Burst Detection & Annotations

Find moments where N sessions simultaneously enter the same wait state within a
narrow window (< 10ms). Surface as clickable annotations on the AAS chart.

- Algorithm: scan events sorted by timestamp, sliding window convergence detection
- Detects: checkpoint storms, lock convoys, buffer mapping contention
- UI: annotation markers on AAS timeline, click → session timeline for affected PIDs
- Builds on peak concurrency (#5) and session timeline (#1)

---

## Phase 4 — Cross-session analysis

### 7. Lock Chain Detection

Find concurrent Lock:transactionid waiters and reconstruct wait chains.

- Scan events for overlapping Lock waits, match by PID + query_id correlation
- Reconstruct chains: A waits for B, B waits for C
- UI: new "Locks" section or tab, or overlay within session timeline
- Shows exact lock hold duration and chain depth

### 8. Cross-Session Interference Scoring

Temporal correlation between PIDs — mathematically prove "noisy neighbor" effects.

- "Every time PID 100 does IO, PIDs 200-250 enter LWLock"
- Cross-correlation at configurable time resolution
- Computationally expensive — targeted investigation tool, not real-time
- UI: interference report, victim/aggressor identification

---

## Phase 5 — ML / anomaly detection (research)

### 9. HMM Anomaly Detection

Train a Hidden Markov Model on "normal" transition matrices per query_id.
Alert when transition probabilities deviate significantly.

- Detects structural anomalies (buffer cache poisoning, plan regression) **before**
  latency spikes — the transition pattern changes even if total time hasn't yet
- Requires baseline collection period
- Research phase: evaluate whether the signal-to-noise ratio justifies the complexity

---

## Key insight: tracing vs sampling

| Capability | Sampling (1Hz) | Tracing |
|---|---|---|
| Events shorter than 1s | Missed | Captured |
| Event count vs duration | Unknown | Exact |
| State transition sequence | Lost | Preserved |
| Exact percentiles | Estimated | Exact |
| Micro-burst detection | Averaged away | Visible |
| CPU starvation (long CPU events) | Invisible | Detectable |
| Per-execution query analysis | Impossible | Session timeline |
| Plan change detection (no EXPLAIN) | Impossible | Wait fingerprint |
