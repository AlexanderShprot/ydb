#pragma once

#include "datashard.h"
#include "datashard_locks.h"
#include "datashard_trans_queue.h"
#include "datashard_outreadset.h"
#include "datashard_pipeline.h"
#include "datashard_schema_snapshots.h"
#include "datashard_snapshots.h"
#include "datashard_s3_downloads.h"
#include "datashard_s3_uploads.h"
#include "datashard_user_table.h"
#include "datashard_build_index.h"
#include "datashard_repl_offsets.h"
#include "datashard_repl_offsets_client.h"
#include "datashard_repl_offsets_server.h"
#include "change_exchange.h"
#include "change_record.h"
#include "progress_queue.h"
#include "read_iterator.h"

#include <ydb/core/tx/time_cast/time_cast.h>
#include <ydb/core/tx/tx_processing.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/tablet_pipe.h>
#include <ydb/core/base/kikimr_issue.h>
#include <ydb/core/engine/mkql_engine_flat_host.h>
#include <ydb/core/tablet/pipe_tracker.h>
#include <ydb/core/tablet/tablet_exception.h>
#include <ydb/core/tablet/tablet_pipe_client_cache.h>
#include <ydb/core/tablet/tablet_counters.h>
#include <ydb/core/tablet_flat/flat_cxx_database.h>
#include <ydb/core/tablet_flat/tablet_flat_executed.h>
#include <ydb/core/tablet_flat/tablet_flat_executor.h>
#include <ydb/core/tablet_flat/flat_page_iface.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/core/protos/tx.pb.h>
#include <ydb/core/protos/tx_datashard.pb.h>
#include <ydb/core/protos/subdomains.pb.h>
#include <ydb/core/protos/counters_datashard.pb.h>

#include <ydb/public/api/protos/ydb_status_codes.pb.h>

#include <library/cpp/actors/interconnect/interconnect.h>

#include <util/string/join.h>

namespace NKikimr {
namespace NDataShard {

extern TStringBuf SnapshotTransferReadSetMagic;

using NTabletFlatExecutor::ITransaction;
using NTabletFlatExecutor::TScanOptions;

// For CopyTable and MoveShadow
class TTxTableSnapshotContext : public NTabletFlatExecutor::TTableSnapshotContext {
public:
    TTxTableSnapshotContext(ui64 step, ui64 txId, TVector<ui32>&& tables)
        : StepOrder(step, txId)
        , Tables(tables)
    {}

    const TStepOrder& GetStepOrder() const {
        return StepOrder;
    }

    virtual TConstArrayRef<ui32> TablesToSnapshot() const override {
        return Tables;
    }

private:
    TStepOrder StepOrder;
    TVector<ui32> Tables;
};

// For Split
class TSplitSnapshotContext : public NTabletFlatExecutor::TTableSnapshotContext {
public:
    TSplitSnapshotContext(ui64 txId, TVector<ui32> &&tables,
                          TRowVersion completeEdge = TRowVersion::Min(),
                          TRowVersion incompleteEdge = TRowVersion::Min(),
                          TRowVersion lowWatermark = TRowVersion::Min())
        : TxId(txId)
        , CompleteEdge(completeEdge)
        , IncompleteEdge(incompleteEdge)
        , LowWatermark(lowWatermark)
        , Tables(tables)
    {}

    virtual TConstArrayRef<ui32> TablesToSnapshot() const override {
        return Tables;
    }

    ui64 TxId;
    TRowVersion CompleteEdge;
    TRowVersion IncompleteEdge;
    TRowVersion LowWatermark;

private:
    TVector<ui32> Tables;
};

// Base class for non-Transactional scans of DataShard data
class INoTxScan : public NTable::IScan {
public:
    virtual void OnFinished(TDataShard* self) = 0;
};

struct TReadWriteVersions {
    TReadWriteVersions(const TRowVersion& readVersion, const TRowVersion& writeVersion)
        : ReadVersion(readVersion)
        , WriteVersion(writeVersion)
    {}

    TReadWriteVersions(const TRowVersion& version)
        : ReadVersion(version)
        , WriteVersion(version)
    {}

