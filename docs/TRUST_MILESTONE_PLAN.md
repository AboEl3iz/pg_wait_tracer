# Trust Milestone (Track T) — Correctness & Honesty Hardening

Status: 🔄 IN PROGRESS — T0–T7 ✅ merged; EL9 close-out validation found one more gap → **T8 (docs/T8_MEASURED_CPU_PLAN.md) now gates v0.13**, then EL8/EL9 live runs
Date: 2026-07-06
Origin: five-perspective adversarial code review of master @ fd21630
(capture core, tiered orchestration, data path/format, client/UI,
tests/CI), run after the v0.12 rework completed.

## Why this milestone exists

The rework (REWORK_PLAN.md, A0–A6/B1–B5/E) delivered the right
architecture — per-task watchpoints, tiered capture with read-time
exact-wins merge, pure-builder UI. The adversarial review confirmed the
design but found that **every one of the system's honesty guarantees is
violated by at least one code path**:

- *"exact-wins merge prevents double counting"* — long waits inside
  escalation windows double-count (FID-1); the live view double-counts
  during every escalation (ESC-3).
- *"never a silent empty result"* — the summary fast-path silently
  drops all sampled data for windows ≥ 120 s and labels the result
  `"fidelity":"exact"` (FID-2), in the **default tiered mode**, for the
  most common query ("last 15 minutes").
- *"worst-case overhead is bounded by configuration, not by hope"* —
  the escalation budget can be overspent ~2× via extensions (ESC-1),
  ledger overflow gives unbilled windows (ESC-5), and `--escalation-
  budget 0` silently means unlimited (ESC-6).
- *"fail loudly, never trace garbage"* — a wrong offset can be blessed
  by a validator that accepts `0` as proof (CAP-2); >512 backends
  silently record nothing (CAP-1); a total sampler read failure renders
  as an idle database forever (SMP-1).
- *"the tool is for incidents"* — a pure CPU storm produces AAS ≈ 0 and
  never triggers escalation (AAS-1); when the SSH transport dies the UI
  shows "No data" under a green connected pill (UI-1).

And the meta-finding: **the test net is stretched over the wrong half
of the cliff.** All four field escapes (#8, #24, #30, #31) lived in the
capture/discovery slice that CI never executes; the claimed support
matrix is ~24 cells and automated coverage is 1; `run_all.sh` passes
when everything skips; the last three field fixes shipped with zero
regression tests (TST-1..9).

**Rule for this milestone: no feature work (B6, Track C, Track D) until
Track T is done.** Every new feature built on the current merge/budget/
anomaly semantics inherits their bugs.

## Principles

1. **Verify before fixing.** Findings come from code review, not all
   from live reproduction. Each phase starts by reproducing its
   findings with a failing test (unit where possible, live otherwise).
   If a finding doesn't reproduce as described, document why and close
   it — no speculative fixes.
2. **Root cause, never workaround** (standing project rule).
3. **A fix without a regression test is not done.** This is the lesson
   of #24/#30/#31 — all three shipped fixes with no test that would
   catch recurrence.
4. **T0 lands first** (mirrors the B1 lesson: safety net before
   surgery). Every later phase must keep the new real-PG CI job green.
5. Branch + PR per phase, reviewed and merged before the next pair
   starts; parallel phases must not share a working tree.

---

# Findings register

Severity: 🔴 critical (wrong/lost data or broken core guarantee),
🟠 major, 🟡 minor, 🔵 design question/decision.
Each finding notes the phase that owns it.

## CAP — Capture core (watchpoints, discovery, BPF)

- **CAP-1 🔴 (T4)** `src/bpf/pg_wait_tracer.bpf.c:57` — `state_map`
  max_entries=512 < MAX_BACKENDS(1024) < common `max_connections`.
  Above 512 live backends, inserts fail with an unchecked return
  (bpf.c:256, backend.c preseed too); those backends **never record a
  single event, no error**. Silent under-count exactly under
  high-concurrency incidents. Fix: size to registry capacity, check
  inserts, surface a `state_map_full` counter.
- **CAP-2 🟠 (T4)** `src/discovery.c:315-339` + `src/backend.c:221` —
  `pgwt_validate_wait_addr` accepts `wei==0` as valid (the most likely
  read from a *wrong* offset), checks one backend once, then sets
  `wait_offset_validated` forever. A wrong PG13 offset on a custom
  build traces garbage labeled as real. Fix: require a non-zero,
  class-valid reading before trusting; validate across several
  backends; re-validate periodically.
- **CAP-3 🟠 (T4)** `src/discovery.c:229-280` — PG13 PGPROC/QueryDesc
  offsets hardcoded from one build (13.23), gated only by arch. PGPROC
  layout is not ABI-stable across configure flags/forks. Backstop is
  only CAP-2's weak validator. Fix rides on CAP-2 (strict runtime
  validation = refuse to attach on mismatch, loud error).
