#!/usr/bin/env python3
"""Generate PostgreSQL 13 wait-event name tables for src/wait_event.c.

PG13 predates the pg_wait_events view (PG17+) and its wait-event enum
orderings differ from PG17/18, so the tracer needs hardcoded PG13 tables.
This generator derives them from the PG13 server *headers* (data, not
hand-typed code), so they are reproducible and auditable.

Inputs (from postgresql13-devel, /usr/pgsql-13/include/server):
  - pgstat.h               WaitEventActivity/Client/IPC/Timeout/IO enums
  - storage/lock.h         LockTagType (Lock class subtypes)
  - storage/lwlocknames.h  individual LWLock names + NUM_INDIVIDUAL_LWLOCKS
  - storage/lwlock.h       BuiltinTrancheIds (LWLock tranches)

Why headers and not debuginfo: PG13 is EOL (2025-11); no debuginfo package
exists anywhere and the binary is stripped. The -devel headers are the only
authoritative offset/enum source. See docs/ROADMAP_AND_STATUS.md.

The enum-constant -> display-string transformation mirrors what PG's
pgstat_get_wait_*() switch statements emit: split on '_', TitleCase each
word, then apply a fixed substitution table for the multi-letter tokens PG
keeps cased specially (BufFile, MessageQueue, LibPQWalReceiver, WalSender,
AddToDataDir, ...). The substitution table was validated against the
known-correct PG17 tables already committed in src/wait_event.c (every
overlapping event name matches exactly).

Usage:
  tools/gen_pg13_wait_events.py \
      --pgstat /usr/pgsql-13/include/server/pgstat.h \
      --lock   /usr/pgsql-13/include/server/storage/lock.h \
      --lwnames /usr/pgsql-13/include/server/storage/lwlocknames.h \
      --lwlock /usr/pgsql-13/include/server/storage/lwlock.h \
      > /tmp/pg13_tables.inc

The output is pasted into src/wait_event.c between the PG13 BEGIN/END markers.
"""
import argparse
import re
import sys

# Multi-word tokens PG keeps in a specific case. Applied to the
# underscore-separated, lowercased enum-tail words BEFORE TitleCasing.
# Key = the run of lowercased words (joined by '_'); value = the literal
# display fragment PG uses. Validated against PG17 io/ipc/client tables.
# Wait-event classes (Activity/Client/IPC/Timeout/IO) display "Wal".
WORD_SUBST = {
    "buffile": "BufFile",
    "mq": "MessageQueue",
    "libpqwalreceiver": "LibPQWalReceiver",
    "walsender": "WalSender",
    "walreceiver": "WalReceiver",
    "addtodatadir": "AddToDataDir",
    "recheckdatadir": "ReCheckDataDir",
    "pgstat": "PgStat",
    "dsm": "Dsm",
    "slru": "Slru",
    "wal": "Wal",
    "gss": "Gss",
    "ssl": "Ssl",
    "btree": "Btree",
    "xact": "Xact",
    "bgworker": "BgWorker",
    "io": "IO",
}

# LWLock individual locks + builtin tranches use a DIFFERENT casing for a
# handful of multi-letter tokens than the wait-event classes do (notably
# "WAL" all-caps, "DSA"/"DSM"/"IO" all-caps, "CommitTs", "MultiXact*", ...).
# Validated against the committed PG17 lwlock_tranches[] table.
WORD_SUBST_LWLOCK = {
    "wal": "WAL",
    "dsa": "DSA",
    "dsm": "DSM",
    "io": "IO",
    "committs": "CommitTs",
    "multixactgen": "MultiXactGen",
    "multixactoffset": "MultiXactOffset",
    "multixactmember": "MultiXactMember",
    "multixacttruncation": "MultiXactTruncation",
    "fastpath": "FastPath",
    "tidbitmap": "TidBitmap",
    "tuplestore": "TupleStore",
    "twophase": "TwoPhase",
    "relcache": "RelCache",
    "subtrans": "Subtrans",
    "slru": "SLRU",
    "btree": "Btree",
    "oid": "Oid",
    "xid": "Xid",
    "sinval": "SInval",
}


def titlecase_word(w, subst):
    if w in subst:
        return subst[w]
    return w[:1].upper() + w[1:]


def enum_to_display(const, prefix, subst=WORD_SUBST):
    """WAIT_EVENT_DATA_FILE_READ -> DataFileRead"""
    tail = const[len(prefix):].lstrip("_").lower()
    words = tail.split("_")
    return "".join(titlecase_word(w, subst) for w in words)


def parse_enum_block(text, typename):
    """Return the list of enum constants in declaration order for the
    `typedef enum { ... } typename;` block."""
    # Match the brace block immediately preceding `} <typename>`. The body
    # contains no nested braces (these are flat enums), so [^{}]* is safe and
    # anchors on the correct block even with many `typedef enum {` above.
    m = re.search(r"\{([^{}]*)\}\s*" + re.escape(typename) + r"\s*;",
                  text, re.S)
    if not m:
        sys.exit("enum %s not found" % typename)
    body = m.group(1)
    consts = []
    for line in body.splitlines():
        line = line.split("/*")[0].split("//")[0].strip()
        if not line:
            continue
        mm = re.match(r"([A-Z_][A-Z0-9_]*)", line)
        if mm:
            consts.append(mm.group(1))
    return consts