    const TRowVersion ReadVersion;
    const TRowVersion WriteVersion;
};

enum class TSwitchState {
    READY,
    SWITCHING,
    DONE
};

class TDataShardEngineHost;
struct TSetupSysLocks;

///
class TDataShard
    : public TActor<TDataShard>
    , public NTabletFlatExecutor::TTabletExecutedFlat
{
    class TTxStopGuard;
    class TTxGetShardState;
    class TTxInit;
    class TTxInitSchema;
    class TTxInitSchemaDefaults;
    class TTxPlanStep;
    class TTxProgressResendRS;
    class TTxProgressTransaction;
    class TTxCleanupTransaction;
    class TTxProposeDataTransaction;
    class TTxProposeSchemeTransaction;
    class TTxCancelTransactionProposal;
    class TTxProposeTransactionBase;
    class TTxReadSet;
    class TTxSchemaChanged;
    class TTxInitiateBorrowedPartsReturn;
    class TTxReturnBorrowedPart;
    class TTxReturnBorrowedPartAck;
    class TTxInitSplitMergeDestination;
    class TTxSplit;
    class TTxStartSplit;
    class TTxSplitSnapshotComplete;
    class TTxSplitReplicationSourceOffsets;
    class TTxSplitTransferSnapshot;
    class TTxSplitTransferSnapshotAck;
    class TTxSplitPartitioningChanged;
    class TTxStoreTablePath;
    class TTxGoOffline;
    class TTxGetTableStats;
    class TTxMonitoring;
    class TTxMonitoringCleanupBorrowedParts;
    class TTxMonitoringCleanupBorrowedPartsActor;
    class TTxMonitoringResetSchemaVersion;
    class TTxUndelivered;
    class TTxS3Listing;
    class TTxInterruptTransaction;
    class TTxInitiateStatsUpdate;
    class TTxCheckInReadSets;
    class TTxRemoveOldInReadSets;
    class TTxRead;
    class TTxReadContinue;
    class TTxReadColumns;
    class TTxGetInfo;
    class TTxListOperations;
    class TTxGetOperation;
    class TTxStoreScanState;
    class TTxRefreshVolatileSnapshot;
    class TTxDiscardVolatileSnapshot;
    class TTxCleanupRemovedSnapshots;
    class TTxMigrateSchemeShard;
    class TTxGetS3Upload;
    class TTxStoreS3UploadId;
    class TTxChangeS3UploadStatus;
    class TTxGetS3DownloadInfo;
    class TTxStoreS3DownloadInfo;
    class TTxUnsafeUploadRows;
    class TTxExecuteMvccStateChange;
    class TTxGetRemovedRowVersions;
    class TTxCompactBorrowed;
    class TTxCompactTable;
    class TTxPersistFullCompactionTs;

    template <typename T> friend class TTxDirectBase;
    class TTxUploadRows;
    class TTxEraseRows;

    ITransaction *CreateTxMonitoring(TDataShard *self,
                                     NMon::TEvRemoteHttpInfo::TPtr ev);
    ITransaction *CreateTxGetInfo(TDataShard *self,
                                  TEvDataShard::TEvGetInfoRequest::TPtr ev);
    ITransaction *CreateTxListOperations(TDataShard *self,
                                         TEvDataShard::TEvListOperationsRequest::TPtr ev);
    ITransaction *CreateTxGetOperation(TDataShard *self,
                                       TEvDataShard::TEvGetOperationRequest::TPtr ev);

    ITransaction *CreateTxMonitoringCleanupBorrowedParts(
            TDataShard *self,
            NMon::TEvRemoteHttpInfo::TPtr ev);

    ITransaction *CreateTxMonitoringResetSchemaVersion(
            TDataShard *self,
            NMon::TEvRemoteHttpInfo::TPtr ev);

    friend class TDataShardMiniKQLFactory;
    friend class TDataTransactionProcessor;
    friend class TSchemeTransactionProcessor;
    friend class TScanTransactionProcessor;
    friend class TDataShardEngineHost;
    friend class TTxS3Listing;
    friend class TExecuteKqpScanTxUnit;
    friend class TTableScan;
    friend class TKqpScan;

    friend class TTransQueue;
    friend class TOutReadSets;
    friend class TPipeline;
    friend class TLocksDataShardAdapter<TDataShard>;
    friend class TActiveTransaction;
    friend class TValidatedDataTx;
    friend class TEngineBay;
    friend class NMiniKQL::TKqpScanComputeContext;
    friend class TSnapshotManager;
    friend class TSchemaSnapshotManager;
    friend class TReplicationSourceOffsetsClient;
    friend class TReplicationSourceOffsetsServer;

    friend class TAsyncTableStatsBuilder;
    friend class TReadTableScan;
    friend class TWaitForStreamClearanceUnit;
    friend class TBuildIndexScan;
    friend class TReadColumnsScan;
    friend class TCondEraseScan;
    friend class TDatashardKeySampler;

    friend class TS3UploadsManager;
    friend class TS3DownloadsManager;
    friend class TS3Downloader;
    friend struct TSetupSysLocks;

    friend class TTxStartMvccStateChange;
    friend class TTxExecuteMvccStateChange;

    class TFindSubDomainPathIdActor;
    class TTxPersistSubDomainPathId;
    class TTxPersistSubDomainOutOfSpace;

    class TTxRequestChangeRecords;
    class TTxRemoveChangeRecords;
    class TTxChangeExchangeHandshake;
    class TTxApplyChangeRecords;
    class TTxActivateChangeSender;
    class TTxActivateChangeSenderAck;
    class TTxChangeExchangeSplitAck;

    class TTxApplyReplicationChanges;

    struct TEvPrivate {
        enum EEv {
            EvProgressTransaction = EventSpaceBegin(TKikimrEvents::ES_PRIVATE), // WARNING: tests use ES_PRIVATE + 0
            EvCleanupTransaction,
            EvDelayedProposeTransaction, // WARNING: tests use ES_PRIVATE + 2
            EvProgressResendReadSet,
            EvFlushOperationCounters,
            EvDelayedFlushOperationCounters,
            EvProgressOperationHistogramScan,
            EvPeriodicWakeup,
            EvAsyncTableStats,
            EvRemoveOldInReadSets, // WARNING: tests use ES_PRIVATE + 9
            EvRegisterScanActor,
            EvNodeDisconnected,
            EvScanStats,
            EvPersistScanState,
            EvPersistScanStateAck,
            EvConditionalEraseRowsRegistered,
            EvAsyncJobComplete,
            EvSubDomainPathIdFound,
            EvRequestChangeRecords,
            EvRemoveChangeRecords,
            EvReplicationSourceOffsets,
            EvEnd
        };

        static_assert(EvEnd < EventSpaceEnd(TKikimrEvents::ES_PRIVATE), "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_PRIVATE)");

        struct TEvProgressTransaction : public TEventLocal<TEvProgressTransaction, EvProgressTransaction> {};
        struct TEvCleanupTransaction : public TEventLocal<TEvCleanupTransaction, EvCleanupTransaction> {};
        struct TEvDelayedProposeTransaction : public TEventLocal<TEvDelayedProposeTransaction, EvDelayedProposeTransaction> {};

        struct TEvProgressResendReadSet : public TEventLocal<TEvProgressResendReadSet, EvProgressResendReadSet> {
            TEvProgressResendReadSet(ui64 seqno)
                : Seqno(seqno)
            {}

            const ui64 Seqno;
        };

        struct TEvFlushOperationCounters : public TEventLocal<TEvFlushOperationCounters, EvFlushOperationCounters> {};
        struct TEvDelayedFlushOperationCounters : public TEventLocal<TEvDelayedFlushOperationCounters, EvDelayedFlushOperationCounters> {};

        struct TEvProgressOperationHistogramScan : public TEventLocal<TEvProgressOperationHistogramScan, EvProgressOperationHistogramScan> {};

        struct TEvPeriodicWakeup : public TEventLocal<TEvPeriodicWakeup, EvPeriodicWakeup> {};

        struct TEvAsyncTableStats : public TEventLocal<TEvAsyncTableStats, EvAsyncTableStats> {
            ui64 TableId = -1;
            ui64 IndexSize = 0;
            TInstant StatsUpdateTime;
            NTable::TStats Stats;
            THashSet<ui64> PartOwners;
            ui64 PartCount = 0;
            ui64 MemRowCount = 0;
            ui64 MemDataSize = 0;
            ui64 SearchHeight = 0;
        };

        struct TEvRemoveOldInReadSets : public TEventLocal<TEvRemoveOldInReadSets, EvRemoveOldInReadSets> {};

        struct TEvRegisterScanActor : public TEventLocal<TEvRegisterScanActor, EvRegisterScanActor> {
            TEvRegisterScanActor(ui64 txId)
                : TxId(txId)
            {
            }

            ui64 TxId;
        };

        struct TEvNodeDisconnected : public TEventLocal<TEvNodeDisconnected, EvNodeDisconnected> {
            TEvNodeDisconnected(ui32 nodeId)
                : NodeId(nodeId)
            {
            }

            ui32 NodeId;
        };

        struct TEvScanStats : public TEventLocal<TEvScanStats, EvScanStats> {
            TEvScanStats(ui64 rows, ui64 bytes) : Rows(rows), Bytes(bytes) {}
            ui64 Rows;
            ui64 Bytes;
        };

        // Also updates scan statistic, i.e. like TEvScanStats but persist state for given tx
        struct TEvPersistScanState : public TEventLocal<TEvPersistScanState, EvPersistScanState> {
            TEvPersistScanState(
                ui64 txId,
                TString lastKey,
                ui64 rows,
                ui64 bytes,
                Ydb::StatusIds::StatusCode statusCode,
                const NYql::TIssues& issues)
                : TxId(txId)
                , LastKey(lastKey)
                , Rows(rows)
                , Bytes(bytes)
                , StatusCode(statusCode)
                , Issues(issues)
            {}
            ui64 TxId;
            TString LastKey;
            ui64 Rows;
            ui64 Bytes;
            Ydb::StatusIds::StatusCode StatusCode;
            NYql::TIssues Issues;
        };

        struct TEvPersistScanStateAck : public TEventLocal<TEvPersistScanStateAck, EvPersistScanStateAck> {};

        struct TEvConditionalEraseRowsRegistered : public TEventLocal<TEvConditionalEraseRowsRegistered, EvConditionalEraseRowsRegistered> {
            explicit TEvConditionalEraseRowsRegistered(ui64 txId, const TActorId& actorId)
                : TxId(txId)
                , ActorId(actorId)
            {
            }

            const ui64 TxId;
            const TActorId ActorId;
        };

        struct TEvAsyncJobComplete : public TEventLocal<TEvAsyncJobComplete, EvAsyncJobComplete> {
            explicit TEvAsyncJobComplete(TAutoPtr<IDestructable> prod)
                : Prod(prod)
            {
            }

            TAutoPtr<IDestructable> Prod;
        };

        struct TEvSubDomainPathIdFound : public TEventLocal<TEvSubDomainPathIdFound, EvSubDomainPathIdFound> {
            TEvSubDomainPathIdFound(ui64 schemeShardId, ui64 localPathId)
                : SchemeShardId(schemeShardId)
                , LocalPathId(localPathId)
            { }

            const ui64 SchemeShardId;
            const ui64 LocalPathId;
        };

        struct TEvRequestChangeRecords : public TEventLocal<TEvRequestChangeRecords, EvRequestChangeRecords> {};
        struct TEvRemoveChangeRecords : public TEventLocal<TEvRemoveChangeRecords, EvRemoveChangeRecords> {};

        struct TEvReplicationSourceOffsets : public TEventLocal<TEvReplicationSourceOffsets, EvReplicationSourceOffsets> {
            struct TSplitKey {
                TSerializedCellVec SplitKey;
                i64 MaxOffset = -1;
            };

            TEvReplicationSourceOffsets(ui64 srcTabletId, const TPathId& pathId)
                : SrcTabletId(srcTabletId)
                , PathId(pathId)
            { }

            const ui64 SrcTabletId;
            const TPathId PathId;

            // Source -> SplitKey -> MaxOffset
            // Note that keys are NOT sorted in any way
            THashMap<TString, TVector<TSplitKey>> SourceOffsets;
        };
    };

    struct Schema : NIceDb::Schema {
        struct Sys : Table<1> {
            struct Id :             Column<1, NScheme::NTypeIds::Uint64> {};
            struct Bytes :          Column<2, NScheme::NTypeIds::String> {};
            struct Uint64 :         Column<3, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<Id>;
            using TColumns = TableColumns<Id, Bytes, Uint64>;
        };

        // Note that table UserTablesStats must be always updated with this one
        struct UserTables : Table<2> {
            struct Tid :            Column<1, NScheme::NTypeIds::Uint64> {};
            struct LocalTid :       Column<2, NScheme::NTypeIds::Uint32> {};
            struct Path :           Column<3, NScheme::NTypeIds::String> {};
            struct Name :           Column<4, NScheme::NTypeIds::String> {};
            struct Schema :         Column<5, NScheme::NTypeIds::String> { using Type = TString; };
            struct ShadowTid :      Column<6, NScheme::NTypeIds::Uint32> { static constexpr ui32 Default = 0; };

            using TKey = TableKey<Tid>;
            using TColumns = TableColumns<Tid, LocalTid, Path, Name, Schema, ShadowTid>;
        };

        struct TxMain : Table<3> {
            struct TxId :           Column<1, NScheme::NTypeIds::Uint64> {};
            struct Kind :           Column<2, NScheme::NTypeIds::Uint32> { using Type = EOperationKind; };
            struct Flags :          Column<3, NScheme::NTypeIds::Uint32> {};
            struct State :          Column<4, NScheme::NTypeIds::Uint32> {};
            struct InRSRemain :     Column<5, NScheme::NTypeIds::Uint64> {};
            struct MaxStep :        Column<6, NScheme::NTypeIds::Uint64> {};
            struct ReceivedAt :     Column<7, NScheme::NTypeIds::Uint64> {};
            struct Flags64 :        Column<8, NScheme::NTypeIds::Uint64> {};
            struct Source :         Column<9, NScheme::NTypeIds::ActorId> {};
            struct Cookie :         Column<10, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<TxId>;
            using TColumns = TableColumns<TxId, Kind, Flags, State, InRSRemain, MaxStep, ReceivedAt, Flags64, Source, Cookie>;
        };

        struct TxDetails : Table<4> {
            struct TxId :           Column<1, NScheme::NTypeIds::Uint64> {};
            struct Origin :         Column<2, NScheme::NTypeIds::Uint64> {};
            struct InReadSetState : Column<3, NScheme::NTypeIds::Uint64> {}; // Not used
            struct Body :           Column<4, NScheme::NTypeIds::String> { using Type = TString; };
            struct Source :         Column<5, NScheme::NTypeIds::ActorId> {};

            using TKey = TableKey<TxId, Origin>;
            using TColumns = TableColumns<TxId, Origin, InReadSetState, Body, Source>;
        };

        struct InReadSets : Table<5> {
            struct TxId :           Column<1, NScheme::NTypeIds::Uint64> {};
            struct Origin :         Column<2, NScheme::NTypeIds::Uint64> {};
            struct From :           Column<3, NScheme::NTypeIds::Uint64> {};
            struct To :             Column<4, NScheme::NTypeIds::Uint64> {};
            struct Body :           Column<5, NScheme::NTypeIds::String> { using Type = TString; };
            struct BalanceTrackList :      Column<6, NScheme::NTypeIds::String> { using Type = TString; };

            using TKey = TableKey<TxId, Origin, From, To>;
            using TColumns = TableColumns<TxId, Origin, From, To, Body, BalanceTrackList>;
        };

        struct OutReadSets : Table<6> {
            struct Seqno :          Column<1, NScheme::NTypeIds::Uint64> {};
            struct Step :           Column<2, NScheme::NTypeIds::Uint64> {};
            struct TxId :           Column<3, NScheme::NTypeIds::Uint64> {};
            struct Origin :         Column<4, NScheme::NTypeIds::Uint64> {};
            struct From :           Column<5, NScheme::NTypeIds::Uint64> {};
            struct To :             Column<6, NScheme::NTypeIds::Uint64> {};
            struct Body :           Column<7, NScheme::NTypeIds::String> { using Type = TString; };
            struct SplitTraj :      Column<8, NScheme::NTypeIds::String> { using Type = TString; };

            using TKey = TableKey<Seqno>;
            using TColumns = TableColumns<Seqno, Step, TxId, Origin, From, To, Body, SplitTraj>;
        };

        struct PlanQueue : Table<7> {
            struct Step :           Column<1, NScheme::NTypeIds::Uint64> {};
            struct TxId :           Column<2, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<Step, TxId>;
            using TColumns = TableColumns<Step, TxId>;
        };

        struct DeadlineQueue : Table<8> {
            struct MaxStep :        Column<1, NScheme::NTypeIds::Uint64> {};
            struct TxId :           Column<2, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<MaxStep, TxId>;
            using TColumns = TableColumns<MaxStep, TxId>;
        };

        struct SchemaOperations : Table<9> {
            struct TxId :           Column<1, NScheme::NTypeIds::Uint64> {};
            struct Operation :      Column<2, NScheme::NTypeIds::Uint32> {};
            struct Source :         Column<3, NScheme::NTypeIds::ActorId> {};
            struct SourceTablet :   Column<4, NScheme::NTypeIds::Uint64> {};
            struct MinStep :        Column<5, NScheme::NTypeIds::Uint64> {};
            struct MaxStep :        Column<6, NScheme::NTypeIds::Uint64> {};
            struct PlanStep :       Column<7, NScheme::NTypeIds::Uint64> {};
            struct ReadOnly :       Column<8, NScheme::NTypeIds::Bool> {};

            struct Success :        Column<9, NScheme::NTypeIds::Bool> {};
            struct Error :          Column<10, NScheme::NTypeIds::String> { using Type = TString; };
            struct DataSize :       Column<11, NScheme::NTypeIds::Uint64> {}; // Bytes
            struct Rows :           Column<12, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<TxId>;
            using TColumns = TableColumns<TxId, Operation, Source, SourceTablet,
                MinStep, MaxStep, PlanStep, ReadOnly, Success, Error, DataSize, Rows>;
        };

        // Here we persist snapshots metadata to preserve it across Src datashard restarts
        struct SplitSrcSnapshots : Table<10> {
            struct DstTabletId :    Column<1, NScheme::NTypeIds::Uint64> {};
            struct SnapshotMeta :   Column<2, NScheme::NTypeIds::String> { using Type = TString; };

            using TKey = TableKey<DstTabletId>;
            using TColumns = TableColumns<DstTabletId, SnapshotMeta>;
        };

        // Here we persist the fact that snapshot has ben received by Dst datashard
        struct SplitDstReceivedSnapshots : Table<11> {
            struct SrcTabletId :    Column<1, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<SrcTabletId>;
            using TColumns = TableColumns<SrcTabletId>;
        };

        // Additional tx artifacts which can be reused on tx restart.
        struct TxArtifacts : Table<12> {
            struct TxId :            Column<1, NScheme::NTypeIds::Uint64> {};
            // Specify which tx artifacts have been stored to local DB and can be
            // reused on tx replay. See TActiveTransaction::EArtifactFlags.
            struct Flags :           Column<2, NScheme::NTypeIds::Uint64> {};
            struct Locks :           Column<3, NScheme::NTypeIds::String> { using Type = TVector<TSysTables::TLocksTable::TLock>; };

            using TKey = TableKey<TxId>;
            using TColumns = TableColumns<TxId, Flags, Locks>;
        };

        struct ScanProgress : Table<13> {
            struct TxId :            Column<1, NScheme::NTypeIds::Uint64> {};
            struct LastKey :         Column<2, NScheme::NTypeIds::String> {};
            struct LastBytes :       Column<3, NScheme::NTypeIds::Uint64> {};
            struct LastStatus :      Column<4, NScheme::NTypeIds::Uint64> {};
            struct LastIssues :      Column<5, NScheme::NTypeIds::String> { using Type = TString; };

            using TKey = TableKey<TxId>;
            using TColumns = TableColumns<TxId, LastKey, LastBytes, LastStatus, LastIssues>;
        };

        struct Snapshots : Table<14> {
            struct Oid :             Column<1, NScheme::NTypeIds::Uint64> {}; // PathOwnerId
            struct Tid :             Column<2, NScheme::NTypeIds::Uint64> {}; // LocalPathId
            struct Step :            Column<3, NScheme::NTypeIds::Uint64> {};
            struct TxId :            Column<4, NScheme::NTypeIds::Uint64> {};
            struct Name :            Column<5, NScheme::NTypeIds::String> {};
            struct Flags :           Column<6, NScheme::NTypeIds::Uint64> {};
            struct TimeoutMs :       Column<7, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<Oid, Tid, Step, TxId>;
            using TColumns = TableColumns<Oid, Tid, Step, TxId, Name, Flags, TimeoutMs>;
        };

        struct S3Uploads : Table<15> {
            struct TxId :            Column<1, NScheme::NTypeIds::Uint64> {};
            struct UploadId :        Column<2, NScheme::NTypeIds::String> { using Type = TString; };
            struct Status :          Column<3, NScheme::NTypeIds::Uint8> { using Type = TS3Upload::EStatus; static constexpr Type Default = TS3Upload::EStatus::UploadParts; };
            struct Error :           Column<4, NScheme::NTypeIds::Utf8> { using Type = TString; };

            using TKey = TableKey<TxId>;
            using TColumns = TableColumns<TxId, UploadId, Status, Error>;
        };

        // deprecated
        struct S3UploadParts : Table<20> {
            struct TxId :            Column<1, NScheme::NTypeIds::Uint64> {};
            struct PartNumber :      Column<2, NScheme::NTypeIds::Uint32> {};
            struct ETag :            Column<3, NScheme::NTypeIds::String> { using Type = TString; };

            using TKey = TableKey<TxId>;
            using TColumns = TableColumns<TxId, PartNumber, ETag>;
        };

        struct S3UploadedParts : Table<23> {
            struct TxId :            Column<1, NScheme::NTypeIds::Uint64> {};
            struct PartNumber :      Column<2, NScheme::NTypeIds::Uint32> {};
            struct ETag :            Column<3, NScheme::NTypeIds::String> { using Type = TString; };

            using TKey = TableKey<TxId, PartNumber>;
            using TColumns = TableColumns<TxId, PartNumber, ETag>;
        };

        struct S3Downloads : Table<16> {
            struct TxId :            Column<1, NScheme::NTypeIds::Uint64> {};
            struct SchemeETag :      Column<2, NScheme::NTypeIds::String> { using Type = TString; };
            struct DataETag :        Column<3, NScheme::NTypeIds::String> { using Type = TString; };
            struct ProcessedBytes :  Column<4, NScheme::NTypeIds::Uint64> {};
            struct WrittenBytes :    Column<5, NScheme::NTypeIds::Uint64> {};
            struct WrittenRows :     Column<6, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<TxId>;
            using TColumns = TableColumns<TxId, SchemeETag, DataETag, ProcessedBytes, WrittenBytes, WrittenRows>;
        };

        struct ChangeRecords : Table<17> {
            struct Order :       Column<1, NScheme::NTypeIds::Uint64> {};
            struct Group :       Column<2, NScheme::NTypeIds::Uint64> {};
            struct PlanStep :    Column<3, NScheme::NTypeIds::Uint64> {};
            struct TxId :        Column<4, NScheme::NTypeIds::Uint64> {};
            struct PathOwnerId : Column<5, NScheme::NTypeIds::Uint64> {};
            struct LocalPathId : Column<6, NScheme::NTypeIds::Uint64> {};
            struct BodySize    : Column<7, NScheme::NTypeIds::Uint64> {};
            struct SchemaVersion : Column<8, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<Order>;
            using TColumns = TableColumns<Order, Group, PlanStep, TxId, PathOwnerId, LocalPathId, BodySize, SchemaVersion>;
        };

        struct ChangeRecordDetails : Table<18> {
            struct Order : Column<1, NScheme::NTypeIds::Uint64> {};
            struct Kind :  Column<2, NScheme::NTypeIds::Uint8> { using Type = TChangeRecord::EKind; };
            struct Body :  Column<3, NScheme::NTypeIds::String> { using Type = TString; };

            using TKey = TableKey<Order>;
            using TColumns = TableColumns<Order, Kind, Body>;
        };

        struct ChangeSenders : Table<19> {
            struct Origin :          Column<1, NScheme::NTypeIds::Uint64> {};
            struct Generation :      Column<2, NScheme::NTypeIds::Uint64> {};
            struct LastRecordOrder : Column<3, NScheme::NTypeIds::Uint64> {};
            struct LastSeenAt :      Column<4, NScheme::NTypeIds::Uint64> { using Type = TInstant::TValue; };

            using TKey = TableKey<Origin>;
            using TColumns = TableColumns<Origin, Generation, LastRecordOrder, LastSeenAt>;
        };

        // Table<20> was taken by S3UploadParts

        struct SrcChangeSenderActivations : Table<21> {
            struct DstTabletId : Column<1, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<DstTabletId>;
            using TColumns = TableColumns<DstTabletId>;
        };

        struct DstChangeSenderActivations : Table<22> {
            struct SrcTabletId : Column<1, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<SrcTabletId>;
            using TColumns = TableColumns<SrcTabletId>;
        };

        // Table<23> is taken by S3UploadedParts

        struct ReplicationSourceOffsets : Table<24> {
            struct PathOwnerId : Column<1, NScheme::NTypeIds::Uint64> {};
            struct TablePathId : Column<2, NScheme::NTypeIds::Uint64> {};
            struct SourceId : Column<3, NScheme::NTypeIds::Uint64> {};
            struct SplitKeyId : Column<4, NScheme::NTypeIds::Uint64> {};
            // Note: Column Source (id=5) was abandoned, but may be present in some trunk clusters
            struct SplitKey : Column<6, NScheme::NTypeIds::String> {}; // TSerializedCellVec of PK columns
            struct MaxOffset : Column<7, NScheme::NTypeIds::Int64> {};

            using TKey = TableKey<PathOwnerId, TablePathId, SourceId, SplitKeyId>;
            using TColumns = TableColumns<PathOwnerId, TablePathId, SourceId, SplitKeyId, SplitKey, MaxOffset>;
        };

        struct ReplicationSources : Table<25> {
            struct PathOwnerId : Column<1, NScheme::NTypeIds::Uint64> {};
            struct TablePathId : Column<2, NScheme::NTypeIds::Uint64> {};
            struct SourceId : Column<3, NScheme::NTypeIds::Uint64> {};
            struct SourceName : Column<4, NScheme::NTypeIds::String> {};

            using TKey = TableKey<PathOwnerId, TablePathId, SourceId>;
            using TColumns = TableColumns<PathOwnerId, TablePathId, SourceId, SourceName>;
        };

        struct DstReplicationSourceOffsetsReceived : Table<26> {
            struct SrcTabletId : Column<1, NScheme::NTypeIds::Uint64> {};
            struct PathOwnerId : Column<2, NScheme::NTypeIds::Uint64> {};
            struct TablePathId : Column<3, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<SrcTabletId, PathOwnerId, TablePathId>;
            using TColumns = TableColumns<SrcTabletId, PathOwnerId, TablePathId>;
        };

        struct UserTablesStats : Table<27> {
            struct Tid :                 Column<1, NScheme::NTypeIds::Uint64> {};

            // seconds since epoch
            struct FullCompactionTs :    Column<2, NScheme::NTypeIds::Uint64> { static constexpr ui64 Default = 0; };

            using TKey = TableKey<Tid>;
            using TColumns = TableColumns<Tid, FullCompactionTs>;
        };

        struct SchemaSnapshots : Table<28> {
            struct PathOwnerId :   Column<1, NScheme::NTypeIds::Uint64> {};
            struct LocalPathId :   Column<2, NScheme::NTypeIds::Uint64> {};
            struct SchemaVersion : Column<3, NScheme::NTypeIds::Uint64> {};
            struct Step :          Column<4, NScheme::NTypeIds::Uint64> {};
            struct TxId :          Column<5, NScheme::NTypeIds::Uint64> {};
            struct Schema :        Column<6, NScheme::NTypeIds::String> {};

            using TKey = TableKey<PathOwnerId, LocalPathId, SchemaVersion>;
            using TColumns = TableColumns<PathOwnerId, LocalPathId, SchemaVersion, Step, TxId, Schema>;
        };

        using TTables = SchemaTables<Sys, UserTables, TxMain, TxDetails, InReadSets, OutReadSets, PlanQueue,
            DeadlineQueue, SchemaOperations, SplitSrcSnapshots, SplitDstReceivedSnapshots, TxArtifacts, ScanProgress,
            Snapshots, S3Uploads, S3Downloads, ChangeRecords, ChangeRecordDetails, ChangeSenders, S3UploadedParts,
            SrcChangeSenderActivations, DstChangeSenderActivations,
            ReplicationSourceOffsets, ReplicationSources, DstReplicationSourceOffsetsReceived,
            UserTablesStats, SchemaSnapshots>;

        // These settings are persisted on each Init. So we use empty settings in order not to overwrite what
        // was changed by the user
        struct EmptySettings {
            static void Materialize(NIceDb::TToughDb&) {}
        };

        using TSettings = SchemaSettings<EmptySettings>;

        enum ESysTableKeys : ui64 {
            Sys_Config = 1,
            Sys_State,
            Sys_LastPlannedStep,
            Sys_LastPlannedTx,
            Sys_LastCompleteStep,
            Sys_LastCompleteTx,
            Sys_LastLocalTid,
            Sys_LastSeqno, // Last sequence number of out read set
            Sys_AliveStep, // Last known step we shouldn't drop at
            Sys_TxReadSizeLimit_DEPRECATED, // 10// No longer used but is present in old tables
            Sys_CurrentSchemeShardId, // TabletID of the schmemeshard that manages the datashard right now
            Sys_DstSplitDescription, // Split/Merge operation description at destination shard
            Sys_DstSplitOpId,        // TxId of split operation at destination shard
            Sys_SrcSplitDescription, // Split/Merge operation description at source shard
            Sys_SrcSplitOpId,        // TxId of split operation at source shard
            Sys_LastSchemeShardGeneration,  // LastSchemeOpSeqNo.Generation
            Sys_LastSchemeShardRound,       // LastSchemeOpSeqNo.Round
            Sys_TxReadSizeLimit, // Maximum size in bytes that is allowed to be read by a single Tx
            Sys_SubDomainInfo,  //19 Subdomain setting which owns this table
            Sys_StatisticsDisabled,
            Sys_DstSplitSchemaInitialized,
            Sys_MinWriteVersionStep, // 22 Minimum Step for new writes (past known snapshots)
            Sys_MinWriteVersionTxId, // 23 Minimum TxId for new writes (past known snapshots)
            Sys_PathOwnerId, // TabletID of the schmemeshard that allocated the TPathId(ownerId,localId)

            SysMvcc_State,
            SysMvcc_CompleteEdgeStep,
            SysMvcc_CompleteEdgeTxId,
            SysMvcc_IncompleteEdgeStep,
            SysMvcc_IncompleteEdgeTxId,
            SysMvcc_LowWatermarkStep,
            SysMvcc_LowWatermarkTxId,
            SysMvcc_KeepSnapshotTimeout,

            Sys_SubDomainOwnerId, // 33 OwnerId of the subdomain path id
            Sys_SubDomainLocalPathId, // 34 LocalPathId of the subdomain path id
            Sys_SubDomainOutOfSpace, // 35 Boolean flag indicating database is out of space

            Sys_NextChangeRecordOrder, // 36 Next order of change record
            Sys_LastChangeRecordGroup, // 37 Last group number of change records

            // reserved
            SysPipeline_Flags = 1000,
            SysPipeline_LimitActiveTx,
            SysPipeline_LimitDataTxCache,
        };

        static_assert(ESysTableKeys::Sys_SubDomainOwnerId == 33, "Sys_SubDomainOwnerId changed its value");
        static_assert(ESysTableKeys::Sys_SubDomainLocalPathId == 34, "Sys_SubDomainLocalPathId changed its value");
        static_assert(ESysTableKeys::Sys_SubDomainOutOfSpace == 35, "Sys_SubDomainOutOfSpace changed its value");

        static constexpr ui64 MinLocalTid = TSysTables::SysTableMAX + 1; // 1000

        static constexpr const char* UserTablePrefix = "__user__";
        static constexpr const char* ShadowTablePrefix = "__shadow__";
    };

    inline static bool SysGetUi64(NIceDb::TNiceDb& db, ui64 row, ui64& value) {
        auto rowset = db.Table<Schema::Sys>().Key(row).Select<Schema::Sys::Uint64>();
        if (!rowset.IsReady())
            return false;
        if (rowset.IsValid())
            value = rowset.GetValue<Schema::Sys::Uint64>();
        return true;
    }

    inline static bool SysGetUi64(NIceDb::TNiceDb& db, ui64 row, ui32& value) {
        auto rowset = db.Table<Schema::Sys>().Key(row).Select<Schema::Sys::Uint64>();
        if (!rowset.IsReady())
            return false;
        if (rowset.IsValid()) {
            ui64 val = rowset.GetValue<Schema::Sys::Uint64>();
            Y_VERIFY(val <= std::numeric_limits<ui32>::max());
            value = static_cast<ui32>(val);
        }
        return true;
    }

    inline static bool SysGetBool(NIceDb::TNiceDb& db, ui64 row, bool& value) {
        auto rowset = db.Table<Schema::Sys>().Key(row).Select<Schema::Sys::Uint64>();
        if (!rowset.IsReady())
            return false;
        if (rowset.IsValid()) {
            ui64 val = rowset.GetValue<Schema::Sys::Uint64>();
            Y_VERIFY(val <= 1, "Unexpected bool value %" PRIu64, val);
            value = (val != 0);
        }
        return true;
    }

    inline static bool SysGetBytes(NIceDb::TNiceDb& db, ui64 row, TString& value) {
        auto rowset = db.Table<Schema::Sys>().Key(row).Select<Schema::Sys::Bytes>();
        if (!rowset.IsReady())
            return false;
        if (rowset.IsValid())
            value = rowset.GetValue<Schema::Sys::Bytes>();
        return true;
    }

    template <typename TEvHandle>
    void ForwardEventToOperation(TAutoPtr<TEvHandle> ev, const TActorContext &ctx) {
        TOperation::TPtr op = Pipeline.FindOp(ev->Get()->Record.GetTxId());
        if (op)
            ForwardEventToOperation(ev, op, ctx);
    }

    template <typename TEvHandle>
    void ForwardEventToOperation(TAutoPtr<TEvHandle> ev,
                                 TOperation::TPtr op,
                                 const TActorContext &ctx) {
        op->AddInputEvent(ev.Release());
        Pipeline.AddCandidateOp(op);
        PlanQueue.Progress(ctx);
    }

    void Handle(TEvents::TEvGone::TPtr &ev);
    void Handle(TEvents::TEvPoisonPill::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvDataShard::TEvGetShardState::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvDataShard::TEvSchemaChangedResult::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvDataShard::TEvStateChangedResult::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvDataShard::TEvProposeTransaction::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvDataShard::TEvProposeTransactionAttach::TPtr &ev, const TActorContext &ctx);
    void HandleAsFollower(TEvDataShard::TEvProposeTransaction::TPtr &ev, const TActorContext &ctx);
    void ProposeTransaction(TEvDataShard::TEvProposeTransaction::TPtr &&ev, const TActorContext &ctx);
    void Handle(TEvTxProcessing::TEvPlanStep::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvTxProcessing::TEvReadSet::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvTxProcessing::TEvReadSetAck::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvPrivate::TEvProgressTransaction::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvPrivate::TEvCleanupTransaction::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvPrivate::TEvDelayedProposeTransaction::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvPrivate::TEvProgressResendReadSet::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvPrivate::TEvRemoveOldInReadSets::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvPrivate::TEvRegisterScanActor::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvPrivate::TEvScanStats::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvPrivate::TEvPersistScanState::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvTabletPipe::TEvClientConnected::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvTabletPipe::TEvClientDestroyed::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvTabletPipe::TEvServerConnected::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvTabletPipe::TEvServerDisconnected::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvMediatorTimecast::TEvRegisterTabletResult::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvMediatorTimecast::TEvNotifyPlanStep::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvCancelTransactionProposal::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvDataShard::TEvReturnBorrowedPart::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvReturnBorrowedPartAck::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvInitSplitMergeDestination::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvSplit::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvSplitTransferSnapshot::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPrivate::TEvReplicationSourceOffsets::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvSplitTransferSnapshotAck::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvSplitPartitioningChanged::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvGetTableStats::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPrivate::TEvAsyncTableStats::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvS3ListingRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvKqpScan::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvUploadRowsRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvEraseRowsRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvConditionalEraseRowsRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPrivate::TEvConditionalEraseRowsRegistered::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvRead::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvReadContinue::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvReadAck::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvReadCancel::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvReadColumnsRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvGetInfoRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvListOperationsRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvGetDataHistogramRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvGetOperationRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvGetReadTableSinkStateRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvGetReadTableScanStateRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvGetReadTableStreamStateRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvGetRSInfoRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvGetSlowOpProfilesRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvRefreshVolatileSnapshotRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvDiscardVolatileSnapshotRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvMigrateSchemeShardRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvGetS3Upload::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvStoreS3UploadId::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvChangeS3UploadStatus::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvGetS3DownloadInfo::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvStoreS3DownloadInfo::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvUnsafeUploadRowsRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvBuildIndexCreateRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPrivate::TEvAsyncJobComplete::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvDataShard::TEvCancelBackup::TPtr &ev, const TActorContext &ctx);
    void Handle(TEvDataShard::TEvCancelRestore::TPtr &ev, const TActorContext &ctx);

    void Handle(TEvTxProcessing::TEvStreamClearanceResponse::TPtr &ev, const TActorContext &ctx) {
        ForwardEventToOperation(ev, ctx);
    }
    void Handle(TEvTxProcessing::TEvStreamClearancePending::TPtr &ev, const TActorContext &ctx) {
        ForwardEventToOperation(ev, ctx);
    }
    void Handle(TEvTxProcessing::TEvInterruptTransaction::TPtr &ev, const TActorContext &ctx) {
        ForwardEventToOperation(ev, ctx);
    }
    void Handle(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr ev, const TActorContext &ctx);

    void Handle(TEvents::TEvUndelivered::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvInterconnect::TEvNodeDisconnected::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvPrivate::TEvSubDomainPathIdFound::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvTxProxySchemeCache::TEvWatchNotifyUpdated::TPtr& ev, const TActorContext& ctx);

    // change sending
    void Handle(TEvChangeExchange::TEvRequestRecords::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvChangeExchange::TEvRemoveRecords::TPtr& ev, const TActorContext& ctx);
    void ScheduleRequestChangeRecords(const TActorContext& ctx);
    void ScheduleRemoveChangeRecords(const TActorContext& ctx);
    void Handle(TEvPrivate::TEvRequestChangeRecords::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPrivate::TEvRemoveChangeRecords::TPtr& ev, const TActorContext& ctx);
    // change receiving
    void Handle(TEvChangeExchange::TEvHandshake::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvChangeExchange::TEvApplyRecords::TPtr& ev, const TActorContext& ctx);
    // activation
    void Handle(TEvChangeExchange::TEvActivateSender::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvChangeExchange::TEvActivateSenderAck::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvChangeExchange::TEvSplitAck::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvDataShard::TEvGetRemovedRowVersions::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvDataShard::TEvCompactBorrowed::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvDataShard::TEvCompactTable::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvDataShard::TEvGetCompactTableStats::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvDataShard::TEvApplyReplicationChanges::TPtr& ev, const TActorContext& ctx);

    void HandleByReplicationSourceOffsetsServer(STATEFN_SIG);

    void DoPeriodicTasks(const TActorContext &ctx);

    TDuration GetDataTxCompleteLag()
    {
        ui64 mediatorTime = MediatorTimeCastEntry ? MediatorTimeCastEntry->Get(TabletID()) : 0;
        return TDuration::MilliSeconds(Pipeline.GetDataTxCompleteLag(mediatorTime));
    }
    TDuration GetScanTxCompleteLag()
    {
        ui64 mediatorTime = MediatorTimeCastEntry ? MediatorTimeCastEntry->Get(TabletID()) : 0;
        return TDuration::MilliSeconds(Pipeline.GetScanTxCompleteLag(mediatorTime));
    }

    void UpdateLagCounters(const TActorContext &ctx);
    static NTabletPipe::TClientConfig GetPipeClientConfig();

    void OnDetach(const TActorContext &ctx) override;
    void OnTabletStop(TEvTablet::TEvTabletStop::TPtr &ev, const TActorContext &ctx) override;
    void OnStopGuardStarting(const TActorContext &ctx);
    void OnStopGuardComplete(const TActorContext &ctx);
    void OnTabletDead(TEvTablet::TEvTabletDead::TPtr &ev, const TActorContext &ctx) override;
    void OnActivateExecutor(const TActorContext &ctx) override;

    void Cleanup(const TActorContext &ctx);
    void SwitchToWork(const TActorContext &ctx);
    void SyncConfig();

    TMaybe<TInstant> GetTxPlanStartTimeAndCleanup(ui64 step);

    void RestartPipeRS(ui64 tabletId, const TActorContext& ctx);
    void AckRSToDeletedTablet(ui64 tabletId, const TActorContext& ctx);

    void DefaultSignalTabletActive(const TActorContext &ctx) override {
        // This overriden in order to pospone SignalTabletActive until TxInit completes
        Y_UNUSED(ctx);
    }

    void PersistSys(NIceDb::TNiceDb& db, ui64 key, const TString& value) const;
    void PersistSys(NIceDb::TNiceDb& db, ui64 key, ui64 value) const;
    void PersistSys(NIceDb::TNiceDb& db, ui64 key, ui32 value) const;
    void PersistSys(NIceDb::TNiceDb& db, ui64 key, bool value) const;
    void PersistUserTable(NIceDb::TNiceDb& db, ui64 tableId, const TUserTable& tableInfo);
    void PersistUserTableFullCompactionTs(NIceDb::TNiceDb& db, ui64 tableId, ui64 ts);
    void PersistMoveUserTable(NIceDb::TNiceDb& db, ui64 prevTableId, ui64 tableId, const TUserTable& tableInfo);

    void DropAllUserTables(TTransactionContext& txc);
    void PurgeTxTables(TTransactionContext& txc);

    bool CheckMediatorAuthorisation(ui64 mediatorId);

    NTabletFlatExecutor::ITransaction* CreateTxInit();
    NTabletFlatExecutor::ITransaction* CreateTxInitSchema();
    NTabletFlatExecutor::ITransaction* CreateTxInitSchemaDefaults();
    NTabletFlatExecutor::ITransaction* CreateTxSchemaChanged(TEvDataShard::TEvSchemaChangedResult::TPtr& ev);
    NTabletFlatExecutor::ITransaction* CreateTxStartSplit();
    NTabletFlatExecutor::ITransaction* CreateTxSplitSnapshotComplete(TIntrusivePtr<TSplitSnapshotContext> snapContext);
    NTabletFlatExecutor::ITransaction* CreateTxInitiateBorrowedPartsReturn();
    NTabletFlatExecutor::ITransaction* CreateTxCheckInReadSets();
    NTabletFlatExecutor::ITransaction* CreateTxRemoveOldInReadSets();
    NTabletFlatExecutor::ITransaction* CreateTxExecuteMvccStateChange();

    TReadWriteVersions GetLocalReadWriteVersions() const;

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::TX_DATASHARD_ACTOR;
    }

