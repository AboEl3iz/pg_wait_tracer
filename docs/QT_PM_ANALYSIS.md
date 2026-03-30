# Queueing Theory + Process Mining for pg_wait_tracer

Applied mathematics framework for extracting actionable insights from
nanosecond-resolution wait event traces.

---

## 1. What the trace data actually represents

Each event record: **backend PID was in state X for `duration_ns`, then moved
to state Y, during query Q.**

Critical observations about what `duration_ns` means per event class:

| Event class | What `duration_ns` includes | Decomposable? |
|---|---|---|
| **CPU** | Useful work + OS run-queue starvation (not distinguishable) | No |
| **IO:DataFileRead** | IO scheduler queue + physical read + possible OS cache hit | No |
| **Lock:transactionid** | Almost pure queueing (waiting for another backend) | Mostly yes |
| **LWLock:*** | Spin + sleep waiting for lightweight lock | Mostly yes |
| **Client:ClientRead** | Waiting for application (not a shared resource) | N/A |
| **IPC:SyncRep** | Waiting for replication acknowledgment | Depends on setup |

`duration_ns` is **sojourn time** (total time in the system = queueing delay +
service time), NOT pure service time and NOT pure wait time.

---

## 2. Queueing Theory — what's honestly computable

### 2.1 Directly measurable (no assumptions)

| Metric | Definition | Computation |
|---|---|---|
| **N(t)** | Backends in state X at time t | Count overlapping intervals |
| **lambda(t)** | Arrival rate into state X | Transitions into X / window |
| **W** | Sojourn time (Ws + Wq combined) | mean(duration_ns) |
| **Sojourn distribution** | Full empirical CDF | Exact from all duration_ns values |
| **X(t)** | Throughput (departures/s) | Transitions out of X / window |

### 2.2 What sojourn distribution shapes reveal

The shape of the duration_ns distribution tells you the mechanism without
knowing resource capacity (c):

- **Exponential** (single slope on log CDF) — memoryless, random arrivals,
  consistent with healthy simple queue.
- **Bimodal** (two peaks) — two populations. For IO:DataFileRead: cache hit
  (fast peak ~5us) vs physical read (slow peak ~200us). The ratio of peaks =
  effective cache hit rate.
- **Heavy tail** (P99 >> P50) — contention or resource starvation. For LWLock:
  occasional convoys. Tail index measures severity.
- **Nearly deterministic** (tight spike) — predictable service. Sequential scan
  IO: each page read takes ~same time.
- **Shifted exponential** (flat left + decay) — constant service time + random
  queueing on top. The shift point estimates Ws.

Track shape over time: unimodal-fast becoming bimodal = cache behavior changed.
Exponential becoming heavy-tailed = contention escalating.

### 2.3 Window size for lambda, mu

Not a fixed window — depends on event frequency:

| Event type | Typical rate | Min window for N>=30 |
|---|---|---|
| LWLock (us range) | 10K-100K/s | ~1ms |
| IO:DataFileRead | 100-10K/s | 10ms-300ms |
| Lock:transactionid | 1-100/s | 1s-30s |
| BufferPin | 0.1-10/s | 10s-5m |

Recommendation: adaptive window per event, governed by sample count (N >= 30),
not clock time. Or use EWMA that updates on every event.

### 2.4 W(N) regression — the key trick

For each occurrence of event X, we know:
- `duration_ns` (sojourn time W)
- N(t_arrival) — concurrent backends in state X when this backend entered

Plot W vs N:

```
W (us)
  |
  |                              .  .
  |                         .  .
  |                    .  .  .
  |              .  .  .
  |         .  .  .
  |    .  .  .
  | .  .
  |. . .     <- flat region: all being served, no queueing
  +--------------------------- N
          ^
      knee ~ c (capacity)
```

What this extracts:
- **Intercept** (W at N=1) -> estimate of pure service time Ws (no queueing)
- **Knee point** -> reveals c (capacity) from the data alone
- **Slope after knee** -> queueing behavior
- **Flat line** (no growth) -> c is large or resource not shared

Works best for events with enough variation in N.

### 2.5 What we CANNOT directly measure

