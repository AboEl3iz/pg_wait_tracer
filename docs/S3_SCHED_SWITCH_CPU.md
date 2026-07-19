# S3 — Exact CPU accounting via sched_switch

Status: ⬜ PLANNED — the correct measured-CPU foundation for v0.13.
Date: 2026-07-19
Base: branch `t8-cpu-revision` (the residual-Off-CPU* compute, which is
correct GIVEN an exact per-interval cpu_ns). S3 replaces the *source* of
cpu_ns — not the compute — so it makes the residual model exact and
closes the EL9 findings that the `sum_exec_runtime` source could not.
Overhead measured before committing (see §7): within noise.

---

## 1. Why (the sum_exec_runtime dead end)

T8 read `task->se.sum_exec_runtime` at each watchpoint wait-boundary.
That accumulator is only current at a scheduler tick (1 ms) or a context
switch, so a sub-millisecond on-CPU burst before a wait is read STALE and
leaks into the following interval. Consequences proven on EL9:
- CPU\* under-reports on wait-heavy/fragmented workloads (pgbench:
  `wait_gap_cpu ≈ CPU*` — ~half the CPU mis-timestamped).
- Neither correction works universally: T8's stale-0→full-gap fallback
  is right for CPU-bound but over-states preempted pure-CPU (replay >
  physical, Finding #1); the residual is right for oversubscription but
  dumps a fragmented backend's real CPU into Off-CPU\* (windowed straddle
  → CPU\* 24%, Off-CPU\* 56%). The stale read is the root; no compute
  choice fixes it.

`sched_switch` eliminates the staleness: the kernel switches a task
on/off a CPU at exact instants, and a BPF program on that tracepoint
sees them. Given the *exact* time a backend came on-CPU, the watchpoint
handler can compute exact on-CPU ns with **zero** tick quantization.

## 2. The one insight that makes this small

S3 is NOT a new accounting model. It is a drop-in replacement for
`read_task_cpu_ns()` — the function that today reads `sum_exec_runtime`.
Everything downstream (trace format v3 `cpu_ns` column, the residual
CPU\*/Off-CPU\* compute, views, tests) stays **byte-for-byte the same**;
it simply receives exact cpu_ns instead of leaky cpu_ns.

Exactness comes from tracking, per tracked backend, the instant it came
on-CPU:

```
exact_cpu_now(pid) = cpu_ns_total[pid] + (on_cpu_ts[pid] ? now - on_cpu_ts[pid] : 0)
```

`cpu_ns_total` is the sum of completed on-CPU stretches (updated by the
sched_switch handler when the task goes OFF-CPU); `on_cpu_ts` is the
start of the current stretch (set when it comes ON-CPU). At a
wait-boundary the backend is still ON-CPU, so `now - on_cpu_ts` is the
current stretch — no stale accumulator, no tick floor. The per-interval
cpu_ns delta between two boundaries is therefore exact.

This also fixes the pure-CPU DO-loop (23k wait-event flips/s): each
sub-tick we==0 interval now reads its exact on-CPU ns (not a stale 0),
so the residual splits CPU\*/Off-CPU\* correctly.

## 3. BPF design (src/bpf/pg_wait_tracer.bpf.c)

### 3.1 State
Extend `struct pgwt_pid_state` (pg_wait_tracer.h) with:
```c
__u64 cpu_ns_total;   /* Σ completed on-CPU stretches (exact) */
__u64 on_cpu_ts;      /* start of the current on-CPU stretch, 0 if off-CPU */
```
(Keep the T8 `last_cpu_ns` field name/usage repurposed to `cpu_ns_total`
if cleaner; the watchpoint handler already tracks a per-interval base.)

