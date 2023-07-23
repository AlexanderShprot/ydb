#include "column_engine_logs.h"
#include "filter.h"
#include "indexed_read_data.h"

#include <ydb/core/tx/columnshard/hooks/abstract/abstract.h>
#include <ydb/core/formats/arrow/one_batch_input_stream.h>
#include <ydb/core/formats/arrow/merging_sorted_input_stream.h>
#include <ydb/library/conclusion/status.h>
#include "changes/indexation.h"
#include "changes/in_granule_compaction.h"
#include "changes/split_compaction.h"
#include "changes/cleanup.h"
#include "changes/ttl.h"

#include <concepts>

namespace NKikimr::NOlap {

std::shared_ptr<NKikimr::NOlap::TCompactColumnEngineChanges> TColumnEngineForLogs::TChangesConstructor::BuildCompactionChanges(std::unique_ptr<TCompactionInfo>&& info,
    const TCompactionLimits& limits, const TSnapshot& initSnapshot, const TCompactionSrcGranule& srcGranule) {
    std::shared_ptr<TCompactColumnEngineChanges> result;
    if (info->InGranule()) {
        result = std::make_shared<TInGranuleCompactColumnEngineChanges>(limits, std::move(info), srcGranule);
    } else {
        result = std::make_shared<TSplitCompactColumnEngineChanges>(limits, std::move(info), srcGranule);
    }
    result->InitSnapshot = initSnapshot;
    return result;
}

std::shared_ptr<NKikimr::NOlap::TCleanupColumnEngineChanges> TColumnEngineForLogs::TChangesConstructor::BuildCleanupChanges(const TSnapshot& initSnapshot) {
    auto changes = std::make_shared<TCleanupColumnEngineChanges>();
    changes->InitSnapshot = initSnapshot;
    return changes;
}

std::shared_ptr<NKikimr::NOlap::TTTLColumnEngineChanges> TColumnEngineForLogs::TChangesConstructor::BuildTtlChanges() {
    return std::make_shared<TTTLColumnEngineChanges>();
}

std::shared_ptr<NKikimr::NOlap::TInsertColumnEngineChanges> TColumnEngineForLogs::TChangesConstructor::BuildInsertChanges(const TMark& defaultMark, std::vector<NOlap::TInsertedData>&& blobsToIndex, const TSnapshot& initSnapshot) {
    auto changes = std::make_shared<TInsertColumnEngineChanges>(defaultMark);
    changes->DataToIndex = std::move(blobsToIndex);
    changes->InitSnapshot = initSnapshot;
    return changes;
}

TColumnEngineForLogs::TColumnEngineForLogs(ui64 tabletId, const TCompactionLimits& limits)
    : GranulesStorage(std::make_shared<TGranulesStorage>(SignalCounters, limits))
    , TabletId(tabletId)
    , LastPortion(0)
    , LastGranule(0)
{
}

ui64 TColumnEngineForLogs::MemoryUsage() const {
    auto numPortions = Counters.GetPortionsCount();

    return Counters.Granules * (sizeof(TGranuleMeta) + sizeof(ui64)) +
        numPortions * (sizeof(TPortionInfo) + sizeof(ui64)) +
        Counters.ColumnRecords * sizeof(TColumnRecord) +
        Counters.ColumnMetadataBytes;
}

const TMap<ui64, std::shared_ptr<TColumnEngineStats>>& TColumnEngineForLogs::GetStats() const {
    return PathStats;
}

const TColumnEngineStats& TColumnEngineForLogs::GetTotalStats() {
    Counters.Tables = PathGranules.size();
    Counters.Granules = Granules.size();
    Counters.EmptyGranules = EmptyGranules.size();
    Counters.OverloadedGranules = GranulesStorage->GetOverloadedGranulesCount();

    return Counters;
}

void TColumnEngineForLogs::UpdatePortionStats(const TPortionInfo& portionInfo, EStatsUpdateType updateType,
                                            const TPortionInfo* exPortionInfo) {
    UpdatePortionStats(Counters, portionInfo, updateType, exPortionInfo);

    ui64 granule = portionInfo.Granule();
    Y_VERIFY(granule);
    Y_VERIFY(Granules.contains(granule));
    ui64 pathId = Granules[granule]->PathId();
    Y_VERIFY(pathId);
    if (!PathStats.contains(pathId)) {
        auto& stats = PathStats[pathId];
        stats = std::make_shared<TColumnEngineStats>();
        stats->Tables = 1;
    }
    UpdatePortionStats(*PathStats[pathId], portionInfo, updateType, exPortionInfo);
}

TColumnEngineStats::TPortionsStats DeltaStats(const TPortionInfo& portionInfo, ui64& metadataBytes) {
    TColumnEngineStats::TPortionsStats deltaStats;
    THashSet<TUnifiedBlobId> blobs;
    for (auto& rec : portionInfo.Records) {
        metadataBytes += rec.Metadata.size();
        blobs.insert(rec.BlobRange.BlobId);
        deltaStats.BytesByColumn[rec.ColumnId] += rec.BlobRange.Size;
    }
    for (auto& rec : portionInfo.Meta.ColumnMeta) {
        deltaStats.RawBytesByColumn[rec.first] += rec.second.RawBytes;
    }
    deltaStats.Rows = portionInfo.NumRows();
    deltaStats.RawBytes = portionInfo.RawBytesSum();
    deltaStats.Bytes = 0;
    for (auto& blobId : blobs) {
        deltaStats.Bytes += blobId.BlobSize();
    }
    deltaStats.Blobs = blobs.size();
    deltaStats.Portions = 1;
    return deltaStats;
}

void TColumnEngineForLogs::UpdatePortionStats(TColumnEngineStats& engineStats, const TPortionInfo& portionInfo,
                                              EStatsUpdateType updateType,
                                              const TPortionInfo* exPortionInfo) const {
    ui64 columnRecords = portionInfo.Records.size();
    ui64 metadataBytes = 0;
    TColumnEngineStats::TPortionsStats deltaStats = DeltaStats(portionInfo, metadataBytes);

    Y_VERIFY(!exPortionInfo || exPortionInfo->Meta.Produced != TPortionMeta::EProduced::UNSPECIFIED);
    Y_VERIFY(portionInfo.Meta.Produced != TPortionMeta::EProduced::UNSPECIFIED);

    TColumnEngineStats::TPortionsStats& srcStats = exPortionInfo
        ? (exPortionInfo->IsActive()
            ? engineStats.StatsByType[exPortionInfo->Meta.Produced]
            : engineStats.StatsByType[TPortionMeta::EProduced::INACTIVE])
        : engineStats.StatsByType[portionInfo.Meta.Produced];
    TColumnEngineStats::TPortionsStats& stats = portionInfo.IsActive()
        ? engineStats.StatsByType[portionInfo.Meta.Produced]
        : engineStats.StatsByType[TPortionMeta::EProduced::INACTIVE];

    const bool isErase = updateType == EStatsUpdateType::ERASE;
    const bool isAdd = updateType == EStatsUpdateType::ADD;

    if (isErase) { // PortionsToDrop
        engineStats.ColumnRecords -= columnRecords;
        engineStats.ColumnMetadataBytes -= metadataBytes;

        stats -= deltaStats;
    } else if (isAdd) { // Load || AppendedPortions
        engineStats.ColumnRecords += columnRecords;
        engineStats.ColumnMetadataBytes += metadataBytes;

        stats += deltaStats;
    } else if (&srcStats != &stats || exPortionInfo) { // SwitchedPortions || PortionsToEvict
        stats += deltaStats;

        if (exPortionInfo) {
            ui64 rmMetadataBytes = 0;
            srcStats -= DeltaStats(*exPortionInfo, rmMetadataBytes);

            engineStats.ColumnRecords += columnRecords - exPortionInfo->Records.size();
            engineStats.ColumnMetadataBytes += metadataBytes - rmMetadataBytes;
        } else {
            srcStats -= deltaStats;
        }
    }
}

void TColumnEngineForLogs::UpdateDefaultSchema(const TSnapshot& snapshot, TIndexInfo&& info) {
    if (!GranulesTable) {
        ui32 indexId = info.GetId();
        GranulesTable = std::make_shared<TGranulesTable>(*this, indexId);
        ColumnsTable = std::make_shared<TColumnsTable>(indexId);
        CountersTable = std::make_shared<TCountersTable>(indexId);
    }
    VersionedIndex.AddIndex(snapshot, std::move(info));
}

bool TColumnEngineForLogs::Load(IDbWrapper& db, THashSet<TUnifiedBlobId>& lostBlobs, const THashSet<ui64>& pathsToDrop) {
    ClearIndex();
    {
        auto guard = GranulesStorage->StartPackModification();
        if (!LoadGranules(db)) {
            return false;
        }
        if (!LoadColumns(db, lostBlobs)) {
            return false;
        }
        if (!LoadCounters(db)) {
            return false;
        }
    }

    THashSet<ui64> emptyGranulePaths;
    for (const auto& [granule, spg] : Granules) {
        if (spg->Empty()) {
            EmptyGranules.insert(granule);
            emptyGranulePaths.insert(spg->PathId());
        }
        for (const auto& [_, portionInfo] : spg->GetPortions()) {
            UpdatePortionStats(portionInfo, EStatsUpdateType::ADD);
            if (portionInfo.CheckForCleanup()) {
                CleanupPortions.emplace(portionInfo.GetAddress());
            }
        }
    }

    // Cleanup empty granules
    for (auto& pathId : emptyGranulePaths) {
        for (auto& emptyGranules : EmptyGranuleTracks(pathId)) {
            // keep first one => merge, keep nothing => drop.
            bool keepFirst = !pathsToDrop.contains(pathId);
            for (auto& [mark, granule] : emptyGranules) {
                if (keepFirst) {
                    keepFirst = false;
                    continue;
                }

                Y_VERIFY(Granules.contains(granule));
                auto spg = Granules[granule];
                Y_VERIFY(spg);
                GranulesTable->Erase(db, spg->Record);
                EraseGranule(pathId, granule, mark);
            }
        }
    }

    Y_VERIFY(!(LastPortion >> 63), "near to int overflow");
    Y_VERIFY(!(LastGranule >> 63), "near to int overflow");
    return true;
}

bool TColumnEngineForLogs::LoadGranules(IDbWrapper& db) {
    auto callback = [&](const TGranuleRecord& rec) {
        Y_VERIFY(SetGranule(rec, true));
    };

    return GranulesTable->Load(db, callback);
}

bool TColumnEngineForLogs::LoadColumns(IDbWrapper& db, THashSet<TUnifiedBlobId>& lostBlobs) {
    return ColumnsTable->Load(db, [&](const TColumnRecord& rec) {
        auto& indexInfo = GetIndexInfo();
        Y_VERIFY(rec.Valid());
        // Do not count the blob as lost since it exists in the index.
        lostBlobs.erase(rec.BlobRange.BlobId);
        // Locate granule and append the record.
        if (const auto gi = Granules.find(rec.Granule); gi != Granules.end()) {
            gi->second->AddColumnRecord(indexInfo, rec);
        } else {
            Y_VERIFY(false);
        }
    });
}

bool TColumnEngineForLogs::LoadCounters(IDbWrapper& db) {
    auto callback = [&](ui32 id, ui64 value) {
        switch (id) {
        case LAST_PORTION:
            LastPortion = value;
            break;
        case LAST_GRANULE:
            LastGranule = value;
            break;
        case LAST_PLAN_STEP:
            LastSnapshot = TSnapshot(value, LastSnapshot.GetTxId());
            break;
        case LAST_TX_ID:
            LastSnapshot = TSnapshot(LastSnapshot.GetPlanStep(), value);
            break;
        }
    };

    return CountersTable->Load(db, callback);
}

std::shared_ptr<TInsertColumnEngineChanges> TColumnEngineForLogs::StartInsert(const TCompactionLimits& /*limits*/, std::vector<TInsertedData>&& dataToIndex) {
    Y_VERIFY(dataToIndex.size());

    auto changes = TChangesConstructor::BuildInsertChanges(DefaultMark(), std::move(dataToIndex), LastSnapshot);
    ui32 reserveGranules = 0;
    for (const auto& data : changes->DataToIndex) {
        const ui64 pathId = data.PathId;

        if (changes->PathToGranule.contains(pathId)) {
            continue;
        }

        if (PathGranules.contains(pathId)) {
            // Abort inserting if the path has overloaded granules.
            if (GranulesStorage->GetOverloaded(pathId)) {
                return {};
            }

            // TODO: cache PathToGranule for hot pathIds
            const auto& src = PathGranules[pathId];
            changes->PathToGranule[pathId].assign(src.begin(), src.end());
        } else {
            // It could reserve more than needed in case of the same pathId in DataToIndex
            ++reserveGranules;
        }
    }

    if (reserveGranules) {
        changes->FirstGranuleId = LastGranule + 1;
        changes->ReservedGranuleIds = reserveGranules;
        LastGranule += reserveGranules;
    }

    return changes;
}

std::shared_ptr<TCompactColumnEngineChanges> TColumnEngineForLogs::StartCompaction(std::unique_ptr<TCompactionInfo>&& info,
                                                                            const TCompactionLimits& limits) {
    const ui64 pathId = info->GetPlanCompaction().GetPathId();
    Y_VERIFY(PathGranules.contains(pathId));

    auto& g = info->GetObject<TGranuleMeta>();
    for (const auto& [mark, pathGranule] : PathGranules[pathId]) {
        if (pathGranule == g.GetGranuleId()) {
            TCompactionSrcGranule srcGranule = TCompactionSrcGranule(pathId, g.GetGranuleId(), mark);
            auto changes = TChangesConstructor::BuildCompactionChanges(std::move(info), limits, LastSnapshot, srcGranule);
            NYDBTest::TControllers::GetColumnShardController()->OnStartCompaction(changes);
            return changes;
        }
    }
    Y_VERIFY(false);
    return nullptr;
}

std::shared_ptr<TCleanupColumnEngineChanges> TColumnEngineForLogs::StartCleanup(const TSnapshot& snapshot,
                                                                         const TCompactionLimits& /*limits*/,
                                                                         THashSet<ui64>& pathsToDrop,
                                                                         ui32 maxRecords) {
    auto changes = TChangesConstructor::BuildCleanupChanges(snapshot);
    ui32 affectedRecords = 0;

    // Add all portions from dropped paths
    THashSet<ui64> dropPortions;
    THashSet<ui64> emptyPaths;
    for (ui64 pathId : pathsToDrop) {
        if (!PathGranules.contains(pathId)) {
            emptyPaths.insert(pathId);
            continue;
        }

        for (const auto& [_, granule]: PathGranules[pathId]) {
            Y_VERIFY(Granules.contains(granule));
            auto spg = Granules[granule];
            Y_VERIFY(spg);
            for (auto& [portion, info] : spg->GetPortions()) {
                affectedRecords += info.NumRecords();
                changes->PortionsToDrop.push_back(info);
                dropPortions.insert(portion);
            }

            if (affectedRecords > maxRecords) {
                break;
            }
        }

        if (affectedRecords > maxRecords) {
            changes->NeedRepeat = true;
            break;
        }
    }
    for (ui64 pathId : emptyPaths) {
        pathsToDrop.erase(pathId);
    }

    if (affectedRecords > maxRecords) {
        return changes;
    }

    // Add stale portions of alive paths
    THashSet<ui64> cleanGranules;
    std::shared_ptr<TGranuleMeta> granuleMeta;
    for (auto it = CleanupPortions.begin(); it != CleanupPortions.end();) {
        if (!granuleMeta || granuleMeta->GetGranuleId() != it->GetGranuleId()) {
            auto itGranule = Granules.find(it->GetGranuleId());
            if (itGranule == Granules.end()) {
                it = CleanupPortions.erase(it);
                continue;
            }
            granuleMeta = itGranule->second;
        }
        Y_VERIFY(granuleMeta);
        auto* portionInfo = granuleMeta->GetPortionPointer(it->GetPortionId());
        if (!portionInfo) {
            it = CleanupPortions.erase(it);
        } else if (portionInfo->CheckForCleanup(snapshot)) {
            affectedRecords += portionInfo->NumRecords();
            changes->PortionsToDrop.push_back(*portionInfo);
            it = CleanupPortions.erase(it);
            if (affectedRecords > maxRecords) {
                changes->NeedRepeat = true;
                break;
            }
        } else {
            Y_VERIFY(portionInfo->CheckForCleanup());
            ++it;
        }
    }

    return changes;
}

std::shared_ptr<TTTLColumnEngineChanges> TColumnEngineForLogs::StartTtl(const THashMap<ui64, TTiering>& pathEviction, const std::shared_ptr<arrow::Schema>& schema,
                                                                     ui64 maxEvictBytes) {
    if (pathEviction.empty()) {
        return {};
    }

    auto changes = TChangesConstructor::BuildTtlChanges();
    ui64 evicttionSize = 0;
    bool allowEviction = true;
    ui64 dropBlobs = 0;
    bool allowDrop = true;

    auto& indexInfo = GetIndexInfo();
    for (const auto& [pathId, ttl] : pathEviction) {
        if (!PathGranules.contains(pathId)) {
            continue; // It's not an error: allow TTL over multiple shards with different pathIds presented
        }

        auto expireTimestamp = ttl.EvictScalar(schema);
        Y_VERIFY(expireTimestamp);

        auto ttlColumnNames = ttl.GetTtlColumns();
        Y_VERIFY(ttlColumnNames.size() == 1); // TODO: support different ttl columns
        ui32 ttlColumnId = indexInfo.GetColumnId(*ttlColumnNames.begin());

        for (const auto& [ts, granule] : PathGranules[pathId]) {
            auto spg = Granules[granule];
            Y_VERIFY(spg);

            for (auto& [portion, info] : spg->GetPortions()) {
                if (!info.IsActive()) {
                    continue;
                }

                allowEviction = (evicttionSize <= maxEvictBytes);
                allowDrop = (dropBlobs <= TCompactionLimits::MAX_BLOBS_TO_DELETE);
                bool tryEvictPortion = allowEviction && ttl.HasTiers()
                    && info.EvictReady(TCompactionLimits::EVICT_HOT_PORTION_BYTES);

                if (auto max = info.MaxValue(ttlColumnId)) {
                    bool keep = NArrow::ScalarLess(expireTimestamp, max);
                    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "scalar_less_result")("keep", keep)("tryEvictPortion", tryEvictPortion)("allowDrop", allowDrop);
                    if (keep && tryEvictPortion) {
                        TString tierName;
                        for (auto& tierRef : ttl.GetOrderedTiers()) { // TODO: lower/upper_bound + move into TEviction
                            auto& tierInfo = tierRef.Get();
                            if (!indexInfo.AllowTtlOverColumn(tierInfo.GetEvictColumnName())) {
                                SignalCounters.OnPortionNoTtlColumn(info.BlobsBytes());
                                continue; // Ignore tiers with bad ttl column
                            }
                            if (NArrow::ScalarLess(tierInfo.EvictScalar(schema), max)) {
                                tierName = tierInfo.GetName();
                            } else {
                                break;
                            }
                        }
                        if (info.TierName != tierName) {
                            evicttionSize += info.BlobsSizes().first;
                            bool needExport = ttl.NeedExport(tierName);
                            changes->PortionsToEvict.emplace_back(
                                info, TPortionEvictionFeatures(tierName, pathId, needExport));
                            SignalCounters.OnPortionToEvict(info.BlobsBytes());
                        }
                    }
                    if (!keep && allowDrop) {
                        dropBlobs += info.NumRecords();
                        changes->PortionsToDrop.push_back(info);
                        SignalCounters.OnPortionToDrop(info.BlobsBytes());
                    }
                } else {
                    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "scalar_less_not_max");
                    SignalCounters.OnPortionNoBorder(info.BlobsBytes());
                }
            }
        }
    }

    if (changes->PortionsToDrop.empty() &&
        changes->PortionsToEvict.empty()) {
        return {};
    }

    if (!allowEviction || !allowDrop) {
        changes->NeedRepeat = true;
    }
    return changes;
}