| Metric | Why not | Estimation method |
|---|---|---|
| **c** (capacity) | Not in trace | Config (CPU cores, WAL locks) OR W(N) knee detection |
| **Ws** (service time) | Bundled with Wq | W at N=1, or min/P5, or W(N) intercept |
| **Wq** (queue time) | Bundled with Ws | W - Ws (once Ws estimated) |
| **rho = lambda/(c*mu)** | Requires c | Only when c known or inferred |

### 2.6 Little's Law — works at system level

L = lambda * W, where:
- L = avg backends in state X (our N)
- lambda = arrival rate
- W = mean sojourn time (our duration_ns mean)

Does NOT require knowledge of c, distribution, or queue discipline.

Primary use: **stationarity check**. Compute R(t) = L(t) / (lambda(t) * W(t)):
- R ~ 1 -> system is stable, measurements consistent
- R > 1 -> backends accumulating (load increasing or resource degrading)
- R < 1 -> backends draining
- The moment R deviates from 1 marks a regime change

### 2.7 Throughput saturation detection

Compare lambda(t) (arrivals) vs X(t) (departures) per event:
- X ~ lambda -> steady state
- X < lambda sustained -> resource overloaded, queue building
- The gap (lambda - X) = queue buildup rate

Model-free overload detector. No assumptions about c or distribution.

---

## 3. Process Mining — applied to PostgreSQL traces

### 3.1 Vocabulary mapping

| PM term | PostgreSQL equivalent |
|---|---|
| **Case** | One query execution: EXEC_START -> ... -> EXEC_END for (pid, query_id) |
| **Activity** | Wait event (CPU, IO:DataFileRead, LWLock:WALInsertLock, ...) |
| **Event** | One trace record: backend entered state X, stayed duration_ns |
| **Trace** | Ordered sequence of wait events for one query execution |
| **Event log** | Entire trace file |

### 3.2 Level A — Discovery: what actually happens

**Directly-Follows Graph (DFG)** — already implemented (transitions endpoint).
For every pair (A, B), count transitions A -> B.

DFG limitation: cannot distinguish sequence from choice:

```
Pattern 1 (sequence): CPU -> IO -> CPU -> WAL -> CPU
Pattern 2 (choice):   CPU -> IO -> CPU   (sometimes)
                       CPU -> WAL -> CPU  (other times)
```

Both produce identical DFG edges. But they represent fundamentally different
execution behaviors.

**Process models capture structure:**

- **Sequence**: A then B, always.
  "After reading data pages, always write WAL."

- **Choice (XOR)**: Sometimes A, sometimes B, never both.
  "Query either does index scan (few IOs) or seq scan (many IOs)."

- **Parallelism (AND)**: A and B both happen, order varies.
  "Parallel query workers: worker1 does IO on partition A, worker2 on partition B."

- **Loop**: A repeats N times before moving on.
  "Read page, process, read page, process, ... until done."
  (Already detected by variants computation — loop compression.)

**Algorithms**: Heuristics Miner (noise-tolerant, frequency-based) and
Inductive Miner (guarantees sound model, recursive decomposition) are the
most practical for this data.

**What it reveals**: the actual execution recipe as observed by the kernel.
Not what EXPLAIN says should happen, but what did happen, including all
resource interactions (WAL, locks, IPC) that EXPLAIN never shows.

### 3.3 Level B — Conformance: what changed

Build a reference model from a "healthy" baseline period. Replay new traces.
Measure deviation.

**Fitness** (0 to 1): fraction of observed behavior explained by the model.
- 1.0 = every trace fits perfectly
- 0.7 = 30% of events are unexplained (new paths, extra steps)

**Precision** (0 to 1): how much the model allows that wasn't observed.
- 1.0 = model is tight
- 0.5 = model is too permissive

What this detects:
- **Plan regression**: loop count changes (3 IO reads -> 80 IO reads)
- **Lock contention appeared**: Lock events appear in traces that never had them
- **New IO path**: TOAST reads appear after schema change
- **Structural change vs performance change**: conformance catches structural
  changes; distribution analysis (QT) catches performance changes on same structure

