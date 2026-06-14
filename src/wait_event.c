/* wait_event.c — Wait event decode tables for PostgreSQL 14+
 *
 * PG17+ auto-generates enums alphabetically (case-insensitive) within each
 * class, starting from 0. PG16 and earlier use manually-ordered enums.
 *
 * Tables here cover PG17 and PG18. For PG16 and earlier, events that don't
 * match the current table gracefully fall back to numeric display ("IO:id=N").
 *
 * Dynamic name resolution: pgwt_load_event_names_from_pg() queries the running
 * PG instance's pg_wait_events view (PG17+) and builds name tables at runtime.
 * pgwt_write_names_json() / pgwt_load_names_json() persist the mapping as a
 * sidecar file alongside trace files, so pgwt-server can resolve names for
 * any PG version without hardcoded tables.
 */
#include "wait_event.h"
#include "pg_wait_tracer.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* PG major version, set by pgwt_init_event_names() */
static int pg_version = 18;

/* Dynamic name storage — heap-allocated when loaded from PG or sidecar.
 * Each class has an array of strdup'd names indexed by event_id. */
#define DYN_MAX_EVENTS_PER_CLASS 512
static char *dyn_names[16][DYN_MAX_EVENTS_PER_CLASS]; /* [class_byte][event_id] */
static int   dyn_max[16];                              /* max event_id per class */
static int   dyn_loaded = 0;                           /* 1 if dynamic names active */

/* ── IO Events PG18 (class 0x0A, 81 events, 0-indexed, alphabetical) ── */
static const char *io_events_pg18[] = {
    [0]  = "AioIoCompletion",
    [1]  = "AioIoUringExecution",
    [2]  = "AioIoUringSubmit",
    [3]  = "BasebackupRead",
    [4]  = "BasebackupSync",
    [5]  = "BasebackupWrite",
    [6]  = "BufFileRead",
    [7]  = "BufFileTruncate",
    [8]  = "BufFileWrite",
    [9]  = "ControlFileRead",
    [10] = "ControlFileSync",
    [11] = "ControlFileSyncUpdate",
    [12] = "ControlFileWrite",
    [13] = "ControlFileWriteUpdate",
    [14] = "CopyFileCopy",
    [15] = "CopyFileRead",
    [16] = "CopyFileWrite",
    [17] = "DataFileExtend",
    [18] = "DataFileFlush",
    [19] = "DataFileImmediateSync",
    [20] = "DataFilePrefetch",
    [21] = "DataFileRead",
    [22] = "DataFileSync",
    [23] = "DataFileTruncate",
    [24] = "DataFileWrite",
    [25] = "DsmAllocate",
    [26] = "DsmFillZeroWrite",
    [27] = "LockFileAddToDataDirRead",
    [28] = "LockFileAddToDataDirSync",
    [29] = "LockFileAddToDataDirWrite",
    [30] = "LockFileCreateRead",
    [31] = "LockFileCreateSync",
    [32] = "LockFileCreateWrite",
    [33] = "LockFileReCheckDataDirRead",
    [34] = "LogicalRewriteCheckpointSync",
    [35] = "LogicalRewriteMappingSync",
    [36] = "LogicalRewriteMappingWrite",
    [37] = "LogicalRewriteSync",
    [38] = "LogicalRewriteTruncate",
    [39] = "LogicalRewriteWrite",
    [40] = "RelationMapRead",
    [41] = "RelationMapReplace",
    [42] = "RelationMapWrite",
    [43] = "ReorderBufferRead",
    [44] = "ReorderBufferWrite",
    [45] = "ReorderLogicalMappingRead",
    [46] = "ReplicationSlotRead",
    [47] = "ReplicationSlotRestoreSync",
    [48] = "ReplicationSlotSync",
    [49] = "ReplicationSlotWrite",
    [50] = "SlruFlushSync",
    [51] = "SlruRead",
    [52] = "SlruSync",
    [53] = "SlruWrite",
    [54] = "SnapbuildRead",
    [55] = "SnapbuildSync",
    [56] = "SnapbuildWrite",
    [57] = "TimelineHistoryFileSync",
    [58] = "TimelineHistoryFileWrite",
    [59] = "TimelineHistoryRead",
    [60] = "TimelineHistorySync",
    [61] = "TimelineHistoryWrite",
    [62] = "TwophaseFileRead",
    [63] = "TwophaseFileSync",
    [64] = "TwophaseFileWrite",
    [65] = "VersionFileSync",
    [66] = "VersionFileWrite",
    [67] = "WalSenderTimelineHistoryRead",
    [68] = "WalBootstrapSync",
    [69] = "WalBootstrapWrite",
    [70] = "WalCopyRead",
    [71] = "WalCopySync",
    [72] = "WalCopyWrite",
    [73] = "WalInitSync",
    [74] = "WalInitWrite",
    [75] = "WalRead",
    [76] = "WalSummaryRead",
    [77] = "WalSummaryWrite",
    [78] = "WalSync",
    [79] = "WalSyncMethodAssign",
    [80] = "WalWrite",
};
#define IO_EVENTS_PG18_MAX 80

