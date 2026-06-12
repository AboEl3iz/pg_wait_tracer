# Rework Plan: Tiered-Fidelity Capture + UI Restructure

Status: PROPOSED
Date: 2026-06-11

Two tracks. Track A makes the daemon an always-on monitor with on-demand
full-fidelity escalation. Track B makes the web UI testable, extensible,
and supportable. The tracks are independent until Phase A5/B4, where the
UI grows fidelity-aware features on top of the restructured codebase.

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
- Reader accepts v1 (all blocks implicitly `TRANSITIONS`) and v2.
  Writer always emits v2. `gen_test_traces` learns to emit both.

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

### Phase A0 — Control socket + self-metrics (groundwork)
Scope: D4 socket with `status`/`metrics` only; daemon metrics counters;
pgwt-server `control` proxy command; `--dump` prints daemon status if
socket present.
Files: `src/daemon.c`, new `src/control.c/h`, `src/server.c`.
Tests: unit test for the socket protocol; integration check in
`run_all.sh` (start daemon, query status).
Acceptance: daemon answers status/metrics over the socket with zero
change to capture behavior.

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

### Phase A5 — Anomaly-triggered escalation
Scope: D5 rules engine: AAS-vs-baseline and lock-class-fraction
triggers, hysteresis, cooldown, budget interaction, escalation-reason
records in the trace.
Files: `src/escalation.c`, `src/compute.c` (rolling baseline on the
live sample stream).
Tests: synthetic trigger tests (scripted sample streams → expected
fire/no-fire); live test with injected `LOCK TABLE` contention
(reuse `demos/workload.sh`).
Acceptance: an injected lock storm auto-escalates, captures full
transitions, de-escalates, and the trace records why.

### Phase A6 — Cooperative provider stub (interface freeze only)
Scope: `coop` provider compiled but returning "not available"; document
the contract the extension must satisfy (same event schema, EXACT
fidelity, its own start/stop). No further work in this track.

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

### Phase B1 — Safety net (before touching anything)
Scope:
- Playwright fixture fails any test on console error / pageerror /
  unhandled rejection.
- GitHub Actions workflow: build pgwt, run `test_web_ui.py` against
  `mock_server.py` on every PR/push. In CI, missing Playwright is a
  **failure**, not a skip (fix the silent skip at
  `tests/run_all.sh:174-179` for CI context).
- Real-server fixture: commit a small trace file (from
  `gen_test_traces`), run one suite pass against actual
  `pgwt-server --replay` to catch mock-vs-real protocol drift.
Acceptance: a deliberately introduced JS exception fails CI.

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

### Phase B5 — Fidelity-aware UI (joins Track A)
Depends on: A3 (fidelity field in responses), A4 (escalation via
control proxy), B3 (view registry).
Scope:
- AAS chart shades sampled-fidelity time ranges (subtle background
  band); legend explains it.
- Views that require EXACT render an explicit "no full-fidelity data in
  this window — escalate to capture" state instead of an empty chart.
- "Escalate 60s" button wired through pgwt-server's control proxy;
  shows budget remaining; escalation windows and trigger reasons
  rendered as chart annotations.
- Daemon self-metrics panel (events/s, drops, overhead estimate).
Tests: mock server extended with fidelity-tagged responses + control
commands; builder tests for shading/annotation models; Playwright test
for the escalate flow against the mock.
Acceptance: a mixed-fidelity trace is visually unambiguous about which
data is which, and escalation is operable from the browser.

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

# Track D — Older PostgreSQL support (14–16 full; 12–13 optional)

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

### Phase D3 (optional, demand-driven) — PG 12/13
Scope: same D1 resolution path (no `my_wait_event_info` at all pre-14)
+ name tables for 12/13. **No query attribution**: in-core query_id
(`compute_query_id`, `pgstat_report_query_id`) is PG 14+ — query views
report explicit "unavailable on PG < 14" (same pattern as fidelity
unavailability in A3), never silently empty. Query *text* via
`debug_query_string` could be added later if demand justifies it.
Note EOL: PG 13 EOL 2025-11, PG 12 EOL 2024-11 — implement only if
users ask.

### Not supported: PG ≤ 11
Technically possible (wait_event_info exists since 9.6) but PG itself
instruments far fewer wait points before 12 (IO/IPC classes and
coverage grew release by release), so captured data would be
misleadingly sparse; combined with deep EOL and test-matrix cost, the
cut line is 12.

Dependencies: D1/D2 are independent of Tracks A–C and can land anytime;
doing D1 before/with A2 is ideal since the sampler shares the registry.
Test matrix: PGDG 14–16 installed alongside 17/18 on the remote test
VM (deps on remote only).

# Sequencing

```
Track A:  A0 ──▶ A1 ──▶ A2 ──▶ A3 ──▶ A4 ──▶ A5      A6 (anytime after A2)
Track B:  B1 ──▶ B2 ──▶ B3 ──▶ B4 ─────────────▶ B5
                                            (B5 needs A3+A4)
Track C:        (A0+A2) ──▶ C1 ──▶ C2 ──▶ C3
Track D:  D1 ──▶ D2  (independent; ideally with A2)      D3 if demanded
```

- B1/B2 are small and should land **first** regardless of Track A —
  they protect everything else.
- The tracks parallelize fully until B5.
- Default daemon mode flips to `tiered` only after A4's
  cross-validation test passes its tolerances.

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