Compared to HMM anomaly detection (TRACING_ANALYSIS_PLAN Phase 5):
conformance checking tells you WHAT changed, not just that something changed.
"Lock:transactionid appeared where model says CPU->IO" is actionable.
"HMM probability dropped to 0.03" requires further investigation.

### 3.4 Level C — Enhancement: where is time spent

Overlay sojourn time statistics on the discovered process model.

Key insight: **the bottleneck is a path, not a node.**

Top_events says: "IO:DataFileRead consumed 45% of DB time." Incomplete.
Enhanced process model says: "IO:DataFileRead consumed 45% of DB time, but
only on the loop path when loop_count > 50. When loop_count < 10, IO is
negligible — time is in LWLock."

Same event, bottleneck for one variant, irrelevant for another. Without
process structure, you average across different behaviors.

Root cause attribution example:
```
Total execution: 150ms

  CPU (8ms, 5%)
  -> LOOP x85 [                          <- loop count is the problem
       IO:DataFileRead (avg 1.2ms x85 = 102ms, 68%)
       CPU (avg 0.3ms x85 = 25.5ms, 17%)
     ]
  -> LWLock:WALInsertLock (5ms, 3%)
  -> IO:WALWrite (9.5ms, 6%)

Conclusion: slow because of 85 loop iterations, not because IO is slow.
Each IO is fast (1.2ms). The problem is page count. Fix the plan, not storage.
```

---

## 4. Four fundamental signals

All database performance problems reduce to combinations of four signals:

| Signal | Detection method | What it means |
|---|---|---|
| **S1: Resource saturation** | rho -> 1, or W grows with N, or X < lambda | A resource can't keep up with demand |
| **S2: Process shape change** | Conformance drop, new variant, loop count shift | The execution recipe changed |
| **S3: Distribution shift** | Sojourn CDF changes shape (KS test, mixture model) | Same path, different performance |
| **S4: Resource coupling** | Cross-correlation of lambda/N between events, causal chain in process model | Resources affect each other |

---

## 5. Practical problems — what the DBA/developer sees

### Problem 1: "Database is slow right now. What's the bottleneck?"

**Signals**: S1 + S4

**Current tools**: pg_stat_activity shows 40 backends on Lock:transactionid.
But is that 40 backends sharing 10 slots (fine) or fighting over 1 (crisis)?

**QT+PM answer**:
- W(N) for Lock shows knee at N=2 -> capacity ~2. With 40 arrivals: overloaded.
- lambda(Lock) vs X(Lock) -> departures can't keep up -> queue building.
- PM: Query A (UPDATE users) is 70% of lock arrivals.
  Process model shows lock hold includes WAL flush:
  `CPU -> [Lock -> CPU -> IO:WALWrite -> CPU] -> done`
  Lock hold = 3ms service + 8ms WAL = 11ms effective.
- Root cause: WAL latency extends lock hold time. Fix WAL -> fix locks.

### Problem 2: "Query got slower after deployment. EXPLAIN shows same plan."

**Signals**: S2 + S3

**Current tools**: pg_stat_statements: mean_time 5ms -> 50ms. EXPLAIN: same
index scan. DBA is stuck.

**QT+PM answer**:
- Conformance: baseline model `CPU -> IO(x3) -> CPU -> WAL -> CPU`.
  Current: `CPU -> IO(x80) -> CPU -> WAL -> CPU`. Loop count changed 3 -> 80.
- Distribution: IO shifted from unimodal(4us, all cache) to bimodal
  (20% at 4us, 80% at 300us = physical reads).
- Root cause: same plan, but data grew or cache was evicted.
  The deployment didn't change THIS query — look for new queries polluting cache.

### Problem 3: "How many connections should I configure?"

**Signals**: S1 + S2

**Current tools**: trial and error, or "2 * cores" rule of thumb.

**QT+PM answer**:
- MVA throughput curve from trace data: throughput(N) peaks at N=32-40.
- Above N=48: Lock:transactionid saturates. Throughput decreases.
- PM: at N>48, query process models gain Lock steps absent at lower N.
  The process itself changes shape at high concurrency.