/* ── IO Events PG17 (class 0x0A, 77 events, 0-indexed, alphabetical) ──
 * PG17 uses the same alphabetical ordering as PG18 but lacks 4 events
 * added in PG18: AioIoCompletion, AioIoUringExecution, AioIoUringSubmit,
 * CopyFileCopy. This shifts all subsequent indices. */
static const char *io_events_pg17[] = {
    [0]  = "BasebackupRead",
    [1]  = "BasebackupSync",
    [2]  = "BasebackupWrite",
    [3]  = "BufFileRead",
    [4]  = "BufFileTruncate",
    [5]  = "BufFileWrite",
    [6]  = "ControlFileRead",
    [7]  = "ControlFileSync",
    [8]  = "ControlFileSyncUpdate",
    [9]  = "ControlFileWrite",
    [10] = "ControlFileWriteUpdate",
    [11] = "CopyFileRead",
    [12] = "CopyFileWrite",
    [13] = "DataFileExtend",
    [14] = "DataFileFlush",
    [15] = "DataFileImmediateSync",
    [16] = "DataFilePrefetch",
    [17] = "DataFileRead",
    [18] = "DataFileSync",
    [19] = "DataFileTruncate",
    [20] = "DataFileWrite",
    [21] = "DsmAllocate",
    [22] = "DsmFillZeroWrite",
    [23] = "LockFileAddToDataDirRead",
    [24] = "LockFileAddToDataDirSync",
    [25] = "LockFileAddToDataDirWrite",
    [26] = "LockFileCreateRead",
    [27] = "LockFileCreateSync",
    [28] = "LockFileCreateWrite",
    [29] = "LockFileReCheckDataDirRead",
    [30] = "LogicalRewriteCheckpointSync",
    [31] = "LogicalRewriteMappingSync",
    [32] = "LogicalRewriteMappingWrite",
    [33] = "LogicalRewriteSync",
    [34] = "LogicalRewriteTruncate",
    [35] = "LogicalRewriteWrite",
    [36] = "RelationMapRead",
    [37] = "RelationMapReplace",
    [38] = "RelationMapWrite",
    [39] = "ReorderBufferRead",
    [40] = "ReorderBufferWrite",
    [41] = "ReorderLogicalMappingRead",
    [42] = "ReplicationSlotRead",
    [43] = "ReplicationSlotRestoreSync",
    [44] = "ReplicationSlotSync",
    [45] = "ReplicationSlotWrite",
    [46] = "SlruFlushSync",
    [47] = "SlruRead",
    [48] = "SlruSync",
    [49] = "SlruWrite",
    [50] = "SnapbuildRead",
    [51] = "SnapbuildSync",
    [52] = "SnapbuildWrite",
    [53] = "TimelineHistoryFileSync",
    [54] = "TimelineHistoryFileWrite",
    [55] = "TimelineHistoryRead",
    [56] = "TimelineHistorySync",
    [57] = "TimelineHistoryWrite",
    [58] = "TwophaseFileRead",
    [59] = "TwophaseFileSync",
    [60] = "TwophaseFileWrite",
    [61] = "VersionFileSync",
    [62] = "VersionFileWrite",
    [63] = "WalSenderTimelineHistoryRead",
    [64] = "WalBootstrapSync",
    [65] = "WalBootstrapWrite",
    [66] = "WalCopyRead",
    [67] = "WalCopySync",
    [68] = "WalCopyWrite",
    [69] = "WalInitSync",
    [70] = "WalInitWrite",
    [71] = "WalRead",
    [72] = "WalSummaryRead",
    [73] = "WalSummaryWrite",
    [74] = "WalSync",
    [75] = "WalSyncMethodAssign",
    [76] = "WalWrite",
};
#define IO_EVENTS_PG17_MAX 76