### 3.2 The sched_switch program
```c
SEC("tp_btf/sched_switch")            /* raw_tp/sched_switch fallback for no-BTF */
int BPF_PROG(on_sched_switch, bool preempt,
             struct task_struct *prev, struct task_struct *next)
{
    u64 now = bpf_ktime_get_ns();
    u32 ppid = BPF_CORE_READ(prev, pid);
    u32 npid = BPF_CORE_READ(next, pid);

    struct pgwt_pid_state *ps = bpf_map_lookup_elem(&state_map, &ppid);
    if (ps && ps->on_cpu_ts) {            /* prev tracked & was on-CPU */
        ps->cpu_ns_total += now - ps->on_cpu_ts;
        ps->on_cpu_ts = 0;
    }
    struct pgwt_pid_state *ns = bpf_map_lookup_elem(&state_map, &npid);
    if (ns)
        ns->on_cpu_ts = now;              /* next tracked → on-CPU now */
    return 0;
}
```
- **Cost is a state_map lookup per switch; both misses (untracked pids)
  return in ~tens of ns** — the measured-negligible path (§7). Keyed off
  the SAME state_map the tool already maintains, so no new pid registry.
- `tp_btf` needs BTF (present on every full-tier platform incl. EL8.2+);
  a `raw_tracepoint/sched_switch` variant (read the ctx tuple by offset)
  is the no-BTF fallback, selected by the capability probe.
- Concurrency: state is per-pid; two CPUs never switch the SAME pid
  simultaneously (a task is on ≤1 CPU), so no cross-CPU race on a given
  entry. `cpu_ns_total`/`on_cpu_ts` are plain u64 (no atomics needed —
  each pid's entry is touched by at most one CPU at the switch instant).

### 3.3 read_task_cpu_ns() becomes exact
Replace the `sum_exec_runtime` body with:
```c
static __always_inline u64 read_task_cpu_ns_for(struct pgwt_pid_state *st, u64 now)
{
    if (!cpu_accounting || !st) return 0;
    u64 t = st->cpu_ns_total;
    if (st->on_cpu_ts && now >= st->on_cpu_ts) t += now - st->on_cpu_ts;
    return t;
}
```
The watchpoint handler and the exit handler call this with the state
they already hold and `bpf_ktime_get_ns()`. The `read_task_cpu_ns()`
free-function (current-task variant) is dropped — S3 always has the
state entry.

### 3.4 Delete the sum_exec_runtime path
Remove the `se.sum_exec_runtime` CO-RE read entirely. It is strictly
worse than S3 (leaky) and than gap-inference (its stale-0 landmine), so
it is not kept as a fallback. Fallback chain becomes: **S3 → gap-
inference** (see §5).

## 4. Userspace (src/backend.c, src/sampler.c, src/daemon.c)

- **Seed**: `preseed_state_map()` sets `on_cpu_ts = now` if the backend
  is currently on-CPU (read `/proc/<pid>/stat` field 3 == 'R'), else 0;
  `cpu_ns_total = 0`. (The exact base is 0 at attach — we only need
  deltas from attach onward.) Drop the schedstat seed read.
- **Terminal flush** (escalation.c): compute the open interval's cpu_ns
  from `read_task_cpu_ns_for(st, now)` − the interval's base, same as
  today; the value is now exact. Drop the `pgwt_read_sched_cpu_ns`
  `/proc/schedstat` read.
- **Sampler**: UNCHANGED this phase (sampled tier keeps its estimate;
  S3 targets the exact tier where the findings live). A later phase may
  feed the sampler from the state_map's exact `cpu_ns_total`.
- **daemon**: attach `on_sched_switch` in the load path alongside the
  other programs (it is always attached when the full tier is armed;
  detached with the tier). It must be attached BEFORE the first
  backend runs so on_cpu_ts is tracked from the start of tracing.

## 5. Capability probe + fallback (src/daemon.c, Track-C style)

At startup, when the full tier is selected:
1. **S3**: try to load+attach `on_sched_switch`. tp_btf if BTF present,
   else raw_tracepoint. Success → `cpu_accounting = MEASURED (sched_switch)`.
2. **Fallback**: attach fails (locked-down kernel, no tracepoint BPF) →
   `cpu_accounting = legacy`; cpu_ns stamped UNKNOWN → compute uses
   gap-inference (CPU\* = full gap, **no Off-CPU\***). No sum_exec_runtime.
Report the mode in the startup log and the control-socket
`status`/`metrics` `cpu_accounting` field ("sched_switch" | "legacy").

## 6. Compute / format / views — NO CHANGE

The residual model already on `t8-cpu-revision` is correct given exact
cpu_ns: CPU\* = Σ all cpu_ns (conserved), Off-CPU\* = max(0, DB − CPU\* −
Σwaits). Trace format v3 (`cpu_ns` column), golden fixtures, mock,
`test_data_offcpu_identity.py` all stay. S3 only changes where cpu_ns
comes from, so PR #49's red windowed-straddle cell turns green (exact
cpu_ns → no stale-0 → no misattribution).

