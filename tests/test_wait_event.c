/* test_wait_event.c — Unit tests for wait event decode tables (PG18) */
#include "wait_event.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

static int tests_run = 0;
static int tests_passed = 0;

/* WEI is provided by pg_wait_tracer.h */

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

#define CHECK_NAME(wei, expected) do { \
    char buf[128]; \
    pgwt_event_full_name(wei, buf, sizeof(buf)); \
    CHECK(strcmp(buf, expected) == 0, \
          "event 0x%08x: expected \"%s\", got \"%s\"", wei, expected, buf); \
} while(0)

static void test_cpu(void)
{
    printf("--- CPU ---\n");
    CHECK_NAME(0, "CPU*");
    CHECK(strcmp(pgwt_class_name(0), "CPU") == 0,
          "class_name(0) expected CPU");
    CHECK(strcmp(pgwt_event_name(0), "CPU") == 0,
          "event_name(0) expected CPU");
    CHECK(pgwt_is_idle_event(0) == 0, "CPU should not be idle");
    CHECK(pgwt_is_hidden_event(0) == 0, "CPU should not be hidden");
}

static void test_io_events(void)
{
    printf("--- IO Events ---\n");
    CHECK_NAME(WEI(PG_WAIT_IO, 0),  "IO:AioIoCompletion");
    CHECK_NAME(WEI(PG_WAIT_IO, 3),  "IO:BasebackupRead");
    CHECK_NAME(WEI(PG_WAIT_IO, 17), "IO:DataFileExtend");
    CHECK_NAME(WEI(PG_WAIT_IO, 18), "IO:DataFileFlush");
    CHECK_NAME(WEI(PG_WAIT_IO, 21), "IO:DataFileRead");
    CHECK_NAME(WEI(PG_WAIT_IO, 24), "IO:DataFileWrite");
    CHECK_NAME(WEI(PG_WAIT_IO, 50), "IO:SlruFlushSync");
    CHECK_NAME(WEI(PG_WAIT_IO, 75), "IO:WalRead");
    CHECK_NAME(WEI(PG_WAIT_IO, 78), "IO:WalSync");
    CHECK_NAME(WEI(PG_WAIT_IO, 80), "IO:WalWrite");
    /* Class name */
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_IO, 0)), "IO") == 0,
          "class_name for IO");
    /* Not idle, not hidden */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_IO, 21)) == 0,
          "IO events should not be idle");
    CHECK(pgwt_is_hidden_event(WEI(PG_WAIT_IO, 21)) == 0,
          "IO events should not be hidden");
}

static void test_lock_events(void)
{
    printf("--- Lock Events (0-indexed, matches LockTagType) ---\n");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 0),  "Lock:relation");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 1),  "Lock:extend");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 3),  "Lock:page");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 4),  "Lock:tuple");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 5),  "Lock:transactionid");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 6),  "Lock:virtualxid");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 10), "Lock:advisory");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 11), "Lock:applytransaction");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_LOCK, 0)), "Lock") == 0,
          "class_name for Lock");
}

static void test_lwlock_events(void)
{
    printf("--- LWLock Events ---\n");
    /* Predefined LWLocks */
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 1),  "LWLock:ShmemIndex");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 3),  "LWLock:XidGen");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 4),  "LWLock:ProcArray");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 8),  "LWLock:WALWrite");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 9),  "LWLock:ControlFile");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 22), "LWLock:Autovacuum");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 32), "LWLock:SyncRep");
    /* Builtin tranches */
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 54), "LWLock:XactBuffer");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 61), "LWLock:WALInsert");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 62), "LWLock:BufferContent");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 66), "LWLock:BufferMapping");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 67), "LWLock:LockManager");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 92), "LWLock:XactSLRU");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 94), "LWLock:AioUringCompletion");
    /* Unknown tranche → numeric fallback */
    char buf[128];
    pgwt_event_full_name(WEI(PG_WAIT_LWLOCK, 200), buf, sizeof(buf));
    CHECK(strstr(buf, "id=200") != NULL,
          "LWLock unknown tranche should show id=200, got \"%s\"", buf);
    /* Removed slots (e.g. 10, 11) → numeric fallback */
    pgwt_event_full_name(WEI(PG_WAIT_LWLOCK, 10), buf, sizeof(buf));
    CHECK(strstr(buf, "id=10") != NULL,
          "LWLock removed slot 10 should show id=10, got \"%s\"", buf);

    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_LWLOCK, 1)), "LWLock") == 0,
          "class_name for LWLock");
}