std::vector<std::vector<std::pair<TMark, ui64>>> TColumnEngineForLogs::EmptyGranuleTracks(ui64 pathId) const {
    Y_VERIFY(PathGranules.contains(pathId));
    const auto& pathGranules = PathGranules.find(pathId)->second;

    std::vector<std::vector<std::pair<TMark, ui64>>> emptyGranules;
    ui64 emptyStart = 0;
    for (const auto& [mark, granule]: pathGranules) {
        Y_VERIFY(Granules.contains(granule));
        auto spg = Granules.find(granule)->second;
        Y_VERIFY(spg);

        if (spg->Empty()) {
            if (!emptyStart) {
                emptyGranules.push_back({});
                emptyStart = granule;
            }
            emptyGranules.back().emplace_back(mark, granule);
        } else if (emptyStart) {
            emptyStart = 0;
        }
    }

    return emptyGranules;
}

bool TColumnEngineForLogs::ApplyChanges(IDbWrapper& db, std::shared_ptr<TColumnEngineChanges> indexChanges, const TSnapshot& snapshot) noexcept {
    // Update tmp granules with real ids
    {
        TFinalizationContext context(LastGranule, LastPortion, snapshot);
        indexChanges->Compile(context);
    }
    {
        TApplyChangesContext context(db, snapshot);
        if (!indexChanges->ApplyChanges(*this, context, true)) { // validate only
            return false;
        }
        Y_VERIFY(indexChanges->ApplyChanges(*this, context, false));
    }
    CountersTable->Write(db, LAST_PORTION, LastPortion);
    CountersTable->Write(db, LAST_GRANULE, LastGranule);

    if (LastSnapshot < snapshot) {
        LastSnapshot = snapshot;
        CountersTable->Write(db, LAST_PLAN_STEP, LastSnapshot.GetPlanStep());
        CountersTable->Write(db, LAST_TX_ID, LastSnapshot.GetTxId());
    }
    return true;
}