    TDataShard(const TActorId &tablet, TTabletStorageInfo *info);


    void PrepareAndSaveOutReadSets(ui64 step,
                                   ui64 txId,
                                   const TMap<std::pair<ui64, ui64>, TString>& outReadSets,
                                   TVector<THolder<TEvTxProcessing::TEvReadSet>> &preparedRS,
                                   TTransactionContext& txc,
                                   const TActorContext& ctx);
    THolder<TEvTxProcessing::TEvReadSet> PrepareReadSet(ui64 step, ui64 txId, ui64 source, ui64 target,
                                                        const TString& body, ui64 seqno);
    void SendReadSet(const TActorContext& ctx, ui64 step, ui64 txId, ui64 source, ui64 target, const TString& body, ui64 seqno);
    void SendReadSets(const TActorContext& ctx,
                      TVector<THolder<TEvTxProcessing::TEvReadSet>> &&readsets);
    void ResendReadSet(const TActorContext& ctx, ui64 step, ui64 txId, ui64 source, ui64 target, const TString& body, ui64 seqno);
    void SendDelayedAcks(const TActorContext& ctx, TVector<THolder<IEventHandle>>& delayedAcks) const;
    void SendResult(const TActorContext &ctx,
                    TOutputOpData::TResultPtr &result,
                    const TActorId &target,
                    ui64 step,
                    ui64 txId);
    void FillSplitTrajectory(ui64 origin, NKikimrTx::TBalanceTrackList& tracks);