/* Active IO event table pointer (set by pgwt_init_event_names).
 * Default to PG18 tables in case init is not called. */
static const char **io_events = io_events_pg18;
static int io_events_max = IO_EVENTS_PG18_MAX;

/* ── Lock Events (class 0x03) ────────────────────────────── */
/* Lock types match LockTagType enum, 0-indexed */
static const char *lock_events[] = {
    [0]  = "relation",
    [1]  = "extend",
    [2]  = "frozenid",
    [3]  = "page",
    [4]  = "tuple",
    [5]  = "transactionid",
    [6]  = "virtualxid",
    [7]  = "spectoken",
    [8]  = "object",
    [9]  = "userlock",
    [10] = "advisory",
    [11] = "applytransaction",
};
#define LOCK_EVENTS_MAX 11

/* ── Timeout Events (class 0x09, 10 events, 0-indexed) ──── */
static const char *timeout_events[] = {
    [0] = "BaseBackupThrottle",
    [1] = "CheckpointWriteDelay",
    [2] = "PgSleep",
    [3] = "RecoveryApplyDelay",
    [4] = "RecoveryRetrieveRetryInterval",
    [5] = "RegisterSyncRequest",
    [6] = "SpinDelay",
    [7] = "VacuumDelay",
    [8] = "VacuumTruncate",
    [9] = "WalSummarizerError",
};
#define TIMEOUT_EVENTS_MAX 9

/* ── Activity Events (class 0x05, 18 events, 0-indexed) ──── */
static const char *activity_events[] = {
    [0]  = "ArchiverMain",
    [1]  = "AutovacuumMain",
    [2]  = "BgwriterHibernate",
    [3]  = "BgwriterMain",
    [4]  = "CheckpointerMain",
    [5]  = "CheckpointerShutdown",
    [6]  = "IoWorkerMain",
    [7]  = "LogicalApplyMain",
    [8]  = "LogicalLauncherMain",
    [9]  = "LogicalParallelApplyMain",
    [10] = "RecoveryWalStream",
    [11] = "ReplicationSlotsyncMain",
    [12] = "ReplicationSlotsyncShutdown",
    [13] = "SysloggerMain",
    [14] = "WalReceiverMain",
    [15] = "WalSenderMain",
    [16] = "WalSummarizerWal",
    [17] = "WalWriterMain",
};
#define ACTIVITY_EVENTS_MAX 17

/* ── Client Events (class 0x06, 9 events, 0-indexed) ────── */
static const char *client_events[] = {
    [0] = "ClientRead",
    [1] = "ClientWrite",
    [2] = "GssOpenServer",
    [3] = "LibPQWalReceiverConnect",
    [4] = "LibPQWalReceiverReceive",
    [5] = "SslOpenServer",
    [6] = "WaitForStandbyConfirmation",
    [7] = "WalSenderWaitForWal",
    [8] = "WalSenderWriteData",
};
#define CLIENT_EVENTS_MAX 8