static void test_timeout_events(void)
{
    printf("--- Timeout Events ---\n");
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 0), "Timeout:BaseBackupThrottle");
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 1), "Timeout:CheckpointWriteDelay");
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 2), "Timeout:PgSleep");
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 6), "Timeout:SpinDelay");
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 9), "Timeout:WalSummarizerError");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_TIMEOUT, 0)), "Timeout") == 0,
          "class_name for Timeout");
}

static void test_client_events(void)
{
    printf("--- Client Events ---\n");
    CHECK_NAME(WEI(PG_WAIT_CLIENT, 0), "Client:ClientRead");
    CHECK_NAME(WEI(PG_WAIT_CLIENT, 1), "Client:ClientWrite");
    CHECK_NAME(WEI(PG_WAIT_CLIENT, 5), "Client:SslOpenServer");
    CHECK_NAME(WEI(PG_WAIT_CLIENT, 8), "Client:WalSenderWriteData");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_CLIENT, 0)), "Client") == 0,
          "class_name for Client");
    /* Client:ClientRead is IDLE for LOAD accounting — excluded from DB
     * Time / AAS, like Oracle's "SQL*Net message from client" — but it is
     * NOT hidden: it must remain visible in event lists/graphs. See the
     * load-vs-visibility split in src/wait_event.c. */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_CLIENT, 0)) == 1,
          "Client:ClientRead should be idle (excluded from DB Time)");
    CHECK(pgwt_is_hidden_event(WEI(PG_WAIT_CLIENT, 0)) == 0,
          "Client:ClientRead should NOT be hidden (stays visible)");
    /* Other Client events are NOT idle and NOT hidden */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_CLIENT, 1)) == 0,
          "Client:ClientWrite should NOT be idle");
    CHECK(pgwt_is_hidden_event(WEI(PG_WAIT_CLIENT, 1)) == 0,
          "Client:ClientWrite should NOT be hidden");
}

static void test_activity_events(void)
{
    printf("--- Activity Events ---\n");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 0),  "Activity:ArchiverMain");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 4),  "Activity:CheckpointerMain");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 6),  "Activity:IoWorkerMain");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 17), "Activity:WalWriterMain");
    /* Activity events ARE idle AND hidden */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_ACTIVITY, 0)) != 0,
          "Activity events should be idle");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_ACTIVITY, 4)) != 0,
          "Activity:CheckpointerMain should be idle");
    CHECK(pgwt_is_hidden_event(WEI(PG_WAIT_ACTIVITY, 0)) != 0,
          "Activity events should be hidden");
    CHECK(pgwt_is_hidden_event(WEI(PG_WAIT_ACTIVITY, 4)) != 0,
          "Activity:CheckpointerMain should be hidden");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_ACTIVITY, 0)), "Activity") == 0,
          "class_name for Activity");
}

static void test_ipc_events(void)
{
    printf("--- IPC Events ---\n");
    CHECK_NAME(WEI(PG_WAIT_IPC, 0),  "IPC:AppendReady");
    CHECK_NAME(WEI(PG_WAIT_IPC, 8),  "IPC:BufferIO");
    CHECK_NAME(WEI(PG_WAIT_IPC, 12), "IPC:CheckpointStart");
    CHECK_NAME(WEI(PG_WAIT_IPC, 52), "IPC:SyncRep");
    CHECK_NAME(WEI(PG_WAIT_IPC, 56), "IPC:XactGroupUpdate");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_IPC, 0)), "IPC") == 0,
          "class_name for IPC");
}

static void test_bufferpin(void)
{
    printf("--- BufferPin ---\n");
    CHECK_NAME(WEI(PG_WAIT_BUFFERPIN, 0), "BufferPin:BufferPin");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_BUFFERPIN, 0)), "BufferPin") == 0,
          "class_name for BufferPin");
}

