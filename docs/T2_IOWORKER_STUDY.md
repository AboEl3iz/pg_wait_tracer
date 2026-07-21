# T2 io_worker empirical study — PG18 async I/O vs wait-event AAS (DRAFT)

Status: COMPLETE (T2 decision-gate input). Do not merge as-is.

Decision gate for Phase T2 (docs/ROADMAP_AND_STATUS.md) and docs/ROADMAP_AND_STATUS.md
"Risks and open questions" #6: how should pg_wait_tracer's AAS treat PG18
`io_method=worker` io_workers, and should on-CPU sessions be sampled as
active (finding AAS-1)?

## Test environment

- Box: Hetzner cx23 VM (dmitry-oriole-test, 116.202.96.172), Ubuntu 24.04,
  kernel 6.8.0-117-generic, 2 vCPU, 3.7 GB RAM, QEMU virtual SSD
  (rotational=0). (The original shared box was deleted externally mid-study;
  the whole sweep below was run on this dedicated replacement.)
- PostgreSQL 18.4 (Ubuntu 18.4-1.pgdg24.04+1, PGDG). Dedicated cluster
  `18/pgwt` on port 5439; packaged `18/main` left stopped.
- Cluster settings: `shared_buffers=128MB`, `track_io_timing=on`,
  `compute_query_id=on`, `autovacuum=off`, `io_workers=3` (PG18 default).
- Datasets: `t2bench` pgbench scale 300 (`pgbench_accounts` 3.8 GB, pkey
  642 MB; db ≈ 4.5 GB > RAM — sustained real disk reads) — the first two
  runs (r1/r2) used scale 200 (3.0 GB; partially re-cached by the OS during
  the window, noted below). `t2small` scale 10 (~150 MB, fully cached) for
  the CPU-bound windows.
- Workloads, 120 s windows, 4 client streams:
  - index-probe: `pgbench -S -n -c 4 -j 2 -T 120` (random PK point reads);
  - "seq-scan" (rA): same but `-f` = `SELECT count(*) FROM pgbench_accounts;`
    — **caveat discovered post-hoc: the planner picked a Parallel Index
    Only Scan on the 642 MB pkey** (2 parallel workers per query, ~11
    active procs, IPC:BtreePage convoy), so rA is a parallel-IOS convoy
    workload, *not* a read_stream/AIO workload. Kept as-is, labeled
    honestly;
  - heap-scan (rD): `SET max_parallel_workers_per_gather = 0; SELECT
    sum(abalance) FROM pgbench_accounts;` — forced single-process
    read_stream heap scan; this is the real AIO/io_worker test;
  - CPU-bound: `pgbench -S` on `t2small` (no cache drop).
  `sync; echo 3 > /proc/sys/vm/drop_caches` before every cold window.
- Tracer: master @ a4dd0e9 built on the box (`make`, distro bpftool 7.4.0,
  BTF present). Hardware watchpoints verified available (perf
  PERF_TYPE_BREAKPOINT probe succeeds).
- Per window: tracer `--mode full` (or `tiered` where stated) writing a
  trace dir; custom `t2_analyze` (event_reader-based, same objects as
  tests/cross_validate) → per-pid per-event exact/sampled ns + query_id
  split, clipped to the exact pgbench wall window; 1 s `pg_stat_activity`
  sampling of ALL pids (backend_type, state, wait_event, query_id);
  `pg_stat_io` before/after deltas; `iostat -x 5`.
