# Rework Plan: Tiered-Fidelity Capture + UI Restructure

Status: ✅ COMPLETE (all phases merged — master c027d3c, 2026-06-18)
Date: 2026-06-11 (completed 2026-06-18)

Two tracks. Track A makes the daemon an always-on monitor with on-demand
full-fidelity escalation. Track B makes the web UI testable, extensible,
and supportable. The tracks are independent until Phase A5/B4, where the
UI grows fidelity-aware features on top of the restructured codebase.

---

## Completion summary (2026-06-18)

The **core rework is complete**: tiered-fidelity capture (A0–A6), the UI
restructure + testability + fidelity UI (B1–B5), and the Track E
live-suite cleanup are all merged to master and verified end-to-end —
**CI green** (build-and-unit, web-ui incl. chaos+soak, snapshots,
protocol-drift) **and a full live `run_all.sh`** on the test box =
**46 passed / 0 failed / 1 skipped** (the skip is the web-UI snapshot
job, which the CI snapshots job covers authoritatively).

**Still open (in the plan, NOT done in this rework):** B6 (new analysis
views — per-execution waterfall, latency scatter, transition matrix),
Track C (Rocky 8 / RHEL 8 support), Track D (older PostgreSQL 14–16).
These are future work; the sections below remain the design for them.

**Default capture mode is now `tiered`** — always-on sampled tier with
bounded/budgeted on-demand + anomaly-triggered escalation to full
watchpoints. Proven results:
- Sampled tier ≈ **0% impact on PostgreSQL** (daemon CPU 0.6% @10Hz;
  pgbench TPS within noise) vs the full watchpoint tier's **29–30%**
  (the documented range).
- **Cross-validation: 0.9pp** worst-case event-share disagreement
  between sampled and exact over the same window @10Hz, 5/5 top-5
  overlap — so the cheap always-on default is genuinely accurate.

**Merged PRs:** #6 B1 · #7 A0 · #8 Lock-naming fix · #9 Track E ·
#10 ClientRead idle-but-visible · #11 B2 · #12 A1 · #13 A2 · #14 B3p1 ·
#15 A3 · #16 B3p2 · #17 A4 · #18 B3p3 · #19 A5 · #20 A6 · #21 B5 ·
#22 B4 · #23 default→tiered.

