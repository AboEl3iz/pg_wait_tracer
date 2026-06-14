"""testutil.py — Shared test helpers for pg_wait_tracer Python tests.

Usage:
    from testutil import find_postmaster
    pm_pid = find_postmaster()          # highest PG version available
    pm_pid = find_postmaster(pg_version=16)  # specific version
"""
import os
import re
import subprocess


def pg_wait_sampling_available():
    """Return True if the pg_wait_sampling extension is loaded and queryable.

    Tests that cross-check against pg_wait_sampling (Extension-class waits,
    sample-count cross-validation) must skip cleanly when it is absent —
    e.g. on a stock PostgreSQL or in CI. Note: a per-test psql() helper
    that returns "" on error cannot be used to detect this, since the
    failure is silent; we must inspect the process return code here.
    """
    r = subprocess.run(
        ["psql", "-U", "postgres", "-d", "postgres", "-tAc",
         "SELECT 1 FROM pg_wait_sampling_profile LIMIT 1"],
        capture_output=True, text=True)
    return r.returncode == 0


def find_postmaster(pg_version=None):
    """Find the PostgreSQL postmaster PID.

    If pg_version is specified, find that exact version.
    Otherwise find the highest available version.

    A postmaster is a postgres process whose parent is NOT also postgres.

    Returns PID (int) or None.
    """
    result = subprocess.run(["pgrep", "-x", "postgres"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return None

    best_pid = None
    best_ver = 0

    for pid_str in result.stdout.strip().split('\n'):
        if not pid_str:
            continue
        pid = int(pid_str)

        # Skip children — a postmaster's parent is not postgres
        try:
            ppid = None
            with open(f"/proc/{pid}/status") as f:
                for line in f:
                    if line.startswith("PPid:"):
                        ppid = int(line.split()[1])
                        break
            if ppid is None:
                continue
            parent_comm = open(f"/proc/{ppid}/comm").read().strip()
            if parent_comm == "postgres":
                continue
        except (FileNotFoundError, PermissionError, ValueError):
            continue

        # Get version from exe path: /usr/lib/postgresql/18/bin/postgres
        try:
            exe = os.readlink(f"/proc/{pid}/exe")
        except OSError:
            continue

        m = re.search(r'postgresql/(\d+)/', exe)
        if m:
            ver = int(m.group(1))
        else:
            # Fallback: run postgres --version
            try:
                out = subprocess.run([exe, "--version"],
                                     capture_output=True, text=True, timeout=5)
                m2 = re.search(r'PostgreSQL\)?\s+(\d+)', out.stdout)
                ver = int(m2.group(1)) if m2 else 0
            except Exception:
                continue

        if pg_version is not None:
            if ver == pg_version:
                return pid
        else:
            if ver > best_ver:
                best_ver = ver
                best_pid = pid

    return best_pid
