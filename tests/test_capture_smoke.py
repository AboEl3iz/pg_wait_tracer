#!/usr/bin/env python3
"""test_capture_smoke.py — end-to-end capture smoke test (Phase T0: TST-1/2).

The one test whose entire purpose is: if the tracer silently records
NOTHING from a real PostgreSQL, this must go red. All four field escapes
(#8, #24, #30, #31) lived in the capture/discovery slice that no CI job
executed; this test (driven by tests/ci_smoke.sh in the CI capture-smoke
job) closes that hole.

Deterministic workload, asserted with bounded values:
  1. pg_sleep(3)              -> Timeout:PgSleep, total within tolerance
  2. blocked LOCK TABLE pair  -> Lock:relation wait, bounded duration
  3. short pgbench run        -> query_event view shows query_ids that
                                 cross-check against pg_stat_statements

Wait events are asserted in BOTH:
  - the live view output (--view system_event / query_event), and
  - the written trace file, read back through pgwt-server (the same JSON
    protocol the web UI uses; --replay is NOT used because replay of
    tiered traces is fidelity-broken until T1/FID-5).

Regression coverage (referenced by PR number, per the Trust Milestone):
  - PR #30: in sampled/tiered mode the sampler must feed the live
    accumulator — the tiered live-view assertions here are empty if that
    regresses.
  - PR #31: the query-attribution uprobe must actually fire (on PG13 it
    must probe standard_ExecutorStart) — the query-attribution assertions
    here go empty if it regresses. The assertion cross-checks captured
    query_ids against pg_stat_statements.queryid, so junk/phantom ids
    (e.g. marker leakage, FID-4) cannot satisfy it.
  - PR #24 (load base) would make this whole test capture zero events on
    non-PIE layouts; the unit-level guard is tests/test_discovery*.

Environment requirements (ci.yml configures both; fail loudly otherwise):
  - pg_stat_statements in shared_preload_libraries (PG13: required for
    query attribution at all; PG14+: activates compute_query_id=auto).
  - pgbench + psql for the workload, connecting as "postgres" via local
    trust auth.

Usage: sudo python3 tests/test_capture_smoke.py --mode {full,tiered}
           [--pid POSTMASTER_PID]
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testutil import find_postmaster

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACER = os.path.join(PROJECT_DIR, "pg_wait_tracer")
SERVER = os.path.join(PROJECT_DIR, "pgwt-server")
STRIP_ANSI = re.compile(r'\x1b\[[0-9;]*[a-zA-Z]')

tests_run = 0
tests_passed = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


def psql(sql, timeout=15):
    result = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc", sql],
        capture_output=True, text=True, timeout=timeout
    )
    return result.stdout.strip()


def cleanup_stale_backends():
    try:
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND datname = 'postgres' "
             "AND backend_type = 'client backend' AND state != 'active'")
        psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE pid != pg_backend_pid() AND query LIKE '%pg_sleep%'")
    except subprocess.TimeoutExpired:
        pass
    time.sleep(1)


def pgss_query_ids():
    """query_ids (as unsigned 64-bit) that pg_stat_statements recorded for
    the pgbench workload — the ground truth for attribution asserts."""
    out = psql("SELECT queryid FROM pg_stat_statements "
               "WHERE queryid IS NOT NULL AND calls >= 3")
    ids = set()
    for line in out.split('\n'):
        line = line.strip()
        if re.fullmatch(r'-?\d+', line):
            ids.add(int(line) & 0xFFFFFFFFFFFFFFFF)
    return ids


# ── deterministic workload ─────────────────────────────────────────

class Workload:
    """Three psql sessions held open on stdin pipes:
         sleeper — runs SELECT pg_sleep(3) when fire()d, then sits idle
                   (idle = Client:ClientRead, which is hidden — so the
                   session's PgSleep total stays exactly ~3s; an extra
                   keepalive pg_sleep would contaminate the assertion)
         holder  — BEGIN; LOCK TABLE ... ACCESS EXCLUSIVE (idle-in-txn,
                   holds the lock for the whole phase)
         waiter  — SELECT on the table when fire()d -> blocks on
                   Lock:relation until the phase ends

    In phases 1-3, sessions are opened BEFORE the tracer starts (the
    initial backend scan finds them — same technique as
    test_deterministic.py). Phase 4 (fork-after-attach) opens them AFTER
    the tracer attaches: backends forked post-attach used to be subject to
    the fork->attach race (bootstrap watchpoint armed after the child had
    already written its pointer -> never fires -> zero events, silently;
    observed live on Ubuntu 24.04/kernel 6.8 by the T0 CI run, fixed in T4
    by re-checking the pointer right after arming). The waits are fired
    AFTER the tracer attaches so their durations are fully observed.
    release() commits the holder so the lock wait ENDS inside the
    observation window — in full mode a transition is only written to the
    trace when the wait ends, so a never-ending lock wait would be absent
    from the trace file (the ESC-2/FID-class open-interval gap, owned by
    T1/T3)."""

    LOCK_TABLE = "_smoke_lock_wait"

    def __init__(self):
        self.sleeper = None
        self.holder = None
        self.waiter = None

    def _session(self):
        return subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, text=True)

    def open_sessions(self):
        psql(f"CREATE TABLE IF NOT EXISTS {self.LOCK_TABLE} (id int)")
        self.sleeper = self._session()
        self.holder = self._session()
        self.waiter = self._session()
        self.holder.stdin.write(
            f"BEGIN; LOCK TABLE {self.LOCK_TABLE} IN ACCESS EXCLUSIVE MODE;\n")
        self.holder.stdin.flush()
        time.sleep(1.5)   # sessions connected, lock held
        held = psql(
            f"SELECT count(*) FROM pg_locks "
            f"WHERE relation = '{self.LOCK_TABLE}'::regclass AND granted")
        check(held != "" and int(held) >= 1,
              f"workload: holder acquired AccessExclusiveLock (held={held!r})")

    def fire(self, sleep_s=3):
        """Start the observable waits (call after the tracer attached)."""
        self.sleeper.stdin.write(f"SELECT pg_sleep({sleep_s});\n")
        self.sleeper.stdin.flush()
        self.waiter.stdin.write(f"SELECT count(*) FROM {self.LOCK_TABLE};\n")
        self.waiter.stdin.flush()
        time.sleep(1.5)
        waiters = psql(
            f"SELECT count(*) FROM pg_locks "
            f"WHERE relation = '{self.LOCK_TABLE}'::regclass AND NOT granted")
        check(waiters != "" and int(waiters) >= 1,
              f"workload: waiter blocked on {self.LOCK_TABLE} (waiters={waiters!r})")

    def release(self):
        """Commit the holder -> the waiter's lock wait ends (and gets a
        transition record in full mode)."""
        self.holder.stdin.write("COMMIT;\n")
        self.holder.stdin.flush()
        time.sleep(0.5)

    def stop(self):
        for p in (self.sleeper, self.holder, self.waiter):
            if p is None:
                continue
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        self.sleeper = self.holder = self.waiter = None
        try:
            psql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                 "WHERE pid != pg_backend_pid() AND datname = 'postgres' "
                 f"AND query LIKE '%{self.LOCK_TABLE}%'")
            time.sleep(1)
            psql(f"DROP TABLE IF EXISTS {self.LOCK_TABLE}")
        except subprocess.TimeoutExpired:
            pass


# ── output parsers (same formats test_deterministic/test_query_event use) ──

def parse_system_events(output):
    events = []
    for line in output.split('\n'):
        m = re.match(
            r'^(\S+(?::\S+)?)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+|—)%?',
            line.strip())
        if m:
            events.append({
                'name': m.group(1),
                'count': int(m.group(2)),
                'total_ms': float(m.group(3)),
            })
    return events


def parse_query_event_ids(output):
    ids = set()
    for line in output.split('\n'):
        m = re.match(r'^(-?\d+)\s+(\S+(?::\S+)?)\s+(\d+)\s+([\d.]+)\s',
                     line.strip())
        if m:
            qid = int(m.group(1))
            if qid != 0:
                ids.add(qid & 0xFFFFFFFFFFFFFFFF)
    return ids


def run_tracer(args, timeout):
    proc = subprocess.Popen([TRACER] + args,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()
    out = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    err = stderr.decode('utf-8', errors='replace')
    return out, err


# ── pgwt-server trace-file reader ──────────────────────────────────

def server_query(trace_dir, cmd, extra=None):
    """One-shot pgwt-server JSON query against a trace dir."""
    proc = subprocess.Popen([SERVER, trace_dir],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    try:
        proc.stdin.write(json.dumps({"id": 1, "cmd": "info"}) + "\n")
        proc.stdin.flush()
        info = json.loads(proc.stdout.readline())
        req = {"id": 2, "cmd": cmd,
               "from": max(0, info.get("from_ns", 0) - 30_000_000_000),
               "to": info.get("to_ns", 0) + 30_000_000_000}
        if extra:
            req.update(extra)
        proc.stdin.write(json.dumps(req) + "\n")
        proc.stdin.flush()
        resp = json.loads(proc.stdout.readline())
    finally:
        proc.stdin.close()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    return resp


# ── shared assertions ──────────────────────────────────────────────

def assert_wait_events(events, source, sleep_hi=6000):
    """Deterministic-wait assertions, applied to live view or trace rows.

    sleep_hi: live-view callers pass 7000 — in tiered mode the live
    accumulator is fed by both the sampler and the exact stream, inflating
    totals up to ~2x (the known ESC-3 double-count, owned by Phase T3;
    observed 5710ms for a 3s sleep). Once T3 gates sampler accumulation
    during exact coverage, tighten this back to 6000."""
    names = [e['name'] for e in events]

    sleep_ev = [e for e in events if e['name'] == 'Timeout:PgSleep']
    check(len(sleep_ev) > 0, f"{source}: Timeout:PgSleep present (saw: {names})")
    if sleep_ev:
        total = sleep_ev[0]['total_ms']
        # One pg_sleep(3), fired after attach, session idle afterwards.
        # Sampled tier quantizes at the sample period; bounded either way:
        # zero and wildly-wrong both fail.
        check(1200 <= total <= sleep_hi,
              f"{source}: PgSleep total = {total:.0f}ms (expect ~3000 ±tol)")

    lock_ev = [e for e in events if e['name'] == 'Lock:relation']
    check(len(lock_ev) > 0, f"{source}: Lock:relation present (saw: {names})")
    if lock_ev:
        total = lock_ev[0]['total_ms']
        # The waiter blocks from fire() until the phase ends (>= ~8s of
        # observation); open-interval accounting reports it at tick time.
        check(total >= 4000, f"{source}: Lock:relation total = {total:.0f}ms (expect >= 4000)")
        check(total <= 25000, f"{source}: Lock:relation total = {total:.0f}ms (expect <= 25000)")


# ── phases ─────────────────────────────────────────────────────────

def phase_live_system_event(pm_pid, mode):
    print(f"--- Phase 1: live view (system_event, --mode {mode}) ---")
    wl = Workload()
    wl.open_sessions()
    tracer = subprocess.Popen(
        [TRACER, "--mode", mode, "--pid", str(pm_pid),
         "--interval", "12", "--duration", "15",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(2.5)   # BPF load + initial scan + (full) watchpoint attach
    try:
        wl.fire(sleep_s=3)     # t≈2.5: pg_sleep(3) + lock wait begin
        time.sleep(6.5)
        wl.release()           # t≈10.5: lock wait ends (~8s) before the tick
        stdout, stderr = tracer.communicate(timeout=40)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, stderr = tracer.communicate()
    finally:
        wl.stop()

    out = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    err = stderr.decode('utf-8', errors='replace')
    events = parse_system_events(out)
    # THE zero-event guard: an empty view here means capture is dead (#30).
    check(len(events) > 0,
          "live view shows at least one event" if events else
          f"live view shows at least one event (stderr tail: {err[-300:]!r})")
    assert_wait_events(events, f"live/{mode}", sleep_hi=7000)


def phase_live_query_event(pm_pid, mode):
    """PR #31 regression: the query-attribution path works end to end —
    query_event view ids must intersect pg_stat_statements.queryid."""
    print(f"--- Phase 2: query attribution (query_event, --mode {mode}) ---")

    pgb_init = subprocess.run(
        ["pgbench", "-U", "postgres", "-d", "postgres", "-i", "-s", "1"],
        capture_output=True, text=True)
    check(pgb_init.returncode == 0,
          "pgbench -i succeeded" if pgb_init.returncode == 0 else
          f"pgbench -i succeeded: {pgb_init.stderr[-200:]}")
    psql("SELECT pg_stat_statements_reset()")

    # pgbench starts FIRST so its backends are found by the initial scan
    # (keeps this phase deterministic; the fork-after-attach path has its
    # own dedicated phase 4).
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres", "-c", "4", "-T", "14"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)

    tracer = subprocess.Popen(
        [TRACER, "--mode", mode, "--pid", str(pm_pid),
         "--interval", "10", "--duration", "12",
         "--view", "query_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        stdout, stderr = tracer.communicate(timeout=40)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, stderr = tracer.communicate()
    pgbench.wait(timeout=30)

    out = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    view_ids = parse_query_event_ids(out)
    truth = pgss_query_ids()
    check(len(truth) > 0,
          f"pg_stat_statements recorded the pgbench queries ({len(truth)} ids)")
    matched = view_ids & truth
    check(len(matched) > 0,
          f"query_event view ids cross-check against pg_stat_statements "
          f"(view={len(view_ids)}, pgss={len(truth)}, matched={len(matched)}) "
          f"[PR #31 regression]")


def phase_trace_file(pm_pid, mode):
    print(f"--- Phase 3: written trace file (pgwt-server read, --mode {mode}) ---")
    trace_dir = tempfile.mkdtemp(prefix=f"pgwt_smoke_{mode}_")
    os.chmod(trace_dir, 0o755)

    wl = Workload()
    wl.open_sessions()
    psql("SELECT pg_stat_statements_reset()")
    # pgbench starts first so its backends are found by the initial scan
    # (the fork-after-attach path is covered by phase 4).
    pgbench = subprocess.Popen(
        ["pgbench", "-U", "postgres", "-d", "postgres", "-c", "2", "-T", "16"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    tracer = subprocess.Popen(
        [TRACER, "--mode", mode, "--pid", str(pm_pid),
         "-T", trace_dir, "--duration", "18", "--quiet",
         "--interval", "5"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(2.5)
    try:
        wl.fire(sleep_s=3)     # t≈2.5 after attach
        time.sleep(8)
        wl.release()           # lock wait ends (~10s) inside the trace window
        _, stderr = tracer.communicate(timeout=45)
        err = stderr.decode('utf-8', errors='replace')
        pgbench.wait(timeout=30)

        resp = server_query(trace_dir, "top_events")
        rows = resp.get("rows", [])
        check(len(rows) > 0,
              f"trace file has events (top_events rows={len(rows)})" if rows
              else f"trace file has events (daemon stderr tail: {err[-300:]!r})")
        events = [{'name': r.get('name'), 'count': r.get('count', 0),
                   'total_ms': r.get('total_ms', 0.0)} for r in rows]
        assert_wait_events(events, f"trace/{mode}")

        # Query attribution must land in the trace too — and cross-check
        # against pg_stat_statements so phantom ids can't satisfy it.
        qresp = server_query(trace_dir, "top_queries")
        trace_ids = set()
        for r in qresp.get("rows", []):
            try:
                qid = int(str(r.get("query_id", "0")), 10)
            except ValueError:
                continue
            if qid != 0:
                trace_ids.add(qid & 0xFFFFFFFFFFFFFFFF)
        truth = pgss_query_ids()
        matched = trace_ids & truth
        check(len(matched) > 0,
              f"trace file query attribution cross-checks against "
              f"pg_stat_statements (trace={len(trace_ids)}, pgss={len(truth)}, "
              f"matched={len(matched)}) [PR #31 regression]")
    finally:
        wl.stop()
        subprocess.run(["rm", "-rf", trace_dir])


def phase_fork_after_attach(pm_pid, mode):
    """T4/CAP-8 regression: backends forked AFTER the tracer attached must
    record events. The fork->attach race (bootstrap watchpoint armed after
    the child already wrote its my_wait_event_info/MyProc pointer -> the
    watchpoint never fires -> the backend records nothing for its whole
    life, silently) was observed live in --mode full by the T0 CI run; the
    T4 fix re-checks the pointer immediately after arming the bootstrap
    watchpoint, closing the gap. In tiered mode the same phase guards the
    sampler's lazy-resolve path for post-attach forks."""
    print(f"--- Phase 4: fork-after-attach (--mode {mode}) ---")

    tracer = subprocess.Popen(
        [TRACER, "--mode", mode, "--pid", str(pm_pid),
         "--interval", "14", "--duration", "17",
         "--view", "system_event"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(2.5)   # BPF load + initial scan complete — NOW fork backends

    wl = Workload()
    try:
        wl.open_sessions()     # all three backends fork post-attach
        wl.fire(sleep_s=3)
        time.sleep(5)
        wl.release()
        stdout, stderr = tracer.communicate(timeout=40)
    except subprocess.TimeoutExpired:
        tracer.kill()
        stdout, stderr = tracer.communicate()
    finally:
        wl.stop()

    out = STRIP_ANSI.sub('', stdout.decode('utf-8', errors='replace'))
    err = stderr.decode('utf-8', errors='replace')
    events = parse_system_events(out)
    names = [e['name'] for e in events]
    check(len(events) > 0,
          "fork-after-attach: events captured from post-attach backends"
          if events else
          f"fork-after-attach: events captured from post-attach backends "
          f"(stderr tail: {err[-300:]!r})")

    sleep_ev = [e for e in events if e['name'] == 'Timeout:PgSleep']
    check(len(sleep_ev) > 0,
          f"fork-after-attach: Timeout:PgSleep from a post-attach backend "
          f"(saw: {names}) [fork->attach race regression]")
    if sleep_ev:
        total = sleep_ev[0]['total_ms']
        check(1200 <= total <= 7000,
              f"fork-after-attach: PgSleep total = {total:.0f}ms "
              f"(expect ~3000 ±tol)")

    lock_ev = [e for e in events if e['name'] == 'Lock:relation']
    check(len(lock_ev) > 0,
          f"fork-after-attach: Lock:relation from a post-attach backend "
          f"(saw: {names})")


class CpuStorm:
    """N psql sessions each executing a long run of SHORT CPU-bound
    statements (~30-100 ms each, fully cached): pg_stat_activity shows
    them state='active' nearly continuously, and the real command
    boundaries exercise the T2 command-open gate exactly like production
    OLTP (one long DO block would hide the gate)."""

    def __init__(self, n):
        self.sessions = [subprocess.Popen(
            ["psql", "-U", "postgres", "-d", "postgres"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, text=True) for _ in range(n)]
        time.sleep(1.5)   # connected (and, pre-attach, scanned)

    def fire(self, reps=400):
        stmt = "SELECT count(*) FROM generate_series(1,300000);\n"
        for s in self.sessions:
            s.stdin.write(stmt * reps)
            s.stdin.flush()

    def stop(self):
        for s in self.sessions:
            s.terminate()
            try:
                s.wait(timeout=5)
            except subprocess.TimeoutExpired:
                s.kill()
        cleanup_stale_backends()


def sample_active_sessions(duration_s, interval=0.5):
    """pg_stat_activity 1-per-interval ground truth: count of client
    backends with state='active', excluding the sampling connection."""
    counts = []
    end = time.time() + duration_s
    while time.time() < end:
        out = psql("SELECT count(*) FROM pg_stat_activity "
                   "WHERE state = 'active' "
                   "AND backend_type = 'client backend' "
                   "AND pid != pg_backend_pid()", timeout=10)
        if re.fullmatch(r'\d+', out or ''):
            counts.append(int(out))
        time.sleep(interval)
    return counts


def aas_bucket_total(bucket):
    return sum(v for k, v in bucket.items()
               if k not in ("t", "total", "cat") and isinstance(v, (int, float)))


def ctl_query(trace_dir, cmd):
    """One JSON request over the daemon control socket."""
    import socket
    path = os.path.join(trace_dir, "pgwt.sock")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    try:
        s.connect(path)
        s.sendall((json.dumps({"cmd": cmd}) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            c = s.recv(4096)
            if not c:
                break
            buf += c
        return json.loads(buf.decode().split("\n")[0])
    finally:
        s.close()


def phase_sampled_aas_truth(pm_pid):
    """T2 (AAS-1) definition-of-done: sampled AAS — now CPU-inclusive —
    must match pg_stat_activity 1s-sampling ground truth. A CPU-bound
    storm (3 hogs) + a pg_sleep session; pre-T2 the sampler skipped all
    we==0 and reported AAS ~ 0.0x for exactly this shape (study Q4:
    -98%%). Anomaly escalation is disabled (huge factor) so the window is
    PURE sampled tier."""
    print("--- Phase 5: sampled CPU-inclusive AAS vs pg_stat_activity ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_aas_")
    os.chmod(trace_dir, 0o755)

    storm = CpuStorm(3)
    sleeper = subprocess.Popen(
        ["psql", "-U", "postgres", "-d", "postgres"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, text=True)
    time.sleep(1.0)

    tracer = subprocess.Popen(
        [TRACER, "--mode", "tiered", "--pid", str(pm_pid),
         "-T", trace_dir, "--duration", "17", "--quiet",
         "--interval", "5", "--anomaly-aas-factor", "1000000"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(3.0)   # BPF load + scan + first ticks
    try:
        win_from = time.time_ns()
        storm.fire()
        sleeper.stdin.write("SELECT pg_sleep(10);\n")
        sleeper.stdin.flush()
        time.sleep(0.5)
        psa = sample_active_sessions(9.0, interval=0.5)
        win_to = time.time_ns()
        _, stderr = tracer.communicate(timeout=40)
        err = stderr.decode('utf-8', errors='replace')
    except subprocess.TimeoutExpired:
        tracer.kill()
        _, stderr = tracer.communicate()
        err = stderr.decode('utf-8', errors='replace')
    finally:
        storm.stop()
        sleeper.terminate()

    psa_mean = sum(psa) / len(psa) if psa else 0.0
    check(psa_mean >= 2.0,
          f"ground truth: pg_stat_activity mean active = {psa_mean:.2f} "
          f"(storm running; n={len(psa)})")

    resp = server_query(trace_dir, "aas",
                        extra={"from": win_from, "to": win_to, "buckets": 9})
    buckets = resp.get("buckets", [])
    check(len(buckets) > 0,
          "aas view has buckets over the storm window" if buckets else
          f"aas view has buckets (stderr tail: {err[-300:]!r})")
    if buckets:
        totals = [aas_bucket_total(b) for b in buckets]
        tracer_mean = sum(totals) / len(totals)
        cpu_mean = sum(b.get("cpu", 0.0) for b in buckets) / len(buckets)
        tol = max(0.9, 0.35 * psa_mean)
        check(abs(tracer_mean - psa_mean) <= tol,
              f"sampled AAS matches pg_stat_activity ground truth: "
              f"tracer={tracer_mean:.2f} vs psa={psa_mean:.2f} (tol ±{tol:.2f}) "
              f"[AAS-1 definition of done]")
        check(cpu_mean >= 1.0,
              f"CPU class carries the storm: cpu AAS = {cpu_mean:.2f} "
              f"(pre-T2 sampler reported ~0 here)")
        check(resp.get("fidelity") == "sampled",
              f"window is pure sampled tier (fidelity={resp.get('fidelity')!r})")

    subprocess.run(["rm", "-rf", trace_dir])


def phase_cpu_storm_escalation(pm_pid):
    """T2 definition-of-done: a SELECT-storm CPU incident raises sampled
    AAS enough to trigger anomaly escalation (the engine was blind to CPU
    before — AAS-1), and the AAS chart shows no step artifact across the
    sampled->exact tier switch."""
    print("--- Phase 6: CPU storm triggers escalation; no AAS step ---")
    trace_dir = tempfile.mkdtemp(prefix="pgwt_smoke_esc_")
    os.chmod(trace_dir, 0o755)

    storm = CpuStorm(3)
    tracer = subprocess.Popen(
        [TRACER, "--mode", "tiered", "--pid", str(pm_pid),
         "-T", trace_dir, "--duration", "20", "--quiet",
         "--interval", "5"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(7.0)   # BPF load + scan + anomaly baseline warmup (~5 s idle)
    try:
        storm_from = time.time_ns()
        storm.fire()
        time.sleep(5.0)
        metrics = {}
        try:
            metrics = ctl_query(trace_dir, "metrics")
        except OSError as e:
            print(f"  (control socket: {e})")
        storm_active = sample_active_sessions(3.0, interval=0.5)
        storm_to = time.time_ns()
        _, stderr = tracer.communicate(timeout=40)
        err = stderr.decode('utf-8', errors='replace')
    except subprocess.TimeoutExpired:
        tracer.kill()
        _, stderr = tracer.communicate()
        err = stderr.decode('utf-8', errors='replace')
    finally:
        storm.stop()

    fires = metrics.get("anomaly_fires_total", 0)
    windows = metrics.get("escalation_windows_total", 0)
    check(fires >= 1 and windows >= 1,
          f"CPU storm triggered anomaly escalation "
          f"(anomaly_fires_total={fires}, escalation_windows_total={windows}, "
          f"tier={metrics.get('tier')!r}) [AAS-1: engine no longer CPU-blind]"
          + ("" if fires >= 1 else f" (tracer stderr tail: {err[-300:]!r})"))

    # No step artifact: every interior 1s bucket across the tier switch
    # must show the storm. The anomaly fires ~0.3-1 s into the storm, so
    # starting the window at +0.7 s puts the sampled->exact switch INSIDE
    # the asserted range. Pre-T2, sampled buckets read ~0.0 while exact
    # (escalated) buckets read ~3 — a hard step.
    resp = server_query(trace_dir, "aas",
                        extra={"from": storm_from + 700_000_000,
                               "to": storm_to - 1_000_000_000,
                               "buckets": 7})
    buckets = resp.get("buckets", [])
    check(len(buckets) >= 3, f"aas buckets across the tier switch "
          f"(got {len(buckets)}, fidelity={resp.get('fidelity')!r})")
    if buckets:
        totals = [aas_bucket_total(b) for b in buckets]
        lo = min(totals)
        truth = (sum(storm_active) / len(storm_active)) if storm_active else 3.0
        check(lo >= 1.2,
              f"no AAS step artifact at the tier switch: min bucket = "
              f"{lo:.2f}, buckets = {[f'{t:.1f}' for t in totals]}, "
              f"psa during storm = {truth:.1f}")

    subprocess.run(["rm", "-rf", trace_dir])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, help='Postmaster PID')
    parser.add_argument('--mode', required=True, choices=['full', 'tiered'])
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("ERROR: must run as root (sudo)")
        sys.exit(1)
    for binpath in (TRACER, SERVER):
        if not os.path.exists(binpath):
            print(f"ERROR: {binpath} not built")
            sys.exit(1)

    pm_pid = args.pid or find_postmaster()
    if not pm_pid:
        print("ERROR: cannot find PostgreSQL postmaster PID")
        sys.exit(1)

    print(f"=== test_capture_smoke --mode {args.mode} (postmaster PID {pm_pid}) ===")

    # Environment gate: query-attribution assertions need query_ids to be
    # computed at all. Fail LOUDLY rather than skipping (this test exists
    # to prevent vacuous greens).
    preload = psql("SHOW shared_preload_libraries")
    if "pg_stat_statements" not in preload:
        print("ERROR: pg_stat_statements is not in shared_preload_libraries "
              f"(got: {preload!r}).\n"
              "  PG13 needs it for query attribution; PG14+ need it (or "
              "compute_query_id=on) for query_ids.\n"
              "  ci.yml configures this — do the same on this box.")
        sys.exit(1)
    psql("CREATE EXTENSION IF NOT EXISTS pg_stat_statements")

    cleanup_stale_backends()

    phase_live_system_event(pm_pid, args.mode)
    phase_live_query_event(pm_pid, args.mode)
    phase_trace_file(pm_pid, args.mode)
    phase_fork_after_attach(pm_pid, args.mode)
    if args.mode == 'tiered':
        # T2 (AAS semantics): CPU-inclusive sampled AAS vs pg_stat_activity
        # ground truth, and the CPU-storm escalation + tier-switch
        # continuity checks (docs/AAS_SEMANTICS_DECISION.md).
        phase_sampled_aas_truth(pm_pid)
        phase_cpu_storm_escalation(pm_pid)

    print(f"\n{tests_passed}/{tests_run} checks passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == '__main__':
    main()
