/* wait_event.c — Wait event decode tables for PostgreSQL 18
 *
 * PG18 auto-generates enums alphabetically (case-insensitive) within each
 * class, starting from 0. Tables here must match that ordering exactly.
 */
#include "wait_event.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <string.h>

/* ── IO Events (class 0x0A, 81 events, 0-indexed, alphabetical) ── */
static const char *io_events[] = {
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
#define IO_EVENTS_MAX 80

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

/* ── Decode Functions ─────────────────────────────────────── */

/* 0-indexed lookup */
static const char *lookup0(const char **tbl, int max, int id)
{
    if (id >= 0 && id <= max && tbl[id])
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

    switch (cls) {
    case PG_WAIT_IO:
        name = lookup0(io_events, IO_EVENTS_MAX, id);
        return name ? name : "unknown_io";
    case PG_WAIT_LOCK:
        name = lookup0(lock_events, LOCK_EVENTS_MAX, id);
        return name ? name : "unknown_lock";
    case PG_WAIT_TIMEOUT:
        name = lookup0(timeout_events, TIMEOUT_EVENTS_MAX, id);
        return name ? name : "unknown_timeout";
    case PG_WAIT_ACTIVITY:
        name = lookup0(activity_events, ACTIVITY_EVENTS_MAX, id);
        return name ? name : "unknown_activity";
    case PG_WAIT_CLIENT:
        name = lookup0(client_events, CLIENT_EVENTS_MAX, id);
        return name ? name : "unknown_client";
    case PG_WAIT_IPC:
        name = lookup0(ipc_events, IPC_EVENTS_MAX, id);
        return name ? name : "unknown_ipc";
    case PG_WAIT_LWLOCK:
        name = lookup0(lwlock_tranches, LWLOCK_TRANCHES_MAX, id);
        return name ? name : "unknown_lwlock";
    case PG_WAIT_BUFFERPIN:
        return "BufferPin";
    case PG_WAIT_EXTENSION:
        return "Extension";
    default:
        return "unknown";
    }
}

void pgwt_event_full_name(uint32_t wei, char *buf, size_t bufsz)
{
    if (wei == 0) {
        snprintf(buf, bufsz, "CPU");
        return;
    }

    const char *cls = pgwt_class_name(wei);
    int id = WE_EVENT(wei);

    /* For LWLock with unknown tranche, show numeric ID */
    if (WE_CLASS(wei) == PG_WAIT_LWLOCK) {
        const char *name = lookup0(lwlock_tranches, LWLOCK_TRANCHES_MAX, id);
        if (name)
            snprintf(buf, bufsz, "%s:%s", cls, name);
        else
            snprintf(buf, bufsz, "%s:id=%d", cls, id);
        return;
    }

    snprintf(buf, bufsz, "%s:%s", cls, pgwt_event_name(wei));
}

int pgwt_is_idle_event(uint32_t wei)
{
    return WE_CLASS(wei) == PG_WAIT_ACTIVITY;
}