bool TColumnEngineForLogs::SetGranule(const TGranuleRecord& rec, bool apply) {
    const TMark mark(rec.Mark);

    if (apply) {
        // There should be only one granule with (PathId, Mark).
        Y_VERIFY(PathGranules[rec.PathId].emplace(mark, rec.Granule).second);

        // Allocate granule info and ensure that there is no granule with same id inserted before.
        Y_VERIFY(Granules.emplace(rec.Granule, std::make_shared<TGranuleMeta>(rec, GranulesStorage, SignalCounters.RegisterGranuleDataCounters())).second);
    } else {
        // Granule with same id already exists.
        if (Granules.contains(rec.Granule)) {
            return false;
        }

        // Granule with same (PathId, Mark) already exists.
        if (PathGranules.contains(rec.PathId) && PathGranules[rec.PathId].contains(mark)) {
            return false;
        }
    }

    return true;
}

void TColumnEngineForLogs::EraseGranule(ui64 pathId, ui64 granule, const TMark& mark) {
    Y_VERIFY(PathGranules.contains(pathId));
    auto it = Granules.find(granule);
    Y_VERIFY(it != Granules.end());
    Y_VERIFY(it->second->IsErasable());
    Granules.erase(it);
    EmptyGranules.erase(granule);
    PathGranules[pathId].erase(mark);
}