def emit_table(out, name, prefix, consts, c_max_macro):
    out.append("static const char *%s[] = {" % name)
    for i, c in enumerate(consts):
        out.append('    [%d] = "%s",' % (i, enum_to_display(c, prefix)))
    out.append("};")
    out.append("#define %s %d" % (c_max_macro, len(consts) - 1))
    out.append("")


def parse_lock_tags(text):
    consts = parse_enum_block(text, "LockTagType")
    # display names come from LockTagTypeNames[]: relation, extend, frozenid,
    # page, tuple, transactionid, virtualxid, spectoken, object, userlock,
    # advisory  — these are stable and identical to the existing lock_events
    # table for indices 0..10, so PG13 reuses lock_events (no new table).
    return consts


def parse_individual_lwlocks(text):
    """Parse lwlocknames.h: '#define FooLock (&MainLWLockArray[N].lock)'.
    Display name strips the trailing 'Lock'. Returns dict id->name and
    NUM_INDIVIDUAL_LWLOCKS."""
    names = {}
    for m in re.finditer(r"#define\s+(\w+)\s+\(&MainLWLockArray\[(\d+)\]\.lock\)",
                         text):
        nm, idx = m.group(1), int(m.group(2))
        disp = nm[:-4] if nm.endswith("Lock") else nm
        names[idx] = disp
    mm = re.search(r"#define\s+NUM_INDIVIDUAL_LWLOCKS\s+(\d+)", text)
    num_indiv = int(mm.group(1)) if mm else (max(names) + 1)
    return names, num_indiv


def parse_builtin_tranches(text, num_indiv):
    """Parse BuiltinTrancheIds enum; first member == NUM_INDIVIDUAL_LWLOCKS.
    Display names come from BuiltinTrancheNames[] in lwlock.c; the
    transformation LWTRANCHE_FOO_BAR -> FooBar matches those strings (the
    PG17 lwlock_tranches table confirms the convention)."""
    consts = parse_enum_block(text, "BuiltinTrancheIds")
    # Drop the trailing sentinel LWTRANCHE_FIRST_USER_DEFINED.
    consts = [c for c in consts if c != "LWTRANCHE_FIRST_USER_DEFINED"]
    out = {}
    for i, c in enumerate(consts):
        out[num_indiv + i] = enum_to_display(c, "LWTRANCHE", WORD_SUBST_LWLOCK)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pgstat", required=True)
    ap.add_argument("--lock", required=True)
    ap.add_argument("--lwnames", required=True)
    ap.add_argument("--lwlock", required=True)
    args = ap.parse_args()

    pgstat = open(args.pgstat).read()
    lockh = open(args.lock).read()
    lwnames = open(args.lwnames).read()
    lwlock = open(args.lwlock).read()

    out = []
    out.append("/* ===== PG13 wait-event tables — GENERATED by")
    out.append(" *       tools/gen_pg13_wait_events.py from postgresql13-devel headers.")
    out.append(" *       Do not edit by hand; re-run the generator. ===== */")
    out.append("")

    # Activity / Client / IPC / Timeout / IO
    emit_table(out, "activity_events_pg13", "WAIT_EVENT",
               parse_enum_block(pgstat, "WaitEventActivity"),
               "ACTIVITY_EVENTS_PG13_MAX")
    emit_table(out, "client_events_pg13", "WAIT_EVENT",
               parse_enum_block(pgstat, "WaitEventClient"),
               "CLIENT_EVENTS_PG13_MAX")
    emit_table(out, "ipc_events_pg13", "WAIT_EVENT",
               parse_enum_block(pgstat, "WaitEventIPC"),
               "IPC_EVENTS_PG13_MAX")
    emit_table(out, "timeout_events_pg13", "WAIT_EVENT",
               parse_enum_block(pgstat, "WaitEventTimeout"),
               "TIMEOUT_EVENTS_PG13_MAX")
    emit_table(out, "io_events_pg13", "WAIT_EVENT",
               parse_enum_block(pgstat, "WaitEventIO"),
               "IO_EVENTS_PG13_MAX")

    # LWLock: individual + builtin tranches, into one sparse table.
    indiv, num_indiv = parse_individual_lwlocks(lwnames)
    tranches = parse_builtin_tranches(lwlock, num_indiv)
    merged = dict(indiv)
    merged.update(tranches)
    maxid = max(merged)
    out.append("/* LWLock: individual locks (lwlocknames.h, NUM_INDIVIDUAL_LWLOCKS=%d)"
               % num_indiv)
    out.append(" * + builtin tranches (BuiltinTrancheIds). Sparse — removed slots NULL. */")
    out.append("static const char *lwlock_tranches_pg13[] = {")
    for i in range(maxid + 1):
        if i in merged:
            out.append('    [%d] = "%s",' % (i, merged[i]))
    out.append("};")
    out.append("#define LWLOCK_TRANCHES_PG13_MAX %d" % maxid)
    out.append("")

    # Sanity note on Lock class.
    lock_tags = parse_lock_tags(lockh)
    out.append("/* Lock class (LockTagType): %d subtypes (0..%d) — identical to"
               % (len(lock_tags), len(lock_tags) - 1))
    out.append(" * the shared lock_events[] table for these indices, so PG13")
    out.append(" * reuses lock_events (no PG13-specific Lock table needed). */")
    out.append("")

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