    void SetCounter(NDataShard::ESimpleCounters counter, ui64 num) const {
        TabletCounters->Simple()[counter].Set(num);
    }

    void IncCounter(NDataShard::ECumulativeCounters counter, ui64 num = 1) const {
        TabletCounters->Cumulative()[counter].Increment(num);
    }

    void IncCounter(NDataShard::EPercentileCounters counter, ui64 num) const {
        TabletCounters->Percentile()[counter].IncrementFor(num);
    }

    void IncCounter(NDataShard::EPercentileCounters counter, const TDuration& latency) const {
        TabletCounters->Percentile()[counter].IncrementFor(latency.MilliSeconds());
    }

    static NDataShard::ECumulativeCounters NotEnoughMemoryCounter(ui64 count) {
        if (count == 1)
            return COUNTER_TX_NOT_ENOUGH_MEMORY_1;
        if (count == 2)
            return COUNTER_TX_NOT_ENOUGH_MEMORY_2;
        if (count == 3)
            return COUNTER_TX_NOT_ENOUGH_MEMORY_3;
        return COUNTER_TX_NOT_ENOUGH_MEMORY_4;
    }

    bool IsStateActive() const {
        return State == TShardState::Ready ||
                State == TShardState::Readonly ||
                State == TShardState::WaitScheme ||
                State == TShardState::SplitSrcWaitForNoTxInFlight ||
                State == TShardState::Frozen;
    }

    bool IsStateFrozen() const {
        return State == TShardState::Frozen;
    }


    ui32 Generation() const { return Executor()->Generation(); }
    bool IsFollower() const { return Executor()->GetStats().IsFollower; }
    bool SyncSchemeOnFollower(NTabletFlatExecutor::TTransactionContext &txc, const TActorContext &ctx,
                           NKikimrTxDataShard::TError::EKind & status, TString& errMessage);

    ui64 GetMaxTxInFly() { return MaxTxInFly; }

    static constexpr ui64 DefaultTxStepDeadline() { return 30 * 1000; }
    static constexpr ui64 PipeClientCachePoolLimit() { return 100; }

    ui64 TxInFly() const { return TransQueue.TxInFly(); }
    ui64 TxPlanned() const { return TransQueue.TxPlanned(); }
    ui64 TxPlanWaiting() const { return TransQueue.TxPlanWaiting(); }
    ui64 ImmediateInFly() const { return Pipeline.ImmediateInFly(); }
    ui64 TxWaiting() const { return Pipeline.WaitingTxs(); }

    inline TRowVersion LastCompleteTxVersion() const {
        auto order = Pipeline.GetLastCompleteTx();
        return TRowVersion(order.Step, order.TxId);
    }

    bool CanDrop() const {
        Y_VERIFY(State != TShardState::Offline, "Unexpexted repeated drop");
        return (TxInFly() == 1) && OutReadSets.Empty() && (State != TShardState::PreOffline);
    }

    void UpdateProposeQueueSize() const;

    void CheckDelayedProposeQueue(const TActorContext &ctx);

    bool CheckDataTxReject(const TString& opDescr,
                           const TActorContext &ctx,
                           NKikimrTxDataShard::TEvProposeTransactionResult::EStatus& rejectStatus,
                           TString &reason);
    bool CheckDataTxRejectAndReply(TEvDataShard::TEvProposeTransaction* msg, const TActorContext& ctx);

    TSysLocks& SysLocksTable() { return SysLocks; }

    static const TString& GetUserTablePrefix() {
        static TString prefix = Schema::UserTablePrefix;
        return prefix;
    }

    void RemoveUserTable(const TPathId& tableId) {
        TableInfos.erase(tableId.LocalPathId);
        SysLocks.RemoveSchema(tableId);
        Pipeline.GetDepTracker().RemoveSchema(tableId);
    }

    void AddUserTable(const TPathId& tableId, TUserTable::TPtr tableInfo) {
        TableInfos[tableId.LocalPathId] = tableInfo;
        SysLocks.UpdateSchema(tableId, *tableInfo);
        Pipeline.GetDepTracker().UpdateSchema(tableId, *tableInfo);
    }

    bool IsUserTable(const TTableId& tableId) const {
        return (TableInfos.find(tableId.PathId.LocalPathId) != TableInfos.end())
                && !TSysTables::IsSystemTable(tableId);
    }

    const THashMap<ui64, TUserTable::TCPtr> &GetUserTables() const { return TableInfos; }

    ui64 GetLocalTableId(const TTableId& tableId) const {
        Y_VERIFY(!TSysTables::IsSystemTable(tableId));
        auto it = TableInfos.find(tableId.PathId.LocalPathId);
        return it == TableInfos.end() ? 0 : it->second->LocalTid;
    }

    ui64 GetShadowTableId(const TTableId& tableId) const {
        Y_VERIFY(!TSysTables::IsSystemTable(tableId));
        auto it = TableInfos.find(tableId.PathId.LocalPathId);
        return it == TableInfos.end() ? 0 : it->second->ShadowTid;
    }

    ui64 GetTxReadSizeLimit() const {
        return TxReadSizeLimit ? TxReadSizeLimit : (ui64)PerShardReadSizeLimit;
    }

    ui64 GetDataTxProfileLogThresholdMs() const {
        return DataTxProfileLogThresholdMs;
    }

    ui64 GetDataTxProfileBufferThresholdMs() const {
        return DataTxProfileBufferThresholdMs;
    }

    ui64 GetDataTxProfileBufferSize() const {
        return DataTxProfileBufferSize;
    }

    ui64 GetOutdatedCleanupStep() const {
        ui64 mediatorTime = MediatorTimeCastEntry ? MediatorTimeCastEntry->Get(TabletID()) : 0;
        ui64 pipelineTime = Pipeline.OutdatedCleanupStep();
        return Max(mediatorTime, pipelineTime);
    }

    ui64 GetBackupReadAheadLoOverride() const {
        return BackupReadAheadLo;
    }

    ui64 GetBackupReadAheadHiOverride() const {
        return BackupReadAheadHi;
    }

    template <typename T>
    void ReleaseCache(T& tx) {
        ReleaseTxCache(tx.GetTxCacheUsage());
        tx.SetTxCacheUsage(0);
    }