/* ── IPC Events (class 0x08, 57 events, 0-indexed) ────────── */
static const char *ipc_events[] = {
    [0]  = "AppendReady",
    [1]  = "ArchiveCleanupCommand",
    [2]  = "ArchiveCommand",
    [3]  = "BackendTermination",
    [4]  = "BackupWaitWalArchive",
    [5]  = "BgWorkerShutdown",
    [6]  = "BgWorkerStartup",
    [7]  = "BtreePage",
    [8]  = "BufferIO",
    [9]  = "CheckpointDelayComplete",
    [10] = "CheckpointDelayStart",
    [11] = "CheckpointDone",
    [12] = "CheckpointStart",
    [13] = "ExecuteGather",
    [14] = "HashBatchAllocate",
    [15] = "HashBatchElect",
    [16] = "HashBatchLoad",
    [17] = "HashBuildAllocate",
    [18] = "HashBuildElect",
    [19] = "HashBuildHashInner",
    [20] = "HashBuildHashOuter",
    [21] = "HashGrowBatchesDecide",
    [22] = "HashGrowBatchesElect",
    [23] = "HashGrowBatchesFinish",
    [24] = "HashGrowBatchesReallocate",
    [25] = "HashGrowBatchesRepartition",
    [26] = "HashGrowBucketsElect",
    [27] = "HashGrowBucketsReallocate",
    [28] = "HashGrowBucketsReinsert",
    [29] = "LogicalApplySendData",
    [30] = "LogicalParallelApplyStateChange",
    [31] = "LogicalSyncData",
    [32] = "LogicalSyncStateChange",
    [33] = "MessageQueueInternal",
    [34] = "MessageQueuePutMessage",
    [35] = "MessageQueueReceive",
    [36] = "MessageQueueSend",
    [37] = "MultixactCreation",
    [38] = "ParallelBitmapScan",
    [39] = "ParallelCreateIndexScan",
    [40] = "ParallelFinish",
    [41] = "ProcarrayGroupUpdate",
    [42] = "ProcSignalBarrier",
    [43] = "Promote",
    [44] = "RecoveryConflictSnapshot",
    [45] = "RecoveryConflictTablespace",
    [46] = "RecoveryEndCommand",
    [47] = "RecoveryPause",
    [48] = "ReplicationOriginDrop",
    [49] = "ReplicationSlotDrop",
    [50] = "RestoreCommand",
    [51] = "SafeSnapshot",
    [52] = "SyncRep",
    [53] = "WalReceiverExit",
    [54] = "WalReceiverWaitStart",
    [55] = "WalSummaryReady",
    [56] = "XactGroupUpdate",
};
#define IPC_EVENTS_MAX 56

/* ── LWLock Tranches (class 0x01) ───────────────────────────
 * Predefined LWLocks (0-53) have fixed IDs from lwlocklist.h.
 * Builtin tranches start at NUM_INDIVIDUAL_LWLOCKS and are sequential.
 * We combine both into one table with the actual tranche IDs. */