bool TColumnEngineForLogs::UpsertPortion(const TPortionInfo& portionInfo, bool apply, const TPortionInfo* exInfo) {
    ui64 granule = portionInfo.Granule();

    if (!apply) {
        for (auto& record : portionInfo.Records) {
            if (granule != record.Granule) {
                AFL_ERROR(NKikimrServices::TX_COLUMNSHARD)("event", "inconsistency_granule")("granule", granule)("record_granule", record.Granule);
                return false;
            }
            if (!record.Valid()) {
                AFL_ERROR(NKikimrServices::TX_COLUMNSHARD)("event", "incorrect_record")("record", record.DebugString());
                return false;
            }
        }
        return true;
    }

    Y_VERIFY(portionInfo.Valid());
    auto& spg = Granules[granule];
    Y_VERIFY(spg);

    if (exInfo) {
        UpdatePortionStats(portionInfo, EStatsUpdateType::DEFAULT, exInfo);
    } else {
        UpdatePortionStats(portionInfo, EStatsUpdateType::ADD);
    }

    spg->UpsertPortion(portionInfo);
    return true; // It must return true if (apply == true)
}

bool TColumnEngineForLogs::ErasePortion(const TPortionInfo& portionInfo, bool apply, bool updateStats) {
    Y_VERIFY(!portionInfo.Empty());
    const ui64 portion = portionInfo.Portion();
    auto it = Granules.find(portionInfo.Granule());
    Y_VERIFY(it != Granules.end());
    auto& spg = it->second;
    Y_VERIFY(spg);
    auto* p = spg->GetPortionPointer(portion);

    if (!p) {
        LOG_S_WARN("Portion erased already " << portionInfo << " at tablet " << TabletId);
    } else if (apply) {
        if (updateStats) {
            UpdatePortionStats(*p, EStatsUpdateType::ERASE);
        }
        Y_VERIFY(spg->ErasePortion(portion));
    }
    return true;
}