    ui64 MakeScanSnapshot(ui32 tableId) { return Executor()->MakeScanSnapshot(tableId); }
    ui64 QueueScan(ui32 tableId, TAutoPtr<NTable::IScan> scan, ui64 cookie, const TScanOptions& options = TScanOptions())
    {
        return Executor()->QueueScan(tableId, scan, cookie, options);
    }
    void DropScanSnapshot(ui64 id) { Executor()->DropScanSnapshot(id); }
    void CancelScan(ui32 tableId, ui64 scanId) { Executor()->CancelScan(tableId, scanId); }
    TString BorrowSnapshot(ui32 tableId,
                           const TTableSnapshotContext &ctx,
                           NTable::TRawVals from,
                           NTable::TRawVals to,
                           ui64 loaner) const
    {
        return Executor()->BorrowSnapshot(tableId, ctx, from, to, loaner);
    }

    void SnapshotComplete(TIntrusivePtr<NTabletFlatExecutor::TTableSnapshotContext> snapContext, const TActorContext &ctx) override;
    void CompactionComplete(ui32 tableId, const TActorContext &ctx) override;
    void CompletedLoansChanged(const TActorContext &ctx) override;

    void ReplyCompactionWaiters(ui32 tableId, ui64 edge, const TActorContext &ctx);

    TUserTable::TSpecialUpdate SpecialUpdates(const NTable::TDatabase& db, const TTableId& tableId) const;

    void SetTableAccessTime(const TTableId& tableId, TInstant ts);
    void SetTableUpdateTime(const TTableId& tableId, TInstant ts);
    void SampleKeyAccess(const TTableId& tableId, const TArrayRef<const TCell>& row);
    NMiniKQL::IKeyAccessSampler::TPtr GetKeyAccessSampler();
    void EnableKeyAccessSampling(const TActorContext &ctx, TInstant until);
    void UpdateTableStats(const TActorContext& ctx);
    void UpdateSearchHeightStats(TUserTable::TStats& stats, ui64 newSearchHeight);
    void UpdateFullCompactionTsMetric(TUserTable::TStats& stats);
    void CollectCpuUsage(const TActorContext& ctx);

    void ScanComplete(NTable::EAbort status, TAutoPtr<IDestructable> prod, ui64 cookie, const TActorContext &ctx) override;
    bool ReassignChannelsEnabled() const override;
    ui64 GetMemoryUsage() const override;

    bool HasSharedBlobs() const;
    void CheckInitiateBorrowedPartsReturn(const TActorContext& ctx);
    void CheckStateChange(const TActorContext& ctx);
    void CheckSplitCanStart(const TActorContext& ctx);
    void CheckMvccStateChangeCanStart(const TActorContext& ctx);

    ui32 GetState() const { return State; }
    TSwitchState GetMvccSwitchState() { return MvccSwitchState; }
    void SetPersistState(ui32 state, TTransactionContext &txc)
    {
        NIceDb::TNiceDb db(txc.DB);
        PersistSys(db, Schema::Sys_State, state);

        State = state;
    }

    bool IsStopping() const { return Stopping; }

    ui64 GetPathOwnerId() const { return PathOwnerId; }
    ui64 GetCurrentSchemeShardId() const { return CurrentSchemeShardId; }

    TSchemeOpSeqNo GetLastSchemeOpSeqNo() { return LastSchemeOpSeqNo; }
    void UpdateLastSchemeOpSeqNo(const TSchemeOpSeqNo &newSeqNo,
                                 TTransactionContext &txc);
    void ResetLastSchemeOpSeqNo(TTransactionContext &txc);
    void PersistProcessingParams(const NKikimrSubDomains::TProcessingParams &params,
                                 NTabletFlatExecutor::TTransactionContext &txc);
    void PersistCurrentSchemeShardId(ui64 id,
                                    NTabletFlatExecutor::TTransactionContext &txc);
    void PersistSubDomainPathId(ui64 ownerId, ui64 localPathId,
                                NTabletFlatExecutor::TTransactionContext &txc);
    void PersistOwnerPathId(ui64 id,
                            NTabletFlatExecutor::TTransactionContext &txc);

    TDuration CleanupTimeout() const;
    void PlanCleanup(const TActorContext &ctx) {
        CleanupQueue.Schedule(ctx, CleanupTimeout());
    }

    void SendRegistrationRequestTimeCast(const TActorContext &ctx);

    const NKikimrSubDomains::TProcessingParams *GetProcessingParams() const
    {
        return ProcessingParams.get();
    }

    const TSysLocks &GetSysLocks() const { return SysLocks; }

    using TTabletExecutedFlat::TryCaptureTxCache;

    bool IsAnyChannelYellowMove() const {
        return Executor()->GetStats().IsAnyChannelYellowMove;
    }

    bool IsAnyChannelYellowStop() const {
        return Executor()->GetStats().IsAnyChannelYellowStop;
    }

    bool IsSubDomainOutOfSpace() const
    {
        return SubDomainOutOfSpace;
    }

    ui64 GetExecutorStep() const
    {
        return Executor()->Step();
    }

    TSchemaOperation *FindSchemaTx(ui64 txId) { return TransQueue.FindSchemaTx(txId); }

    TUserTable::TPtr AlterTableSchemaVersion(
        const TActorContext& ctx, TTransactionContext& txc,
        const TPathId& pathId, const ui64 tableSchemaVersion,
        bool persist = true);

    TUserTable::TPtr AlterTableAddIndex(
        const TActorContext& ctx, TTransactionContext& txc,
        const TPathId& pathId, ui64 tableSchemaVersion,
        const NKikimrSchemeOp::TIndexDescription& indexDesc);

    TUserTable::TPtr AlterTableDropIndex(
        const TActorContext& ctx, TTransactionContext& txc,
        const TPathId& pathId, ui64 tableSchemaVersion,
        const TPathId& indexPathId);

    TUserTable::TPtr AlterTableAddCdcStream(
        const TActorContext& ctx, TTransactionContext& txc,
        const TPathId& pathId, ui64 tableSchemaVersion,
        const NKikimrSchemeOp::TCdcStreamDescription& streamDesc);

    TUserTable::TPtr AlterTableDisableCdcStream(
        const TActorContext& ctx, TTransactionContext& txc,
        const TPathId& pathId, ui64 tableSchemaVersion,
        const TPathId& streamPathId);

    TUserTable::TPtr AlterTableDropCdcStream(
        const TActorContext& ctx, TTransactionContext& txc,
        const TPathId& pathId, ui64 tableSchemaVersion,
        const TPathId& streamPathId);

    TUserTable::TPtr CreateUserTable(TTransactionContext& txc, const NKikimrSchemeOp::TTableDescription& tableScheme);
    TUserTable::TPtr AlterUserTable(const TActorContext& ctx, TTransactionContext& txc,
                                    const NKikimrSchemeOp::TTableDescription& tableScheme);
    static THashMap<TPathId, TPathId> GetRemapIndexes(const NKikimrTxDataShard::TMoveTable& move);
    TUserTable::TPtr MoveUserTable(const TActorContext& ctx, TTransactionContext& txc, const NKikimrTxDataShard::TMoveTable& move);
    void DropUserTable(TTransactionContext& txc, ui64 tableId);

    ui32 GetLastLocalTid() const { return LastLocalTid; }

    ui64 AllocateChangeRecordOrder(NIceDb::TNiceDb& db);
    ui64 AllocateChangeRecordGroup(NIceDb::TNiceDb& db);
    void PersistChangeRecord(NIceDb::TNiceDb& db, const TChangeRecord& record);
    void MoveChangeRecord(NIceDb::TNiceDb& db, ui64 order, const TPathId& pathId);
    void RemoveChangeRecord(NIceDb::TNiceDb& db, ui64 order);
    void EnqueueChangeRecords(TVector<TEvChangeExchange::TEvEnqueueRecords::TRecordInfo>&& records);
    void CreateChangeSender(const TActorContext& ctx);
    void KillChangeSender(const TActorContext& ctx);
    void MaybeActivateChangeSender(const TActorContext& ctx);
    const TActorId& GetChangeSender() const { return OutChangeSender; }
    bool LoadChangeRecords(NIceDb::TNiceDb& db, TVector<TEvChangeExchange::TEvEnqueueRecords::TRecordInfo>& changeRecords);


    static void PersistSchemeTxResult(NIceDb::TNiceDb &db, const TSchemaOperation& op);
    void NotifySchemeshard(const TActorContext& ctx, ui64 txId = 0);

    TThrRefBase* GetDataShardSysTables() { return DataShardSysTables.Get(); }

    TSnapshotManager& GetSnapshotManager() { return SnapshotManager; }
    const TSnapshotManager& GetSnapshotManager() const { return SnapshotManager; }

    TSchemaSnapshotManager& GetSchemaSnapshotManager() { return SchemaSnapshotManager; }
    const TSchemaSnapshotManager& GetSchemaSnapshotManager() const { return SchemaSnapshotManager; }

    template <typename... Args>
    bool PromoteCompleteEdge(Args&&... args) {
        return SnapshotManager.PromoteCompleteEdge(std::forward<Args>(args)...);
    }

    TBuildIndexManager& GetBuildIndexManager() { return BuildIndexManager; }
    const TBuildIndexManager& GetBuildIndexManager() const { return BuildIndexManager; }

    // Returns true when datashard is working in mvcc mode
    bool IsMvccEnabled() const;

    // Returns a suitable row version for performing a transaction
    TRowVersion GetMvccTxVersion(EMvccTxMode mode, TOperation* op = nullptr) const;

    TReadWriteVersions GetReadWriteVersions(TOperation* op = nullptr) const;

    void FillExecutionStats(const TExecutionProfile& execProfile, TEvDataShard::TEvProposeTransactionResult& result) const;

    // Executes TTxProgressTransaction without specific operation
    void ExecuteProgressTx(const TActorContext& ctx);

    // Executes TTxProgressTransaction for the specific operation
    void ExecuteProgressTx(TOperation::TPtr op, const TActorContext& ctx);

    // Executes TTxCleanupTransaction
    void ExecuteCleanupTx(const TActorContext& ctx);

    void StopFindSubDomainPathId();
    void StartFindSubDomainPathId(bool delayFirstRequest = true);

    void StopWatchingSubDomainPathId();
    void StartWatchingSubDomainPathId();

    bool WaitPlanStep(ui64 step);
    bool CheckTxNeedWait(const TEvDataShard::TEvProposeTransaction::TPtr& ev) const;

    bool CheckChangesQueueOverflow() const;

private:
    ///
    class TLoanReturnTracker {
        struct TLoanReturnInfo {
            TActorId PipeToOwner;
            THashSet<TLogoBlobID> PartMeta;
        };

        ui64 MyTabletID;
        // TabletID -> non-acked loans
        THashMap<ui64, TLoanReturnInfo> LoanReturns;
        // part -> owner
        THashMap<TLogoBlobID, ui64> LoanOwners;
        NTabletPipe::TClientRetryPolicy PipeRetryPolicy;

    public:
        explicit TLoanReturnTracker(ui64 myTabletId)
            : MyTabletID(myTabletId)
            , PipeRetryPolicy{
                .RetryLimitCount = 20,
                .MinRetryTime = TDuration::MilliSeconds(10),
                .MaxRetryTime = TDuration::MilliSeconds(500),
                .BackoffMultiplier = 2}
        {}

        TLoanReturnTracker(const TLoanReturnTracker&) = delete;
        TLoanReturnTracker& operator=(const TLoanReturnTracker&) = delete;

        void Shutdown(const TActorContext& ctx) {
            for (auto& info : LoanReturns) {
                NTabletPipe::CloseClient(ctx, info.second.PipeToOwner);
            }
            LoanReturns.clear();
        }

        void ReturnLoan(ui64 ownerTabletId, const TVector<TLogoBlobID>& partMetaVec, const TActorContext& ctx) {
            TLoanReturnInfo& info = LoanReturns[ownerTabletId];

            TVector<TLogoBlobID> partsToReturn(Reserve(partMetaVec.size()));
            for (const auto& partMeta : partMetaVec) {
                auto it = LoanOwners.find(partMeta);
                if (it != LoanOwners.end()) {
                    Y_VERIFY(it->second == ownerTabletId,
                        "Part is already registered with a different owner");
                } else {
                    LoanOwners[partMeta] = ownerTabletId;
                }
                if (info.PartMeta.insert(partMeta).second) {
                    partsToReturn.emplace_back(partMeta);
                }
            }

            if (partsToReturn.empty()) {
                return;
            }

            if (!info.PipeToOwner) {
                NTabletPipe::TClientConfig clientConfig;
                clientConfig.CheckAliveness = true;
                clientConfig.RetryPolicy = PipeRetryPolicy;
                info.PipeToOwner = ctx.Register(NTabletPipe::CreateClient(ctx.SelfID, ownerTabletId, clientConfig));
            }

            THolder<TEvDataShard::TEvReturnBorrowedPart> ev = MakeHolder<TEvDataShard::TEvReturnBorrowedPart>(MyTabletID, partMetaVec);
            NTabletPipe::SendData(ctx, info.PipeToOwner, ev.Release());
        }

        void ResendLoans(ui64 ownerTabletId, const TActorContext& ctx) {
            if (!LoanReturns.contains(ownerTabletId))
                return;

            THashSet<TLogoBlobID> toResend;
            toResend.swap(LoanReturns[ownerTabletId].PartMeta);

            LoanReturns.erase(ownerTabletId);

            ReturnLoan(ownerTabletId, {toResend.begin(), toResend.end()}, ctx);
        }

        void AutoAckLoans(ui64 deadTabletId, const TActorContext& ctx) {
            if (!LoanReturns.contains(deadTabletId))
                return;

            TVector<TLogoBlobID> partMetaVec(LoanReturns[deadTabletId].PartMeta.begin(), LoanReturns[deadTabletId].PartMeta.end());

            ctx.Send(ctx.SelfID, new TEvDataShard::TEvReturnBorrowedPartAck(partMetaVec));
        }