static const char *lwlock_tranches[] = {
    /* Predefined (from lwlocklist.h, some slots empty/removed) */
    [1]  = "ShmemIndex",
    [2]  = "OidGen",
    [3]  = "XidGen",
    [4]  = "ProcArray",
    [5]  = "SInvalRead",
    [6]  = "SInvalWrite",
    [7]  = "WALBufMapping",
    [8]  = "WALWrite",
    [9]  = "ControlFile",
    /* 10 = removed (was CheckpointLock) */
    /* 11 = removed (was XactSLRULock) */
    /* 12 = removed (was SubtransSLRULock) */
    [13] = "MultiXactGen",
    /* 14, 15 = removed */
    [16] = "RelCacheInit",
    [17] = "CheckpointerComm",
    [18] = "TwoPhaseState",
    [19] = "TablespaceCreate",
    [20] = "BtreeVacuum",
    [21] = "AddinShmemInit",
    [22] = "Autovacuum",
    [23] = "AutovacuumSchedule",
    [24] = "SyncScan",
    [25] = "RelationMapping",
    /* 26 = removed (was NotifySLRULock) */
    [27] = "NotifyQueue",
    [28] = "SerializableXactHash",
    [29] = "SerializableFinishedList",
    [30] = "SerializablePredicateList",
    /* 31 = removed (was SerialSLRULock) */
    [32] = "SyncRep",
    [33] = "BackgroundWorker",
    [34] = "DynamicSharedMemoryControl",
    [35] = "AutoFile",
    [36] = "ReplicationSlotAllocation",
    [37] = "ReplicationSlotControl",
    /* 38 = removed (was CommitTsSLRULock) */
    [39] = "CommitTs",
    [40] = "ReplicationOrigin",
    [41] = "MultiXactTruncation",
    /* 42 = removed (was OldSnapshotTimeMapLock) */
    [43] = "LogicalRepWorker",
    [44] = "XactTruncation",
    /* 45 = removed (was BackendRandomLock) */
    [46] = "WrapLimitsVacuum",
    [47] = "NotifyQueueTail",
    [48] = "WaitEventCustom",
    [49] = "WALSummarizer",
    [50] = "DSMRegistry",
    [51] = "InjectionPoint",
    [52] = "SerialControl",
    [53] = "AioWorkerSubmissionQueue",
    /* Builtin tranches (from wait_event_names.txt, start at NUM_INDIVIDUAL_LWLOCKS).
     * NUM_INDIVIDUAL_LWLOCKS = 54 on PG18. */
    [54] = "XactBuffer",
    [55] = "CommitTsBuffer",
    [56] = "SubtransBuffer",
    [57] = "MultiXactOffsetBuffer",
    [58] = "MultiXactMemberBuffer",
    [59] = "NotifyBuffer",
    [60] = "SerialBuffer",
    [61] = "WALInsert",
    [62] = "BufferContent",
    [63] = "ReplicationOriginState",
    [64] = "ReplicationSlotIO",
    [65] = "LockFastPath",
    [66] = "BufferMapping",
    [67] = "LockManager",
    [68] = "PredicateLockManager",
    [69] = "ParallelHashJoin",
    [70] = "ParallelBtreeScan",
    [71] = "ParallelQueryDSA",
    [72] = "PerSessionDSA",
    [73] = "PerSessionRecordType",
    [74] = "PerSessionRecordTypmod",
    [75] = "SharedTupleStore",
    [76] = "SharedTidBitmap",
    [77] = "ParallelAppend",
    [78] = "PerXactPredicateList",
    [79] = "PgStatsDSA",
    [80] = "PgStatsHash",
    [81] = "PgStatsData",
    [82] = "LogicalRepLauncherDSA",
    [83] = "LogicalRepLauncherHash",
    [84] = "DSMRegistryDSA",
    [85] = "DSMRegistryHash",
    [86] = "CommitTsSLRU",
    [87] = "MultiXactOffsetSLRU",
    [88] = "MultiXactMemberSLRU",
    [89] = "NotifySLRU",
    [90] = "SerialSLRU",
    [91] = "SubtransSLRU",
    [92] = "XactSLRU",
    [93] = "ParallelVacuumDSA",
    [94] = "AioUringCompletion",
};
#define LWLOCK_TRANCHES_MAX 94

/* ── Initialization ──────────────────────────────────────── */

void pgwt_init_event_names(int pg_major)
{
    pg_version = pg_major;
    switch (pg_major) {
    case 17:
        io_events = io_events_pg17;
        io_events_max = IO_EVENTS_PG17_MAX;
        break;
    default:
        /* PG18+ and fallback for unknown versions */
        io_events = io_events_pg18;
        io_events_max = IO_EVENTS_PG18_MAX;
        break;
    }
}

/* ── Decode Functions ─────────────────────────────────────── */

/* 0-indexed lookup */
static const char *lookup0(const char **tbl, int max, int id)
{
    if (tbl && id >= 0 && id <= max && tbl[id])
        return tbl[id];
    return NULL;
}