static TMap<TSnapshot, std::vector<const TPortionInfo*>> GroupPortionsBySnapshot(const THashMap<ui64, TPortionInfo>& portions, const TSnapshot& snapshot) {
    TMap<TSnapshot, std::vector<const TPortionInfo*>> out;
    for (const auto& [portion, portionInfo] : portions) {
        if (portionInfo.Empty()) {
            continue;
        }

        TSnapshot recSnapshot = portionInfo.GetSnapshot();
        TSnapshot recXSnapshot = portionInfo.GetXSnapshot();

        bool visible = (recSnapshot <= snapshot);
        if (recXSnapshot.GetPlanStep()) {
            visible = visible && snapshot < recXSnapshot;
        }

        if (visible) {
            out[recSnapshot].push_back(&portionInfo);
        }
    }
    return out;
}

std::shared_ptr<TSelectInfo> TColumnEngineForLogs::Select(ui64 pathId, TSnapshot snapshot,
                                                          const THashSet<ui32>& columnIds,
                                                          const TPKRangesFilter& pkRangesFilter) const
{
    auto out = std::make_shared<TSelectInfo>();
    if (!PathGranules.contains(pathId)) {
        return out;
    }

    auto& pathGranules = PathGranules.find(pathId)->second;
    if (pathGranules.empty()) {
        return out;
    }
    out->Granules.reserve(pathGranules.size());
    // TODO: out.Portions.reserve()
    std::optional<TMarksMap::const_iterator> previousIterator;
    const bool compositeMark = UseCompositeMarks();

    for (auto&& filter : pkRangesFilter) {
        std::optional<NArrow::TReplaceKey> indexKeyFrom = filter.KeyFrom(GetIndexKey());
        std::optional<NArrow::TReplaceKey> indexKeyTo = filter.KeyTo(GetIndexKey());

        std::shared_ptr<arrow::Scalar> keyFrom;
        std::shared_ptr<arrow::Scalar> keyTo;
        if (indexKeyFrom) {
            keyFrom = NArrow::TReplaceKey::ToScalar(*indexKeyFrom, 0);
        }
        if (indexKeyTo) {
            keyTo = NArrow::TReplaceKey::ToScalar(*indexKeyTo, 0);
        }

        auto it = pathGranules.begin();
        if (keyFrom) {
            it = pathGranules.lower_bound(*keyFrom);
            if (it != pathGranules.begin()) {
                if (it == pathGranules.end() || compositeMark || *keyFrom != it->first) {
                    // TODO: better check if we really need an additional granule before the range
                    --it;
                }
            }
        }

        if (previousIterator && (previousIterator == pathGranules.end() || it->first < (*previousIterator)->first)) {
            it = *previousIterator;
        }
        for (; it != pathGranules.end(); ++it) {
            auto& mark = it->first;
            ui64 granule = it->second;
            if (keyTo && mark > *keyTo) {
                break;
            }

            auto it = Granules.find(granule);
            Y_VERIFY(it != Granules.end());
            auto& spg = it->second;
            Y_VERIFY(spg);
            auto& portions = spg->GetPortions();
            bool granuleHasDataForSnaphsot = false;

            TMap<TSnapshot, std::vector<const TPortionInfo*>> orderedPortions = GroupPortionsBySnapshot(portions, snapshot);
            for (auto& [snap, vec] : orderedPortions) {
                for (const auto* portionInfo : vec) {
                    TPortionInfo outPortion;
                    outPortion.Meta = portionInfo->Meta;
                    outPortion.Records.reserve(columnIds.size());

                    for (auto& rec : portionInfo->Records) {
                        Y_VERIFY(rec.Valid());
                        if (columnIds.contains(rec.ColumnId)) {
                            outPortion.Records.push_back(rec);
                        }
                    }
                    Y_VERIFY(outPortion.Produced());
                    if (!pkRangesFilter.IsPortionInUsage(outPortion, GetIndexInfo())) {
                        AFL_TRACE(NKikimrServices::TX_COLUMNSHARD_SCAN)("event", "portion_skipped")
                            ("granule", granule)("portion", portionInfo->Portion());
                        continue;
                    } else {
                        AFL_TRACE(NKikimrServices::TX_COLUMNSHARD_SCAN)("event", "portion_selected")
                            ("granule", granule)("portion", portionInfo->Portion());
                    }
                    out->Portions.emplace_back(std::move(outPortion));
                    granuleHasDataForSnaphsot = true;
                }
            }

            if (granuleHasDataForSnaphsot) {
                out->Granules.push_back(spg->Record);
            }
        }
        previousIterator = it;
    }

    return out;
}

