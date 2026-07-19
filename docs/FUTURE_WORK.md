# Future work (parked, post-v0.13)

Ideas deferred with rationale, so they aren't lost. Add to this list;
don't delete an entry until it ships (then move it to a CHANGELOG).

## On-CPU-spin vs off-CPU-blocked, *within* each wait class

**What.** Sub-split every wait class into the time the backend spent
ON-CPU (spinning) vs OFF-CPU (genuinely blocked), while keeping the wait
label. E.g.:

```
LWLock:LockManager   500 ms   (420 ms on-CPU spin / 80 ms blocked)
IO:DataFileRead      300 ms   (0 ms spin / 300 ms blocked)
```

**Why it matters (the user's diagnostic point, 2026-07-19).** The wait
label is the actionable signal — "LWLock:LockManager" points straight at
lock-manager contention in a way "high CPU" never could. So spin CPU
must STAY under its wait label, never be relabeled as anonymous CPU\*
(that was the rejected Option B). But *within* the label, the spin vs
blocked split tells you HOW it's contending:
- mostly **on-CPU spin** → a hot lightweight lock burning cores
  (LWLock/latch/spin-delay under contention) — fix the hot path / reduce
  acquisition rate.
- mostly **off-CPU blocked** → a genuinely serialized wait (heavyweight
  lock, IO) — fix the blocking dependency.
Same label, opposite remediation. Today (v0.13) the wait shows total
time under its label and CPU\* is query-CPU only; this feature adds the
sub-split without changing the label.

**Why it's feasible now.** S3 (docs/S3_SCHED_SWITCH_CPU.md) already
measures on/off-CPU per context switch exactly, and the trace v3 records
per-interval `cpu_ns`. A wait interval's `cpu_ns` IS its exact on-CPU
spin (already summed into the `wait_gap_cpu` observability metric). The
work is: carry per-wait-class spin `cpu_ns` through compute (a second
column per wait row), and surface it in time_model / the AAS tooltip /
the views. No new capture mechanism — it's a compute + presentation
feature on data already captured.

**Scope notes.** Keep DB-Time conservation (spin is already inside the
wait's wall time; the sub-split only *labels* it, doesn't add to the
total). Sampled tier can't produce it (no per-instant on/off-CPU) — show
it only where measured cpu_ns exists (exact tier), like Off-CPU\*.

## Off-CPU analysis (runqueue latency, voluntary vs involuntary)

S3's `sched_switch` program sees `prev_state` at every switch, so it can
distinguish voluntary (blocked) from involuntary (preempted) switches
and measure runqueue-wait latency per backend. That's a richer
"scheduler pressure" view than the single Off-CPU\* residual v0.13 ships
(runqueue + unaccounted lumped together). Deferred; the data is one
field away in the existing sched_switch handler.

## Sampler feed from the exact CPU accumulator

The sampled (default) tier still estimates CPU from sample counts. S3's
`state_map.cpu_ns_total` is exact and always maintained (the sched_switch
program runs whenever the full-tier BPF is loaded). A later phase could
have the sampler read `cpu_ns_total` deltas per tick to tighten sampled
CPU\* toward exact, without changing the sampled tier's ~0-overhead
character (the read is one map lookup per backend per tick, already done
for query_id).

## Investigate: pure-compute straddling CLIENT backend intermittently untracked

Found in EL9 validation (2026-07-19) via test_cpu_time. A client backend
running a pure-compute DO block (no waits) that STRADDLES attach was
sometimes NOT in the tracer's initial-scan set (only aux processes
attached), so its CPU showed as DB≈0 in the live `--view`. The recorded
TRACE path and the fork-after-attach path capture it correctly, and
phase_pure_cpu_straddle (CI, all PG versions) validates the capability —
so this is an intermittent initial-scan/connect race for a specific
setup (kill+restart the client backend, then attach quickly), not a
systematic gap. Root-cause the scan timing (does the scan run before the
client backend's PGPROC is resolvable / does address-validation skip it
silently?). Related: the live `--view time_model` of a genuinely on-CPU
backend must never render it as idle — verify the map_reader open-
interval classification once the scan issue is understood.

## Parallel-worker CPU roll-up to the leader's query

T2 counts parallel workers' CPU under their own query_id in
foreground/execution; rolling it up to the leader's query needs
`leader_pid` in `backends.jsonl` (documented gap in the T2 PR). Small,
additive.