- Answer: pool = 32-40. Reads can go higher (no locks). Writes stay under 30.

### Problem 4: "Periodic latency spikes every 5 minutes"

**Signals**: S1 + S2

**Current tools**: monitoring shows spikes, suspect checkpoints, can't prove it.

**QT+PM answer**:
- lambda(IO:DataFileWrite) spikes every 300s. During spikes: rho(IO) = 0.95.
- W(IO:DataFileRead) increases 5x — reads compete with checkpoint writes.
- PM: during spikes, query models gain IO:DataFileWrite (FPI) and IO:WALSync
  steps that aren't in baseline. Checkpoint injects extra steps into execution.
- Fix: checkpoint_completion_target = 0.9, or separate WAL disk.

### Problem 5: "Is my query design efficient?" (developer question)

**Signals**: S2 + S3

**Current tools**: EXPLAIN shows cost estimates. Developer can't judge.

**QT+PM answer**:
- Process model: `CPU -> LOOP x500 [IO:DataFileRead -> CPU -> IO:DataFileRead -> CPU]`
  Two reads per loop iteration (outer page + inner index lookup). Nested loop join.
- Variant: 90% do 400-600 iterations. 3% do 5000+ (skewed join key).
- Distribution: inner IO bimodal (50% cached, 50% physical). For the 3% worst case:
  90% physical -> 5000 x 2 x 300us = 3 seconds.
- Answer: hash join for large keys, or pg_prewarm the inner index.

### Problem 6: "Reporting queries kill OLTP"

**Signals**: S3 + S4

**Current tools**: OLTP latency spikes when reports run. Suspects "resource
competition" but can't quantify.

**QT+PM answer**:
- Cross-correlation: when N(IO, query=REPORT) increases, W(IO, query=OLTP)
  increases 200ms later.
- OLTP IO distribution shifts: unimodal(5us) -> bimodal(5us + 300us) only
  while reports run.
- Report model: `CPU -> LOOP x100000 [IO:DataFileRead -> CPU]` — seq scan, 100K pages.
- Root cause: report evicts OLTP pages. OLTP hit rate drops 95% -> 40%.
- Fix: move reports to replica, or schedule outside peak hours.

### Problem 7: Autovacuum interference

**Signals**: S1 + S4

- BufferPin waits spike when autovacuum runs.
- PM: OLTP queries gain BufferPin step absent during normal operation.
- W(N) for BufferPin: c=1 per page, vacuum holds cleanup lock.
- Cross-correlation: N(BufferPin, OLTP) correlates with N(CPU, autovacuum).

### Problem 8: Index bloat over weeks

**Signals**: S2 (gradual)

- Same query, same plan. IO loop count grows: 10 -> 15 -> 25 -> 40 over weeks.
- Each IO is fast (no distribution shift) — it's not a cache problem.
- Root cause: index bloat or data growth. More leaf pages to scan.
- Detection: track loop count per query_id over days.

### Problem 9: Parallel query worker starvation

**Signals**: S4 + S1

- Workers' IO arrivals are correlated (lambda cross-correlation).
- IO saturates only during parallel queries (rho(IO) < 0.3 normally, > 0.9
  during parallel execution).
- PM: leader's process model shows IPC:ParallelFinish wait grows with worker count.
- Root cause: workers compete for IO bandwidth. Diminishing returns above K workers.

### Problem 10: Connection storm at application restart

**Signals**: S1 + S2

- CPU saturates for 2-3 seconds at app restart.
- PM: during storm, process models dominated by authentication/startup steps,
  normal query execution delayed.
- lambda(CPU) spikes by 10x, exceeds capacity.

### Problem 11: Replication lag from specific write patterns

**Signals**: S2 + S1

- Certain queries have WAL-heavy process models (large updates, FPI-heavy ops).
- PM: query 0xABCD's model: `CPU -> LOOP x200 [IO:DataFileWrite -> CPU -> IO:WALWrite -> CPU]`
  generates disproportionate WAL volume.
- QT: when lambda(IO:WALWrite, query=0xABCD) spikes, replication lag increases.

### Problem 12: Temp file spill

