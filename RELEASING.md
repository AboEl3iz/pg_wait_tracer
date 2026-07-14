# Releasing pg_wait_tracer

A release is this checklist, not a memory (TST-11). The goal is that every
released version ships from a known-green matrix and carries a version that the
client/server handshake can compare.

## Versioning model

- The client (Go `pgwt`) and the server/daemon (C `pgwt-server` /
  `pg_wait_tracer`) share **one** version string. Each binary embeds
  `git describe --tags --always --dirty` at build time:
  - C: the Makefile injects `-DPGWT_BUILD_VERSION`; reported by `pgwt-server`
    in the `info` response and by the daemon in the control-socket `status`.
  - Go: `make pgwt-client` passes `-ldflags "-X main.buildVersion=…"`; reported
    on `/session` and used for the skew banner.
- **Protocol revision** (`PGWT_PROTOCOL_REV` in `src/pg_wait_tracer.h`, mirrored
  by `protocolRev` in `web/main.go` and `PROTOCOL_REV` in
  `tests/mock_server.py`). Bump it **only** on a breaking change to the
  JSON-line command surface (renamed/retyped fields, changed command semantics);
  additive fields do not bump it. A protocol bump must update all three mirrors
  in the same change — `tests/test_protocol_drift.py` enforces the mock side.

Because the Mac client is rebuilt on the laptop while the Linux server is
deployed separately, a **skewed pair is the normal state**. The handshake warns
(bridge stderr + a UI banner); it never refuses. Aligning versions is
housekeeping, not a hard requirement — but a *protocol* mismatch means responses
may be misread, so treat that banner as a stop sign.

## Pre-release checklist

Run in order. Do not tag until every gate is green.

1. **Working tree is clean and on `master`** at the commit you intend to tag
   (`git status`, `git log -1`). `git describe` must not report `-dirty`.

2. **Full CI green.** All jobs in `.github/workflows/ci.yml` pass on the release
   commit: `build-and-unit`, `capture-smoke` (PG 13/16/17/18), `web-ui`,
   `protocol-drift`, `snapshots`. In particular the golden-fixture decode test
   (in `build-and-unit`) must be green — see step 6 for what a red one means.

3. **Nightly matrix green.** Trigger `.github/workflows/nightly.yml` manually
   (Actions → nightly → Run workflow) on the release commit, or confirm the most
   recent scheduled run for it is green. All cells (rockylinux:8 / rockylinux:9 /
   ubuntu:24.04) must build, pass the unit suite, and pass the capture smoke.
   Note: full-mode watchpoint assertions may be reported as a LOUD skip in a
   cell whose hypervisor lacks hardware breakpoints — that is expected; the
   tiered/sampled assertions are the hard gate and must pass.

4. **Live runs on real EL8 + EL9 boxes.** On each box (provision per `INSTALL.md`
   — EL8/EL9 sections, a running PostgreSQL, root), run twice consecutively:

   ```bash
   sudo tests/run_all.sh --require-live
   ```

   Both runs must be green **with zero skips in the live section** — under
   `--require-live` a live skip is a failure, which is the whole point (an
   all-skip run used to pass). The EL8 box re-validates the non-PIE +
   static-libbpf platform (the #24 cell). Record which PG version each box ran.

5. **Version bump / embed points.** There is no hardcoded version to edit — the
   build derives it from the tag via `git describe`. Confirm the embed wiring is
   intact if the build system changed:
   - `PGWT_BUILD_VERSION` reaches `CFLAGS` (Makefile) and shows up in
     `echo '{"id":1,"cmd":"info"}' | ./pgwt-server <trace-dir>` → `server_version`.
   - `make pgwt-client` embeds the same string (`./pgwt --help` logs it, or check
     `/session`).
   - Bump `PGWT_PROTOCOL_REV` (+ its two mirrors) only if the protocol changed
     this cycle.

6. **Golden-fixture check for format changes.** If anything under the on-disk
   trace format changed this cycle, the committed golden fixture guards it:
   - A red `test_golden_fixture` on an *unchanged* fixture means the
     reader/writer altered how existing files decode — a compatibility
     regression. Fix it, do not edit the expected checksum.
   - An *intentional* format bump (new `PGWT_TRACE_VERSION`) must **add** a new
     `tests/fixtures/golden/rev<N+1>/` fixture and test case, keeping the old
     one — see `tests/fixtures/golden/README.md`. Never replace a shipped
     revision's bytes.

7. **CHANGELOG.** Move the `Unreleased` section's entries under a new
   `## [X.Y] — <date>` heading in `CHANGELOG.md`, and start a fresh `Unreleased`.
   Keep entries factual and one paragraph per theme.

## Tagging

```bash
git tag -a vX.Y -m "pg_wait_tracer vX.Y"
git push origin vX.Y
```

The tag is what the build embeds, so tag the exact reviewed commit. After
pushing the tag, a from-scratch build (`make && make pgwt-client`) reports
`vX.Y` (no `-dirty`, no `-<n>-g<sha>` suffix).

## After the release

- Verify a fresh client built from the tag connects to a server built from the
  tag with **no** version-skew banner.
- Announce the supported matrix for this version (see `INSTALL.md`).