## 7. Overhead (measured on the EL9 box, 2026-07-19)

Pessimistic proxy: a `sched_switch` bpftrace probe accumulating on-CPU
time for **all** pids (real S3 filters to tracked backends via the
state_map lookup, cheaper). pgbench on 4 vCPU:
- ctx-switch rate: 110k–126k/s.
- TPS: baseline 5520–5619 vs probe 5469–5706 (−c8); indistinguishable.
- Latency: avg 1.40→1.4x ms, p99 within baseline spread, p99.9 dominated
  by unrelated run-to-run variance (baseline itself 5.3→10.6 ms).
Net: **≤ low-single-digit %, at the noise floor.** Cheap enough to run
always-on with the full tier; no escalation-window gating needed.

## 8. Tests / acceptance

1. `test_data_offcpu_identity.py` (already committed): the identity
   holds — unchanged; S3 doesn't alter compute.
2. **Live EL9 — the cases the sum_exec_runtime source failed:**
   - **Windowed straddle** (the PR #49 red cell): `--mode full`
     generate_series straddle, aas over a SUB-window → cpu AAS ≈ 1
     (backend fully on CPU), not 0.04. This is the headline S3 win.
   - **Conservation**: pgbench full-mode → tracer CPU\* within ±5% of Σ
     `/proc` utime+stime of the traced backends (the sum_exec_runtime
     source missed by ~4×; S3 must conserve).
   - **Oversubscription**: N_hogs > vCPU pure-CPU → Off-CPU\* ≈ the
     runqueue fraction, `CPU* + Off-CPU* ≈ DB`, replay CPU\* ≤ physical.
   - **Free core**: single compute backend → Off-CPU\* ≈ 0 (no false
     runqueue).
3. Capability probe reported MEASURED(sched_switch) on EL9 + CI runners;
   the raw_tracepoint fallback exercised where BTF tp_btf is refused.
4. All 8 CI jobs green; the capture-smoke straddle + pure-CPU phases
   green on all 4 PG cells (they now pass because cpu_ns is exact).
5. Metrics: keep `cpu_ns_total`/`offcpu_ns_total`/`cpu_clamped_total`;
   drop `wait_gap_cpu_ns_total` (a sum_exec_runtime artifact — with S3,
   an on-CPU spin during a wait_event is real and correctly in CPU\*).

## 9. Deferred (future, not v0.13)
- **Spin/blocked wait decomposition**: sched_switch can also attribute a
  wait's on-CPU (spin) vs off-CPU (blocked) split by reading the current
  wait_event in the handler — a real feature the exact source enables.
  Out of scope here; noted as the natural follow-on.
- **Off-CPU analysis**: runqueue-latency and voluntary/involuntary
  switch stats fall out of the same program.
- **Sampler feed** from `cpu_ns_total` to tighten the sampled tier.

## 10. Workflow
Branch `s3-sched-switch-cpu` off `t8-cpu-revision`. BPF changes need the
box (no local bpftool) + the CI matrix. No AI attribution; root-cause
only; verify live before asserting. When green, this supersedes the T8
sum_exec_runtime source; fold T8 + revision + S3 into one measured-CPU
PR to master, then resume the EL9→EL8→Ubuntu validation matrix and
v0.13.