**Signals**: S2 + S3

- IO:DataFileWrite appears in queries that normally never write.
- PM: conformance drop — query model gains write loop not in baseline.
- Distribution: new IO population with different latency profile (temp file writes).
- Root cause: work_mem exceeded, sort/hash spilled to disk.

---

## 6. Interface concept: Investigation Canvas

### 6.1 Three layers

**Layer 1 — Timeline (WHEN)**

Existing AAS chart with automated anomaly markers overlaid:

```
AAS ||||||||||||||||||||||||||||||||||||||||||||||||
    ^              ^         ^
    |              |         |
    S1:Lock        S3:IO     S2:query 0xABCD
    saturating     bimodal   new variant
                   shift     appeared
```

Markers in DBA language: "Lock:transactionid approaching saturation", not
"rho > 0.8". QT/PM are invisible — they're the detection engine.

**Layer 2 — Investigation (WHAT and WHY)**

Click a marker -> evidence chain panel opens:

```
+-- FINDING: Lock:transactionid is overloaded -----+
|                                                    |
|  EVIDENCE 1: Sojourn time grows with concurrency   |
|  [W(N) chart]                                      |
|  At N=1: W=3ms (service time)                      |
|  Knee at N=2 -> capacity ~ 2                       |
|                                                    |
|  EVIDENCE 2: Top contributors                      |
|  Query 0xA1B2 (UPDATE users) - 70% of arrivals    |
|  Query 0x3C4D (SELECT FOR UPDATE) - 25%       [->] |
|                                                    |
|  EVIDENCE 3: Lock hold includes WAL                |
|  Process model for 0xA1B2:                         |
|  CPU -> [Lock -> CPU -> IO:WALWrite -> CPU]        |
|         |--- lock held ------------------|         |
|                                                    |
|  SUGGESTED CAUSE: WAL flush inside lock path       |
|  SUGGESTED ACTION: faster WAL or async commit      |
|                                                    |
|  [Compare to baseline] [Affected queries] [Predict] |
+----------------------------------------------------+
```

Evidence blocks connected by narrative. Each is a chart, process model, or
table. [->] arrows are cross-links to other investigations.

**Layer 3 — Prediction (WHAT IF)**

From any investigation, tweak parameters:
- "What if pool size = 30?" -> MVA recomputes throughput
- "What if WAL latency halves?" -> W(N) recomputes, predict new rho
- "What if I add an index?" -> loop count drops 200 -> 3, predict new rho(IO)

### 6.2 Investigation pattern

Every investigation follows:

```
SIGNAL -> SCOPE -> MECHANISM -> ROOT CAUSE -> ACTION
```

1. **Signal**: anomaly detected (automated, from S1-S4)
2. **Scope**: which resource, query, time range (narrowing)
3. **Mechanism**: what is physically happening (QT + PM evidence)
4. **Root cause**: why — often a causal chain across resources
5. **Action**: what to change, with predicted impact

### 6.3 Implementation: rule engine + guided exploration

**Option A — Rule engine** (recommended as primary):

Codify investigation patterns as rules (~15-25 rules cover 90% of cases):

```
RULE: resource_saturation
  WHEN: W(N) slope > threshold after knee
    AND N(event, current) > knee * 2
  THEN:
    - compute W(N) chart
    - find top queries by lambda contribution
    - for each: extract process model, check if other events
      are inside the critical section
    - generate evidence chain
    - suggest: reduce concurrency or speed up critical path
```

```
RULE: cache_eviction
  WHEN: IO:DataFileRead distribution shifts unimodal -> bimodal
    AND bimodal shift correlates with N(IO, other_query_type)
  THEN:
    - compute distribution before/after
    - identify evicting query by cross-correlation
    - show process models of victim and aggressor
    - suggest: separate workloads or increase cache
```

Deterministic, transparent, explainable. DBA sees why tool reached conclusion.

**Option B — Guided exploration** (fallback):