**Bugs found & fixed along the way:** Lock-class subtype mislabel on
PG17+ (relation→advisory, real product bug — #8); the ClientRead
idle/visible semantics decision (#10); pre-existing flaky live-test
thresholds (test_overhead 15%-gate vs documented 6–30%; test_client_wait
load-dependent DB-Time threshold) corrected during #23 validation.

**Deferred (not rework-blocking):** cooperative provider IMPLEMENTATION
(extension track — A6 froze the interface); historical-escalation
annotations + mixed-fidelity sub-range shading in the UI (small server
additions); idle-ClientRead %DB cosmetic.

---

## Progress log

Workflow: branch + PR per phase, user merges. Two phases land at a time
(one per track, in parallel). Subagents do the work in worktrees (each
isolated — a parallel pair without worktree isolation collided once,
recovered with no lost work; isolation is mandatory thereafter).

- **2026-06-13 — B1 (CI safety net) MERGED** (PR #6 → master 861c313).
  Delivered the console-error/pageerror guard, `.github/workflows/ci.yml`
  (build-and-unit, web-ui vs mock, protocol-drift jobs), and the
  CI-fails-on-skip behavior. Acceptance proven (canary JS exception
  failed the web-ui job). Bonus: the guard exposed and fixed **two real
  app.js bugs** — the Live button (ReferenceError) and the Concurrency
  tab never loading. Note: master history carries the canary+revert
  pair (05628b1/91815c7, net-neutral).
- **2026-06-13 — A0 (control socket + self-metrics) MERGED** (PR #7 →
  master b1ef3c4, rebased onto B1). Live-validated on the test box:
  `test_control.sh` 16/16 (socket, status/metrics JSON, counters rising
  under load, error handling, concurrent clients + disconnect survival,
  pgwt-server proxy, --dump block, SIGTERM clean unlink). Ringbuf drop
  counter deliberately deferred to A2 (no BPF-side drop map yet).

**Decisions locked in during execution:**
- Trace format is **v2-only** — no deployed installs exist, so A1 drops
  v1 reader compatibility (version field stays for future v3).
- **Prometheus exporter** for AAS/sampled metrics is a planned future
  consumer; A0 metric names are already snake_case + unit-suffixed for
  it. Out of scope for these tracks.

**Testing reality (important for every phase):**
- CI (GitHub Actions) runs unit + synthetic + web-UI-vs-mock +
  protocol-drift. It **cannot** run the live capture tests (no PG, no
  hardware watchpoints on runners).
- Live tests (`test_accuracy`, `test_control`, lwlock/cpu/lock/query
  capture, overhead benchmark) run on **dmitry-micro-test** (Hetzner
  cx23, Rocky 9.7, kernel 5.14, PG 18.4) — see [[test_server_ssh]].
  On 2026-06-13 the watchpoint path verified healthy there:
  test_accuracy 11/11, DB-Time consistency 0.0% error, pg_stat_io
  cross-check ratio 0.80.
- **Definition of done for any capture phase now includes a green live
  run on the box, not just green CI.**

**Pre-existing live-suite debt found on 2026-06-13 (baseline on master,
NOT introduced by B1/A0)** — tracked as Track E below.

---

## Goals

- **A.** Run 24/7 with <0.5% overhead (sampled tier), escalate to full
  watchpoint tracing for bounded windows — manually or on anomaly — so
  "the data from 3am is already on disk."
- **B.** No more manual UI testing after every change: regressions caught
  by CI; most UI logic testable without a browser; new views additive.

## Non-goals

- No userspace rewrite (C daemon/server, Go client stay).
- No new charting framework. ECharts becomes the **only** chart
  library: ApexCharts (currently rendering the main AAS chart) is
  dropped in B3, and its drag-select-window UX is replaced by a custom
  selection overlay (full visual control, ~100 lines, testable).
- No client-side compute relocation (server keeps computing views).
- Cooperative tier (PG patch/extension hooks) is interface-only here;
  implementation belongs to the extension track.
- Prometheus exporter for AAS/sampled metrics: planned for later, out
  of scope here — but A3's view semantics and the A0 control socket
  must not preclude it (stable metric names, queryable aggregates).

---

# Track A — Tiered-Fidelity Capture

## A.0 Target architecture

```
                 ┌─────────────────────────────────────────────┐
                 │ daemon                                      │
                 │  ┌───────────────┐   ┌───────────────────┐  │
 PG backends ───▶│  │ backend       │──▶│ capture providers │  │
 (fork/exit      │  │ registry      │   │  - sampled (24/7) │  │
  lifecycle)     │  │ (pid→PGPROC   │   │  - full (windows) │  │
                 │  │  addr, query) │   │  - cooperative*   │  │
                 │  └───────────────┘   └─────────┬─────────┘  │
                 │  ┌───────────────┐             │            │
   pgwt-server ◀─┼──│ control socket│   ┌─────────▼─────────┐  │
   (escalate,    │  │ + self-metrics│   │ event_writer v2   │  │
    status)      │  └───────┬───────┘   │ (typed blocks)    │  │
                 │  ┌───────▼───────┐   └───────────────────┘  │
                 │  │ escalation    │                          │
                 │  │ engine        │                          │
                 │  └───────────────┘                          │
                 └─────────────────────────────────────────────┘
```

**Provider contract.** One interface, three implementations, one schema:

```c
struct pgwt_capture_provider {
    const char *name;                  /* "sampled" | "full" | "coop" */
    enum pgwt_fidelity fidelity;       /* SAMPLED | EXACT */
    int  (*start)(struct pgwt_daemon *);
    int  (*stop)(struct pgwt_daemon *);
    int  (*poll)(struct pgwt_daemon *);   /* drain into writer */
    void (*self_metrics)(struct pgwt_metrics *);
};
```

The existing watchpoint path becomes the `full` provider with no
behavioral change. `sampled` is new. `coop` is a stub whose contract is
fixed now so the extension track can fill it in later.

## A.1 Key design decisions (with rationale)

### D1. Sampled tier is pure userspace — no BPF in the hot path

The daemon already resolves and tracks each backend's
`PGPROC->wait_event_info` address (`src/backend.c:74-189`, bootstrap →
init flow at `src/backend.c:191-265`). The sampler reuses that registry:

- One `process_vm_readv()` call per tick with up to 1024 remote iovecs
  (4 bytes each, one per backend) reads every backend's
  `wait_event_info` in a **single syscall**. PG's main shm segment is
  inherited anonymous mmap, so addresses are identical across PG
  processes — read via any live backend pid (fall back to per-pid pread
  on the rare EFAULT from a concurrently-exiting backend).
- Zero cost on PG itself: no traps, no signals, no instructions executed
  by backends. Daemon-side cost at 1000 backends × 100 Hz is one syscall
  + ~4 KB copy per tick — well under 0.5% of one core.
- `query_id` per sample: joined from the registry. In daemon mode the
  existing `on_report_query_id` uprobe (`src/bpf/pg_wait_tracer.bpf.c:387`)
  already maintains pid→query_id; the sampler reads the registry, not PG
  memory. (A later no-BPF fallback can read `st_query_id` from
  BackendStatusArray; not in scope for the first cut.)
- Backend lifecycle: daemon mode keeps using the fork/exit BPF
  tracepoints. A degraded no-BPF mode (periodic /proc rescan) is a
  follow-up, not part of this rework.

Default rate: **10 Hz**, configurable 1–1000 Hz (`--sample-rate`).
The A4 cross-validation test (sampled vs. exact on the same workload)
decides whether the default moves.

### D2. Trace format v2 — typed blocks, not sentinel hacks

The format already has a version field (`PGWT_TRACE_VERSION`,
`src/event_writer.h:15-16`). Bump to **v2**:

- Block header gains `block_type` (`TRANSITIONS` | `SAMPLES`) and
  `sample_period_ns` (0 for transition blocks).
- Sample blocks use the columnar layout minus the columns that don't
  apply (no `old_event`, no `duration_ns`): timestamp (delta varint),
  pid, event, query_id.
- **v2-only** (decision 2026-06-15: no deployed installations exist
  yet, so no v1 compatibility burden). Reader and writer speak v2
  exclusively; the version field stays so v3 can be graceful. Old local
  traces are regenerated, not migrated.

Explicitly rejected: encoding samples via `PGWT_MARKER_*`-style sentinel
values inside v1 — it would avoid a version bump but builds a lie into
the schema that every future reader must know about.

### D3. Compute layer: fidelity-aware views, exact-wins merge

`compute.c` assumes exact intervals (`duration_ns` consumed directly,
AAS bucketing at `src/compute.c:221-243`). Changes:

- Each view declares `required_fidelity`:
  - **SAMPLED is enough**: time_model, system_event, session_event,
    query_event, AAS, active. Estimator: each sample of a non-idle
    session contributes `sample_period_ns` to its (event, session,
    query) cells. Standard ASH math.
  - **EXACT required**: histogram, transitions, lock chains,
    interference. Over a window with no transition blocks these return
    `{"unavailable": "requires full-fidelity data"}` — never a silent
    empty result.
- **Merge rule for mixed windows**: time ranges covered by transition
  blocks are authoritative; sample records inside those ranges are
  dropped at read time (block time-range comparison — block headers
  already carry `first_ts`/`last_ts`). This prevents double counting
  while the sampler keeps running through escalation windows (it does —
  stopping it would create gaps if escalation crashes).
- Every view response carries a `fidelity` field per time range so the
  client can render the distinction (see B5).

### D4. Control socket — the daemon grows a control plane

Today the daemon's only runtime interface is SIGTERM
(`src/daemon.c:145-151,384-389`). Add a unix domain socket
(`{trace_dir}/pgwt.sock`, mode 0600), JSON-line protocol:

- `{"cmd":"status"}` → mode, uptime, backends tracked, current tier
- `{"cmd":"escalate","duration_s":60,"reason":"manual"}` → ack/deny
- `{"cmd":"deescalate"}`
- `{"cmd":"metrics"}` → events/s, samples/s, ringbuf drops,
  attach failures, estimated overhead, escalation budget remaining

pgwt-server proxies these to the client (new `control` command in its
JSON protocol), so the browser UI gets an "Escalate 60s" button without
any new network path — same SSH channel.

### D5. Escalation engine — bounded, budgeted, fast

- **Attach speed**: skeleton + ringbuf are loaded at daemon start
  (idle maps cost nothing); escalation only runs the watchpoint attach
  loop — ~3 syscalls/backend (`src/perf_event.c:19-58`), a few ms for
  1000 backends. PGPROC addresses come from the registry, already
  resolved. state_map is pre-seeded exactly as
  `pgwt_scan_existing_backends()` does (`src/backend.c:144-176`) so
  initial wait states aren't lost.
- **Bounded windows**: every escalation has a hard duration; expiry
  detaches watchpoints even if the requester vanished.
- **Budget**: `--escalation-budget` (default: 300s of full fidelity per
  hour). Manual requests beyond budget are denied with the reason;
  anomaly triggers respect it silently. This is the mechanism that makes
  "always-on in production" a defensible claim — worst-case overhead is
  bounded by configuration, not by hope.
- **Anomaly triggers** (phase A5): rules evaluated on the sampled
  stream — `AAS > k × rolling_baseline for N ticks`,
  `lock-class fraction > threshold`. Hysteresis + cooldown so a flapping
  metric can't burn the budget in bursts. Every trigger writes a
  structured "escalation event" into the trace so the UI can show *why*
  full data exists for a window.

### D6. Self-observability

`metrics` (D4) is not optional polish — once the daemon runs 24/7,
"is the monitor itself healthy/cheap" becomes a production question.
Estimated overhead = trap count × measured per-trap cost (calibrated at
startup with a synthetic write loop) + sampler CPU time from
`/proc/self`. Exposed via control socket and recorded into the trace as
periodic marker records.

## A.2 Phases

Each phase is independently shippable and keeps `make && tests/run_all.sh`
green. The current single-mode behavior must remain the default until A4
completes.

### Phase A0 — Control socket + self-metrics (groundwork) ✅ DONE (2026-06-13, merge b1ef3c4)
Scope: D4 socket with `status`/`metrics` only; daemon metrics counters;
pgwt-server `control` proxy command; `--dump` prints daemon status if
socket present.
Files: `src/daemon.c`, new `src/control.c/h`, `src/server.c`,
`tests/test_control.sh`.
Tests: `tests/test_control.sh` (standalone integration, 16/16 live on
the box). Wiring it into `run_all.sh` deferred at merge time — **still
TODO** (do it alongside the Track E cleanup, see E3).
Metrics shipped: `events_total`, `events_per_sec`,
`lifecycle_events_total`, `wp_attach_failures_total`,
`trace_events_written_total`, `trace_bytes_written_total`,
`backends_tracked`. Ringbuf drops intentionally NOT reported (no
BPF-side drop map until A2 — `control.h` documents this; A2 must add it
and surface it here).
Acceptance: met — status/metrics answered over the socket, zero capture
behavior change.

### Phase A1 — Trace format v2
Scope: D2. Typed blocks, v1-compat reader, writer emits v2,
`gen_test_traces` emits both, `pgwt-server` + replay read both.
Files: `src/event_writer.c/h`, `src/event_reader.c/h`,
`src/summary_*.c`, `tests/gen_test_traces`.
Tests: round-trip unit tests (v1 file → v2 reader; v2 sample blocks →
correct decode); existing synthetic data tests must pass unchanged.
Acceptance: old traces replay identically; new traces carry block types.

### Phase A2 — Sampled provider
Scope: D1 sampler behind the provider interface; extract the existing
watchpoint path into the `full` provider (mechanical refactor, no
behavior change); `--mode sampled|full|tiered` flag (default `full`,
i.e. today's behavior).
Files: new `src/sampler.c/h`, new `src/provider.h`, `src/daemon.c`,
`src/backend.c` (registry exposure).
Tests: sampler unit test against a fake registry; live test: run
`--mode sampled` against pgbench, verify sample blocks land with sane
rates; perf check: daemon CPU at 100 Hz × N backends below budget.
Acceptance: `--mode sampled` runs without watchpoints, writes sample
blocks; `--mode full` byte-identical behavior to today.

### Phase A3 — Fidelity-aware compute
Scope: D3. Sample estimators for the six sampled-capable views;
`required_fidelity` declarations; exact-wins merge; `fidelity` field in
every view response; `--dump` annotates fidelity.
Files: `src/compute.c`, `src/server.c`, `src/event_stream.c`.
Tests: synthetic tests with known sample streams → exact expected
estimates (extend `test_data_*.py`); mixed-window merge tests
(overlapping sample + transition blocks → no double counting);
"EXACT-required view over sampled-only window → explicit unavailable".
Acceptance: a sampled-only trace produces correct time_model /
system_event / AAS; histograms/transitions report unavailable, not
empty.

### Phase A4 — Tiered orchestration + cross-validation
Scope: D5 minus anomaly rules. `--mode tiered`: sampler always on,
escalation via control socket, bounded windows, budget enforcement,
state_map pre-seeding on escalate.
Files: new `src/escalation.c/h`, `src/daemon.c`, `src/control.c`.
Tests: escalate/expire/deny-over-budget integration tests; **the
cross-validation test** — run tiered mode under pgbench, escalate for
60s, compare sampled estimators vs. exact data over the same window
(time_model within agreed tolerance, e.g. top-5 events match, shares
within ±10% at 10 Hz). This test both validates the estimators and
empirically sets the default sample rate.
Acceptance: tiered mode survives PG restarts (existing re-attach logic),
escalation windows produce mixed traces that all views handle, default
mode can flip to `tiered`.

### Phase A5 — Anomaly-triggered escalation ✅ DONE
Delivered: AAS-vs-baseline + lock-class-fraction rules on the live sampled
stream (`src/anomaly.c/h`), evaluated each sampler tick. Rolling EWMA
baseline (not updated while AAS is anomalous, so a sustained incident can't
raise the bar and silence the rule). Hysteresis (sustained-N-ticks) +
cooldown so a flapping metric can't burst-burn the budget; an anomaly while
escalated EXTENDS the window via A4's `pgwt_escalate` extend path. Over-budget
anomaly triggers are dropped SILENTLY (log only) — unlike manual escalate
which denies. Every near-trigger is logged for data-driven tuning. Anomaly
windows carry `PGWT_ESC_REASON_ANOMALY` in the trace markers, distinct from
manual. Config flags (tiered-only, conservative defaults):
`--anomaly-aas-factor` (3.0), `--anomaly-aas-ticks` (3),
`--anomaly-lock-fraction` (0.30), `--anomaly-cooldown-s` (120),
`--anomaly-window-s` (60). Counters exposed via the control-socket metrics
(`anomaly_fires_total`, `anomaly_near_total`, `anomaly_dropped_budget_total`,
`anomaly_dropped_cooldown_total`, `anomaly_baseline_aas`).
Tests: `tests/test_anomaly.c` — 74/74 pure-rule checks (sustain, cooldown,
baseline-protection, budget boundary, metrics derivation, combined fire);
wired into run_all.sh + CI. Live on the box: `tests/test_anomaly_live.sh`
11/11 — a 12-backend `LOCK TABLE` storm AUTO-escalated (tier=escalated,
START reason=anomaly), the bounded window captured transitions and
auto-de-escalated (END reason=expired), and cooldown blocked an immediate
re-fire. `tests/dump_markers.c` decodes the reason-tagged markers (no view
surfaces them until B5).
Note for B5: anomaly-reason escalations are already in the trace (marker
reason byte = 1) and in the metrics; the UI just needs to render them.

Implementation note vs. the original plan: the rule state + rolling
baseline live in the new `src/anomaly.c/h` (a pure, unit-testable core)
rather than in `escalation.c`/`compute.c` — the baseline is derived from
the sampler's per-tick batch in `sampler.c`, so no `compute.c` change was
needed.

### Phase A6 — Cooperative provider stub (interface freeze only) ✅ DONE
Scope: `coop` provider compiled but returning "not available"; document
the contract the extension must satisfy (same event schema, EXACT
fidelity, its own start/stop). No further work in this track.

Delivered: `src/provider_coop.{c,h}` — a `coop` provider implementing the
`struct pgwt_capture_provider` vtable, advertising `PGWT_FIDELITY_EXACT`,
whose `start()` cleanly reports "cooperative provider not available in this
build" and returns -1 so the daemon aborts startup with a clear message (no
crash). `enum pgwt_mode` gains `PGWT_MODE_COOP`; `--mode coop` parses to it
(`src/pg_wait_tracer.c`) and `pgwt_daemon_init()` selects the stub
(`src/daemon.c`); coop arms neither watchpoints nor the sampler
(`pgwt_mode_uses_watchpoints`/`_uses_sampler` in `daemon.h`). Default
behavior and full/sampled/tiered selection are unchanged.

The FROZEN CONTRACT for the extension track is documented in detail in the
`provider_coop.h` header comment. In short, the cooperative provider must:
(1) emit the SAME v2 trace records as the other tiers — a wait transition is
a TRANSITIONS record byte-identical to the `full` tier's; (2) advertise
`PGWT_FIDELITY_EXACT` so it satisfies the EXACT-required views with no
view-side change; (3) hand every event to the trace writer through the SAME
path the full tier uses (poll() drains the extension's delivery into
`src/event_stream.c` `handle_event` → `src/event_writer.c`), taking backend
identity from the SHARED registry (`src/backend.c`); (4) implement all four
vtable entry points (start/stop/poll/self_metrics); (5) not own backend
discovery (mode-independent registry/lifecycle stays in the daemon). The
extension track replaces the stub bodies in `provider_coop.c`; nothing else
in the daemon changes.

Tests: `tests/test_coop.c` — BPF-free unit test (built `-DPGWT_SERVER`,
matching test_sampler/test_anomaly) asserting the coop provider registers,
advertises EXACT, exposes the full vtable, and that `start()` returns the
clean not-available status (with NULL-daemon tolerance and no-op stop/poll/
metrics while unarmed). Wired into `run_all.sh` C-unit + CI build-and-unit.

---

# Track B — UI Rework

## B.0 Target architecture

Native ES modules, no build step, still embedded via `go:embed`.
The rule that makes the UI testable: **every view is a pure function
from server JSON to a render model; only a thin mount layer touches the
DOM and ECharts.**

```
web/static/
  app.js                 → shrinks to bootstrap + router (~100 lines)
  vendor/
    echarts.min.js       → exact version, vendored (no CDN, no floating
                           tags — deterministic builds, airgapped-safe,
                           stable B4 snapshots)
  lib/
    transport.js         → WebSocket, request ids, single-flight, abort
    view-manager.js      → enter/leave lifecycle, response chokepoint
    state.js             → filters/breadcrumbs/time-range, explicit
    table.js             → the one shared table/drill-down component
    selection.js         → drag-select window overlay: pointer events +
                           styled band div + convertFromPixel → time
                           range (replaces ApexCharts' selection UX)
  views/
    overview.js  events.js  sessions.js  queries.js
    histogram.js transitions.js active.js
      each exports: { id, requests(state),            // what to fetch
                      build(data, state),              // PURE → model
                      mount(el, model, callbacks),     // thin DOM/ECharts
                      enter(), leave() }               // owns its chart
```

- **view-manager** is the single chokepoint: guarantees `leave()`
  completes before the next `enter()`; drops any response addressed to
  a view that is no longer active. This replaces the per-path generation
  counters (`app.js:176-186`) by construction.
- **transport** owns request ids and single-flight per view: a new
  refresh supersedes the pending one. No more stale-response-overwrites
  -fresh-data.
- **Chart lifecycle**: each view creates its ECharts instance in
  `enter()` and disposes it in `leave()` — nowhere else. The five
  module-level chart globals and ad-hoc dispose calls
  (`app.js:429-445,1443,1462`) disappear.
- **Pure builders** return ECharts option objects / table row models as
  plain data. They run — and are tested — in Node, no browser.

## B.1 Phases

### Phase B1 — Safety net (before touching anything) ✅ DONE (2026-06-13, merge 861c313)
Scope:
- Playwright fixture fails any test on console error / pageerror /
  unhandled rejection. (Allowlist only on the reconnection test, which
  must stay last.)
- GitHub Actions workflow (`.github/workflows/ci.yml`): build pgwt, run
  `test_web_ui.py` against `mock_server.py` on every PR/push. Missing
  Playwright is a **failure** in CI (env `CI` set), still a skip
  locally.
- Protocol-drift: `tests/test_protocol_drift.py` generates the fixture
  in CI (no binary committed — deviation from the original "commit a
  trace file" text) and diffs real `pgwt-server` vs mock response
  schemas.
Acceptance: met — canary JS exception failed the web-ui job, then
reverted.
Notes: the guard caught two real app.js bugs (Live button
ReferenceError; Concurrency tab not loading), both fixed. CI builds
bpftool from source (GH runners lack it). Several stale tests that were
silently `exit 0` now propagate failure.

### Phase B2 — Adversarial mock
Scope: chaos mode in `tests/mock_server.py` — configurable latency
jitter (50–300 ms), out-of-order delivery for concurrent requests,
late responses after navigation. New tests that replicate manual
testing: rapid tab switching, clicking before data loads, toggling live
mode mid-refresh, drag-zoom during refresh — all under chaos mode, all
asserting zero console errors and correct final state.
Acceptance: chaos suite fails against current `app.js` (it should —
these are the real bugs), and the failures form the acceptance list
for B3.

### Phase B3 — The restructure
Scope: B.0 architecture. Mechanical migration, one view at a time, in
this order (simplest → hardest): active → overview → events → sessions
→ queries → histogram → transitions. Live mode moves into
view-manager + transport (one refresh loop, superseding, honoring
"last 15 min means NOW"). Shared table component extracted once, used
by all table views.

Chart library consolidation (during the overview view's migration):
- **AAS chart moves from ApexCharts to ECharts**; ApexCharts is
  deleted entirely (today: AAS on ApexCharts at `app.js:586-761`,
  everything else on ECharts — two libraries to theme, learn, and
  debug, with the SVG-based one carrying the most data-intensive
  chart).
- **Custom selection overlay** (`lib/selection.js`) replaces the
  library's drag-select: pointer events on the chart container, a
  styled band element, `convertFromPixel` to map pixels → time range.
  Full visual control over the band (the reason ApexCharts was added),
  reusable on the concurrency chart, and testable: unit-test the
  pixel→time math in Node, drive the drag in Playwright, snapshot the
  band's look in B4.
- **Vendor exact-version ECharts** into `web/static/vendor/` and drop
  the CDN `<script>` tags (`index.html:7-8` currently float
  `echarts@5`): deterministic builds, works airgapped, stable visual
  snapshots, no supply-chain exposure.
- Builders stay library-agnostic: they emit view models; only the
  mount layer speaks ECharts. (Keeps a future per-chart library
  experiment — e.g. uPlot for AAS — a contained adapter swap, not a
  migration.)

Per-view definition of done: pure builder extracted with Node unit
tests; chart owned by enter/leave; existing Playwright tests pass;
chaos tests for that view pass.
Acceptance: full chaos suite green; `app.js` is bootstrap-only; zero
module-level chart variables; ApexCharts gone from `index.html`; no
CDN script tags; drag-select on the AAS chart works via the overlay
(Playwright-driven drag selects the expected time range); soak test
(50 random tab/filter transitions) shows no console errors and stable
listener counts.

### Phase B4 — Builder test suite + deepened assertions
Scope:
- Node test runner (`node --test`, no framework) for all pure builders:
  snapshot tests for option objects, filter-correctness tests
  (filtered input → filtered rows), edge cases (empty window, single
  session, self-loop transitions).
- Playwright assertions upgraded to read data via
  `page.evaluate(() => chart.getOption())` instead of canvas-exists;
  end-to-end filter test (set filter → table *rows* change).
- Visual regression snapshots (Playwright `toHaveScreenshot`): baseline
  screenshots of each view against the fixed mock dataset — AAS chart,
  histogram heatmap, Sankey transitions, drill-down tables — with a
  small pixel-diff tolerance to absorb antialiasing noise. Pin browser
  version + viewport + device scale in the Playwright config so
  baselines are deterministic in CI; animations disabled for snapshot
  runs. Baselines committed to the repo; intentional visual changes are
  reviewed as baseline diffs in the PR. This catches the
  "renders without errors but looks wrong" class (missing series, color
  regressions, layout breakage) that DOM and getOption() assertions
  cannot see.
Acceptance: a deliberate off-by-one in a builder is caught by a Node
test in milliseconds, without Playwright; removing a series color
mapping (no JS error, data still present) is caught by a snapshot diff
in CI.

### Phase B5 — Fidelity-aware UI (joins Track A) ✅ DONE
Depends on: A3 (fidelity field in responses), A4 (escalation via
control proxy), B3 (view registry).
Delivered (pure-builder + thin-mount + view-manager pattern, no B3 regression):
- **Sampled shading**: `lib/builders/fidelity.js` turns a view response's
  top-level `fidelity` ("exact"|"sampled"|"mixed") + optional
  `fidelity_ranges` into an ECharts markArea band model; the AAS builder
  attaches it behind the stacked areas. Sampled → amber band over the whole
  window; mixed → bands only over sampled sub-ranges if present, else the
  window marked mixed. A legend chip (`#aas-fidelity-chip`) explains it.
- **Unavailable panels**: the EXACT-required views (histogram, transitions,
  concurrency) detect the `{"unavailable":"requires full-fidelity data"}`
  shape and render an explicit "No full-fidelity data in this window —
  escalate to capture" panel (with the escalate affordance) instead of an
  empty chart — `buildUnavailablePanel` + `lib/panels.js`.
- **Escalate control**: header "Escalate 60s" button + budget readout + a
  Stop affordance while active, wired through `lib/control.js` →
  pgwt-server's `control` proxy (escalate/deescalate). Shows granted window /
  budget remaining / denial reason. Hidden when no daemon (static replay).
- **Escalation annotations**: the active escalation window is rendered on the
  AAS timeline (markArea + labeled markLine), visually distinguishing
  reason=manual (cyan) vs reason=anomaly (red). Drove a small server addition:
  `build_status` now emits `escalation_reason` for the open window.
- **Daemon self-metrics panel** (`#metrics-btn`): events/s, samples/s,
  ringbuf drops, estimated overhead, anomaly counters, budget remaining —
  from the control-socket `metrics`.
Mock: `tests/mock_server.py` gained `PGWT_MOCK_FIDELITY` (exact|sampled|mixed
+ `sample_period_ns`), the `{"unavailable":...}` shape for EXACT views over a
sampled window, and the `control` command (status/metrics/escalate/deescalate
state machine mirroring src/control.c, incl. over-budget denial). Defaults
keep every existing test + protocol-drift green (exact, no daemon).
Tests: `tests/web_unit/fidelity.test.mjs` — 29 Node builder checks (markArea
model, mixed sub-ranges, unavailable panel, manual-vs-anomaly annotation,
metrics + escalate-control models, overhead). Playwright: 4 new B5 tests
(sampled shading, unavailable panels, escalate flow, metrics panel) against a
second sampled+daemon mock. Gates: test_web_ui 171/171 + chaos 25/25 green,
zero console errors, no chart-global regressions.
Acceptance: met — a sampled/mixed window is visually unambiguous, and
escalation is operable from the browser.

### Phase B6 — New analysis views
Depends on: B3 (view registry), A3 (fidelity declarations — all three
require EXACT data and must report "needs full-fidelity data" over
sampled-only windows). Each view is additive: a server view + a
registry entry (requests / pure builder / mount) — this phase is also
the proof that the restructure made views cheap.

1. **Per-execution waterfall** (the 10046 view). Drill path: query
   fingerprint → executions list (sorted by duration) → one
   execution's lifetime as a span timeline — each wait a colored bar,
   gaps = on-CPU, phase boundaries from the existing
   `PGWT_MARKER_PLAN_START`/`EXEC_START` markers. Server: new
   `executions` view (per-fingerprint execution list from markers) +
   `execution_detail` (events for one pid/time range — existing
   filtering covers it). Mount: ECharts custom series (same technique
   as the session timeline).
2. **Latency scatter**. Every wait as a dot on (time, log duration),
   colored by class — exposes modes and outlier bands that aggregates
   hide (two `DataFileRead` bands = cache vs disk; a stripe at a round
   number = a timeout). Server: new `scatter` view with **server-side
   density downsampling** (cap points per screen-pixel bucket, never
   ship raw millions); response includes how many points were dropped
   (no silent truncation). Click/lasso → drill to the underlying
   events.
3. **Transition matrix** — the Sankey's scalable sibling, same data:
   from-event rows × to-event columns, color = count or total time,
   ordered/grouped by wait class; readable where the Sankey saturates
   (>~20 nodes); every cell drills to the matching transitions. Server:
   reuse the existing transitions computation with a matrix-shaped
   response. Offered as an alternate rendering on the transitions tab
   (toggle Sankey ⇄ matrix).

Tests: pure-builder Node tests with synthetic fixtures (waterfall span
layout math, scatter bucket downsampling expectations against the
mock, matrix ordering/grouping); mock server gains the three response
shapes; B4 visual snapshots for each; Playwright drill-path test
(fingerprint → execution → waterfall).
Acceptance: each view lands as registry entry + server view with no
view-manager/transport changes; all three correctly report
unavailability over sampled-only windows.

---

# Track C — Rocky 8 / RHEL 8 support (kernel 4.18 + backports)

Goal: run on RHEL 8-family kernels (Rocky/Alma/RHEL 8, EOL 2029).
Principle: **runtime feature probing, never kernel-version gating** —
RHEL backports make version numbers meaningless. The daemon probes at
startup and degrades gracefully, always reporting exactly which tier is
available and why.

What already works on 4.18: hardware-breakpoint perf events (2.6.33),
`PERF_EVENT_IOC_SET_BPF` (4.1), perf-event BPF programs (4.9), uprobes,
tracepoints, `PERCPU_HASH`. The **sampled tier needs none of the modern
BPF features at all** — after A2 it runs on Rocky 8 unmodified.

Gaps to close for the full tier:
- **BTF/CO-RE**: present from RHEL 8.2+ (4.18.0-193+). For older
  minors, support `--btf <path>` with BTFHub-sourced BTF; otherwise
  require 8.2+.
- **Ringbuf** (upstream 5.8, backported only in later 8.x minors):
  probe via `libbpf_probe_bpf_map_type()`; fall back to
  `BPF_MAP_TYPE_PERF_EVENT_ARRAY` for both `event_ringbuf` and
  `lifecycle_rb`.
- **`bpf_probe_read_user[_str]`** (upstream 5.5, backport varies):
  probe at load; fall back to legacy `bpf_probe_read` (valid for user
  memory on x86_64).

### Phase C1 — Feature-probe module + graceful degradation
Scope: startup probes (BTF, ringbuf, helpers); structured capability
report in `status`/`metrics` (control socket, A0) and startup log;
tiered mode auto-restricts to sampled-only with an explicit reason when
the full tier is unavailable.
Acceptance: on a kernel lacking ringbuf, daemon starts, runs sampled,
and `status` says precisely what is missing.

### Phase C2 — Perf-buffer fallback path
Scope: dual map/program flavors in one skeleton
(`bpf_program__set_autoload` / `bpf_map__set_autocreate` after
probing); perfbuf consumer in `event_stream.c`; **reorder buffer**
before the trace writer (per-CPU perf buffers deliver out of timestamp
order; v2 blocks delta-encode timestamps and require sorted input —
sort within the flush interval). Durations are unaffected (precomputed
in BPF per backend).
Tests: synthetic out-of-order delivery test for the reorder buffer;
full live suite on a Rocky 8 VM; cross-check perfbuf vs. ringbuf trace
equivalence on a kernel that has both.
Acceptance: full tier produces correct traces via perfbuf on Rocky 8.

### Phase C3 — Helper fallbacks + external BTF
Scope: CO-RE-guarded `bpf_probe_read` fallback; `--btf <path>` external
BTF loading; INSTALL.md matrix for RHEL 8 minors.
Acceptance: full tier works on Rocky 8.2+ stock kernel; documented
path for older minors.

Dependencies: C1 needs A0 (control socket) and A2 (provider split).
C2/C3 are independent of Tracks A3+ and B. Testing requires a Rocky 8
VM added to the test matrix (deps installed on the remote box only).

# Track D — Older PostgreSQL support (13–16; PG13 feasibility VERIFIED)

Current state: PG 17/18 full; on 14–16 the tracer starts but does not
capture correctly (INSTALL.md Troubleshooting). Root cause is **address
discovery, not the watchpoint mechanism**: discovery resolves the
`my_wait_event_info` global (`src/discovery.c:650`) and the bootstrap
watchpoint watches that pointer being set, but PG 14–16 backends write
`MyProc->wait_event_info` directly. The watched *field* itself exists
in PGPROC since PG 9.6, and a watchpoint on its address catches writes
regardless of which pointer the code wrote through — so support is a
resolution problem, solvable with machinery that already exists
(ELF symbol resolution + `/proc/<pid>/mem` reads).

Design principle (same spirit as Track C): **PG version knowledge lives
in data tables with runtime validation, not in scattered version
checks.** One version-adapter module owns: symbol names to resolve,
`offsetof(PGPROC, wait_event_info)` per major version, wait-event name
tables, and feature flags (query_id available? pg_wait_events view?).
Every resolved address is validated before trust: read the value and
check the class byte against known wait-event classes; refuse to attach
on mismatch (custom builds can shift offsets — fail loudly, never
trace garbage).

### Phase D1 — MyProc-based address resolution (unlocks PG 14–16)
Scope: fallback discovery path — resolve `MyProc` symbol, read the
pointer per backend via `/proc/<pid>/mem`, add per-version
`offsetof(PGPROC, wait_event_info)`, runtime-validate. Bootstrap flow
for new backends watches the `MyProc` variable write instead of
`my_wait_event_info`. Adapter chooses the path by PG major version.
Benefits both tiers: the sampler (A2) consumes the same registry
addresses.
Tests: live suite against stock PGDG 14/15/16 on the test VM;
validation-rejection unit test (wrong offset → refuse, clear error).
Acceptance: full + sampled capture verified correct on PG 14, 15, 16
under pgbench (cross-check totals against pg_stat_activity sampling).

### Phase D2 — Per-version wait-event name tables (14–16)
Scope: name tables for 14/15/16 enum orderings (PG 17 re-ordered
enums alphabetically; ≤16 used hand-ordered enums — current tables at
`src/wait_event.c` cover only 17/18). Generate tables by script from
PG source headers per major version (data, not hand-maintained code);
commit generated tables + the generator. `pg_wait_events` runtime
discovery stays the preferred path on 17+.
Acceptance: no hex-code event names on 14–16 traces in the live suite.

### Phase D3 — PG 13 (FEASIBILITY VERIFIED — GO, 2026-06-19)
Validated on a real **PostgreSQL 13.23** (Rocky 8.10 box, port 5433) by
direct probe — see [[project_pg13_feasibility]]. PG13 gets the full
wait-event analysis (both tiers) **plus query attribution** via
pg_stat_statements. Three pieces:

**(a) Wait capture — the prerequisite (= Phase D1).** PG13 has no
`my_wait_event_info` global and writes `MyProc->wait_event_info`
directly, so it needs the D1 MyProc-resolution path. Proven live: with
`offsetof(PGPROC, wait_event_info)=684` on 13.23, reading
`*(MyProc)+684` from `/proc/<pid>/mem` matched `pg_stat_activity`
(Lock=0x03000000, Timeout:PgSleep=0x09000001). PG13 postgres is
**non-PIE** on EL8, so the #24 non-PIE load-base fix applies. `MyProc`
resolves from `.dynsym`.

**(b) PG13 wait-event name tables (= Phase D2, extended to 13).**
`pg_wait_events` is PG17+ only; generate PG13's table from its headers.

**(c) Query attribution — Route B1 (pg_stat_statements-based).** PG13
has no in-core query_id, BUT when `pg_stat_statements` is loaded its
`post_parse_analyze` hook populates the core `PlannedStmt.queryId`
field (which exists in PG13). Proven: that field is populated and
**matches `pg_stat_statements.queryid`** — confirmed both via an
`ActivePortal` outside-read and via gdb at `ExecutorStart` entry (queryId
already set when ExecutorStart fires).
- Design: uprobe **`ExecutorStart`** (arg0 = `QueryDesc*`) →
  `+8` (plannedstmt) → `+8` (queryId, uint64) → store in `state_map`
  (the EXISTING attribution pipeline). One uprobe per query (NOT per
  event). Serves BOTH tiers: the A2 sampler reads the cached id at
  10 Hz; the full-tier watchpoint reads it per event. (Offsets on
  13.23: `QueryDesc.plannedstmt=8`, `PlannedStmt.queryId=8`,
  `PortalData.queryDesc=136`.)
- **pgss is required for PG13 query attribution.** If pgss is not
  loaded, query views report "unavailable (requires pg_stat_statements)"
  — the A3 unavailable pattern. (Decision: the text-fingerprint
  alternative, "Route A", is NOT pursued.)
- Coverage gap: utility statements (CHECKPOINT/VACUUM/DDL) go through
  `ProcessUtility`, not `ExecutorStart`, so the ExecutorStart uprobe
  won't attribute them — same gap as other PG versions; an optional
  `ProcessUtility` uprobe can close it later.

**Offset-resolution constraint (important):** **no PG13 debuginfo exists
anywhere** (purged after PG13's Nov-2025 EOL; the binary is stripped).
So PG13 (and any EOL version) offsets MUST be derived from the
`postgresql*-devel` **headers** at build time (compile an `offsetof()`
probe) or hardcoded from header-derived values — never from a debuginfo
package. Symbols (`MyProc`, `ExecutorStart`, `ActivePortal`) come from
`.dynsym`, which survives stripping. Symbol VAs are build-specific →
resolve at runtime from the target binary, don't hardcode.

Note: PG13 is EOL (2025-11); the D1 prerequisite also unlocks PG14–16,
which get **native** query_id (no pgss needed) via the existing
st_query_id path — so PG14–16 are higher-value for the same core work.

### Not supported: PG ≤ 12
Technically possible (wait_event_info exists since 9.6) but PG itself
instruments far fewer wait points before 13 (IO/IPC classes and
coverage grew release by release), so captured data would be
misleadingly sparse; combined with deep EOL (PG12 EOL 2024-11) and
test-matrix cost, the cut line is 13.

Dependencies: D1/D2 are independent of Tracks A–C and can land anytime;
doing D1 before/with A2 is ideal since the sampler shares the registry.
D3 needs pgss for query attribution. Test matrix: PGDG 13 (archive
repo — dropped from the live EL8 mirror at EOL) + 14–16 installed
alongside 17/18 on the test box (PG13.23 already installed on the box,
port 5433). Validation: full + sampled capture correct + (with pgss)
query attribution matching pg_stat_statements, cross-checked vs
pg_stat_activity.

# Track E — Live-suite cleanup (pre-existing debt)

Surfaced 2026-06-13 when the full `run_all.sh` first ran on a real box
since CI was added. These fail/flake on **master baseline** (NOT caused
by B1/A0) and were invisible because CI can't run live capture tests.
Small, independent, do early so "green live run" is a meaningful gate
for later capture phases.

### Phase E1 — De-flake and fix stale live tests
- `test_cross_validate`: flaky — sampling side came back empty on a
  freshly-started PG (passed on the second run). Add warmup / longer
  sampling window / retry so it's deterministic.
- `test_deterministic` "Lock:relation" assertion: gets `Lock:advisory`
  on PG 18 (the lock wait IS captured; lock-hold/wait detection passes)
  — fix the expected event for the PG version, or make the workload
  take the intended relation lock.
- `test_data_idle` / `test_client_wait`: stale ClientRead idle
  semantics — **already fixed by B1**; confirm green after B1 on the
  box (should be resolved).
Acceptance: `run_all.sh` functional section green on dmitry-micro-test,
twice in a row (no flakes).

### Phase E2 — Skip-not-fail for extension-dependent tests
Tests that require the **patched PG** (`pg_wait_event_timing` events,
extension in time_model) currently FAIL on stock PG 18. They must
detect the absent extension and **SKIP** (counted as skipped, not
failed), so a stock-PG box can go fully green.
Acceptance: on stock PG, extension tests skip; on patched PG, they run.

### Phase E3 — Wire test_control.sh into run_all.sh
Deferred from A0 (avoided colliding with B1's run_all.sh edits). Add it
to the live suite. Acceptance: `run_all.sh` runs test_control 16/16.

Independent of all other tracks. E1+E2+E3 are one small PR's worth.

# Sequencing

```
Track A:  A0✅ ─▶ A1✅ ─▶ A2✅ ─▶ A3✅ ─▶ A4✅ ─▶ A5✅    A6✅
Track B:  B1✅ ─▶ B2✅ ─▶ B3✅ ─▶ B4✅ ─────────▶ B5✅    B6 ⬜ (deferred)
Track C:  C1 ⬜  C2 ⬜  C3 ⬜   (Rocky 8 — future)
Track D:  D1 ⬜  D2 ⬜   D3 ⬜  (older PG 14–16 — future)
Track E:  E1✅ E2✅ E3✅  (live-suite cleanup — done across #8/#9/#23)
```

- ✅ = merged to master. ⬜ = not started (future). Core rework
  (A0–A6, B1–B5, E) complete 2026-06-18 @ master c027d3c.
- B1/B2 are small and should land **first** regardless of Track A —
  they protect everything else.
- The tracks parallelize fully until B5.
- Default daemon mode flips to `tiered` only after A4's
  cross-validation test passes its tolerances.
- A capture phase is "done" only with a **green live run on the box**
  (see Progress log → Testing reality), not just green CI.

# Risks and open questions

1. **process_vm_readv vs. PG shm layout** (A2): assumption that PGPROC
   addresses are identical across PG processes (inherited mmap) — true
   for PG's anonymous shm on Linux, but verify against huge_pages=on
   and EXEC_BACKEND-style edge cases early in A2. Mitigation: the
   registry already stores per-pid addresses; per-pid pread fallback is
   a small constant-factor cost.
2. **Sampled estimator error at low rates** (A3/A4): 10 Hz may
   under-represent short-lived events (sub-100ms waits flicker below
   the sampling floor). The cross-validation test quantifies this;
   the UI's fidelity shading keeps it honest with the user. Do not
   paper over it with extrapolation heuristics.
3. **Escalation attach race** (A4): backends forked mid-escalation must
   get watchpoints via the existing fork→bootstrap→init flow; verify
   the lifecycle path is active in tiered mode even while de-escalated
   (it must be, for the registry).
4. **B3 regression risk**: mitigated by ordering (B1/B2 first) and
   per-view migration with the full suite green between views. No
   big-bang rewrite of `app.js`.
5. **Anomaly rule tuning** (A5): baselines differ wildly across
   workloads. Ship conservative defaults, make every threshold
   configurable, and log every near-trigger to the trace so tuning is
   data-driven.