static void test_extension(void)
{
    printf("--- Extension ---\n");
    CHECK_NAME(WEI(PG_WAIT_EXTENSION, 0), "Extension:Extension");
    CHECK(strcmp(pgwt_class_name(WEI(PG_WAIT_EXTENSION, 0)), "Extension") == 0,
          "class_name for Extension");
}

static void test_unknown_fallbacks(void)
{
    printf("--- Unknown / Out-of-Range ---\n");
    char buf[128];
    /* IO out of range → numeric fallback "IO:id=999" */
    pgwt_event_full_name(WEI(PG_WAIT_IO, 999), buf, sizeof(buf));
    CHECK(strstr(buf, "id=999") != NULL,
          "IO:999 should contain 'id=999', got \"%s\"", buf);
    /* Lock id=99 (out of range) → numeric fallback "Lock:id=99" */
    pgwt_event_full_name(WEI(PG_WAIT_LOCK, 99), buf, sizeof(buf));
    CHECK(strstr(buf, "id=99") != NULL,
          "Lock:99 should contain 'id=99', got \"%s\"", buf);
    /* Unknown class */
    pgwt_event_full_name(WEI(0xFF, 0), buf, sizeof(buf));
    CHECK(strstr(buf, "Unknown") != NULL || strstr(buf, "id=") != NULL,
          "class 0xFF should be unknown or numeric, got \"%s\"", buf);
}

/* Regression: dynamic names loaded from pg_wait_events arrive ordered by
 * NAME (alphabetical), which is NOT enum order for the Lock class
 * (LockTagType: relation=0 … advisory=10). The loader must map each name
 * to its correct enum id, not its row position. Before the fix,
 * Lock:relation (id 0) was mislabelled "advisory". */
static void test_dynamic_name_mapping(void)
{
    printf("--- Dynamic Name Mapping (pg_wait_events order) ---\n");

    /* Lock rows exactly as `SELECT type,name ... ORDER BY type,name`
     * returns them: alphabetical by name. Includes a fabricated future
     * event ("zzznewlock") to exercise the unknown-name fallback. */
    const char *buf =
        "Lock|advisory\n"
        "Lock|applytransaction\n"
        "Lock|extend\n"
        "Lock|frozenid\n"
        "Lock|object\n"
        "Lock|page\n"
        "Lock|relation\n"
        "Lock|spectoken\n"
        "Lock|transactionid\n"
        "Lock|tuple\n"
        "Lock|userlock\n"
        "Lock|virtualxid\n"
        "Lock|zzznewlock\n";

    CHECK(pgwt_load_event_names_from_buffer(buf) == 0,
          "load dynamic names from buffer");

    /* Correct enum ids despite alphabetical input order */
    CHECK_NAME(WEI(PG_WAIT_LOCK, 0),  "Lock:relation");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 1),  "Lock:extend");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 5),  "Lock:transactionid");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 6),  "Lock:virtualxid");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 10), "Lock:advisory");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 11), "Lock:applytransaction");
    /* Unknown future name appended after the class max (11) */
    CHECK_NAME(WEI(PG_WAIT_LOCK, 12), "Lock:zzznewlock");
}

/* PG13 has different wait-event enum orderings than 17/18; pgwt_init_event_names(13)
 * swaps in the generated PG13 tables (src/wait_event_pg13.inc). Spot-check the
 * classes that differ most (IO, LWLock, IPC, Activity, Client, Timeout) plus
 * the Lock class which is shared. Offsets verified against PG13.23 headers. */