        void LoanDone(TLogoBlobID partMeta, const TActorContext& ctx) {
            if (!LoanOwners.contains(partMeta))
                return;

            ui64 ownerTabletId = LoanOwners[partMeta];
            LoanOwners.erase(partMeta);
            LoanReturns[ownerTabletId].PartMeta.erase(partMeta);

            if (LoanReturns[ownerTabletId].PartMeta.empty()) {
                NTabletPipe::CloseClient(ctx, LoanReturns[ownerTabletId].PipeToOwner);
                LoanReturns.erase(ownerTabletId);
            }
        }

        bool Has(ui64 ownerTabletId, TActorId pipeClientActorId) const {
            return LoanReturns.contains(ownerTabletId) && LoanReturns.FindPtr(ownerTabletId)->PipeToOwner == pipeClientActorId;
        }

        bool Empty() const {
            return LoanReturns.empty();
        }
    };

    ///
    class TSplitSrcSnapshotSender {
    public:
        TSplitSrcSnapshotSender(TDataShard* self)
            : Self(self)
        { }

        void AddDst(ui64 dstTabeltId) {
            Dst.insert(dstTabeltId);
        }

        const THashSet<ui64>& GetDstSet() const {
            return Dst;
        }

        void SaveSnapshotForSending(ui64 dstTabletId, TAutoPtr<NKikimrTxDataShard::TEvSplitTransferSnapshot> snapshot) {
            Y_VERIFY(Dst.contains(dstTabletId));
            DataToSend[dstTabletId] = snapshot;
        }

        void DoSend(const TActorContext &ctx) {
            Y_VERIFY(Dst.size() == DataToSend.size());
            for (const auto& ds : DataToSend) {
                ui64 dstTablet = ds.first;
                DoSend(dstTablet, ctx);
            }
        }

        void DoSend(ui64 dstTabletId, const TActorContext &ctx) {
            Y_VERIFY(Dst.contains(dstTabletId));
            NTabletPipe::TClientConfig clientConfig;
            PipesToDstShards[dstTabletId] = ctx.Register(NTabletPipe::CreateClient(ctx.SelfID, dstTabletId, clientConfig));

            THolder<TEvDataShard::TEvSplitTransferSnapshot> ev = MakeHolder<TEvDataShard::TEvSplitTransferSnapshot>(0);
            ev->Record.CopyFrom(*DataToSend[dstTabletId]);
            ev->Record.SetSrcTabletGeneration(Self->Generation());

            auto fnCalcTotalSize = [] (const TEvDataShard::TEvSplitTransferSnapshot& ev) {
                ui64 size = 0;
                for (ui32 i = 0; i < ev.Record.TableSnapshotSize(); ++i) {
                    size += ev.Record.GetTableSnapshot(i).GetSnapshotData().size();
                }
                return size;
            };

            LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                            "Sending snapshot for split opId " << ev->Record.GetOperationCookie()
                            << " from datashard " << ev->Record.GetSrcTabletId()
                            << " to datashard " << dstTabletId << " size " << fnCalcTotalSize(*ev));

            NTabletPipe::SendData(ctx, PipesToDstShards[dstTabletId], ev.Release());
        }

        void AckSnapshot(ui64 dstTabletId, const TActorContext &ctx) {
            if (!DataToSend.contains(dstTabletId))
                return;

            NTabletPipe::CloseClient(ctx, PipesToDstShards[dstTabletId]);
            PipesToDstShards.erase(dstTabletId);
            DataToSend.erase(dstTabletId);
        }

        bool AllAcked() const {
            return DataToSend.empty();
        }

        bool Acked(ui64 dstTabletId) const {
            return !DataToSend.contains(dstTabletId);
        }

        bool Has(ui64 dstTabletId, TActorId pipeClientActorId) const {
            return PipesToDstShards.contains(dstTabletId) && *PipesToDstShards.FindPtr(dstTabletId) == pipeClientActorId;
        }

        void Shutdown(const TActorContext &ctx) {
            for (const auto& p : PipesToDstShards) {
                NTabletPipe::CloseClient(ctx, p.second);
            }
        }

    private:
        TDataShard* Self;
        THashSet<ui64> Dst;
        THashMap<ui64, TAutoPtr<NKikimrTxDataShard::TEvSplitTransferSnapshot>> DataToSend;
        THashMap<ui64, TActorId> PipesToDstShards;
    };

    ///
    class TChangeSenderActivator {
    public:
        explicit TChangeSenderActivator(ui64 selfTabletId)
            : Origin(selfTabletId)
            , PipeRetryPolicy{
                .RetryLimitCount = 20,
                .MinRetryTime = TDuration::MilliSeconds(10),
                .MaxRetryTime = TDuration::MilliSeconds(500),
                .BackoffMultiplier = 2
            }
        {
        }

        void AddDst(ui64 dstTabletId) {
            Dst.insert(dstTabletId);
        }

        const THashSet<ui64>& GetDstSet() const {
            return Dst;
        }

        void DoSend(ui64 dstTabletId, const TActorContext& ctx) {
            Y_VERIFY(Dst.contains(dstTabletId));
            NTabletPipe::TClientConfig clientConfig;
            clientConfig.CheckAliveness = true;
            clientConfig.RetryPolicy = PipeRetryPolicy;
            PipesToDstShards[dstTabletId] = ctx.Register(NTabletPipe::CreateClient(ctx.SelfID, dstTabletId, clientConfig));

            auto ev = MakeHolder<TEvChangeExchange::TEvActivateSender>();
            ev->Record.SetOrigin(Origin);

            LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Activate change sender"
                << ": origin# " << ev->Record.GetOrigin()
                << ", dst# " << dstTabletId);

            NTabletPipe::SendData(ctx, PipesToDstShards[dstTabletId], ev.Release());
        }

        bool Ack(ui64 dstTabletId, const TActorContext &ctx) {
            if (!Dst.contains(dstTabletId)) {
                return false;
            }

            NTabletPipe::CloseClient(ctx, PipesToDstShards[dstTabletId]);
            PipesToDstShards.erase(dstTabletId);
            Dst.erase(dstTabletId);

            return true;
        }

        void AutoAck(ui64 dstTabletId, const TActorContext &ctx) {
            if (!Ack(dstTabletId, ctx)) {
                return;
            }

            auto ev = MakeHolder<TEvChangeExchange::TEvActivateSenderAck>();
            ev->Record.SetOrigin(dstTabletId);
            ctx.Send(ctx.SelfID, ev.Release());
        }

        bool AllAcked() const {
            return Dst.empty();
        }

        bool Acked(ui64 dstTabletId) const {
            return !Dst.contains(dstTabletId);
        }

        bool Has(ui64 dstTabletId, TActorId pipeClientActorId) const {
            auto it = PipesToDstShards.find(dstTabletId);
            if (it == PipesToDstShards.end()) {
                return false;
            }

            return it->second == pipeClientActorId;
        }

        void Shutdown(const TActorContext &ctx) {
            for (const auto& p : PipesToDstShards) {
                NTabletPipe::CloseClient(ctx, p.second);
            }
        }

        TString Dump() const {
            return JoinSeq(", ", Dst);
        }

    private:
        const ui64 Origin;
        NTabletPipe::TClientRetryPolicy PipeRetryPolicy;

        THashSet<ui64> Dst;
        THashMap<ui64, TActorId> PipesToDstShards;
    };

    class TChangeExchangeSplitter {
    public:
        explicit TChangeExchangeSplitter(const TDataShard* self)
            : Self(self)
        {
        }

        void AddDst(ui64 dstTabletId) {
            DstTabletIds.insert(dstTabletId);
        }

        void DoSplit(const TActorContext& ctx) {
            Y_VERIFY(DstTabletIds);
            Worker = ctx.Register(CreateChangeExchangeSplit(Self, TVector<ui64>(DstTabletIds.begin(), DstTabletIds.end())));
            Acked = false;
        }

        void Ack() {
            Acked = true;
        }

        bool Done() const {
            return !DstTabletIds || Acked;
        }

        void Shutdown(const TActorContext& ctx) {
            if (Worker) {
                ctx.Send(std::exchange(Worker, TActorId()), new TEvents::TEvPoisonPill());
            }
        }

    private:
        const TDataShard* Self;

        THashSet<ui64> DstTabletIds;
        TActorId Worker;
        bool Acked = false;
    };

    // For follower only
    struct TFollowerState {
        ui64 LastSchemeUpdate = 0;
        ui64 LastSnapshotsUpdate = 0;
    };

    //

    TTabletCountersBase* TabletCounters;
    TAutoPtr<TTabletCountersBase> TabletCountersPtr;
    TAlignedPagePoolCounters AllocCounters;

    TTxProgressIdempotentScalarQueue<TEvPrivate::TEvProgressTransaction> PlanQueue;
    TTxProgressIdempotentScalarScheduleQueue<TEvPrivate::TEvCleanupTransaction> CleanupQueue;
    TTxProgressQueue<ui64, TNoOpDestroy, TEvPrivate::TEvProgressResendReadSet> ResendReadSetQueue;

    class TProposeQueue : private TTxProgressIdempotentScalarQueue<TEvPrivate::TEvDelayedProposeTransaction> {
    public:
        struct TItem : public TMoveOnly {
            TItem(TEvDataShard::TEvProposeTransaction::TPtr&& event, TInstant receivedAt, ui64 tieBreakerIndex)
                : Event(std::move(event))
                , ReceivedAt(receivedAt)
                , TieBreakerIndex(tieBreakerIndex)
                , Next(nullptr)
                , Cancelled(false)
            { }

            TEvDataShard::TEvProposeTransaction::TPtr Event;
            TInstant ReceivedAt;
            ui64 TieBreakerIndex;
            TItem* Next;
            bool Cancelled;
        };

        struct TItemList {
            TItem* First = nullptr;
            TItem* Last = nullptr;
        };

        void Enqueue(TEvDataShard::TEvProposeTransaction::TPtr event, TInstant receivedAt, ui64 tieBreakerIndex, const TActorContext& ctx) {
            TItem* item = &Items.emplace_back(std::move(event), receivedAt, tieBreakerIndex);

            const ui64 txId = item->Event->Get()->GetTxId();

            auto& links = TxIds[txId];
            if (Y_UNLIKELY(links.Last)) {
                links.Last->Next = item;
            } else {
                links.First = item;
            }
            links.Last = item;

            Progress(ctx);
        }

        TItem Dequeue() {
            TItem* first = &Items.front();
            const ui64 txId = first->Event->Get()->GetTxId();

            auto it = TxIds.find(txId);
            Y_VERIFY(it != TxIds.end() && it->second.First == first,
                "Consistency check: proposed txId %" PRIu64 " in deque, but not in hashmap", txId);

            // N.B. there should almost always be exactly one propose per txId
            it->second.First = first->Next;
            if (Y_LIKELY(it->second.First == nullptr)) {
                TxIds.erase(it);
            } else {
                first->Next = nullptr;
            }

            TItem item = std::move(*first);
            Items.pop_front();
            return item;
        }

        void Cancel(ui64 txId) {
            auto it = TxIds.find(txId);
            if (it != TxIds.end()) {
                auto* item = it->second.First;
                while (item) {
                    item->Cancelled = true;
                    item = item->Next;
                }
            }
        }

        void Ack(const TActorContext& ctx) {
            Reset(ctx);
            if (Items) {
                Progress(ctx);
            }
        }

        explicit operator bool() const {
            return bool(Items);
        }

        size_t Size() const {
            return Items.size();
        }

    private:
        TDeque<TItem> Items;
        THashMap<ui64, TItemList> TxIds;
    };

    TProposeQueue ProposeQueue;
    TVector<THolder<TEvDataShard::TEvProposeTransaction::THandle>> DelayedProposeQueue;

    TIntrusivePtr<NTabletPipe::TBoundedClientCacheConfig> PipeClientCacheConfig;
    THolder<NTabletPipe::IClientCache> PipeClientCache;
    TPipeTracker ResendReadSetPipeTracker;
    NTabletPipe::TClientRetryPolicy SchemeShardPipeRetryPolicy;
    TActorId SchemeShardPipe;   // For notifications about schema changes
    TActorId StateReportPipe;   // For notifications about shard state changes
    ui64 PathOwnerId; // TabletID of the schmemeshard that allocated the TPathId(ownerId,localId)
    ui64 CurrentSchemeShardId; // TabletID of SchemeShard wich manages the path right now
    ui64 LastKnownMediator;
    bool RegistrationSended;
    std::unique_ptr<NKikimrSubDomains::TProcessingParams> ProcessingParams;
    TSchemeOpSeqNo LastSchemeOpSeqNo;
    TInstant LastDbStatsUpdateTime;
    TInstant LastDbStatsReportTime;
    TInstant LastCpuWarnTime;
    TInstant LastDataSizeWarnTime;
    TActorId DbStatsReportPipe;
    TActorId TableResolvePipe;
    ui64 StatsReportRound = 0;

    TActorId FindSubDomainPathIdActor;

    std::optional<TPathId> SubDomainPathId;
    std::optional<TPathId> WatchingSubDomainPathId;
    bool SubDomainOutOfSpace = false;

    THashSet<TActorId> Actors;
    TLoanReturnTracker LoanReturnTracker;
    TFollowerState FollowerState;

    TSwitchState MvccSwitchState;
    bool SplitSnapshotStarted;      // Non-persistent flag that is used to restart snapshot in case of datashard restart
    TSplitSrcSnapshotSender SplitSrcSnapshotSender;
    // TODO: make this persitent
    THashSet<ui64> ReceiveSnapshotsFrom;
    ui64 DstSplitOpId;
    ui64 SrcSplitOpId;
    bool DstSplitSchemaInitialized = false;
    std::shared_ptr<NKikimrTxDataShard::TSplitMergeDescription> DstSplitDescription;
    std::shared_ptr<NKikimrTxDataShard::TSplitMergeDescription> SrcSplitDescription;
    THashSet<TActorId> SrcAckSplitTo;
    THashMap<TActorId, THashSet<ui64>> SrcAckPartitioningChangedTo;
    const ui32 SysTablesToTransferAtSplit[4] = {
            Schema::TxMain::TableId,
            Schema::TxDetails::TableId,
            // Schema::InReadSets::TableId, // need to fix InReadSets cleanup
            Schema::PlanQueue::TableId,
            Schema::DeadlineQueue::TableId
        };
    THashSet<ui64> SysTablesPartOnwers;

    // Sys table contents
    ui32 State;
    ui32 LastLocalTid;
    ui64 LastSeqno;
    ui64 NextChangeRecordOrder;
    ui64 LastChangeRecordGroup;
    ui64 TxReadSizeLimit;
    ui64 StatisticsDisabled;
    bool Stopping = false;

    NMiniKQL::IKeyAccessSampler::TPtr DisabledKeySampler;
    NMiniKQL::IKeyAccessSampler::TPtr EnabledKeySampler;
    NMiniKQL::IKeyAccessSampler::TPtr CurrentKeySampler; // Points to enbaled or disabled
    TInstant StartedKeyAccessSamplingAt;
    TInstant StopKeyAccessSamplingAt;

    THashMap<ui64, TUserTable::TCPtr> TableInfos; // tableId -> local table info
    TTransQueue TransQueue;
    TOutReadSets OutReadSets;
    TPipeline Pipeline;
    TSysLocks SysLocks;

    TSnapshotManager SnapshotManager;
    TSchemaSnapshotManager SchemaSnapshotManager;

    TReplicationSourceOffsetsServerLink ReplicationSourceOffsetsServer;

    TBuildIndexManager BuildIndexManager;

    TS3UploadsManager S3Uploads;
    TS3DownloadsManager S3Downloads;

    TIntrusivePtr<TMediatorTimecastEntry> MediatorTimeCastEntry;
    TSet<ui64> MediatorTimeCastWaitingSteps;

    TControlWrapper DisableByKeyFilter;
    TControlWrapper MaxTxInFly;
    TControlWrapper MaxTxLagMilliseconds;
    TControlWrapper CanCancelROWithReadSets;
    TControlWrapper PerShardReadSizeLimit;
    TControlWrapper CpuUsageReportThreshlodPercent;
    TControlWrapper CpuUsageReportIntervalSeconds;
    TControlWrapper HighDataSizeReportThreshlodBytes;
    TControlWrapper HighDataSizeReportIntervalSeconds;

    TControlWrapper DataTxProfileLogThresholdMs;
    TControlWrapper DataTxProfileBufferThresholdMs;
    TControlWrapper DataTxProfileBufferSize;

    TControlWrapper ReadColumnsScanEnabled;
    TControlWrapper ReadColumnsScanInUserPool;

    TControlWrapper BackupReadAheadLo;
    TControlWrapper BackupReadAheadHi;

    // Set of InRS keys to remove from local DB.
    THashSet<TReadSetKey> InRSToRemove;
    TIntrusivePtr<TThrRefBase> DataShardSysTables;

    // Simple volatile counter
    ui64 NextTieBreakerIndex = 1;

    struct TInFlightCondErase {
        ui64 TxId;
        ui64 ScanId;
        TActorId ActorId;
        ui32 Condition;

        TInFlightCondErase() {
            Clear();
        }

        void Clear() {
            TxId = 0;
            ScanId = 0;
            ActorId = TActorId();
            Condition = 0;
        }

        void Enqueue(ui64 txId, ui64 scanId, ui32 condition) {
            TxId = txId;
            ScanId = scanId;
            Condition = condition;
        }

        explicit operator bool() const {
            return bool(ScanId);
        }

        bool IsActive() const {
            return bool(ActorId);
        }
    };

    TInFlightCondErase InFlightCondErase;

    /// change sending & receiving
    struct TInChangeSender {
        ui64 Generation;
        ui64 LastRecordOrder;

        explicit TInChangeSender(ui64 generation, ui64 lastRecordOrder = 0)
            : Generation(generation)
            , LastRecordOrder(lastRecordOrder)
        {
        }
    };

    using TRequestedRecord = TEvChangeExchange::TEvRequestRecords::TRecordInfo;

    // split/merge
    TChangeSenderActivator ChangeSenderActivator;
    TChangeExchangeSplitter ChangeExchangeSplitter;
    THashSet<ui64> ReceiveActivationsFrom;

    // out
    THashMap<TActorId, TSet<TRequestedRecord>> ChangeRecordsRequested;
    TSet<ui64> ChangeRecordsToRemove; // ui64 is order
    bool RequestChangeRecordsInFly = false;
    bool RemoveChangeRecordsInFly = false;
    THashMap<ui64, ui64> ChangesQueue; // order to size
    ui64 ChangesQueueBytes = 0;
    TActorId OutChangeSender;

    // in
    THashMap<ui64, TInChangeSender> InChangeSenders; // ui64 is shard id

    // compactionId, tableId/ownerId, actorId
    using TCompactionWaiter = std::tuple<ui64, TPathId, TActorId>;
    using TCompactionWaiterList = TList<TCompactionWaiter>;

    // tableLocalTid -> waiters, note that compactionId is monotonically
    // increasing and compactions for same table finish in order:
    // thus we always add waiters to the end of the list and remove
    // from the front
    THashMap<ui32, TCompactionWaiterList> CompactionWaiters;

    struct TReplicationSourceOffsetsReceiveState {
        // A set of tables for which we already received offsets
        THashSet<TPathId> Received;
        // A set of tables for which we are waiting source offsets data
        THashSet<TPathId> Pending;
        // The latest pending transfer snapshot event
        TEvDataShard::TEvSplitTransferSnapshot::TPtr Snapshot;
    };

    THashMap<ui64, TReplicationSourceOffsetsReceiveState> ReceiveReplicationSourceOffsetsFrom;
    THashMap<ui64, TSerializedTableRange> SrcTabletToRange;

    friend class TReplicationSourceOffsetsDb;
    friend class TReplicationSourceState;
    friend class TReplicatedTableState;
    THashMap<TPathId, TReplicatedTableState> ReplicatedTables;
    TReplicatedTableState* FindReplicatedTable(const TPathId& pathId);
    TReplicatedTableState* EnsureReplicatedTable(const TPathId& pathId);

    TReadIteratorsMap ReadIterators;