const char *pgwt_class_name(uint32_t wei)
{
    if (wei == 0) return "CPU";
    switch (WE_CLASS(wei)) {
    case PG_WAIT_LWLOCK:    return "LWLock";
    case PG_WAIT_LOCK:      return "Lock";
    case PG_WAIT_BUFFERPIN: return "BufferPin";
    case PG_WAIT_ACTIVITY:  return "Activity";
    case PG_WAIT_CLIENT:    return "Client";
    case PG_WAIT_EXTENSION: return "Extension";
    case PG_WAIT_IPC:       return "IPC";
    case PG_WAIT_TIMEOUT:   return "Timeout";
    case PG_WAIT_IO:        return "IO";
    default:                return "Unknown";
    }
}

const char *pgwt_event_name(uint32_t wei)
{
    if (wei == 0) return "CPU";

    int cls = WE_CLASS(wei);
    int id  = WE_EVENT(wei);
    const char *name;

    /* Try dynamic names first (loaded from PG or sidecar) */
    if (dyn_loaded && cls < 16 && id < DYN_MAX_EVENTS_PER_CLASS &&
        id <= dyn_max[cls] && dyn_names[cls][id])
        return dyn_names[cls][id];

    /* Fall back to hardcoded tables */
    switch (cls) {
    case PG_WAIT_IO:
        name = lookup0(io_events, io_events_max, id);
        return name ? name : NULL;
    case PG_WAIT_LOCK:
        name = lookup0(lock_events, LOCK_EVENTS_MAX, id);
        return name ? name : NULL;
    case PG_WAIT_TIMEOUT:
        name = lookup0(timeout_events, TIMEOUT_EVENTS_MAX, id);
        return name ? name : NULL;
    case PG_WAIT_ACTIVITY:
        name = lookup0(activity_events, ACTIVITY_EVENTS_MAX, id);
        return name ? name : NULL;
    case PG_WAIT_CLIENT:
        name = lookup0(client_events, CLIENT_EVENTS_MAX, id);
        return name ? name : NULL;
    case PG_WAIT_IPC:
        name = lookup0(ipc_events, IPC_EVENTS_MAX, id);
        return name ? name : NULL;
    case PG_WAIT_LWLOCK:
        name = lookup0(lwlock_tranches, LWLOCK_TRANCHES_MAX, id);
        return name ? name : NULL;
    case PG_WAIT_BUFFERPIN:
        return "BufferPin";
    case PG_WAIT_EXTENSION:
        return "Extension";
    default:
        return NULL;
    }
}

void pgwt_event_full_name(uint32_t wei, char *buf, size_t bufsz)
{
    if (wei == 0) {
        snprintf(buf, bufsz, "CPU*");
        return;
    }

    const char *cls = pgwt_class_name(wei);
    const char *ev = pgwt_event_name(wei);

    if (ev)
        snprintf(buf, bufsz, "%s:%s", cls, ev);
    else
        snprintf(buf, bufsz, "%s:id=%d", cls, WE_EVENT(wei));
}

int pgwt_is_idle_event(uint32_t wei)
{
    /* Only Activity-class events are idle (AutoVacuumMain, idle, etc.).
     * Client:ClientRead is NOT idle — it's a real wait event showing
     * time spent waiting for the client to send the next command.
     * Filtering it caused inconsistent behavior between zoom levels
     * and empty Client class breakdown. */
    return WE_CLASS(wei) == PG_WAIT_ACTIVITY;
}

/* ── Dynamic Name Resolution ─────────────────────────────── */

/* Map class name string to class byte (high byte of wait_event_info) */
static int class_name_to_byte(const char *name)
{
    if (strcmp(name, "LWLock") == 0)    return 0x01;
    if (strcmp(name, "Lock") == 0)      return 0x03;
    if (strcmp(name, "BufferPin") == 0 ||
        strcmp(name, "Buffer") == 0)    return 0x04;
    if (strcmp(name, "Activity") == 0)  return 0x05;
    if (strcmp(name, "Client") == 0)    return 0x06;
    if (strcmp(name, "Extension") == 0) return 0x07;
    if (strcmp(name, "IPC") == 0)       return 0x08;
    if (strcmp(name, "Timeout") == 0)   return 0x09;
    if (strcmp(name, "IO") == 0)        return 0x0A;
    if (strcmp(name, "InjectionPoint") == 0) return 0x0B;
    return -1;
}

