/* test_wait_event.c — Unit tests for wait event decode tables (PG18) */
#include "wait_event.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

static int tests_run = 0;
static int tests_passed = 0;

#define WEI(cls, id) (((uint32_t)(cls) << 24) | (id))

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
    /* Not idle */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_IO, 21)) == 0,
          "IO events should not be idle");
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
    /* Client:ClientRead is deliberately NOT idle: it is a real wait event
     * (time waiting for the client's next command) counted under the
     * Client class — see pgwt_is_idle_event() in src/wait_event.c. */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_CLIENT, 0)) == 0,
          "Client:ClientRead should NOT be idle");
    /* Other Client events are NOT idle */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_CLIENT, 1)) == 0,
          "Client:ClientWrite should NOT be idle");
}

static void test_activity_events(void)
{
    printf("--- Activity Events ---\n");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 0),  "Activity:ArchiverMain");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 4),  "Activity:CheckpointerMain");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 6),  "Activity:IoWorkerMain");
    CHECK_NAME(WEI(PG_WAIT_ACTIVITY, 17), "Activity:WalWriterMain");
    /* Activity events ARE idle */
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_ACTIVITY, 0)) != 0,
          "Activity events should be idle");
    CHECK(pgwt_is_idle_event(WEI(PG_WAIT_ACTIVITY, 4)) != 0,
          "Activity:CheckpointerMain should be idle");
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
    test_unknown_fallbacks();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