protected:
    // Redundant init state required by flat executor implementation
    void StateInit(TAutoPtr<NActors::IEventHandle> &ev, const NActors::TActorContext &ctx) {
        TRACE_EVENT(NKikimrServices::TX_DATASHARD);
        switch (ev->GetTypeRewrite()) {
            HFuncTraced(TEvents::TEvPoisonPill, Handle);
        default:
            StateInitImpl(ev, ctx);
        }
    }

    void Enqueue(STFUNC_SIG) override {
        LOG_WARN_S(ctx, NKikimrServices::TX_DATASHARD, "TDataShard::StateInit unhandled event type: " << ev->GetTypeRewrite()
                           << " event: " << (ev->HasEvent() ? ev->GetBase()->ToString().data() : "serialized?"));
    }

    // In this state we are not handling external pipes to datashard tablet (it's just another init phase)
    void StateInactive(TAutoPtr<NActors::IEventHandle> &ev, const NActors::TActorContext &ctx) {
        TRACE_EVENT(NKikimrServices::TX_DATASHARD);
        switch (ev->GetTypeRewrite()) {
            HFuncTraced(TEvMediatorTimecast::TEvRegisterTabletResult, Handle);
            HFuncTraced(TEvents::TEvPoisonPill, Handle);
        default:
            if (!HandleDefaultEvents(ev, ctx)) {
                LOG_WARN_S(ctx, NKikimrServices::TX_DATASHARD, "TDataShard::StateInactive unhandled event type: " << ev->GetTypeRewrite()
                           << " event: " << (ev->HasEvent() ? ev->GetBase()->ToString().data() : "serialized?"));
            }
            break;
        }
    }

    // This is the main state
    void StateWork(TAutoPtr<NActors::IEventHandle> &ev, const NActors::TActorContext &ctx) {
        TRACE_EVENT(NKikimrServices::TX_DATASHARD);
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvents::TEvGone, Handle);
            HFuncTraced(TEvents::TEvPoisonPill, Handle);
            HFuncTraced(TEvDataShard::TEvGetShardState, Handle);
            HFuncTraced(TEvDataShard::TEvSchemaChangedResult, Handle);
            HFuncTraced(TEvDataShard::TEvStateChangedResult, Handle);
            HFuncTraced(TEvDataShard::TEvProposeTransaction, Handle);
            HFuncTraced(TEvDataShard::TEvProposeTransactionAttach, Handle);
            HFuncTraced(TEvDataShard::TEvCancelBackup, Handle);
            HFuncTraced(TEvDataShard::TEvCancelRestore, Handle);
            HFuncTraced(TEvDataShard::TEvGetS3Upload, Handle);
            HFuncTraced(TEvDataShard::TEvStoreS3UploadId, Handle);
            HFuncTraced(TEvDataShard::TEvChangeS3UploadStatus, Handle);
            HFuncTraced(TEvDataShard::TEvGetS3DownloadInfo, Handle);
            HFuncTraced(TEvDataShard::TEvStoreS3DownloadInfo, Handle);
            HFuncTraced(TEvDataShard::TEvUnsafeUploadRowsRequest, Handle);
            HFuncTraced(TEvDataShard::TEvMigrateSchemeShardRequest, Handle);
            HFuncTraced(TEvTxProcessing::TEvPlanStep, Handle);
            HFuncTraced(TEvTxProcessing::TEvReadSet, Handle);
            HFuncTraced(TEvTxProcessing::TEvReadSetAck, Handle);
            HFuncTraced(TEvTxProcessing::TEvStreamClearanceResponse, Handle);
            HFuncTraced(TEvTxProcessing::TEvStreamClearancePending, Handle);
            HFuncTraced(TEvTxProcessing::TEvInterruptTransaction, Handle);
            HFuncTraced(TEvPrivate::TEvProgressTransaction, Handle);
            HFuncTraced(TEvPrivate::TEvCleanupTransaction, Handle);
            HFuncTraced(TEvPrivate::TEvDelayedProposeTransaction, Handle);
            HFuncTraced(TEvPrivate::TEvProgressResendReadSet, Handle);
            HFuncTraced(TEvPrivate::TEvRemoveOldInReadSets, Handle);
            HFuncTraced(TEvPrivate::TEvRegisterScanActor, Handle);
            HFuncTraced(TEvPrivate::TEvScanStats, Handle);
            HFuncTraced(TEvPrivate::TEvPersistScanState, Handle);
            HFuncTraced(TEvTabletPipe::TEvClientConnected, Handle);
            HFuncTraced(TEvTabletPipe::TEvClientDestroyed, Handle);
            HFuncTraced(TEvTabletPipe::TEvServerConnected, Handle);
            HFuncTraced(TEvTabletPipe::TEvServerDisconnected, Handle);
            HFuncTraced(TEvMediatorTimecast::TEvRegisterTabletResult, Handle);
            HFuncTraced(TEvMediatorTimecast::TEvNotifyPlanStep, Handle);
            HFuncTraced(TEvDataShard::TEvCancelTransactionProposal, Handle);
            HFuncTraced(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult, Handle);
            HFunc(TEvDataShard::TEvReturnBorrowedPart, Handle);
            HFunc(TEvDataShard::TEvReturnBorrowedPartAck, Handle);
            HFunc(TEvDataShard::TEvInitSplitMergeDestination, Handle);
            HFunc(TEvDataShard::TEvSplit, Handle);
            HFunc(TEvDataShard::TEvSplitTransferSnapshot, Handle);
            HFunc(TEvPrivate::TEvReplicationSourceOffsets, Handle);
            HFunc(TEvDataShard::TEvSplitTransferSnapshotAck, Handle);
            HFunc(TEvDataShard::TEvSplitPartitioningChanged, Handle);
            HFunc(TEvDataShard::TEvGetTableStats, Handle);
            HFunc(TEvPrivate::TEvAsyncTableStats, Handle);
            HFunc(TEvDataShard::TEvS3ListingRequest, Handle);
            HFunc(TEvDataShard::TEvKqpScan, Handle);
            HFunc(TEvDataShard::TEvUploadRowsRequest, Handle);
            HFunc(TEvDataShard::TEvEraseRowsRequest, Handle);
            HFunc(TEvDataShard::TEvConditionalEraseRowsRequest, Handle);
            HFunc(TEvPrivate::TEvConditionalEraseRowsRegistered, Handle);
            HFunc(TEvDataShard::TEvRead, Handle);
            HFunc(TEvDataShard::TEvReadContinue, Handle);
            HFunc(TEvDataShard::TEvReadAck, Handle);
            HFunc(TEvDataShard::TEvReadCancel, Handle);
            HFunc(TEvDataShard::TEvReadColumnsRequest, Handle);
            HFunc(TEvDataShard::TEvGetInfoRequest, Handle);
            HFunc(TEvDataShard::TEvListOperationsRequest, Handle);
            HFunc(TEvDataShard::TEvGetDataHistogramRequest, Handle);
            HFunc(TEvDataShard::TEvGetOperationRequest, Handle);
            HFunc(TEvDataShard::TEvGetReadTableSinkStateRequest, Handle);
            HFunc(TEvDataShard::TEvGetReadTableScanStateRequest, Handle);
            HFunc(TEvDataShard::TEvGetReadTableStreamStateRequest, Handle);
            HFunc(TEvDataShard::TEvGetRSInfoRequest, Handle);
            HFunc(TEvDataShard::TEvGetSlowOpProfilesRequest, Handle);
            HFunc(TEvDataShard::TEvRefreshVolatileSnapshotRequest, Handle);
            HFunc(TEvDataShard::TEvDiscardVolatileSnapshotRequest, Handle);
            HFuncTraced(TEvDataShard::TEvBuildIndexCreateRequest, Handle);
            HFunc(TEvPrivate::TEvAsyncJobComplete, Handle);
            CFunc(TEvPrivate::EvPeriodicWakeup, DoPeriodicTasks);
            HFunc(TEvents::TEvUndelivered, Handle);
            IgnoreFunc(TEvInterconnect::TEvNodeConnected);
            HFunc(TEvInterconnect::TEvNodeDisconnected, Handle);
            HFunc(TEvPrivate::TEvSubDomainPathIdFound, Handle);
            HFunc(TEvTxProxySchemeCache::TEvWatchNotifyUpdated, Handle);
            IgnoreFunc(TEvTxProxySchemeCache::TEvWatchNotifyDeleted);
            IgnoreFunc(TEvTxProxySchemeCache::TEvWatchNotifyUnavailable);
            HFunc(TEvChangeExchange::TEvRequestRecords, Handle);
            HFunc(TEvChangeExchange::TEvRemoveRecords, Handle);
            HFunc(TEvPrivate::TEvRequestChangeRecords, Handle);
            HFunc(TEvPrivate::TEvRemoveChangeRecords, Handle);
            HFunc(TEvChangeExchange::TEvHandshake, Handle);
            HFunc(TEvChangeExchange::TEvApplyRecords, Handle);
            HFunc(TEvChangeExchange::TEvActivateSender, Handle);
            HFunc(TEvChangeExchange::TEvActivateSenderAck, Handle);
            HFunc(TEvChangeExchange::TEvSplitAck, Handle);
            HFunc(TEvDataShard::TEvGetRemovedRowVersions, Handle);
            HFunc(TEvDataShard::TEvCompactBorrowed, Handle);
            HFunc(TEvDataShard::TEvCompactTable, Handle);
            HFunc(TEvDataShard::TEvGetCompactTableStats, Handle);
            HFunc(TEvDataShard::TEvApplyReplicationChanges, Handle);
            fFunc(TEvDataShard::EvGetReplicationSourceOffsets, HandleByReplicationSourceOffsetsServer);
            fFunc(TEvDataShard::EvReplicationSourceOffsetsAck, HandleByReplicationSourceOffsetsServer);
            fFunc(TEvDataShard::EvReplicationSourceOffsetsCancel, HandleByReplicationSourceOffsetsServer);
        default:
            if (!HandleDefaultEvents(ev, ctx)) {
                LOG_WARN_S(ctx, NKikimrServices::TX_DATASHARD,
                           "TDataShard::StateWork unhandled event type: "<< ev->GetTypeRewrite()
                           << " event: " << (ev->HasEvent() ? ev->GetBase()->ToString().data() : "serialized?"));
            }
            break;
        }
    }

    // This is the main state
    void StateWorkAsFollower(TAutoPtr<NActors::IEventHandle> &ev, const NActors::TActorContext &ctx) {
        TRACE_EVENT(NKikimrServices::TX_DATASHARD);
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvents::TEvGone, Handle);
            HFuncTraced(TEvents::TEvPoisonPill, Handle);
            HFuncTraced(TEvDataShard::TEvProposeTransaction, HandleAsFollower);
            HFuncTraced(TEvPrivate::TEvDelayedProposeTransaction, Handle);
            HFuncTraced(TEvDataShard::TEvReadColumnsRequest, Handle);
            HFuncTraced(TEvTabletPipe::TEvServerConnected, Handle);
            HFuncTraced(TEvTabletPipe::TEvServerDisconnected, Handle);
        default:
            if (!HandleDefaultEvents(ev, ctx)) {
                LOG_WARN_S(ctx, NKikimrServices::TX_DATASHARD, "TDataShard::StateWorkAsFollower unhandled event type: " << ev->GetTypeRewrite()
                           << " event: " << (ev->HasEvent() ? ev->GetBase()->ToString().data() : "serialized?"));
            }
            break;
        }
    }

    // State after tablet takes poison pill
    void StateBroken(TAutoPtr<NActors::IEventHandle> &ev, const NActors::TActorContext &ctx) {
        TRACE_EVENT(NKikimrServices::TX_DATASHARD);
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvents::TEvGone, Handle);
            HFuncTraced(TEvTablet::TEvTabletDead, HandleTabletDead);
        default:
            LOG_WARN_S(ctx, NKikimrServices::TX_DATASHARD, "TDataShard::BrokenState at tablet " << TabletID()
                       << " unhandled event type: " << ev->GetTypeRewrite()
                       << " event: " << (ev->HasEvent() ? ev->GetBase()->ToString().data() : "serialized?"));
            ctx.Send(ev->ForwardOnNondelivery(TEvents::TEvUndelivered::ReasonActorUnknown));
            break;
        }
    }

    void Die(const TActorContext &ctx) override {
        NTabletPipe::CloseAndForgetClient(SelfId(), SchemeShardPipe);
        NTabletPipe::CloseAndForgetClient(SelfId(), StateReportPipe);
        NTabletPipe::CloseAndForgetClient(SelfId(), DbStatsReportPipe);
        NTabletPipe::CloseAndForgetClient(SelfId(), TableResolvePipe);

        if (ReplicationSourceOffsetsServer) {
            InvokeOtherActor(*ReplicationSourceOffsetsServer, &TReplicationSourceOffsetsServer::PassAway);
        }

        for (const TActorId& actorId : Actors) {
            Send(actorId, new TEvents::TEvPoison);
        }
        Actors.clear();

        KillChangeSender(ctx);
        ChangeSenderActivator.Shutdown(ctx);
        ChangeExchangeSplitter.Shutdown(ctx);

        StopFindSubDomainPathId();
        StopWatchingSubDomainPathId();

        LoanReturnTracker.Shutdown(ctx);
        Y_VERIFY(LoanReturnTracker.Empty());
        SplitSrcSnapshotSender.Shutdown(ctx);
        return IActor::Die(ctx);
    }

    void BecomeBroken(const TActorContext &ctx)
    {
        Become(&TThis::StateBroken);
        ctx.Send(Tablet(), new TEvents::TEvPoisonPill);
    }

    void SendViaSchemeshardPipe(const TActorContext &ctx, ui64 tabletId, THolder<TEvDataShard::TEvSchemaChanged> event) {
        Y_VERIFY(tabletId);
        Y_VERIFY(CurrentSchemeShardId == tabletId);

        if (!SchemeShardPipe) {
            NTabletPipe::TClientConfig clientConfig;
            SchemeShardPipe = ctx.Register(NTabletPipe::CreateClient(ctx.SelfID, tabletId, clientConfig));
        }
        NTabletPipe::SendData(ctx, SchemeShardPipe, event.Release());
    }

    void ReportState(const TActorContext &ctx, ui32 state) {
        LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, TabletID() << " Reporting state " << DatashardStateName(State)
                    << " to schemeshard " << CurrentSchemeShardId);
        Y_VERIFY(state != TShardState::Offline || !HasSharedBlobs(),
                 "Datashard %" PRIu64 " tried to go offline while having shared blobs", TabletID());
        if (!StateReportPipe) {
            NTabletPipe::TClientConfig clientConfig;
            clientConfig.RetryPolicy = SchemeShardPipeRetryPolicy;
            StateReportPipe = ctx.Register(NTabletPipe::CreateClient(ctx.SelfID, CurrentSchemeShardId, clientConfig));
        }
        THolder<TEvDataShard::TEvStateChanged> ev(new TEvDataShard::TEvStateChanged(ctx.SelfID, TabletID(), state));
        NTabletPipe::SendData(ctx, StateReportPipe, ev.Release());
    }

    void SendPeriodicTableStats(const TActorContext &ctx) {
        if (StatisticsDisabled)
            return;

        TInstant now = AppData(ctx)->TimeProvider->Now();

        if (LastDbStatsReportTime + gDbStatsReportInterval > now)
            return;

        auto* resourceMetrics = Executor()->GetResourceMetrics();

        for (const auto& t : TableInfos) {
            ui64 tableId = t.first;

            const TUserTable &ti = *t.second;

            // Don't report stats until they are build for the first time
            if (!ti.Stats.StatsUpdateTime)
                break;

            if (!DbStatsReportPipe) {
                NTabletPipe::TClientConfig clientConfig;
                DbStatsReportPipe = ctx.Register(NTabletPipe::CreateClient(ctx.SelfID, CurrentSchemeShardId, clientConfig));
            }

            THolder<TEvDataShard::TEvPeriodicTableStats> ev(new TEvDataShard::TEvPeriodicTableStats(TabletID(), PathOwnerId, tableId));
            ev->Record.SetShardState(State);
            ev->Record.SetGeneration(Executor()->Generation());
            ev->Record.SetRound(StatsReportRound++);
            ev->Record.MutableTableStats()->SetRowCount(ti.Stats.DataStats.RowCount + ti.Stats.MemRowCount);
            ev->Record.MutableTableStats()->SetDataSize(ti.Stats.DataStats.DataSize + ti.Stats.MemDataSize);
            ev->Record.MutableTableStats()->SetIndexSize(ti.Stats.IndexSize);
            ev->Record.MutableTableStats()->SetLastAccessTime(ti.Stats.AccessTime.MilliSeconds());
            ev->Record.MutableTableStats()->SetLastUpdateTime(ti.Stats.UpdateTime.MilliSeconds());

            ev->Record.MutableTableStats()->SetImmediateTxCompleted(TabletCounters->Cumulative()[COUNTER_PREPARE_IMMEDIATE].Get());
            ev->Record.MutableTableStats()->SetPlannedTxCompleted(TabletCounters->Cumulative()[COUNTER_PLANNED_TX_COMPLETE].Get());
            ev->Record.MutableTableStats()->SetTxRejectedByOverload(TabletCounters->Cumulative()[COUNTER_PREPARE_OVERLOADED].Get());
            ev->Record.MutableTableStats()->SetTxRejectedBySpace(TabletCounters->Cumulative()[COUNTER_PREPARE_OUT_OF_SPACE].Get());
            ev->Record.MutableTableStats()->SetTxCompleteLagMsec(TabletCounters->Simple()[COUNTER_TX_COMPLETE_LAG].Get());
            ev->Record.MutableTableStats()->SetInFlightTxCount(TabletCounters->Simple()[COUNTER_TX_IN_FLY].Get() +
                 TabletCounters->Simple()[COUNTER_IMMEDIATE_TX_IN_FLY].Get());

            ev->Record.MutableTableStats()->SetRowUpdates(TabletCounters->Cumulative()[COUNTER_ENGINE_HOST_UPDATE_ROW].Get() +
                                                          TabletCounters->Cumulative()[COUNTER_UPLOAD_ROWS].Get());
            ev->Record.MutableTableStats()->SetRowDeletes(TabletCounters->Cumulative()[COUNTER_ENGINE_HOST_ERASE_ROW].Get());
            ev->Record.MutableTableStats()->SetRowReads(TabletCounters->Cumulative()[COUNTER_ENGINE_HOST_SELECT_ROW].Get());
            ev->Record.MutableTableStats()->SetRangeReads(TabletCounters->Cumulative()[COUNTER_ENGINE_HOST_SELECT_RANGE].Get());
            ev->Record.MutableTableStats()->SetRangeReadRows(TabletCounters->Cumulative()[COUNTER_ENGINE_HOST_SELECT_RANGE_ROWS].Get());
            if (resourceMetrics != nullptr) {
                resourceMetrics->Fill(*ev->Record.MutableTabletMetrics());
            }

            ev->Record.MutableTableStats()->SetPartCount(ti.Stats.PartCount);
            ev->Record.MutableTableStats()->SetSearchHeight(ti.Stats.SearchHeight);
            ev->Record.MutableTableStats()->SetLastFullCompactionTs(ti.Stats.LastFullCompaction.Seconds());

            if (!ti.Stats.PartOwners.contains(TabletID())) {
                ev->Record.AddUserTablePartOwners(TabletID());
            }
            for (const auto& pi : ti.Stats.PartOwners) {
                ev->Record.AddUserTablePartOwners(pi);
            }
            for (const auto& pi : SysTablesPartOnwers) {
                ev->Record.AddSysTablesPartOwners(pi);
            }

            ev->Record.SetNodeId(ctx.ExecutorThread.ActorSystem->NodeId);
            ev->Record.SetStartTime(StartTime().MilliSeconds());

            NTabletPipe::SendData(ctx, DbStatsReportPipe, ev.Release());
        }

        LastDbStatsReportTime = now;
    }

    bool OnRenderAppHtmlPage(NMon::TEvRemoteHttpInfo::TPtr ev, const TActorContext &ctx) override;
    void SerializeHistogram(const TUserTable &tinfo,
                            const NTable::THistogram &histogram,
                            const NScheme::TTypeRegistry &typeRegistry,
                            NKikimrTxDataShard::TEvGetDataHistogramResponse::THistogram &hist);
    void SerializeKeySample(const TUserTable &tinfo,
                            const NTable::TKeyAccessSample &keySample,
                            const NScheme::TTypeRegistry &typeRegistry,
                            NKikimrTxDataShard::TEvGetDataHistogramResponse::THistogram &hist);

    bool ByKeyFilterDisabled() const;
    bool AllowCancelROwithReadsets() const;

    void ResolveTablePath(const TActorContext &ctx);
};

NKikimrTxDataShard::TError::EKind ConvertErrCode(NMiniKQL::IEngineFlat::EResult code);

Ydb::StatusIds::StatusCode ConvertToYdbStatusCode(NKikimrTxDataShard::TError::EKind);

template <class T>
void SetStatusError(T &rec,
                    Ydb::StatusIds::StatusCode status,
                    const TString &msg,
                    ui32 severity = NYql::TSeverityIds::S_ERROR)
{
    rec.MutableStatus()->SetCode(status);
    auto *issue = rec.MutableStatus()->AddIssues();
    issue->set_severity(severity);
    issue->set_message(msg);
}

}}