static void dyn_clear(void)
{
    for (int c = 0; c < 16; c++) {
        for (int i = 0; i <= dyn_max[c]; i++) {
            free(dyn_names[c][i]);
            dyn_names[c][i] = NULL;
        }
        dyn_max[c] = -1;
    }
    dyn_loaded = 0;
}

static void dyn_add(int class_byte, int event_id, const char *name)
{
    if (class_byte < 0 || class_byte >= 16) return;
    if (event_id < 0 || event_id >= DYN_MAX_EVENTS_PER_CLASS) return;

    free(dyn_names[class_byte][event_id]);
    dyn_names[class_byte][event_id] = strdup(name);

    if (event_id > dyn_max[class_byte])
        dyn_max[class_byte] = event_id;
}

/* Return the hardcoded id→name table + max index for a wait-event class
 * byte. Returns 1 if the class has a known table, 0 otherwise. */
static int hardcoded_class_table(int cb, const char ***tbl, int *max)
{
    switch (cb) {
    case PG_WAIT_IO:       *tbl = io_events;       *max = io_events_max;       return 1;
    case PG_WAIT_LOCK:     *tbl = lock_events;     *max = LOCK_EVENTS_MAX;     return 1;
    case PG_WAIT_TIMEOUT:  *tbl = timeout_events;  *max = TIMEOUT_EVENTS_MAX;  return 1;
    case PG_WAIT_ACTIVITY: *tbl = activity_events; *max = ACTIVITY_EVENTS_MAX; return 1;
    case PG_WAIT_CLIENT:   *tbl = client_events;   *max = CLIENT_EVENTS_MAX;   return 1;
    case PG_WAIT_IPC:      *tbl = ipc_events;      *max = IPC_EVENTS_MAX;      return 1;
    case PG_WAIT_LWLOCK:   *tbl = lwlock_tranches; *max = LWLOCK_TRANCHES_MAX; return 1;
    default:               return 0;
    }
}

/* Reverse lookup: event name → numeric event_id within a class's
 * hardcoded table. Returns -1 if the name is not known. This is how we
 * recover the correct enum id for a dynamically-discovered name —
 * pg_wait_events exposes no id column, and its rows are NOT in enum
 * order for every class (e.g. the Lock class follows LockTagType, which
 * is not alphabetical), so positional id assignment is wrong. */
static int hardcoded_event_id(int cb, const char *name)
{
    const char **tbl;
    int max;
    if (!hardcoded_class_table(cb, &tbl, &max))
        return -1;
    for (int i = 0; i <= max; i++)
        if (tbl[i] && strcmp(tbl[i], name) == 0)
            return i;
    return -1;
}

/* Parse "Type|Name" lines from fp into the dynamic name table, assigning
 * each name its CORRECT enum id by reverse-lookup against the hardcoded
 * tables. We cannot derive the id from pg_wait_events: it has no id
 * column, and `ORDER BY name` is not enum order for every class (the
 * Lock class follows LockTagType — relation=0 … advisory=10 — which is
 * not alphabetical), so positional id assignment mislabelled Lock
 * subtypes (relation shown as advisory). Names unknown to this build (a
 * newer PG version) are appended after the class's known maximum —
 * best-effort forward-compat, since their true id is not available.
 * Caller is responsible for dyn_clear() before and dyn_loaded after.
 * Returns the number of names parsed. */
