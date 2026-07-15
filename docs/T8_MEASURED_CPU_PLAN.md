# T8 — Measured CPU for the exact tier (design + implementation plan)

Status: ⬜ PLANNED — **gates v0.13** (extends the Trust Milestone,
docs/TRUST_MILESTONE_PLAN.md; found during its EL9 close-out validation).
Date: 2026-07-15
Base branch: `t2-fix-el9-cpu-gate` (PR #46) — this phase FINISHES that PR
(one known defect in it, see §4) and builds on it.

This document is self-contained: it carries the problem, the evidence,
the rejected designs and why, and the chosen design at implementation
depth. An implementer should not need the discussion that produced it.

---

## 1. The problem

The exact (full) tier's only sensor is a hardware watchpoint on
`PGPROC->wait_event_info` writes. PostgreSQL writes that field on wait
ENTRY and wait EXIT. CPU time is therefore never observed — it is
**inferred as the gap between two wait events**, and the inference is
only materialized (emitted as a trace record) when the NEXT wait begins.

Three consequences, all confirmed live on the EL9 validation box
(Rocky 9.7, PG 18.4):

1. **In-progress CPU is invisible.** A backend computing for 60 s has
   emitted nothing; its CPU appears only retroactively when it next
   waits. Repro: `DO $$ ... FOR i IN 1..200000000 LOOP ... $$` running
   at capture time → `--mode full` time_model shows `CPU* 0.0%`; the
   trace file is empty (341 bytes, zero events).
2. **Capture end loses the open CPU stretch entirely.** Window close,
   de-escalation, daemon exit: T3's `flush_open_intervals()` flushes
   open WAITS but not the on-CPU state — the straddling CPU is never
   written.
3. **The live view lags structurally**: an interval shows 0% CPU during
   a storm, a later interval shows a giant spike that belongs to the
   past.

This is NOT a synthetic-only case. A runaway nested-loop over **cached**
data — the classic bad-plan incident — sets no wait events (shared-
buffer hits don't wait), so it is seconds of pure CPU with zero writes
to the watched field.

**It also reaches the DEFAULT (tiered) mode.** Inside an escalation
window, T1's exact-wins merge (correctly) drops sampled records within
marker-covered ranges — but the exact stream it defers to is blind to
in-progress CPU. So for the duration of every escalated window,
recorded CPU visibility degrades to "whenever transitions happen to
fire." Ironically, post-T2 a CPU storm *triggers* escalation, and
escalation then suppresses the sampled CPU data that triggered it.
The anomaly ENGINE is unaffected (it reads sampler batches pre-merge);
the recorded/viewed window data is affected.

The wait side of the exact tier is untouched by all of this — waits are
event-bounded and exact. The defect is exclusively the CPU quantity.

## 2. The design principle

**Stop inferring CPU. Measure it.** The kernel scheduler already
accounts every task's on-CPU time exactly:
`task_struct->se.sum_exec_runtime` — nanosecond precision, advances
ONLY while the task is actually running on a CPU. Two properties make
it a perfect fit:

- The watchpoint BPF handler already executes **in the target backend's
  task context** on every wait boundary → one kernel-memory read per
  fire captures the accumulator exactly at each boundary.
- It is a **monotonic accumulator**, so "how much CPU since the last
  boundary" is answerable at ANY moment by reading the counter and
  subtracting the stored base — with **zero information loss at any
  read cadence**. Reading an exact accumulator at report time is not
  statistical sampling: nothing flickers below a sampling floor,
  nothing aliases, magnitudes are exact.

Bonus that falls out of the arithmetic:
`gap_wall_time − measured_cpu = off-CPU-unaccounted time` — runqueue
waits, cgroup throttling, page-fault stalls, uninstrumented kernel
blocking. This is the Oracle-10046 "unaccounted-for time" equivalent,
per gap, per query — a diagnosis neither pg_stat_* nor any sampler can
make (e.g. "this query spent 40% of its time waiting for a CPU").
Also note the old gap-inference actively MIS-COUNTS this as CPU: a
preempted/throttled backend is "not waiting" but not running; on a
saturated host (the incident case) the inference lies most.

Built-in self-validation (trust-milestone flavor): across a gap whose
label is a WAIT, the accumulator delta must be ≈0 (the task slept).
Every trace continuously proves its own CPU accounting; assert it in
cross-validation.

## 3. Rejected designs (do not re-litigate)

- **Periodic userspace flush of the inferred gap ("Option A"):** emits
  partial CPU intervals on a poll clock. Rejected: it does not measure
  CPU, it re-infers it with poll quantization (sampling in disguise —
  the project exists to avoid exactly this), and it inherits the
  runqueue-counted-as-CPU error. Also needed fiddly seam-dedup against
  concurrent BPF emissions.
- **Keep sampled CPU inside escalation windows (merge change):** the
  sampler's CPU samples already exist during windows; stop dropping
  them. Rejected: when the straddling command finally waits, the exact
  tier emits one giant retroactive CPU interval overlapping the
  retained samples → double count unless the merge tracks CPU coverage
  at interval granularity (re-creates the FID-1 class of subtlety), and
  it does nothing for `--mode full` users, and CPU stays
  sampled-fidelity inside exact windows.
- **`bpf_timer` periodic BPF emission:** not available on EL8 (4.18),
  which is a first-class target platform. Dead on arrival.
- **sched_switch/finish_task_switch BPF accounting (offcputime-style)
  ("S3"):** exact and even more powerful (per-switch off-CPU
  breakdown), but the hook fires for EVERY context switch system-wide
  (filter cost on 100k+/s events) and is a much larger BPF surface.
  Deferred as the future off-CPU-analysis track, not needed for this
  fix. The chosen design's data model (cpu_ns per gap + unaccounted
  derivation) is forward-compatible with it.

## 4. Prerequisite: finish PR #46 (the straddle command-gate fixes)

PR #46 fixed the *command-open gate* half of the straddle problem
(edge-triggered `on_report_activity` misses commands already in flight
at attach) in both tiers:
- sampler: per-tick `debug_query_string` level-check
  (`pgwt_read_cmd_gate` in src/sampler.c) for at-risk samples;
- exact tier: preseed seeds `cmd_open` from `debug_query_string`
  (src/backend.c, `preseed_state_map`).

**Known defect in #46, caught by its own new CI regression phase
(`phase_cpu_straddle` in tests/test_capture_smoke.py) on the PIE
matrix cells (PG13/PG18 Ubuntu runners, run 29431156369):** both new
reads use `d->debug_query_string_addr` — the raw symbol vaddr from
`pgwt_find_symbol_offset()`. Correct on non-PIE binaries (the EL9
Rocky box where it was verified: `readelf -h` → `Type: EXEC`); WRONG on
PIE binaries, where the per-process runtime VA =
`pgwt_find_load_base(pid, binary) + vaddr` (the #24 lesson). Fix in
this phase, first commit:
- Resolve `debug_query_string` per backend the same way other globals
  are resolved (see `pgwt_resolve_backend_wait_addr` /
  `pgwt_read_pointer` usage in src/backend.c and the load-base API in
  src/discovery.h: `pgwt_find_load_base`, `pgwt_resolve_symbol`).
  For PIE, per-pid VA differs per process only by load base — which is
  identical for all backends of one postmaster (fork inheritance), so
  resolve ONCE per daemon (vaddr + load_base of any live backend) and
  store the runtime VA in `d->debug_query_string_addr`; keep the raw
  vaddr separately for the BPF rodata (the BPF query-text path reads it
  in-task where... NOTE: the BPF-side `debug_query_string_addr` rodata
  has the same PIE bug for the query-TEXT capture path — verify and fix
  with the same load-base-adjusted VA; in-task bpf_probe_read_user uses
  the target's runtime VA too).
- Acceptance: the `phase_cpu_straddle` cells go green on the PIE matrix
  (PG13/17/18 Ubuntu) AND on the non-PIE EL9 box.

## 5. The chosen design (S1 + fallback), implementation spec

### 5.1 BPF side (src/bpf/pg_wait_tracer.bpf.c)

- `struct pgwt_pid_state` (src/pg_wait_tracer.h): add
  `__u64 last_cpu_ns;` — the value of `se.sum_exec_runtime` at the last
  emitted boundary for this pid.
- In the watchpoint handler (the transition path that emits
  `pgwt_trace_event`, currently ~line 238-288): read the current
  accumulator ONCE per fire:
  ```c
  struct task_struct *t = (void *)bpf_get_current_task();
  u64 cpu_now = 0;
  if (bpf_core_field_exists(t->se.sum_exec_runtime))
      cpu_now = BPF_CORE_READ(t, se, sum_exec_runtime);
  ```
  Then: `evt.cpu_ns = cpu_now - st->last_cpu_ns` (0 if either is 0 /
  feature off), and update `st->last_cpu_ns = cpu_now` on EVERY fire
  (including the redundant-write early-return? NO — only when an event
  is emitted or state transitions; the redundant-write suppression path
  must NOT advance last_cpu_ns, or CPU between suppressed fires would
  be lost from the next real gap).
  Also snapshot `last_cpu_ns` in the `!st->wp_live` seed path
  (first-fire-after-seed) — do NOT emit for that pseudo-gap.
- `struct pgwt_trace_event` (internal ringbuf ABI, src/pg_wait_tracer.h):
  add `__u64 cpu_ns;`. Gate everything behind a
  `volatile const bool cpu_accounting;` rodata flag the daemon sets
  after its startup probe (see 5.4), so old kernels/BTF-less builds
  compile out cleanly via the existing pattern.

### 5.2 Userspace seed/flush (src/backend.c, src/escalation.c, src/daemon.c)

- `preseed_state_map()`: when seeding, also read the backend's current
  accumulator (`/proc/<pid>/schedstat` field 1 — the same
  `sum_exec_runtime`, in ns) and store it in `init_state.last_cpu_ns`.
  mem_fd is already open there; schedstat is a separate tiny read
  (`/proc/<pid>/schedstat`, first space-separated field).
- **Terminal flush (fixes symptom #2):** extend T3's
  `flush_open_intervals()` (src/escalation.c) — which currently emits
  open WAITS — to also emit the open CPU stretch for backends whose
  `last_event == 0 && cmd_open`: read schedstat now, emit a final
  record `old_event=0, new_event=0(flush), duration=now-last_ts,
  cpu_ns=sched_now-last_cpu_ns`. Call the same helper on daemon
  shutdown for `--mode full` (today only de-escalation flushes; the
  shutdown path must flush too — that is why the DO-loop trace was
  EMPTY, not just CPU-less).
- **Live view (fixes symptom #3):** the existing open-interval display
  path (`pgwt_read_state_map` / the daemon display tick) computes the
  in-progress interval for on-CPU backends as
  `cpu = schedstat_now - entry.last_cpu_ns`,
  `unaccounted = (now - entry.last_ts) - cpu`. Display-time computation
  only — NO interim trace records are written mid-gap (this is what
  makes the design seam-free: records exist only at real boundaries +
  terminal flush, so there is nothing to dedup).

### 5.3 Trace format v3 (src/event_writer.c, src/event_reader.c, docs/TRACE_FORMAT.md)

- Bump `PGWT_TRACE_VERSION` to 3. TRANSITIONS block encoding gains one
  varint column per event: `cpu_ns`. SAMPLES blocks unchanged.
- Reader: v3 native; v2 files remain readable — their events surface
  `cpu_ns = PGWT_CPU_NS_UNKNOWN` (UINT64_MAX sentinel), and the compute
  layer falls back to legacy gap-inference for those events. v1 stays
  rejected (established decision).
- Golden fixtures: per docs/RELEASING.md + tests/fixtures/golden/README
  (T7's revisioning instructions): ADD `tests/fixtures/golden/rev3/`
  with a deterministic v3 fixture + checksum test; the rev2 decode test
  KEEPS running (that is the whole point of fixture revisions).
- Update docs/TRACE_FORMAT.md (v3 section: column, sentinel, and the
  flush-record convention `old_event=0,new_event=0`).

### 5.4 Capability probe + fallback chain (src/daemon.c, Track-C style)

At startup, in order:
1. **S1 (preferred):** BTF present (already required for the full tier)
   AND `bpf_core_field_exists(se.sum_exec_runtime)` verifies AND
   `/proc/self/schedstat` exists (CONFIG_SCHED_INFO — present on RHEL
   and Ubuntu kernels). → full measured-CPU mode.
2. **S1b:** BPF read works but schedstat missing → per-backend
   `perf_event_open(PERF_COUNT_SW_TASK_CLOCK, pid)` counting fd
   (src/perf_event.c alongside the watchpoint fd; counting-only, no
   ringbuf) provides the residual/seed reads via `read(fd)`. Mind
   RLIMIT_NOFILE headroom (T4 already bumps it; double the per-backend
   budget).
3. **Legacy:** neither → `cpu_accounting=false`, events carry the
   UNKNOWN sentinel, compute uses gap-inference, and the capability is
   reported LOUDLY in startup log + control-socket `status` (existing
   capability-report pattern). Never silent.

### 5.5 Compute + views (src/server.c, src/compute.c, tests/mock_server.py, web)

Semantics decisions (locked here so the implementer doesn't wander):
- **DB Time total is UNCHANGED** (still wall-clock active time =
  waits + gaps). Within a gap: `CPU* = measured cpu_ns`,
  **new row/class `Off-CPU*`** = `gap − cpu_ns` (floor at 0; clamp
  cpu_ns to gap on clock skew, count clamps in a metric). Off-CPU* is
  part of DB Time (it is real query elapsed time), rendered as a
  sibling of CPU* in time_model, and as its own AAS category key
  `offcpu` (stacked color distinct from `cpu`). Fidelity: Off-CPU*
  exists only where measured cpu_ns exists (exact tier, v3 data);
  sampled windows and v2 files show CPU* as today and NO Off-CPU row
  (not zero — absent).
- Wait-labeled gaps: measured cpu_ns during waits (should be ≈0) is
  folded into the wait's row as today (do NOT create per-wait cpu
  splits) but accumulated into a `wait_gap_cpu_ns_total` self-check
  counter (see 5.6).
- query_event / session views: CPU columns switch to measured values
  automatically (they consume the same per-event records); no schema
  change beyond the aas/time_model additions. Mirror every response
  shape change in tests/mock_server.py — protocol-drift enforces.
- Sampled tier: UNCHANGED this phase (different fidelity class by
  design). Do not try to blend schedstat into the sampler here.

### 5.6 Tests / acceptance (the proof, in order of authority)

1. **Pure-CPU straddle goes green:** extend `phase_cpu_straddle`
   (tests/test_capture_smoke.py) with a `pure_cpu=True` variant using
   the DO-loop workload (no waits at all), asserted in `--mode full`
   AND `--mode tiered`: CPU present in live view AND in the trace
   (terminal flush), magnitude cross-checked against
   `/proc/<pid>/stat` utime delta (±20%). This is THE acceptance test —
   it is the exact shape that produced an empty trace.
2. **Self-check assertion:** cross-validation (tests/cross_validate.c
   or test_accuracy) asserts wait-gap cpu_ns ≤ 1% of wait time over a
   pgbench window (the "traces prove their own CPU accounting" check).
3. **Unaccounted sanity:** under a deliberately CPU-oversubscribed
   window (more hogs than vCPUs), Off-CPU* > 0 and
   CPU* + Off-CPU* ≈ gap total. (Nightly/loaded-box friendly: assert
   the identity, not absolute values.)
4. Format: v3 writer/reader round-trip unit test; rev3 golden fixture;
   rev2 fixture still green; v2-file fallback (UNKNOWN sentinel →
   legacy inference) unit-tested.
5. `test_cpu_time.py` should pass UNMODIFIED once this lands (its DO
   workload becomes visible in-window via the live path) — treat it as
   an independent check, not a test to edit.
6. All 8 CI jobs green (the smoke matrix now includes the straddle
   phases on 4 PG versions × PIE), nightly 3-cell green (`--core` mode:
   schedstat reads work in containers; watchpoint-dependent asserts
   stay loudly skipped as established).
7. Metrics: `cpu_ns_total`, `offcpu_ns_total`, `cpu_clamped_total`,
   `wait_gap_cpu_ns_total`, capability field in `status` — mirrored in
   mock_server.py and asserted in test_control.sh.

### 5.7 Also in this phase (small, related)

- **test_multi_window %DB summation fix** (confirmed TEST bug from the
  EL9 runs): it sums parent classes AND their children (Timeout +
  Timeout:PgSleep, …) → 120-130%. Sum top-level rows only (rows without
  ':' in the name, plus CPU* and, after this phase, Off-CPU*). The
  tracer-side invariant `DB Time = CPU + Σclass parents` already passes
  in the same test — keep both assertions.
- Do NOT touch the cross-validate Hz-matrix flakiness in code — it is
  box-load noise on the 2-vCPU validation box (failures shift between
  runs). The close-out runs use a resized box (§6).

## 6. Workflow & close-out

- Branch: `t8-measured-cpu` **from `t2-fix-el9-cpu-gate`** (PR #46).
  First commits: the §4 PIE fix (turns #46 green); then S1 in the §5
  order. Either grow #46 into the full phase PR or stack a second PR on
  it — implementer's choice, but #46 must not merge red.
- Standing rules: no AI attribution in commits/PRs; root cause only;
  every fix ships with a fails-before/passes-after test; verify claims
  live before asserting them.
- Local machine cannot build the full BPF tracer (no bpftool for its
  kernel); `make pgwt-server` + `make -C tests` run locally. Full-build
  iteration: CI matrix (push + `gh run watch`) and/or the EL9
  validation box (see the local memory note reference_test_server; repo
  clone at /root/pgwt).
- After green: resume the milestone close-out — EL9
  `run_all.sh --require-live` twice (on a temporarily resized box,
  cx33+, to keep scheduler noise out of the gate), then rebuild the box
  to rocky-8 and repeat for EL8 (non-PIE + static-libbpf path), then
  v0.13 via docs/RELEASING.md. Known pre-existing EL9 leftovers that
  should now pass: test_cpu_time (via this phase), test_multi_window
  (via §5.7); anything else that fails is NEW information — root-cause
  it, do not rationalize it.

## 7. Effort estimate

2–3 focused implementation days: BPF+state (½d), writer/reader v3 +
fixtures (½d), compute/views + mock (½–1d), flush/live paths (½d),
tests + CI/EL9 iteration (½–1d). The §4 PIE fix is hours, do it first
for an early green signal on #46.