static void test_pg13_names(void)
{
    printf("--- PG13 tables ---\n");
    pgwt_init_event_names(13);

    /* IO: PG13 enum starts at BufFileRead (no Aio/Basebackup events of 17/18). */
    CHECK_NAME(WEI(PG_WAIT_IO, 0),  "IO:BufFileRead");
    CHECK_NAME(WEI(PG_WAIT_IO, 13), "IO:DataFileRead");
    CHECK_NAME(WEI(PG_WAIT_IO, 32), "IO:RelationMapSync");  /* PG17 renamed -> Replace */
    CHECK_NAME(WEI(PG_WAIT_IO, 67), "IO:WalWrite");

    /* Timeout: PG13 ordering (PgSleep at id 1, not 2 as in PG17/18). */
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 1), "Timeout:PgSleep");

    /* Activity: PG13 includes PgStatMain (removed in PG15). */
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 0), "Activity:ArchiverMain");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 7), "Activity:PgStatMain");

    /* Client: PG13 spells WalSenderWaitWal (PG17 -> WaitForWal). */
    CHECK_NAME(WEI(PG_WAIT_CLIENT, 0), "Client:ClientRead");
    CHECK_NAME(WEI(PG_WAIT_CLIENT, 7), "Client:WalSenderWaitWal");

    /* IPC: PG13 ordering, BgWorkerShutdown casing. */
    CHECK_NAME(WEI(PG_WAIT_IPC, 1), "IPC:BgWorkerShutdown");

    /* LWLock: PG13 individual locks + tranches (NUM_INDIVIDUAL_LWLOCKS=48). */
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 4),  "LWLock:ProcArray");
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 11), "LWLock:XactSLRU");   /* SLRU was individual in PG13 */
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 55), "LWLock:WALInsert");  /* first tranche after 48 base + ... */
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 62), "LWLock:LockManager");

    /* Lock class is identical to 17/18 (LockTagType 0..10). */
    CHECK_NAME(WEI(PG_WAIT_LOCK, 0),  "Lock:relation");
    CHECK_NAME(WEI(PG_WAIT_LOCK, 10), "Lock:advisory");

    /* Restore PG18 tables for subsequent tests. */
    pgwt_init_event_names(18);
}

/* Regression (#8 mislabeling class; caught live by the CI capture-smoke
 * PG13 cell): a trace recorded on PG13 must ship a name sidecar carrying
 * the mapping it was WRITTEN with. pgwt_write_names_json() must dump the
 * active version-selected hardcoded tables even when no dynamic names
 * were loaded (PG13 has no pg_wait_events view) — before the fix, PG13
 * traces had no sidecar at all and pgwt-server silently decoded PG13 ids
 * with PG18 tables (PgSleep rendered as CheckpointWriteDelay). */
static void test_pg13_sidecar_roundtrip(void)
{
    printf("--- PG13 name sidecar round-trip ---\n");

    char dir[] = "/tmp/pgwt_names_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp failed");

    /* Daemon side: PG13 tables active, no dynamic names available. */
    pgwt_init_event_names(13);
    CHECK(pgwt_write_names_json(dir) == 0,
          "write sidecar without dynamic names");

    /* Reader side: fresh pgwt-server defaults to PG18 tables, then loads
     * the sidecar — PG13 ids must decode with PG13 names afterwards. */
    pgwt_init_event_names(18);
    CHECK(pgwt_load_names_json(dir) == 0, "load sidecar");

    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 1), "Timeout:PgSleep");      /* PG18 id1 = CheckpointWriteDelay */
    CHECK_NAME(WEI(PG_WAIT_TIMEOUT, 4), "Timeout:VacuumDelay");  /* PG18 id4 = RecoveryRetrieve... */
    CHECK_NAME(WEI(PG_WAIT_LWLOCK, 11), "LWLock:XactSLRU");      /* individual lock in PG13 */
    CHECK_NAME(WEI(PG_WAIT_LOCK, 0),   "Lock:relation");

    char path[600];
    snprintf(path, sizeof(path), "%s/wait_event_names.json", dir);
    remove(path);
    remove(dir);
}

int main(void)
{
    printf("=== test_wait_event ===\n");
    pgwt_init_event_names(18);
    test_cpu();
    test_io_events();
    test_lock_events();
    test_lwlock_events();
    test_timeout_events();
    test_client_events();
    test_activity_events();
    test_ipc_events();
    test_bufferpin();
    test_extension();
    test_pg13_names();
    test_unknown_fallbacks();
    test_dynamic_name_mapping();     /* near-last: sets dyn_loaded */
    test_pg13_sidecar_roundtrip();   /* last: overwrites dyn tables from sidecar */

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