- pid → backend_type mapping from the pg_stat_activity samples ("io worker"
  rows are PG18's io_workers). "psa" below = pg_stat_activity 1 s samples.

## Run matrix (all on PG 18.4, io_workers=3)

| run | io_method | tracer | workload | tps | disk (iostat sda) |
|-----|-----------|--------|----------|-----|--------------------|
| r1  | worker | full | idx-probe, scale 200 cold | 12 809 | 3 285 r/s, 32 MB/s, 25 % util |
| r2  | sync   | full | idx-probe, scale 200 cold | 6 674  | 2 910 r/s, 27 MB/s, 24 % util |
| rB1 | worker | full | idx-probe, scale 300 cold | 11 552 | 7 029 r/s, 68 MB/s, 52 % util |
| rB2 | sync   | full | idx-probe, scale 300 cold | 11 666 | 7 068 r/s, 68 MB/s, 52 % util |
| rA1 | worker | full | parallel IOS count(*), cold | 0.771 | 55 r/s, 5.6 MB/s, 2.3 % util |
| rA2 | sync   | full | parallel IOS count(*), cold | 0.681 | 54 r/s, 5.6 MB/s, 2.4 % util |
| rA3 | io_uring | full | parallel IOS count(*), cold | 0.766 | 52 r/s, 5.6 MB/s, 2.3 % util |
| rQ4 | worker | tiered | parallel IOS count(*), cold | 0.816 | 54 r/s, 5.6 MB/s, 2.0 % util |
| rD1 | worker | full | heap seq-scan sum(), cold | 0.525 | 2 137 r/s, 210 MB/s, 37 % util |
| rD2 | sync   | full | heap seq-scan sum(), cold | 0.527 | 1 745 r/s, 210 MB/s, 37 % util |
| rD3 | io_uring | full | heap seq-scan sum(), cold | 0.656 | 2 427 r/s, 252 MB/s, 33 % util |
| rQ4b | worker | tiered | heap seq-scan sum(), cold | 0.566 | 2 354 r/s, 225 MB/s, 38 % util |
| rC1 | worker | full | CPU-bound (cached -S) | 18 532 | ~0 r/s, 0.9 % util |
| rC2 | worker | tiered | CPU-bound (cached -S) | 23 985 | ~0 |

r1/r2 (scale 200) are kept for the Q3 latency analysis; their tps split
(12.8k vs 6.7k) reflects OS-cache state differences, not io_method — the
clean scale-300 pair rB1/rB2 is 11.5k vs 11.7k (identical within noise).
Reads in rB runs hit the physical device (68 MB/s sustained, dataset >
RAM); r1/r2 were partially re-cached by the OS mid-window; rC hit no disk.
rA windows read mostly from OS page cache (index 642 MB, re-cached after
the first pass: only 5.6 MB/s from disk). rD windows: PG-level read volume
was ~190 GB/window but the device served ~26 GB (210 MB/s × 122 s) — the
3.8 GB table vs 3.7 GB RAM means most 128 KB combined reads were page-cache
hits with a disk-bound minority; read latencies are therefore bimodal.
Worker vs sync tps is identical within noise in every clean pair
(rB1/rB2, rD1/rD2) — io_method made no throughput difference on this box.

Data-validity check (tracer vs independent instrumentation): in every
full-mode window, tracer per-backend exact totals ≈ wall clock (e.g. rA2:
backend non-idle 496.1 s + idle 0.7 s ≈ 4 clients × 123.4 s; rD1: 488.9 s ≈
4 × 121.9 s) and the event mix matches the 1 s pg_stat_activity samples
within a few percent (rA2 backend: tracer CPU 65 % / BtreePage 25 % /
AioIoCompletion 6.8 % / DataFileRead 1.7 % vs psa 66 / 23 / 8.1 / 1.8 %;
rD1 io_worker busy: tracer 80 % vs psa 78.4 %). psa "backend active AAS
5.00" always includes the psa sampling connection itself (+1.0); true
client-backend active AAS is 4.0 in the scan windows. The wait-event
traces are trustworthy; the query_id field is not (defect 1 below).

## Headline finding: OLTP index reads NEVER use io_workers in PG18

Under `io_method=worker` (PG18 default) with a pure random index-probe
workload (the classic OLTP shape), across 2×120 s windows and 2.8 M reads:

- io_workers spent **120.0 s of 120 s in `Activity:IoWorkerMain`** (idle);
  psa sampled io workers 330/330 ticks idle; io_worker `IO:DataFileRead`
  exact total: **0.01 s**.
- Client backends showed plain **`IO:DataFileRead`** (211.8 s), exactly like
  sync mode (rB2: 210.5 s) — **not** `AioIoCompletion` (0.01 s total).

Explanation: PG18 only routes read_stream-driven multi-block reads (seq
scan, bitmap scan, VACUUM, prewarm…) through AIO; single-block index/heap
point reads take the synchronous path inside the backend regardless of
io_method (and even staged AIO reads that haven't been submitted yet are
executed synchronously by the waiting backend). So for OLTP point-read
workloads, PG18 waits look exactly like PG17 and **no io_worker/AAS
question even arises**.

The io_worker question is real only for scan-shaped I/O — measured next.

## Q1 — double-representation (scan workload, worker vs sync)

Clean pair: rD1 (worker) vs rD2 (sync), identical workload and identical
throughput (0.525 vs 0.527 tps, both 64 scans, both ~210 MB/s from disk).
All times are exact tracer totals over the ~121.5 s bench window.

| quantity | rD1 worker | rD2 sync |
|---|---|---|
| backend `IO:AioIoCompletion` | 340.28 s (AAS 2.79) | 198.91 s (AAS 1.64) |
| backend `IO:DataFileRead` | 0.26 s | 92.45 s (AAS 0.76) |
| backend I/O-class waits total | 340.7 s (AAS 2.80) | 293.0 s (AAS 2.41) |
| backend CPU* | 144.67 s (AAS 1.19) | 185.86 s (AAS 1.53) |
| backend non-idle total | 488.9 s (**AAS 4.01**) | 485.9 s (**AAS 4.00**) |
| io_worker `IO:DataFileRead` | 244.70 s (AAS 2.01) | — |
| io_worker CPU* | 48.55 s (AAS 0.40) | — |
| io_worker non-idle total | 293.3 s (**AAS 2.41**) | 0 |
| io_worker busy fraction (3 workers) | 80.2 % (psa: 78.4 %) | — |
| **AAS excluding io_workers** | **4.01** | **4.00** |
| **AAS including io_workers** | **6.42 (+60 %)** | **4.00** |

Even in sync mode `AioIoCompletion` dominates: PG18 stages reads as AIO
handles in all modes; with 4 backends scanning the same table
(synchronized seq scans) one backend executes the staged I/O and the
other three wait on its handle. In worker mode the executor moves into
the io_worker and *all four* backends wait on `AioIoCompletion`.

Double-representation, measured:

- The same 1.546 M physical reads (pg_stat_io read_time 299.9 s) appear
  as **340.3 s of backend `AioIoCompletion` AND 244.7 s of io_worker
  `DataFileRead` — 585.0 s combined, 2.0× the sync-mode equivalent
  (293.0 s) for identical work at identical tps**.
- The backend-side wait alone (340.7 s) already fully represents the I/O
  wall time (≥ the sync-mode 293.0 s; the +16 % is handoff latency:
  submission queue + worker wakeup + completion signaling). The io_worker
  time is a shadow copy of time already counted in the waiting backends.
- Counting io_workers as active sessions breaks the "AAS ≤ connected
  sessions doing work" invariant (6.42 for a 4-session workload) and
  breaks worker-vs-sync comparability (6.42 vs 4.00 for the same load).
- io_worker CPU* (48.6 s) is real offloaded work: backend CPU drops from
  185.9 s (sync) to 144.7 s (worker); 144.7 + 48.6 = 193.3 ≈ 185.9.

io_uring for completeness (rD3): no io_workers; backend waits split into
`IO:AioIoUringSubmit` 102.4 s + `LWLock:AioUringCompletion` 91.4 s +
`IO:AioIoUringExecution` 58.5 s = 252.3 s I/O-class (AAS 2.07) + CPU
224.1 s. Single-copy like sync, but under three new event names.

Parallel-IOS convoy pair (rA1 worker vs rA2 sync) confirms the headline
finding scales: single-block index reads stay in the backends
(`DataFileRead` 12.6 s worker vs 8.3 s sync; io_workers 0.01 s busy in
264 dispatches over 124 s), and `AioIoCompletion` (48.1 vs 33.6 s) is
cross-backend staged-I/O waiting that exists in *both* modes. No
double-representation arises because io_workers never engage.

## Q2 — query attribution

The tracer's own `query_id` field cannot answer this on this box — it is
~0 on >99.9 % of full-mode records (defect 1 below, root cause now
identified). Ground truth from pg_stat_activity 1 s samples instead:

- **Backends: 100 % attributable in every window.** query_id was set on
  570/570 active client-backend samples in rD1, 570/570 in rD2, 568/568
  in rD3, 560/560 in the rA/rQ4 windows. Backend-side I/O waits
  (`AioIoCompletion`, `DataFileRead`, uring events) all occur inside
  statement execution with query_id available.
- **io_workers: 0 % attributable, structurally.** All io_worker psa
  samples carry query_id 0 (342/342 in rD1); an io_worker has no query
  context — it executes I/O staged by arbitrary backends, potentially
  batching for several queries. Per-pid wait capture can never attribute
  io_worker time to a query.

Consequence for the worker-mode I/O picture (rD1): of the 585.0 s
combined I/O wait, the attributable backend share is 340.7 s (58 %) and
the unattributable io_worker share is 244.7 s (42 %). If io_workers were
counted in AAS, 42 % of I/O-wait AAS would be permanent "no query" dark
matter in every per-query view. Under sync, 100 % of I/O wait is
attributable. The backend-side `AioIoCompletion` wait carries the full
per-query I/O story on its own (Q1: it alone ≥ the sync-mode total).

Caveat: with synchronized concurrent scans, a backend's `AioIoCompletion`
wait attributes to the query *consuming* the buffer, which may differ
from the query that staged the read — acceptable ASH-style attribution
(you charge whoever waited).

## Q3 — reconciliation with pg_stat_io: the ~0.80 ratio explained

The old cross-check "tracer IO ≈ 0.80 × pg_stat_io read_time, accepted as
async overlap" is now explained, and it has **nothing to do with AIO**:

| run | io_method | reads (tracer / pg_stat_io) | avg per read (tracer / psio) | delta | ratio |
|-----|-----------|------------------------------|------------------------------|-------|-------|
| r1  | worker | 2 824 210 / 2 824 421 (99.99 %) | 38.6 µs / 47.5 µs | +8.9 µs | 0.813 |
| r2  | sync   | 1 471 148 / 1 471 564 (99.97 %) | 77.2 µs / 85.7 µs | +8.5 µs | 0.901 |
| rB1 | worker | 2 626 648 / 2 626 930 (99.99 %) | 80.6 µs / 89.3 µs | +8.7 µs | 0.903 |
| rB2 | sync   | 2 652 910 / 2 653 157 (99.99 %) | 79.4 µs / 88.0 µs | +8.6 µs | 0.902 |
| rC1 | worker | 393 689 / 393 834 (99.96 %)     | 13.8 µs / 25.3 µs | +11.5 µs | 0.548 |

Scan windows extend the picture to *combined* reads (bulkread context,
~123–127 KB ≈ 15–16 blocks per op). Tracer side = the process actually
executing the read (io_worker `DataFileRead` in worker mode, backend
`DataFileRead` in sync); pg_stat_io credits the staging backend either way:

| run | io_method | reads (tracer / pg_stat_io) | avg per op (tracer / psio) | delta | ratio |
|-----|-----------|------------------------------|----------------------------|-------|-------|
| rD1 | worker (io_worker DFR) | 1 545 912 / 1 545 802 (100.01 %) | 158.3 µs / 194.0 µs | +35.7 µs | 0.816 |
| rD2 | sync (backend DFR)     | 1 128 526 / 1 126 610 (100.17 %) | 81.9 µs / 117.4 µs  | +35.5 µs | 0.699 |

- Event **counts** agree to 0.01–0.2 % everywhere — the tracer loses
  essentially nothing (no ringbuf drops at 20–45 k events/s; the tiny
  surplus in rD is boundary clipping / non-bulkread reads).
- The time gap is a **fixed per-op instrumentation-window difference,
  and it scales with op size**: ≈ 9 µs/op for single 8 KB reads
  (identical across worker/sync/latency in r1–rB2), ≈ 35–36 µs/op for
  16-block combined reads (again identical across worker/sync).
  `pg_stat_io.read_time`'s `INSTR_TIME` window covers the whole read
  path — AIO bookkeeping, submission, and per-page completion processing
  (checksum verification, buffer bookkeeping) — while the tracer measures
  only the `wait_event` set/clear window. Per-page completion work is why
  the gap grows ~linearly with blocks/op (~9 µs base + ~1.7 µs/page).
- The *ratio* therefore floats with mean op latency: 80 µs disk reads →
  0.90; 39 µs partly-cached → 0.81; 14 µs hot-cache → 0.55; 82 µs
  combined-op mixed-cache → 0.70.
- io_uring (rD3) is not reconcilable this way: read waits are spread over
  `AioIoUringSubmit`/`AioIoUringExecution`/`LWLock:AioUringCompletion`
  (160.8 s + 91.4 s) while psio read_time (83.7 s) measures only the
  synchronous portion — different quantities, no meaningful ratio.
- Conclusion: the historical "~0.80 under AIO" was **not** async overlap
  and **not** data loss — it is a per-op definition difference. A correct
  cross-check should compare **counts** (tight, ±0.2 %) and treat
  read_time as `wait_time + N_ops × (≈9 µs + ≈1.7 µs × blocks_per_op)`,
  or compare only on slow-I/O single-block workloads where the ratio →
  ~0.9+. Do not attempt the time cross-check under io_uring.

## Q4 — sampled vs exact AAS gap (AAS-1 measured)

CPU-bound workload (cached pgbench -S, 4 clients on 2 vCPU):

| measure | value |
|---|---|
| exact AAS, waits+CPU (rC1, full mode)     | **2.41** |
| exact AAS, waits only (rC1)               | **0.045** |
| psa `state='active'` (rC1, incl. the psa sampler itself) | 1.82 |
| psa in-statement active, sampler removed (rC1) | ~0.8 |
| EXEC-marker executor-only AAS (rC1)       | 0.35 |
| sampled AAS, tiered (rC2)                 | **0.017** |
| psa `state='active'` (rC2, incl. sampler) | 1.83 |

The sampler (which skips `wait_event_info==0`, AAS-1) reports **0.017 AAS
for a machine-saturating CPU workload whose true active load is ~0.8
(strict in-statement) to 2.41 (all we==0 wall time)** — a ≥98 %
under-report under either definition; the anomaly engine is blind to
exactly this. The exact tier, which records we==0 intervals as CPU,
gets 2.41.

(Correction vs an earlier draft: psa's active count always includes the
sampling connection itself — verified in the scan windows where 4 pegged
clients read as psa 5.00 — so rC1's true in-statement active is ~0.82,
not 1.82. The layering at 18.5 k tps on 2 vCPUs, per statement cycle of
~216 µs/client: executor proper ~19 µs (EXEC AAS 0.35), full in-statement
~44 µs (psa ~0.8), total we==0 ~130 µs (exact 2.41 — additionally
includes protocol/parse time between `ClientRead` segments *and*
runqueue time, inflated on a saturated 2-vCPU box). ASH semantics =
count of state='active' sessions ≈ the ~0.8 figure.)

I/O-bound heap-scan workload (rQ4b tiered vs rD1 full, same workload):

| measure | value |
|---|---|
| sampled AAS, backends (waits only, rQ4b) | 2.83 |
| exact waits-only AAS, backends (rD1) | 2.82 |
| psa ground truth active backends (both) | 4.00 |
| sampled AAS, io_workers (rQ4b) | 2.01 |
| exact io_worker DataFileRead AAS (rD1) | 2.01 |
| psa io_worker busy AAS (rQ4b) | 2.08 |

Two conclusions: (1) **the wait-side sampler is essentially exact** —
sampled wait AAS matches the exact tier to <1 % at 10 Hz over 120 s, for
backends and io_workers alike; (2) the *entire* sampled-vs-truth gap is
the skipped on-CPU share: backends 2.83 sampled vs 4.00 truly active
(−29 %), where the missing 1.19 is exactly rD1's backend CPU* AAS.

Parallel-IOS convoy workload (rQ4 tiered vs rA1 full): sampled non-idle
AAS = 1.18 (backends) + 1.94 (parallel workers) = 3.11 vs psa ground
truth 4.0 + 6.88 = 10.9 — a **71 % under-report**, because on this
2-vCPU box the convoy is mostly `we==0` time (on-CPU + runnable).

So the AAS-1 under-report is workload-dependent: −29 % (I/O-bound) →
−71 % (mixed convoy) → −99 % (pure CPU). The anomaly engine is blind in
proportion to how CPU-shaped the incident is — the worst case being
exactly the CPU-storm incident class.

(Note: the *exact* columns inside the tiered traces themselves are
contaminated and were not used for the comparisons above: the anomaly
engine escalated mid-window (rQ4b: exact capture covered ~60 s of 120 s —
io_worker exact records span 180 s / 3 workers), samples and exact blocks
overlap for those spans, and process-exit records back-fill phantom CPU
(defect 2): rQ4b backend exact "CPU" totals 309 s where ~69 s is real —
subtracting the ~240 s phantom reproduces rD1's 30 % CPU / 70 % Aio mix,
and rQ4b's exact AioIoCompletion rate over the escalated span is AAS
2.79, identical to rD1's 2.79.)

## Q5 — top wait events by process type

Heap seq-scan windows (exact seconds over ~121.5 s; idle events in
parens, excluded from AAS):

| rank | rD1 worker: backends | rD1 worker: io_workers | rD2 sync: backends | rD3 io_uring: backends |
|---|---|---|---|---|
| 1 | IO:AioIoCompletion 340.3 | IO:DataFileRead 244.7 | IO:AioIoCompletion 198.9 | CPU* 224.1 |
| 2 | CPU* 144.7 | CPU* 48.6 | CPU* 185.9 | IO:AioIoUringSubmit 102.4 |
| 3 | LWLock:BufferMapping 3.1 | (Activity:IoWorkerMain 72.2) | IO:DataFileRead 92.5 | LWLock:AioUringCompletion 91.4 |
| 4 | Timeout:SpinDelay 0.4 | LWLock:AioWorkerSubmissionQueue 0.04 | LWLock:BufferMapping 6.5 | IO:AioIoUringExecution 58.5 |
| 5 | IO:DataFileRead 0.3 | Timeout:SpinDelay 0.01 | IPC:BufferIO 1.7 | LWLock:BufferMapping 7.3 |

OLTP index-probe windows (rB1 worker / rB2 sync — identical shape, no
io_worker involvement: io_workers 100 % `Activity:IoWorkerMain`):

| rank | rB1 worker: backends | rB2 sync: backends |
|---|---|---|
| 1 | CPU* 222.7 | CPU* 223.8 |
| 2 | IO:DataFileRead 211.8 | IO:DataFileRead 210.5 |
| 3 | (Client:ClientRead 46.8) | (Client:ClientRead 47.0) |
| 4 | LWLock:BufferMapping 0.03 | Timeout:SpinDelay 0.03 |
| 5 | Timeout:SpinDelay 0.02 | LWLock:BufferMapping 0.02 |

Parallel-IOS convoy (rA1 worker; rA2 sync and rA3 uring are the same
shape ±10 %): backends+parallel workers — CPU* 820.6, IPC:BtreePage
383.9, IO:AioIoCompletion 117.6, IO:DataFileRead 32.6,
LWLock:BufferMapping 6.1; io_workers idle (0.02 s busy).

Takeaways: (1) worker mode's operator-visible signature is
`AioIoCompletion` replacing `DataFileRead` for scan I/O — event-name
mapping/documentation must treat it as first-class I/O wait; (2)
io_workers show exactly two significant states, `IO:DataFileRead` (busy)
and `Activity:IoWorkerMain` (idle) — a clean utilization signal; (3)
io_uring introduces three new backend-side wait names that must be
classified as I/O, including `LWLock:AioUringCompletion` which is an
LWLock-class event that is semantically I/O wait — a lock-fraction
anomaly rule counting it as "lock" would misfire (ESC-4 interaction);
(4) `AioIoCompletion` appears in *sync* mode too (staged-AIO handle
waits between backends) — dashboards must not label it "async-only".

## Tracer defects observed during the study (for the findings register)

1. **query_id attribution silently ~zero in full mode — ROOT CAUSE
   FOUND: non-PIE offset heuristic attaches the query_id uprobe to a
   dead address.** Evidence chain (qid_live experiment, bpftool with
   `kernel.bpf_stats_enabled=1` during a live full-mode trace):
   - tracer log: `pgstat_report_query_id at offset 0x1b0bc0`; bpftool
     link: `uprobe /usr/lib/postgresql/18/bin/postgres+0x1b0bc0`.
   - the PGDG binary is **PIE** (`ET_DYN`), symbol
     `pgstat_report_query_id` st_value = `0x5b0bc0`, and its LOAD
     segments have `p_offset == p_vaddr` — the correct uprobe file
     offset is `0x5b0bc0`.
   - `src/daemon.c:407` computes `qid_func_off = va > 0x400000 ? va -
     0x400000 : va` — a non-PIE-image-base assumption. `0x5b0bc0 -
     0x400000 = 0x1b0bc0` lands mid-function somewhere else in .text.
   - proof it never fires: `on_report_query_id` prog **run_cnt = 0**
     while the USDT exec probes ran 61 869 times in the same trace; all
     BPF `state_map` entries show `last_query_id: 0`.
   - the same heuristic is applied to `es_off` at `src/daemon.c:388`.
   - the USDT markers work because their offsets come from ELF notes
     (proper `uprobe_multi` offsets + ref_ctr), not from this heuristic.
   Severity beyond missing data: the tracer plants a breakpoint at an
   arbitrary byte of a hot PIE text section. Here it was never executed;
   on another binary layout it could fire inside an unrelated function
   (garbage register → garbage query_id) or land mid-instruction on an
   executed path. And the failure is **silent** — violates the "fail
   loudly" principle twice (attach "succeeds", attribution is 0).
   (Partial non-zero attribution seen in tiered windows, 10–18 %, comes
   from the memory-sampler path reading `st_query_id`, not this uprobe.)
2. **Process-exit records fabricate full-window CPU intervals in
   sampled/tiered traces** — rC2 (tiered): each pgbench disconnect emitted
   one exit transition with `old_event=0, duration≈119.9 s` (4 clients ×
   120 s = 479.8 s of phantom "CPU" in a trace whose sampler recorded
   0.017 AAS). Independently reproduced in rQ4b: backend exact "CPU"
   totals 309 s of which only ~69 s is real (escalation covered ~60 s;
   the exit records back-filled ~240 s ≈ 4 × the un-escalated span).
   Any consumer that accumulates transition durations from a
   sampled-tier trace (as A3's merge does for exact blocks) would inflate
   DB Time by ~4 sessions × window at every disconnect. Needs a T2/T4
   check on how `pgwt_replay_events`/server merge treat EXIT records in
   SAMPLES-only traces.
3. **Tiered traces contain overlapping samples AND exact blocks for the
   same span** — in rQ4/rQ4b the anomaly engine escalated mid-window
   (correctly — I/O-wait AAS ≈ 2.8 > threshold) and the trace then
   carries ~10 Hz samples *and* exact transitions for the escalated ~60 s.
   A naive consumer that sums both (samples × period + transition
   durations) double-counts those spans. This is the in-trace analogue
   of ESC-3 (live-view double accumulation); the FID-1 exact-wins merge
   must demonstrably apply to trace replay too.

## Recommendation

### (a) Active = on-CPU + non-idle wait (ASH semantics): YES

Adopt the ASH definition. The measured cost of today's waits-only
sampler (Q4): −29 % AAS on an I/O-bound scan, −71 % on a CPU-heavy
convoy, −≥98 % on a pure CPU storm (0.017 reported vs ~0.8 in-statement
/ 2.41 we==0 true) — the anomaly engine is blind exactly in proportion
to how CPU-shaped the incident is. The fix is cheap and low-risk: the
same measurements show the sampler's wait side is already accurate to
<1 % of the exact tier, so recording `we==0` samples as first-class CPU
samples completes the picture rather than re-tuning anything.

One semantic choice inside (a), quantified by rC1: exact `CPU*` (all
we==0 wall time) = 2.41 AAS, while true in-statement active ≈ 0.8 — a
3× spread on a high-tps OLTP shape, because we==0 also covers
protocol/parse time between `ClientRead` segments and (on a saturated
box) runqueue time. Recommendation: count a `we==0` sample as CPU only
when the backend is inside a statement (EXEC marker open /
`st_activity` running), matching pg_stat_activity's `active` and Oracle
ASH; otherwise idle. Without this gate, CPU-AAS on chatty OLTP would
read ~3× higher than any operator's mental model of "active sessions".
This also keeps the exact tier and sampled tier convergent (the exact
tier's CPU-between-markers can be classified the same way at merge
time).

### (b) io_workers: distinct utilization class, EXCLUDED from AAS/DB Time

Not "count-as-active", not "invisible". Grounds, all measured (Q1/Q2):

1. **Double-counting**: backend `AioIoCompletion` already carries the
   full I/O wall wait (340.7 s ≥ sync-equivalent 293.0 s). Adding
   io_worker busy time counts the same disk seconds twice (585 s vs
   293 s, 2.0×) and inflates AAS to 6.42 for a 4-session workload —
   breaking both the "AAS ≤ working sessions" invariant and
   worker-vs-sync comparability at identical throughput.
2. **Attribution**: io_worker time is structurally query-less (0/342
   psa samples with query_id; no query context exists in the process).
   Counting it in AAS would make 42 % of I/O-wait AAS permanently
   unattributable, while the backend-side wait is 100 % attributable
   and already tells the whole per-query story.
3. **OLTP irrelevance**: single-block index/heap reads never engage
   io_workers (headline finding; 0.01 s busy in 2.8 M-read windows), so
   for the most common workload class the choice changes nothing —
   another reason not to complicate AAS semantics for it.

But io_worker busyness is a valuable *capacity* signal that AAS must
not swallow: rD1 ran the 3 default io_workers at 80 % busy — near
saturation; part of the +16 % backend Aio-wait overhead vs sync is
plausibly queueing on saturated workers. Concretely:

- classify `io worker` backends (and their `IO:DataFileRead` /
  `Activity:IoWorkerMain` states) into a distinct "I/O workers" class,
  excluded from AAS/DB Time and from per-query attribution;
- surface "io_worker busy %" (busy = non-`IoWorkerMain` time / workers ×
  wall) as a utilization metric in views and the control-socket status,
  with a possible advisory ("io_workers ~saturated, consider raising
  io_workers") — treatment analogous to checkpointer/bgwriter, which
  also do session-caused work without being sessions;
- ensure `IO:AioIoCompletion` (and the three io_uring events, incl. the
  LWLock-class `AioUringCompletion`) are classified as I/O wait in the
  event tables and in the anomaly engine's lock-fraction rule (Q5
  takeaway 3), since they are the new face of scan I/O wait on PG18.

### Honest caveats on generality

- 2 vCPU box: `CPU*` conflates on-CPU with runnable-waiting-for-CPU
  (rA windows show 11 "active" procs on 2 cores). The (a) decision is
  unaffected (ASH counts runnable as active on purpose), but absolute
  CPU-AAS numbers here overstate *consumed* CPU.
- Cloud SSD + page cache: rD windows served ~86 % of PG-level read
  bytes from the OS page cache (table 3.8 GB ≈ RAM); latencies are
  bimodal and the 0.7–0.8 Q3 ratios reflect that mix. The
  double-representation *structure* (Q1) is mode-inherent, not
  latency-dependent, but the 80 % io_worker utilization and +16 %
  handoff overhead would shift on slower/faster storage.
- io_workers fixed at the default 3; with more workers the busy % and
  possibly the handoff overhead would drop. The exclusion argument
  (double-count + no attribution) is independent of worker count.
- The rA "seq-scan" windows were actually parallel index-only scans
  (planner choice, discovered post-hoc); the read_stream/AIO
  conclusions rest on the rD heap-scan windows, which were verified to
  hit the AIO path (`bulkread` context, io_workers busy).
- Single PG version (18.4) and single OS; io_uring measured but not
  latency-profiled (different instrumentation semantics, Q3).
- query_id numbers come from pg_stat_activity, not the tracer (tracer
  attribution broken on this box — defect 1, root cause identified).