static int load_names_from_fp(FILE *fp)
{
    char line[256];
    int count = 0;
    int cur_class = -1;
    int next_unknown_id = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;

        char *sep = strchr(line, '|');
        if (!sep) continue;
        *sep = '\0';

        const char *type = line;
        const char *name = sep + 1;
        int cb = class_name_to_byte(type);
        if (cb < 0) continue;

        if (cb != cur_class) {
            const char **tbl;
            int max;
            cur_class = cb;
            next_unknown_id = hardcoded_class_table(cb, &tbl, &max) ? max + 1 : 0;
        }

        int id = hardcoded_event_id(cb, name);
        if (id < 0)
            id = next_unknown_id++;

        dyn_add(cb, id, name);
        count++;
    }
    return count;
}

int pgwt_load_event_names_from_pg(const char *pg_bindir, int pg_port,
                                  const char *pg_user)
{
    char cmd[512];

    /* pg_wait_events view exists since PG17 */
    snprintf(cmd, sizeof(cmd),
             "%s/psql -U %s -p %d -d postgres -tAF'|' "
             "-c \"SELECT type, name FROM pg_wait_events ORDER BY type, name\" 2>/dev/null",
             pg_bindir, pg_user, pg_port);

    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;

    dyn_clear();
    int count = load_names_from_fp(fp);

    int status = pclose(fp);
    if (status != 0 || count == 0) {
        dyn_clear();
        return -1;
    }

    dyn_loaded = 1;
    return 0;
}

/* Test/forward-compat entry point: load names from an in-memory buffer
 * of "Type|Name\n" lines (same format as the pg_wait_events query
 * output). Lets the id-mapping logic be unit-tested without a live PG. */
int pgwt_load_event_names_from_buffer(const char *data)
{
    if (!data)
        return -1;
    FILE *fp = fmemopen((void *)data, strlen(data), "r");
    if (!fp)
        return -1;

    dyn_clear();
    int count = load_names_from_fp(fp);
    fclose(fp);

    if (count == 0) {
        dyn_clear();
        return -1;
    }
    dyn_loaded = 1;
    return 0;
}

int pgwt_write_names_json(const char *trace_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/wait_event_names.json", trace_dir);

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    const char *class_names[] = {
        NULL, "LWLock", NULL, "Lock", "BufferPin", "Activity",
        "Client", "Extension", "IPC", "Timeout", "IO", "InjectionPoint",
        NULL, NULL, NULL, NULL
    };

    for (int c = 0; c < 16; c++) {
        if (dyn_max[c] < 0) continue;

        const char *cname = class_names[c];
        if (!cname) continue;

        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i <= dyn_max[c]; i++) {
            const char *n = dyn_names[c][i];
            cJSON_AddItemToArray(arr, cJSON_CreateString(n ? n : ""));
        }
        cJSON_AddItemToObject(root, cname, arr);
    }

    /* Also store pg_version for reference */
    cJSON_AddNumberToObject(root, "pg_version", pg_version);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(json_str);
        return -1;
    }
    fputs(json_str, fp);
    fputc('\n', fp);
    fclose(fp);
    free(json_str);
    return 0;
}

int pgwt_load_names_json(const char *trace_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/wait_event_names.json", trace_dir);

    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    /* Read entire file */
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (len <= 0 || len > 1024 * 1024) {
        fclose(fp);
        return -1;
    }

    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t nread = fread(buf, 1, len, fp);
    buf[nread] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return -1;

    dyn_clear();

    /* Load pg_version if present */
    cJSON *ver = cJSON_GetObjectItem(root, "pg_version");
    if (ver && cJSON_IsNumber(ver))
        pg_version = (int)ver->valuedouble;

    /* Iterate class arrays */
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsArray(item)) continue;

        int cb = class_name_to_byte(item->string);
        if (cb < 0) continue;

        int idx = 0;
        cJSON *elem;
        cJSON_ArrayForEach(elem, item) {
            if (cJSON_IsString(elem) && elem->valuestring[0] != '\0')
                dyn_add(cb, idx, elem->valuestring);
            idx++;
        }
    }

    cJSON_Delete(root);
    dyn_loaded = 1;
    return 0;
}