When no rule fires but anomaly markers exist:
- Present the raw signals on timeline
- Suggest related views ("you're looking at Lock saturation — here are
  contributing queries, click to see process models")
- DBA connects the dots manually

Over time: when DBAs repeatedly follow the same exploration path, codify
it as a new rule.

---

## 7. Architecture: new server endpoints needed

### 7.1 QT metrics endpoint

Per event, per time window:
```json
{
  "event": "IO:DataFileRead",
  "lambda": 4500.0,
  "X": 4480.0,
  "mean_W_us": 120.5,
  "N_avg": 2.3,
  "N_max": 18,
  "littles_ratio": 1.02,
  "distribution": {
    "p5_us": 3.2,
    "p50_us": 45.0,
    "p95_us": 320.0,
    "p99_us": 1200.0,
    "shape": "bimodal",
    "peaks_us": [4.5, 280.0],
    "peak_weights": [0.35, 0.65]
  },
  "W_N_pairs": [[1, 42.0], [2, 45.0], [3, 48.0], [5, 85.0], ...],
  "W_N_knee": 3,
  "W_N_intercept_us": 40.0
}
```

### 7.2 Conformance endpoint

```json
{
  "query_id": "0xABCD1234",
  "baseline_period": "2024-01-15T10:00:00Z/2024-01-15T11:00:00Z",
  "current_period": "2024-01-15T14:00:00Z/2024-01-15T15:00:00Z",
  "fitness": 0.62,
  "deviations": [
    {"type": "new_activity", "activity": "Lock:transactionid", "frequency": 0.15},
    {"type": "loop_count_change", "loop": "IO:DataFileRead", "baseline_avg": 3.2, "current_avg": 78.5}
  ]
}
```

### 7.3 Anomaly detection endpoint

```json
{
  "findings": [
    {
      "signal": "S1",
      "event": "Lock:transactionid",
      "severity": "critical",
      "summary": "Lock:transactionid approaching saturation",
      "evidence_refs": ["qt_metrics/Lock:transactionid", "wn_curve/Lock:transactionid"],
      "related_queries": ["0xA1B2", "0x3C4D"],
      "timestamp_ns": 1705312800000000000
    }
  ]
}
```

### 7.4 Prediction endpoint (MVA)

```json
{
  "request": {"backends": 60, "modifications": {"IO_Ws_factor": 0.5}},
  "result": {
    "throughput_qps": 4200,
    "bottleneck": "LWLock:WALInsertLock",
    "per_resource": [
      {"event": "CPU", "rho": 0.45, "mean_W_us": 120},
      {"event": "IO:DataFileRead", "rho": 0.38, "mean_W_us": 55},
      {"event": "LWLock:WALInsertLock", "rho": 0.92, "mean_W_us": 340}
    ]
  }
}
```

---

## 8. Implementation order

### Phase 1: QT metrics (foundation for everything)

Add W(N) computation, distribution shape classification, lambda/X/N time
series, and Little's Law ratio to pgwt-server. Extends existing top_events
and heatmap infrastructure.

### Phase 2: Anomaly detection (S1-S4 signals)

Automatic detection of saturation, distribution shifts, throughput divergence.
Produces anomaly markers for timeline overlay.

### Phase 3: Conformance checking

Store baseline process models (from variants). Replay new traces. Compute
fitness score. Detect structural changes.

### Phase 4: Investigation rules

Codify 10-15 investigation patterns connecting signals to evidence chains
and suggested actions.

### Phase 5: Prediction (MVA)

Closed network model from per-resource service demands. Throughput curve.
What-if parameter sensitivity.

---

## 9. What QT + PM give together that neither gives alone

| What you learn | QT alone | PM alone | Both together |
|---|---|---|---|
| Which resource is stressed | rho, W(N), saturation | — | — |
| Which queries hit it | — | Path analysis | Which queries take which paths under which conditions |
| Why it's stressed | Service time vs queueing | Causal chains in process | Root cause: "WAL latency extends lock hold" |
| When behavior changed | Distribution shift, Little's Law | Conformance score | Structural vs performance change |
| What to do | Capacity prediction | Path optimization | Targeted: which knob fixes which path |
| How much it matters | Quantified (W increase, rho) | Identified (which variant) | "Variant B is 10x slower AND affects 15% of traffic" |