- **CAP-4 🟠 (T4)** `src/discovery.c:171-191` — load-base resolution
  does `strstr(line, "postgres")` over the whole maps line and takes
  the lowest match; extension paths like
  `/usr/lib/postgresql/17/lib/pg_stat_statements.so` match too. Under
  PIE, a lower-mapped .so → wrong load base → zero events, no error
  (the #24 class, one directory layout away from recurring). Fix:
  exact pathname-field comparison against the resolved binary path.
- **CAP-5 🟠 (T4)** `src/discovery.c:817-834` — PG17+ path has zero
  runtime sanity check on the resolved `my_wait_event_info` address
  (validation only runs for `use_myproc`). Extend the CAP-2 class-byte
  check to PG17+.
- **CAP-7 🟡 (T4)** `src/wait_event.c:773-776`, `src/discovery.c:385,463`
  — `popen()` command lines with unquoted/weakly-quoted `pg_bindir` /
  `pg_user` in a root daemon. Fix: fork/execve with argv (no shell).
- **CAP-8 🟡 (T4)** `src/backend.c:143-152` — watchpoint ENABLE happens
  before state_map preseed; a transition in the window mis-times the
  opening interval (re-exercised on every escalation). Fix: open
  disabled → preseed → enable.
- **CAP-6/9/10/11/12 🟡/🔵** — `seen_query_ids` (4096) never ages;
  PID-reuse window on the raw first-event path; PID-namespace
  assumption unchecked; postmaster restart exits the daemon (ops
  contract — document + supervisor guidance); no RLIMIT_NOFILE bump
  and EMFILE attach failures are quiet. Fold into T4 where cheap,
  otherwise document.

## AAS / SMP — Sampled tier semantics & robustness

- **AAS-1 🔴 (T2)** `src/sampler.c:103-121` + `src/anomaly.c:38-49` —
  on-CPU sessions (`wait_event_info==0`) are skipped with a comment
  "A3 derives CPU from coverage", but **no coverage-derived CPU exists
  anywhere** in server.c/compute.c. Found independently by two
  reviewers. Consequences: sampled AAS = "waiting sessions" (not ASH
  semantics); DB Time/AAS steps discontinuously at every tier switch
  (FID class); **a pure CPU storm produces AAS≈0 and never escalates**
  — the anomaly engine is blind to CPU incidents. This is the single
  most important semantic gap. Fix requires the T2 decision (below).
- **SMP-1 🟠 (T4)** `src/sampler.c:28-96` — total read failure (e.g.
  EPERM on `process_vm_readv` + fallback) is completely silent and
  indistinguishable from an idle DB; only a counter nobody watches
  moves; the anomaly baseline learns AAS=0. Fix: loud log on
  first/persistent failure + health flag in control-socket `status`.
- **SMP-2 🟠 (T4)** `src/sampler.c:53-91` — the batched sweep reads all
  targets through one pid; the safety argument ("foreign local address
  faults") fails for `.data`/`.bss` addresses mapped in every forked
  child — the read *succeeds* and returns the reader-pid's value,
  misattributed. Fix: only batch targets known to be in shared memory;
  per-pid reads otherwise.
- **SMP-3 🟡 (T4)** `src/daemon.c:167-174` — missed timer ticks
  (`expirations` ignored) drop sample weight → AAS/DB-Time deflate
  under load, exactly when it matters. Fix: weight samples by actual
  inter-tick elapsed time.
- **SMP-4/5 🟡/🔵 (T4/T2)** — per-backend BPF map lookup syscall per
  tick (batch it); dead preallocated iovec fields; fixed-phase
  sampling with no jitter (aliasing with periodic workloads — add
  ±10-20% jitter, decide in T2).

## ESC — Escalation, budget, anomaly triggers

- **ESC-1 🟠 (T3)** `src/escalation.c:253-267` — extension check
  charges `new_deadline − old_deadline` but never the already-committed
  remainder `(old_deadline − now)`; repeated extensions run a window to
  ~2× the hourly budget. Also no mid-window clamp once consumption
  crosses budget. Fix: charge `consumed_up_to_now + (new_deadline −
  now)`; enforce mid-window.
- **ESC-2 🟠 (T3)** `src/escalation.c:141-170` — de-escalation
  `detach_all()` resets state_map, discarding every open wait interval
  (no final TRANSITIONS record), while the END marker extends the
  block's exact-wins range so the covering samples are dropped too — a
  systematic undercount hole at the end of every window, hitting the
  long waits the window exists to capture. Fix: flush open intervals
  as final transitions at de-escalation.
- **ESC-3 🟠 (T3)** `src/sampler.c:239-300` + `src/event_stream.c:19-98`
  — in tiered mode both the sampler and the exact ringbuf feed
  `d->event_accum` during escalation: the **live `--view` inflates up
  to ~2×** exactly while an operator watches an incident. Fix: gate
  sampler accumulation for pids/windows covered by active exact
  capture (live-path analogue of the FID-1 merge fix).
- **ESC-4 🟠 (T3)** `src/anomaly.c:109-114` — lock-fraction rule fires
  on `active>=1`: a single backend's 300 ms row-lock wait triggers a
  60 s full-fidelity window; on routine OLTP the engine duty-cycles the
  whole budget away on noise, leaving nothing for real incidents. Fix:
  minimum-activity guard (like the AAS rule's `aas>=2.0`), consider
  min lock-AAS not fraction.
- **ESC-5 🟡 (T3)** `src/escalation.c:289-293` — ledger overflow (256
  segments/hr) opens windows that are never billed.
- **ESC-6 🟡 (T3)** `src/escalation.c:253` — `--escalation-budget 0`
  disables budgeting entirely while metrics report 0 remaining. Decide:
  0 = deny-all (recommended) or document 0 = unlimited and report it.
- **ESC-7 🟡 (T3)** `src/anomaly.c:122-129` — frozen baseline means a
  legitimate load regime change re-fires every cooldown forever;
  daemon started mid-incident bakes the incident into the baseline.
  Fix: slow learn-through after N sustained-over minutes.
- **ESC-8/9/10 🟡 (T3)** — near-trigger log up to 10 lines/s
  (rate-limit); `metrics` reports `"tier":"sampled"` in `--mode full`
  (`src/control.c:157`); control-socket `unlink()` without a liveness
  probe lets a second daemon steal a live daemon's socket
  (`src/control.c:416`).
- **ESC-11/12 🔵 (T3)** — synchronous `attach_all` stalls the sampler
  at high backend counts (ties into SMP-3: those ticks are *lost*, a
  sampling gap at the most interesting moment); cooldown(120 s) vs
  window(60 s) = 50% duty cycle on sustained incidents. Document the
  chosen semantics; fix SMP-3 makes the stall at least weighted.

### ESC resolution (T3, branch t3-escalation-trust)

All ESC-1..12 fixed / documented. Pure cores unit-tested
(`tests/test_escalation_budget.c` = budget/ledger, extended
`tests/test_anomaly.c` = lock floor + slow-release), integration in
`test_escalation.sh` / `test_control.sh`, live in the capture-smoke
escalation phase.

- **ESC-1** — budget math moved to a pure, unit-tested core
  (`src/escalation_budget.c`). An over-ask is **clamped** to the
  remaining budget and the window's deadline is armed at `now + grant`,
  so extend-every-second caps total full-fidelity time at *exactly* the
  budget; `pgwt_escalation_check_budget()` (daemon timer) is the
  mid-window backstop.
- **ESC-2** — `flush_open_intervals()` writes each open wait as a final
  transition (ts = de-escalation instant) *before* detach + END marker.
- **ESC-3** — `pgwt_sampler_accumulate()` skips pids with a live
  watchpoint while escalated; smoke-test live-view tolerance tightened
  from 7 s back to 6 s.
- **ESC-4** — lock rule needs BOTH `lock_fraction > F` AND lock-class
  `AAS ≥ --anomaly-lock-min-aas` (default **2.0**).
- **ESC-5** — ledger overflow coalesces the two oldest segments
  (bills the gap, never under-bills); a window is never opened unbilled.
- **ESC-6** — `--escalation-budget 0` = **deny-all** (honest `0`
  remaining); `unlimited` (or `-1`) = no cap (`escalation_budget_
  unlimited: true`, remaining = `-1` sentinel = ∞ in the UI).
- **ESC-7** constants — baseline stays frozen while over threshold;
  **slow learn-through** engages only after **15 min**
  (`PGWT_ANOMALY_DEF_LEARN_THROUGH_MIN`) continuously-over, then learns
  at **1/10** the normal EWMA rate
  (`PGWT_ANOMALY_DEF_SLOW_RELEASE_DIV`). Short incidents never move the
  bar; a real regime change is eventually adopted. Both pinned in tests.
- **ESC-8** — near-trigger log rate-limited to reason-mask changes + a
  60 s summary (with suppressed count); `anomaly_near_total` still
  counts every one.
- **ESC-9** — `metrics.tier` and `status.tier` share `daemon_tier_str()`
  (a fixed EXACT provider reports `escalated`, never a bogus `sampled`).
- **ESC-10** — control-socket liveness probe: a live daemon on the trace
  dir makes a second daemon **refuse startup** (fatal, rc −2) instead of
  stealing the socket.
- **ESC-11/12** — documented in README: the ~50% duty cycle (cooldown
  from fire start) and the weight-compensated `attach_all` stall bound
  (SMP-3 keeps the accounting correct; only resolution dips).

## FID — Fidelity merge, summaries, views

- **FID-1 🔴 (T1)** `src/server.c:554-567,713-722` — exact-wins
  coverage is derived from TRANSITIONS block `[first_ts,last_ts]`, but
  transition timestamps are wait-**end** times: a 60 s lock wait emits
  nothing until it ends, so its interior samples survive the merge
  *and* the exact 60 s interval lands → up to 2× inflation of long
  waits inside escalation windows. The authoritative
  `PGWT_MARKER_ESCALATE_START/END` markers are already in the trace
  (`pg_wait_tracer.h:98-99`); the merge ignores them. **Fix: coverage
  = escalation-marker windows (+ whole-file for `--mode full`), not
  per-block timestamp ranges.** Also fixes FID-6 boundary slop.
- **FID-2 🔴 (T1)** `src/server.c:928-937,957-962` —
  `should_use_summaries()` selects the summary path on range length
  alone (≥120 s) and stamps `"fidelity":"exact"`, but summaries are fed
  only by the exact ringbuf path — the sampler never writes them. In
  default tiered mode, "last 15 min" time_model/AAS returns only
  escalation slivers (or nothing), labeled exact. Violates "last 15
  min means NOW" and the A3 contract. Fix: summary path must check
  coverage against the window and fall back to / blend with raw
  sampled data; never hardcode the fidelity label.
- **FID-3 🟠 (T1)** `src/server.c:581-584` + `src/compute.c:558-641` —
  samples normalized to `duration_ns = sample_period` feed the
  count/avg/p50/p95/p99 pipeline: over sampled windows the events view
  reports fabricated percentiles (p95 ≈ sample period) in the same
  columns as real data. Fix: suppress/gate latency columns under
  sampled fidelity (the heatmap already does this correctly).
- **FID-4 🟠 (T1)** `src/event_stream.c:30-44` +
  `src/summary_writer.c:155-224` — markers are pushed to the summary
  writer before the `PGWT_IS_MARKER` check: exec/plan markers inflate
  per-query counts (skewing avg_us low) and escalation markers insert
  **bogus query_ids** (`PGWT_ESC_PACK` values) into summaries; in raw
  compute only transitions/variants filter markers — top_events,
  top_queries, heatmap do not. Fix: filter markers at the accumulation
  chokepoints; add a regression test with a marker-bearing trace.
- **FID-5 🟠 (T1)** `src/replay.c` — `--replay` consumes
  `old_event/duration_ns` directly: decoded samples contribute nothing
  and markers create junk entries; replaying a tiered trace shows
  near-zero activity with no warning. Fix: fidelity-aware replay (or
  explicit "sampled data present, use --view" notice).
- **FID-7 🟡 (T1)** `src/server.c:534-539` — per-file mono→wall offset
  fixed at file open; NTP steps skew cross-file comparisons. Fix:
  single clock domain for the merge; re-anchor wall mapping.

## DUR — Durability, retention, format

- **DUR-1 🔴 (T5)** `src/event_writer.c:155`,
  `src/summary_writer.c:412` — `fopen("wb")` on fixed-name
  `current.trace`/`current.summary`: daemon crash + restart at :59
  **erases up to an hour** of otherwise-recoverable data. Fix: recover
  and rename-aside an existing current file (collision-safe name),
  never truncate.
- **DUR-2 🟠 (T5)** `src/event_writer.c:439-448` — rotated filename
  derived from start-hour; restart mid-hour (or DST fold via
  `localtime_r`) renames onto the same archive name, clobbering it.
  Fix: collision-safe rename (suffix on existing target).
- **DUR-3 🟠 (T5)** — retention is hours-only; no size cap. Full-tier
  on a busy server can fill the disk inside the retention window.
  Orphaned `current.trace`, `query_texts.jsonl`, `backends.jsonl` are
  never cleaned. Fix: `--retention-gb` cap + include orphans.
- **DUR-4 🟠 (T5)** `src/query_text.c:142` — `query_texts.jsonl`
  truncated on every restart, orphaning text for all retained
  query_ids (server assumes append). Fix: append-open + dedup-on-load.
- **DUR-9 🟠 (T5)** `src/server.c:753-815` — pid-filtered long-window
  queries force the raw path and load every event in range into one
  unbounded doubling array (the immutable-file cache has a budget;
  this working array doesn't). Fix: filter during load; add a bound.
- **DUR-5/6/7/8 🟡 (T5)** — no fsync policy (document chosen
  durability level); no written format spec (little-endian/packed is
  implicit — write `docs/TRACE_FORMAT.md`); meta-path block scan lacks
  the sanity caps the fallback scan has (`event_reader.c:85-99` vs
  `:117`); one SAMPLES block per tick → ~36k tiny blocks/hour at 10 Hz
  and footer's 100k-block cap breaks at high sample rates (batch ~1 s
  of ticks per block).
- **DUR-10 🟡 (T5)** `src/query_text.c` — raw query text stored with
  no redaction option, silent 4096-id cap, first-seen TOCTOU can
  capture the wrong statement, and looser file perms than traces.
  Minimum: log the cap, match trace-file perms, document PII.

## UI — Client, bridge, web UI

- **UI-1 🔴 (T6)** `web/static/lib/transport.js:57` +
  `web/static/app.js:135-185` — bridge error envelopes
  (`{"id":N,"error":...}`) are **resolved as data**; nothing outside
  control.js reads `.error`. When SSH dies: tables show "No data",
  the AAS chart keeps stale paint, Live keeps ticking on a frozen
  window — under a green "connected" pill. Worst possible failure mode
  for an incident tool. Fix: reject on `.error` (~5 lines) + visible
  degraded-transport state.
- **UI-2 🟠 (T6)** `web/bridge.go:137-143` — request-ids are
  client-generated per tab but `pending` is shared per-bridge: two
  tabs collide and one tab receives the *other tab's data* as a valid
  reply. Fix: namespace ids per WS connection in the bridge.
- **UI-3 🟠 (T6)** `web/main.go:43-47` + `web/bridge.go:91-117` — SSH
  session death is terminal (no respawn; browser WS happily reconnects
  to the dead-bridged local server → permanent UI-1 state). Fix:
  bridge respawns SSH with backoff; add
  `ServerAliveInterval`/`BatchMode` options.
- **UI-4 🟠 (T6)** `web/static/app.js:109-113` — `init()` runs on every
  WS reconnect and is not idempotent: duplicate `echarts.init` without
  dispose, a new ViewManager, duplicate tab/resize listeners (N+1
  handlers after N reconnects). Fix: idempotent init / init-once +
  explicit resync path.
- **UI-5 🟠 (T6)** `web/static/views/active.js:61` — empty AAS response
  early-returns, leaving the previous window's paint on screen while
  the tables say "No data" — chart and tables silently disagree, in
  the one view outside the view-manager. Fix: clear/empty-state the
  chart on `!hasData`.
- **UI-6 🟠 (T6)** `web/static/lib/builders/timeline.js:41` — query
  text concatenated unescaped into tooltip HTML: any DB user's SQL can
  inject markup into the DBA's browser. Fix: `esc()` it (table path
  already does); Node test for the formatter.
- **UI-7 🟠 (T6)** — nothing tests `web/bridge.go`/`web/main.go` (the
  mock replaces both bridge and C server), and `transport.js` /
  `view-manager.js` — the centerpiece of the rework — have no Node
  tests despite being dependency-free. Fix: bridge integration test
  (spawn real `pgwt-server` locally over a pipe, no SSH needed) +
  transport/view-manager Node tests (error envelopes, superseding,
  epoch cancellation).
- **UI-8..13 🟡 (T6)** — `extractID` is a substring scan (unstated
  "id-first" protocol invariant — parse JSON or document+test it);
  daemon control plane never re-probed after one failure
  (`app.js:530-541`); escalation annotation shades the entire window
  and contradicts its own comment (`fidelity.js:135-204` — the Node
  test pins the misleading render); **all times are UTC with no label
  and the `datetime-local` inputs read as local time** (a UTC+3 user
  gets a 3 h-off window); localhost-origin is the only auth for a
  socket that can escalate against production (add a session token);
  vendored ECharts has no provenance/update note.

## TST — Tests, CI, release

- **TST-1 🔴 (T0)** — CI never executes the capture path; 4/4 field
  escapes (#8/#24/#30/#31) were in the CI-unexecuted slice (~27% of
  LOC, 100% of the escape record).
- **TST-2 🟠 (T0)** — GitHub runners can run BPF + real PostgreSQL
  today (the build job already asserts `/sys/kernel/btf/vmlinux`); no
  smoke job exists. Highest-ROI gap in the project.
- **TST-3 🟠 (T0)** — CI and `run_all.sh` maintain two drifted unit
  lists (event_writer/reader missing from run_all; `test_concurrent_rw`
  built but run nowhere). One shared list.
- **TST-4 🟠 (T0)** `tests/run_all.sh:105-185,224-231` — all-skip is a
  green run; no `--require-live`, no minimum-executed floor.
- **TST-5 🟠 (T0/T7)** — overhead gate: only fails >50%, takes ~70 min
  inside run_all (guaranteeing it's skipped), measures only
  `--mode full` — the shipped default's overhead is never gated, and
  no trend is stored. Use `--quick` in run_all + CSV trend file;
  gate tiered/sampled too.
- **TST-6/13 🟠 (T0)** — cross-validation accepts any catalog-valid
  name (the PR#8 bug class passes); pg_wait_sampling tolerance is
  0.1×–10× (vacuous). Expand the deterministic-workload
  exact-event-assertion pattern; tighten tolerances.
- **TST-7 🟠 (T0)** — `test_cross_validate_tiered.sh` (the test that
  justified flipping the default to tiered) and `test_anomaly_live.sh`
  are wired into nothing. Wire into run_all.
- **TST-8 🔴 (T0)** — `pgwt_find_load_base`/`pgwt_find_symbol_offset`:
  pure, twice-broken (#24 class), zero tests. Commit maps-file fixtures
  (EL8 non-PIE, EL9 PIE, Ubuntu) + tiny -pie/-no-pie ELF fixtures and
  unit-test resolution. Fixes recur otherwise.
- **TST-9 🔴 (T7)** — claimed matrix ≈24 cells (PG13/17/18 × EL8/EL9/
  Ubuntu × PIE/non-PIE), automated coverage = 1 cell (highest-version
  PG on one box). EL8/non-PIE validated exactly once ever. Nightly
  containerized matrix (rockylinux:8/9, ubuntu — host kernel shared,
  so capture works) + PGDG PG version matrix in the CI smoke job.
- **TST-10 🟠 (T7)** — format tests are same-code round-trips; commit
  a golden trace fixture per released format revision, decode+checksum
  in CI.
- **TST-11 🟠 (T7)** — no release process behind v0.1–v0.12 (no
  CHANGELOG/RELEASING.md/tag workflow); stale prebuilt binaries at the
  repo root months older than the source; **no client/server version
  handshake** although a skewed Mac-client/Linux-server pair is the
  normal deployment state.
- **TST-12 🟡 (T7)** — the EL8 static-libbpf build path is never built
  in CI (a `rockylinux:8` container build job covers it).

---

# Phases

Each phase: branch + PR, reproduce-first, fix + regression test,
green CI (including the new T0 job once it lands) + green live run
where capture behavior changed.

### Phase T0 ✅ — Safety net: real-PG CI + gate hardening
Owns: TST-1..8 (TST-5 partially), groundwork for everything else.
Scope:
- **CI capture-smoke job**: install PGDG PostgreSQL on the runner,
  start it, run `pg_wait_tracer` in `--mode full` AND `--mode tiered`;
  deterministic workload (`pg_sleep`, blocked `LOCK TABLE`, short
  pgbench); assert *specific* events with bounded durations in live
  view and trace file; assert query_id attribution non-empty. Matrix
  `pg-version: [13, 16, 17, 18]` via PGDG apt.
- **discovery.c unit tests**: committed `/proc/maps` fixtures (EL8
  non-PIE PGDG, EL9 PIE, Ubuntu) + `-pie`/`-no-pie` ELF fixtures
  compiled in CI; assert `pgwt_find_load_base` /
  `pgwt_find_symbol_offset` results.
- **run_all.sh**: `--require-live` mode (root+PG-section skips become
  failures); single shared unit-test list consumed by both CI and
  run_all (add missing `test_event_writer/reader`,
  `test_concurrent_rw`); wire `test_cross_validate_tiered.sh`,
  `test_anomaly_live.sh`, `test_control.sh`; overhead via `--quick`
  with CSV trend appended to a tracked results file.
- Regression tests for the three shipped-untested fixes: sampled
  `--view` shows data (#30 symptom — covered by the smoke job),
  query-attr uprobe fires (#31 — smoke job assertion), load-base
  (#24 — the fixtures above).
Acceptance: CI red if capture silently records nothing on any matrix
cell; a deliberate load-base regression fails a unit test in
milliseconds; `run_all.sh --require-live` cannot pass vacuously.

### Phase T1 ✅ — Fidelity merge & summary honesty
Owns: FID-1..5, FID-7.
Scope: escalation markers become the exact-wins coverage authority
(FID-1/6); `should_use_summaries` becomes coverage-aware and never
hardcodes the fidelity label, falling back to/blending raw sampled
data (FID-2); markers filtered from summaries and all raw compute
paths (FID-4); latency columns gated under sampled fidelity (FID-3);
`--replay` fidelity-aware (FID-5); merge in one clock domain (FID-7).
Files: `src/server.c`, `src/compute.c`, `src/summary_writer.c`,
`src/event_stream.c`, `src/replay.c`.
Tests: synthetic mixed-window traces with a long wait spanning an
escalation window (the FID-1 repro) → totals must equal ground truth;
15-min tiered-default query over sampled-only data → correct data with
`"fidelity":"sampled"`; marker-bearing trace → no phantom query_ids.
Acceptance: the two headline claims of the rework ("no double count",
"never a silent empty result") hold under adversarial synthetic
traces, enforced by CI.

### Phase T2 ✅ — AAS semantics: one definition, all paths (DECISION GATE)
Owns: AAS-1, SMP-5; folds in the io_worker open question
(REWORK_PLAN risk #6).
**GATE PASSED (2026-07-12).** Empirical study: docs/T2_IOWORKER_STUDY.md;
decision: docs/AAS_SEMANTICS_DECISION.md — the full decomposed model
(active = non-idle wait + on-CPU; command-open gate for client
backends, ungated for backgrounds whose idle is instrumented;
categories foreground plan/exec/command-overhead + maintenance +
background; io_workers excluded from AAS, surfaced as a busy%
utilization metric). The study also registered three tracer defects
for the implementation to absorb: (1) PIE uprobe attach offset
(`va − 0x400000` heuristic → silently dead uprobes on PIE builds;
prerequisite fix for the gate probe), (2) exit-record phantom CPU in
traces, (3) in-trace samples/exact overlap observed on a pre-T1 build
— re-verify on current master, likely already fixed.

Original decision framing (superseded by the above):
- Recommended: ASH standard — active = on-CPU + non-idle wait. Sampler
  records on-CPU samples (`we==0` for a backend in an active
  transaction/query) as first-class `CPU` samples; anomaly engine sees
  them; exact windows already count CPU implicitly.
- io_workers (PG18 `io_method=worker`, the default): decide count-as-
  today / exclude-from-AAS / distinct "I/O on behalf of sessions"
  class. Needs one empirical run on a PG18 box (backend
  `AioIoCompletion` + io_worker `DataFileRead` reconciled against
  `pg_stat_io`) before choosing — provision a test box for this.
Scope after decision: sampler batch includes on-CPU actives; anomaly
metrics include CPU (a CPU storm must trip the AAS rule); server-side
sampled CPU estimator so DB Time/AAS are continuous across tier
switches; optional sampling-phase jitter (SMP-5). Write the decision
into `docs/` (like the ClientRead decision).
Files: `src/sampler.c`, `src/anomaly.c`, `src/server.c`,
`src/compute.c`, decision doc.
Tests: cross-validation extended to CPU-heavy workload (sampled AAS vs
exact AAS under pgbench `-S` CPU-bound run within tolerance); anomaly
unit tests for CPU-storm trigger.
Acceptance: a `SELECT`-storm CPU incident raises sampled AAS and can
trigger escalation; AAS shows no step artifact at tier switches.

### Phase T3 ✅ — Escalation budget & trigger quality
Owns: ESC-1..12. Depends on T2 (anomaly metrics shape settles there).
Scope: extension charge = committed-remainder-aware + mid-window
budget clamp (ESC-1); de-escalation flushes open intervals as final
transitions (ESC-2); live accumulator dedup during escalation (ESC-3);
lock-rule minimum-activity guard (ESC-4); ledger-overflow billing
(ESC-5); `budget 0` semantics decided + honest metrics (ESC-6);
baseline slow-release (ESC-7); near-trigger log rate-limit (ESC-8);
`metrics` tier correctness (ESC-9); socket liveness-probe before
unlink (ESC-10); document duty-cycle semantics (ESC-12).
Files: `src/escalation.c`, `src/anomaly.c`, `src/control.c`,
`src/sampler.c` (accumulate gate), `src/event_stream.c`.
Tests: budget adversarial unit tests (extend-every-second must cap at
budget; ledger overflow billed; zero-budget behavior); de-escalation
repro (long wait open at window end → appears exactly once in trace
and live view); anomaly guard tests (single-backend lock wait must NOT
fire); live: escalate/extend/expire under pgbench, verify billed time
== window time.
Acceptance: worst-case full-fidelity seconds per hour ≤ configured
budget under adversarial requests — provable by test; live view during
escalation matches post-hoc trace view within tolerance.

### Phase T4 ✅ — Capture & sampler hardening (silent-wrong-data class)
Owns: CAP-1..12, SMP-1..4.
Scope: state_map sized to registry capacity + checked inserts +
`state_map_full` counter surfaced in metrics (CAP-1); strict
validation — non-zero class-valid reading required, multi-backend,
periodic revalidation, PG17+ included, refuse-to-attach loudly on
mismatch (CAP-2/3/5); load-base exact-path match (CAP-4); execve
instead of popen (CAP-7); preseed-before-enable (CAP-8); loud sampler
failure + health in `status` (SMP-1); shared-memory-only batching
(SMP-2); tick-weight compensation (SMP-3); batched query-id lookups +
remove dead iovec fields (SMP-4); RLIMIT_NOFILE bump + EMFILE-specific
warning (CAP-12); document PID-ns and postmaster-restart contracts
(CAP-10/11).
Files: `src/bpf/pg_wait_tracer.bpf.c`, `src/discovery.c`,
`src/backend.c`, `src/sampler.c`, `src/daemon.c`, `src/perf_event.c`,
`src/wait_event.c`.
Tests: >512-backend live test (state_map full must be visible in
metrics and impossible to hit silently at default sizing);
validation-rejection tests (wrong offset → refuse + clear error — the
existing test_pg13_resolve pattern, extended); maps fixtures from T0
extended with the extension-.so-below-binary case; sampler EPERM
simulation → loud.
Acceptance: no known path where the daemon records wrong-or-nothing
without a loud signal (log + control-socket health + metric).

### Phase T5 ✅ — Durability & retention
Owns: DUR-1..10.
Scope: never truncate — recover/rename-aside `current.trace`/
`current.summary` on startup (DUR-1); collision-safe archive renames
(DUR-2); size-based retention cap + orphan cleanup (DUR-3);
`query_texts.jsonl` append + dedup-on-load + perms parity + cap
logging (DUR-4/10); batch SAMPLES blocks ~1 s (DUR-8); pid-filter
memory bound (DUR-9); `docs/TRACE_FORMAT.md` (endianness, packing,
fsync/durability policy — DUR-5/6); meta-path sanity caps (DUR-7).
Files: `src/event_writer.c`, `src/summary_writer.c`,
`src/query_text.c`, `src/event_reader.c`, `src/server.c`, docs.
Tests: kill -9 daemon mid-hour → restart → previous hour's data
readable and re-archived (no loss beyond the unflushed tail); restart
twice in one hour → both archives present; disk-cap test with tiny
cap; golden-fixture decode test (with T7's fixture).
Acceptance: a daemon crash loses at most the unflushed tail, never a
committed hour; disk usage provably bounded.

### Phase T6 ✅ — Client transport trust
Owns: UI-1..13. Independent of T1–T5 (client-only) — pairs with T0.
Scope: transport rejects `.error` envelopes + visible degraded state
distinct from "no data" (UI-1); per-connection id namespacing in
bridge (UI-2); SSH respawn with backoff + keepalive options (UI-3);
idempotent init/reconnect resync (UI-4); AAS empty-state clears chart
(UI-5); tooltip escaping + formatter test (UI-6); JSON-parse or
document+test the id-first invariant (UI-8); daemon re-probe (UI-9);
escalation annotation shades the actual window only (UI-10); UTC
labels + local-time-correct custom-range inputs (UI-11); WS session
token (UI-12); `vendor/README` provenance (UI-13).
Tests (UI-7): `transport.test.mjs` + `view-manager.test.mjs` (error
envelopes, single-flight superseding, epoch cancellation); **bridge
integration test** — spawn real `pgwt-server` over a local pipe (no
SSH) and drive the Go bridge end-to-end, including error-envelope and
two-connection id-collision cases; Playwright: kill the mock
mid-session → UI must show degraded transport, not "No data".
Acceptance: SSH/server death is visibly distinguishable from an idle
database within one refresh interval, and recovers without a page
reload; two tabs never exchange data.

### Phase T7 ✅ — Release engineering & matrix
Owns: TST-9..12, repo hygiene.
Scope: nightly containerized matrix workflow (rockylinux:8 — also
covers the static-libbpf build path, rockylinux:9, ubuntu:24.04; build
+ unit + capture smoke; host kernel is shared so BPF works from the
privileged container); golden trace fixture per released format
revision + decode/checksum CI test; `RELEASING.md` (run matrix, check
skip count, tag, record versions) + `CHANGELOG.md`; **client/server
protocol version handshake** (server reports version; client warns on
mismatch — the Mac-client/Linux-server split makes skew the normal
state); remove stale prebuilt binaries from the repo root (build
artifacts don't belong in git); `.gitignore` for local build products.
Acceptance: a release is a checklist, not a memory; EL8 build path
exercised nightly; version skew warns instead of misbehaving.

---

# Sequencing

Pairs (one PR per phase; the two phases of a pair are file-disjoint by
construction and can be developed in parallel):

```
Pair 1:  T0 ✅ (CI/tests)        ∥  T6 ✅ (client/bridge)
Pair 2:  T1 ✅ (merge/summaries) ∥  T4 ✅ (capture/sampler hardening)
Gate  :  T2 ✅ decision (AAS semantics + io_worker — signed off,
         PG18 io_worker run done: docs/T2_IOWORKER_STUDY.md)
Pair 3:  T2 ✅ (AAS impl)        ∥  T5 ✅ (durability)
Pair 4:  T3 ✅ (escalation)      ∥  T7 ✅ (release eng)
```

- T0 first, non-negotiable — it is the net every other phase is
  verified against (B1 lesson).
- T3 after T2 because the anomaly metric shape changes in T2.
- Every capture-behavior phase (T1–T5) requires a green live run on a
  real box in addition to CI (standing rule), now with
  `--require-live` so the gate cannot pass vacuously.

# Definition of done (milestone)

1. ✅ CI includes a real-PG capture smoke job, matrix PG 13/16/17/18,
   green (T0). The nightly containerized matrix (T7) additionally
   exercises EL8/EL9/Ubuntu cells.
2. 🔄 Adversarial synthetic-trace tests prove: no double counting across
   escalation boundaries (trace AND live view), correct fidelity
   labels on every response path (T1 ✅), budget mathematically bounded
   (T3 ✅).
3. ✅ A CPU-storm incident is detectable (sampled AAS includes on-CPU;
   anomaly engine fires on it) (T2).
4. 🔄 No known silent-wrong-data path: every degraded state is visible in
   logs + control-socket status + UI. Capture/sampler (T4 ✅), transport
   (T6 ✅) done; escalation-billing honesty done (T3 ✅).
5. ✅ Daemon crash/restart loses at most the unflushed tail (T5).
6. ⬜ PENDING (milestone close-out): `run_all.sh --require-live` green
   twice consecutively on EL9 and EL8 boxes with zero live-section skips
   (the EL8 cell re-validated for the first time since #24). Procedure in
   `RELEASING.md` step 4.
7. ⬜ PENDING (milestone close-out): `RELEASING.md` exists (T7 ✅) and
   v0.13 ships through it (executes after T3 merges).

# Explicitly parked / resumed after Track T

- **B6 new analysis views** — resume after T1/T3: all three views are
  EXACT-required, so their value depends on escalation windows
  containing *correct* exact data.
- **Track D (PG14–16)** — resume after T4: D1's version-adapter
  design ("data tables + runtime validation, fail loudly") is the same
  machinery T4 hardens; doing D1 right afterwards is nearly free.
- **Track C2/C3 (pre-8.2 RHEL perfbuf/BTF fallbacks)** — parked
  indefinitely: shrinking population, not needed for current targets.
- **Prometheus exporter** — unchanged (future consumer; metric names
  already stable).