std::unique_ptr<TCompactionInfo> TColumnEngineForLogs::Compact(const TCompactionLimits& limits, const THashSet<ui64>& busyGranuleIds) {
    const auto filter = [&](const ui64 granuleId) {
        if (busyGranuleIds.contains(granuleId)) {
            return false;
        }
        return GetGranulePtrVerified(granuleId)->NeedCompaction(limits);
    };
    auto gCompaction = GranulesStorage->GetGranuleForCompaction(filter);
    if (!gCompaction) {
        AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "no_granule_for_compaction");
        SignalCounters.NoCompactGranulesSelection->Add(1);
        return {};
    }
    std::shared_ptr<TGranuleMeta> compactGranule = GetGranulePtrVerified(*gCompaction);

    if (compactGranule->IsOverloaded(limits)) {
        SignalCounters.CompactOverloadGranulesSelection->Add(1);
    }
    const auto compactionType = compactGranule->GetCompactionType(limits);
    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "take_granule")("granule", compactGranule->DebugString())("compaction", compactionType);
    switch (compactionType) {
        case TGranuleAdditiveSummary::ECompactionClass::NoCompaction:
        case TGranuleAdditiveSummary::ECompactionClass::WaitInternal:
        {
            SignalCounters.NoCompactGranulesSelection->Add(1);
            return {};
        }
        case TGranuleAdditiveSummary::ECompactionClass::Split:
        {
            SignalCounters.SplitCompactGranulesSelection->Add(1);
            return std::make_unique<TCompactionInfo>(compactGranule, false);
        }
        case TGranuleAdditiveSummary::ECompactionClass::Internal:
        {
            SignalCounters.InternalCompactGranulesSelection->Add(1);
            return std::make_unique<TCompactionInfo>(compactGranule, true);
        }
    }
}

} // namespace NKikimr::NOlap
