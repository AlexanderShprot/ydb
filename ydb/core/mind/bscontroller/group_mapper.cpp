#include "group_mapper.h"
#include "group_geometry_info.h"
#include "group_layout_checker.h"

namespace NKikimr::NBsController {

    using namespace NLayoutChecker;

    class TGroupMapper::TImpl : TNonCopyable {
        struct TPDiskInfo : TPDiskRecord {
            TPDiskLayoutPosition Position;
            bool Matching;
            ui32 NumDomainMatchingDisks;
            ui32 SkipToNextRealmGroup;
            ui32 SkipToNextRealm;
            ui32 SkipToNextDomain;

            TPDiskInfo(const TPDiskRecord& pdisk, TPDiskLayoutPosition position)
                : TPDiskRecord(pdisk)
                , Position(std::move(position))
            {
                std::sort(Groups.begin(), Groups.end());
            }

            bool IsUsable() const {
                return Usable && !Decommitted && NumSlots < MaxSlots;
            }

            void InsertGroup(ui32 groupId) {
                if (const auto it = std::lower_bound(Groups.begin(), Groups.end(), groupId); it == Groups.end() || *it < groupId) {
                    Groups.insert(it, groupId);
                }
            }

            void EraseGroup(ui32 groupId) {
                if (const auto it = std::lower_bound(Groups.begin(), Groups.end(), groupId); it != Groups.end() && !(*it < groupId)) {
                    Groups.erase(it);
                }
            }

            ui32 GetPickerScore() const {
                return NumSlots;
            }
        };

        using TPDisks = THashMap<TPDiskId, TPDiskInfo>;
        using TPDiskByPosition = std::vector<std::pair<TPDiskLayoutPosition, TPDiskInfo*>>;

        struct TComparePDiskByPosition {
            bool operator ()(const TPDiskByPosition::value_type& x, const TPDiskLayoutPosition& y) const {
                return x.first < y;
            }

            bool operator ()(const TPDiskLayoutPosition& x, const TPDiskByPosition::value_type& y) const {
                return x < y.first;
            }
        };

        using TGroup = std::vector<TPDiskInfo*>;

        struct TAllocator {
            TImpl& Self;
            const TBlobStorageGroupInfo::TTopology Topology;
            THashSet<TPDiskId> OldGroupContent; // set of all existing disks in the group, inclusing ones which are replaced
            const i64 RequiredSpace;
            const bool RequireOperational;
            TForbiddenPDisks ForbiddenDisks;
            THashMap<ui32, unsigned> LocalityFactor;
            TGroupLayout GroupLayout;
            std::optional<TScore> WorstScore;

            TAllocator(TImpl& self, const TGroupGeometryInfo& geom, i64 requiredSpace, bool requireOperational,
                    TForbiddenPDisks forbiddenDisks, const THashMap<TVDiskIdShort, TPDiskId>& replacedDisks)
                : Self(self)
                , Topology(geom.GetType(), geom.GetNumFailRealms(), geom.GetNumFailDomainsPerFailRealm(), geom.GetNumVDisksPerFailDomain(), true)
                , RequiredSpace(requiredSpace)
                , RequireOperational(requireOperational)
                , ForbiddenDisks(std::move(forbiddenDisks))
                , GroupLayout(Topology)
            {
                for (const auto& [vdiskId, pdiskId] : replacedDisks) {
                    OldGroupContent.insert(pdiskId);
                }
            }

            TGroup ProcessExistingGroup(const TGroupDefinition& group, TString& error) {
                TGroup res(Topology.GetTotalVDisksNum());

                struct TExError { TString error; };

                try {
                    Traverse(group, [&](TVDiskIdShort vdisk, TPDiskId pdiskId) {
                        if (pdiskId != TPDiskId()) {
                            const ui32 orderNumber = Topology.GetOrderNumber(vdisk);

                            const auto it = Self.PDisks.find(pdiskId);
                            if (it == Self.PDisks.end()) {
                                throw TExError{TStringBuilder() << "existing group contains missing PDiskId# " << pdiskId};
                            }
                            TPDiskInfo& pdisk = it->second;
                            res[orderNumber] = &pdisk;

                            const auto [_, inserted] = OldGroupContent.insert(pdiskId);
                            if (!inserted) {
                                throw TExError{TStringBuilder() << "group contains duplicate PDiskId# " << pdiskId};
                            }

                            if (!pdisk.Decommitted) {
                                AddUsedDisk(pdisk);
                                GroupLayout.AddDisk(pdisk.Position, orderNumber);
                            }
                        }
                    });
                } catch (const TExError& e) {
                    error = e.error;
                    return {};
                }

                return res;
            }

            void Decompose(const TGroup& in, TGroupDefinition& out) {
                for (ui32 i = 0; i < in.size(); ++i) {
                    const TVDiskIdShort vdisk = Topology.GetVDiskId(i);
                    out[vdisk.FailRealm][vdisk.FailDomain][vdisk.VDisk] = in[i]->PDiskId;
                }
            }

            bool DiskIsUsable(const TPDiskInfo& pdisk) const {
                if (!pdisk.IsUsable()) {
                    return false; // disk is not usable in this case
                }
                if (OldGroupContent.contains(pdisk.PDiskId) || ForbiddenDisks.contains(pdisk.PDiskId)) {
                    return false; // can't allow duplicate disks
                }
                if (RequireOperational && !pdisk.Operational) {
                    return false;
                }
                if (pdisk.SpaceAvailable < RequiredSpace) {
                    return false;
                }
                return true;
            }

            TPDiskByPosition SetupMatchingDisks(ui32 maxScore) {
                TPDiskByPosition res;
                res.reserve(Self.PDiskByPosition.size());

                ui32 realmGroupBegin = 0;
                ui32 realmBegin = 0;
                ui32 domainBegin = 0;
                TPDiskLayoutPosition prev;

                std::vector<ui32> numMatchingDisksInDomain(Self.DomainMapper.GetIdCount(), 0);
                for (const auto& [position, pdisk] : Self.PDiskByPosition) {
                    pdisk->Matching = pdisk->GetPickerScore() <= maxScore && DiskIsUsable(*pdisk);
                    if (pdisk->Matching) {
                        if (position.RealmGroup != prev.RealmGroup) {
                            for (; realmGroupBegin < res.size(); ++realmGroupBegin) {
                                res[realmGroupBegin].second->SkipToNextRealmGroup = res.size() - realmGroupBegin;
                            }
                        }
                        if (position.Realm != prev.Realm) {
                            for (; realmBegin < res.size(); ++realmBegin) {
                                res[realmBegin].second->SkipToNextRealm = res.size() - realmBegin;
                            }
                        }
                        if (position.Domain != prev.Domain) {
                            for (; domainBegin < res.size(); ++domainBegin) {
                                res[domainBegin].second->SkipToNextDomain = res.size() - domainBegin;
                            }
                        }
                        prev = position;

                        res.emplace_back(position, pdisk);
                        ++numMatchingDisksInDomain[position.Domain.Index()];
                    }
                }
                for (; realmGroupBegin < res.size(); ++realmGroupBegin) {
                    res[realmGroupBegin].second->SkipToNextRealmGroup = res.size() - realmGroupBegin;
                }
                for (; realmBegin < res.size(); ++realmBegin) {
                    res[realmBegin].second->SkipToNextRealm = res.size() - realmBegin;
                }
                for (; domainBegin < res.size(); ++domainBegin) {
                    res[domainBegin].second->SkipToNextDomain = res.size() - domainBegin;
                }
                for (const auto& [position, pdisk] : res) {
                    pdisk->NumDomainMatchingDisks = numMatchingDisksInDomain[position.Domain.Index()];
                }

                return std::move(res);
            }

            struct TUndoLog {
                struct TItem {
                    ui32 Index;
                    TPDiskInfo *PDisk;
                };

                std::vector<TItem> Items;

                void Log(ui32 index, TPDiskInfo *pdisk) {
                    Items.push_back({index, pdisk});
                }

                size_t GetPosition() const {
                    return Items.size();
                }
            };

            void AddDiskViaUndoLog(TUndoLog& undo, TGroup& group, ui32 index, TPDiskInfo *pdisk) {
                undo.Log(index, pdisk);
                group[index] = pdisk;
                AddUsedDisk(*pdisk);
                GroupLayout.AddDisk(pdisk->Position, index);
                WorstScore.reset(); // invalidate score
            }

            void Revert(TUndoLog& undo, TGroup& group, size_t until) {
                for (; undo.Items.size() > until; undo.Items.pop_back()) {
                    const auto& item = undo.Items.back();
                    group[item.Index] = nullptr;
                    RemoveUsedDisk(*item.PDisk);
                    GroupLayout.RemoveDisk(item.PDisk->Position, item.Index);
                    WorstScore.reset(); // invalidate score
                }
            }

            bool FillInGroup(ui32 maxScore, TUndoLog& undo, TGroup& group) {
                // determine PDisks that fit our requirements (including score)
                auto v = SetupMatchingDisks(maxScore);

                // find which entities we need to allocate -- whole group, some realms, maybe some domains within specific realms?
                bool isEmptyGroup = true;
                std::vector<bool> isEmptyRealm(Topology.GetTotalFailRealmsNum(), true);
                std::vector<bool> isEmptyDomain(Topology.GetTotalFailDomainsNum(), true);
                for (ui32 orderNumber = 0; orderNumber < group.size(); ++orderNumber) {
                    if (group[orderNumber]) {
                        const TVDiskIdShort vdisk = Topology.GetVDiskId(orderNumber);
                        isEmptyGroup = false;
                        isEmptyRealm[vdisk.FailRealm] = false;
                        const ui32 domainIdx = Topology.GetFailDomainOrderNumber(vdisk);
                        isEmptyDomain[domainIdx] = false;
                    }
                }

                auto allocate = [&](auto what, ui32 index) {
                    TDynBitMap forbiddenEntities;
                    forbiddenEntities.Reserve(Self.DomainMapper.GetIdCount());
                    if (!AllocateWholeEntity(what, group, undo, index, {v.begin(), v.end()}, forbiddenEntities)) {
                        Revert(undo, group, 0);
                        return false;
                    }
                    return true;
                };

                if (isEmptyGroup) {
                    return allocate(TAllocateWholeGroup(), 0);
                }

                const ui32 numFailDomainsPerFailRealm = Topology.GetNumFailDomainsPerFailRealm();
                const ui32 numVDisksPerFailDomain = Topology.GetNumVDisksPerFailDomain();
                ui32 domainOrderNumber = 0;
                ui32 orderNumber = 0;

                // scan all fail realms and allocate missing realms or their parts
                for (ui32 failRealmIdx = 0; failRealmIdx < isEmptyRealm.size(); ++failRealmIdx) {
                    if (isEmptyRealm[failRealmIdx]) {
                        // we have an empty realm -- we have to allocate it fully
                        if (!allocate(TAllocateWholeRealm(), failRealmIdx)) {
                            return false;
                        }
                        // skip to next realm
                        domainOrderNumber += numFailDomainsPerFailRealm;
                        orderNumber += numVDisksPerFailDomain * numFailDomainsPerFailRealm;
                        continue;
                    }

                    // scan through domains of this realm, find unallocated ones
                    for (ui32 failDomainIdx = 0; failDomainIdx < numFailDomainsPerFailRealm; ++failDomainIdx, ++domainOrderNumber) {
                        if (isEmptyDomain[domainOrderNumber]) {
                            // try to allocate full domain
                            if (!allocate(TAllocateWholeDomain(), domainOrderNumber)) {
                                return false;
                            }
                            // skip to next domain
                            orderNumber += numVDisksPerFailDomain;
                            continue;
                        }

                        // scan individual disks of the domain and fill gaps
                        for (ui32 vdiskIdx = 0; vdiskIdx < numVDisksPerFailDomain; ++vdiskIdx, ++orderNumber) {
                            if (!group[orderNumber] && !allocate(TAllocateDisk(), orderNumber)) {
                                return false;
                            }
                        }
                    }
                }

                Y_VERIFY(domainOrderNumber == Topology.GetTotalFailDomainsNum());
                Y_VERIFY(orderNumber == Topology.GetTotalVDisksNum());

                return true;
            }

            using TAllocateResult = TPDiskLayoutPosition*;

            struct TAllocateDisk {};

            struct TAllocateWholeDomain {
                static constexpr auto GetEntityCount = &TBlobStorageGroupInfo::TTopology::GetNumVDisksPerFailDomain;
                using TNestedEntity = TAllocateDisk;

                static std::pair<TPDiskLayoutPosition, TPDiskLayoutPosition> MakeRange(const TPDiskLayoutPosition& x, TEntityId& scope) {
                    scope = x.Domain;
                    return {x, x};
                }
            };

            struct TAllocateWholeRealm {
                static constexpr auto GetEntityCount = &TBlobStorageGroupInfo::TTopology::GetNumFailDomainsPerFailRealm;
                using TNestedEntity = TAllocateWholeDomain;

                static std::pair<TPDiskLayoutPosition, TPDiskLayoutPosition> MakeRange(const TPDiskLayoutPosition& x, TEntityId& scope) {
                    scope = x.Realm;
                    return {{x.RealmGroup, x.Realm, TEntityId::Min()}, {x.RealmGroup, x.Realm, TEntityId::Max()}};
                }
            };

            struct TAllocateWholeGroup {
                static constexpr auto GetEntityCount = &TBlobStorageGroupInfo::TTopology::GetTotalFailRealmsNum;
                using TNestedEntity = TAllocateWholeRealm;

                static std::pair<TPDiskLayoutPosition, TPDiskLayoutPosition> MakeRange(const TPDiskLayoutPosition& x, TEntityId& scope) {
                    scope = x.RealmGroup;
                    return {{x.RealmGroup, TEntityId::Min(), TEntityId::Min()}, {x.RealmGroup, TEntityId::Max(), TEntityId::Max()}};
                }
            };

            using TDiskRange = std::pair<TPDiskByPosition::const_iterator, TPDiskByPosition::const_iterator>;

            template<typename T>
            TAllocateResult AllocateWholeEntity(T, TGroup& group, TUndoLog& undo, ui32 parentEntityIndex, TDiskRange range,
                    TDynBitMap& forbiddenEntities) {
                // number of enclosed child entities within this one
                const ui32 entityCount = (Topology.*T::GetEntityCount)();
                Y_VERIFY(entityCount);
                parentEntityIndex *= entityCount;
                // remember current undo stack size
                const size_t undoPosition = undo.GetPosition();

                for (;;) {
                    auto [from, to] = range;
                    TPDiskLayoutPosition *prefix;
                    TEntityId scope;

                    for (ui32 index = 0;; ++index) {
                        // allocate nested entity
                        prefix = AllocateWholeEntity(typename T::TNestedEntity(), group, undo, parentEntityIndex + index,
                            {from, to}, forbiddenEntities);

                        if (prefix) {
                            if (!index) {
                                // reduce range to specific realm/domain entity
                                auto [min, max] = T::MakeRange(*prefix, scope);
                                from = std::lower_bound(from, to, min, TComparePDiskByPosition());
                                to = std::upper_bound(from, to, max, TComparePDiskByPosition());
                            }
                            if (index + 1 == entityCount) {
                                // disable filled entity from further selection if it was really allocated
                                forbiddenEntities.Set(scope.Index());
                                return prefix;
                            }
                        } else if (index) {
                            // disable just checked entity (to prevent its selection again)
                            forbiddenEntities.Set(scope.Index());
                            // try another entity at this level
                            Revert(undo, group, undoPosition);
                            // break the loop and retry
                            break;
                        } else {
                            // no chance to allocate new entity, exit
                            return {};
                        }
                    }
                }
            }

            TAllocateResult AllocateWholeEntity(TAllocateDisk, TGroup& group, TUndoLog& undo, ui32 index, TDiskRange range,
                    TDynBitMap& forbiddenEntities) {
                TPDiskInfo *pdisk = group[index];
                Y_VERIFY(!pdisk);
                auto process = [this, &pdisk](TPDiskInfo *candidate) {
                    if (!pdisk || DiskIsBetter(*candidate, *pdisk)) {
                        pdisk = candidate;
                    }
                };
                FindMatchingDiskBasedOnScore(process, group, index, range, forbiddenEntities);
                if (pdisk) {
                    AddDiskViaUndoLog(undo, group, index, pdisk);
                    pdisk->Matching = false;
                    return &pdisk->Position;
                } else {
                    return {};
                }
            }

            TScore CalculateWorstScoreWithCache(const TGroup& group) {
                if (!WorstScore) {
                    // find the worst disk from a position of layout correctness and use it as a milestone for other
                    // disks -- they can't be misplaced worse
                    TScore worstScore;
                    for (ui32 i = 0; i < Topology.GetTotalVDisksNum(); ++i) {
                        if (TPDiskInfo *pdisk = group[i]; pdisk && !pdisk->Decommitted) {
                            // calculate score for this pdisk, removing it from the set first -- to prevent counting itself
                            const TScore score = GroupLayout.GetExcludedDiskScore(pdisk->Position, i);
                            if (worstScore.BetterThan(score)) {
                                worstScore = score;
                            }
                        }
                    }
                    WorstScore = worstScore;
                }
                return *WorstScore;
            }

            template<typename TCallback>
            void FindMatchingDiskBasedOnScore(
                    TCallback&&   cb,                  // callback to be invoked for every matching candidate
                    const TGroup& group,               // group with peer disks
                    ui32          orderNumber,         // order number of disk being allocated
                    TDiskRange    range,               // range of PDisk candidates to scan
                    TDynBitMap&   forbiddenEntities) { // a set of forbidden TEntityId's prevented from allocation
                // first, find the best score for current group layout -- we can't make failure model inconsistency
                // any worse than it already is
                TScore bestScore = CalculateWorstScoreWithCache(group);

                std::vector<TPDiskInfo*> candidates;

                // scan the candidate range
                while (range.first != range.second) {
                    const auto& [position, pdisk] = *range.first++;

                    // skip inappropriate disks, whole realm groups, realms and domains
                    if (!pdisk->Matching) {
                        // just do nothing, skip this candidate disk
                    } else if (forbiddenEntities[position.RealmGroup.Index()]) {
                        range.first += Min<ui32>(std::distance(range.first, range.second), pdisk->SkipToNextRealmGroup - 1);
                    } else if (forbiddenEntities[position.Realm.Index()]) {
                        range.first += Min<ui32>(std::distance(range.first, range.second), pdisk->SkipToNextRealm - 1);
                    } else if (forbiddenEntities[position.Domain.Index()]) {
                        range.first += Min<ui32>(std::distance(range.first, range.second), pdisk->SkipToNextDomain - 1);
                    } else {
                        const TScore score = GroupLayout.GetCandidateScore(position, orderNumber);
                        if (score.BetterThan(bestScore)) {
                            candidates.clear();
                            bestScore = score;
                        }
                        if (score.SameAs(bestScore)) {
                            candidates.push_back(pdisk);
                        }
                    }
                }

                for (TPDiskInfo *pdisk : candidates) {
                    cb(pdisk);
                }
            }

            bool DiskIsBetter(TPDiskInfo& pretender, TPDiskInfo& king) const {
                if (pretender.NumSlots != king.NumSlots) {
                    return pretender.NumSlots < king.NumSlots;
                } else if (GivesLocalityBoost(pretender, king) || BetterQuotaMatch(pretender, king)) {
                    return true;
                } else {
                    if (pretender.NumDomainMatchingDisks != king.NumDomainMatchingDisks) {
                        return pretender.NumDomainMatchingDisks > king.NumDomainMatchingDisks;
                    }
                    return pretender.PDiskId < king.PDiskId;
                }
            }

            bool GivesLocalityBoost(TPDiskInfo& pretender, TPDiskInfo& king) const {
                const ui32 a = GetLocalityFactor(pretender);
                const ui32 b = GetLocalityFactor(king);
                return Self.Randomize ? a < b : a > b;
            }

            bool BetterQuotaMatch(TPDiskInfo& pretender, TPDiskInfo& king) const {
                return pretender.SpaceAvailable < king.SpaceAvailable;
            }

            void AddUsedDisk(const TPDiskInfo& pdisk) {
                for (ui32 groupId : pdisk.Groups) {
                    ++LocalityFactor[groupId];
                }
            }

            void RemoveUsedDisk(const TPDiskInfo& pdisk) {
                for (ui32 groupId : pdisk.Groups) {
                    if (!--LocalityFactor[groupId]) {
                        LocalityFactor.erase(groupId);
                    }
                }
            }

            unsigned GetLocalityFactor(const TPDiskInfo& pdisk) const {
                unsigned res = 0;
                for (ui32 groupId : pdisk.Groups) {
                    res += GetLocalityFactor(groupId);
                }
                return res;
            }

            unsigned GetLocalityFactor(ui32 groupId) const {
                const auto it = LocalityFactor.find(groupId);
                return it != LocalityFactor.end() ? it->second : 0;
            }
        };

    private:
        const TGroupGeometryInfo Geom;
        const bool Randomize;
        TDomainMapper DomainMapper;
        TPDisks PDisks;
        TPDiskByPosition PDiskByPosition;
        bool Dirty = false;

    public:
        TImpl(TGroupGeometryInfo geom, bool randomize)
            : Geom(std::move(geom))
            , Randomize(randomize)
        {}

        bool RegisterPDisk(const TPDiskRecord& pdisk) {
            // calculate disk position
            const TPDiskLayoutPosition p(DomainMapper, pdisk.Location, pdisk.PDiskId, Geom);

            // insert PDisk into specific map
            TPDisks::iterator it;
            bool inserted;
            std::tie(it, inserted) = PDisks.try_emplace(pdisk.PDiskId, pdisk, p);
            if (inserted) {
                PDiskByPosition.emplace_back(it->second.Position, &it->second);
                Dirty = true;
            }

            return inserted;
        }

        void UnregisterPDisk(TPDiskId pdiskId) {
            const auto it = PDisks.find(pdiskId);
            Y_VERIFY(it != PDisks.end());
            auto x = std::remove(PDiskByPosition.begin(), PDiskByPosition.end(), std::make_pair(it->second.Position, &it->second));
            Y_VERIFY(x + 1 == PDiskByPosition.end());
            PDiskByPosition.pop_back();
            PDisks.erase(it);
        }

        void AdjustSpaceAvailable(TPDiskId pdiskId, i64 increment) {
            const auto it = PDisks.find(pdiskId);
            Y_VERIFY(it != PDisks.end());
            it->second.SpaceAvailable += increment;
        }

        TString FormatPDisks(const TAllocator& allocator) const {
            TStringStream s;
            s << "PDisks# ";

            if (!PDiskByPosition.empty()) {
                s << "{[(";
                TPDiskLayoutPosition prevPosition = PDiskByPosition.front().first;
                const char *space = "";
                for (const auto& [position, pdisk] : PDiskByPosition) {
                    if (prevPosition != position) {
                        s << (prevPosition.Domain != position.Domain ? ")" : "")
                            << (prevPosition.Realm != position.Realm ? "]" : "")
                            << (prevPosition.RealmGroup != position.RealmGroup ? "} {" : "")
                            << (prevPosition.Realm != position.Realm ? "[" : "")
                            << (prevPosition.Domain != position.Domain ? "(" : "");
                        space = "";
                    }

                    s << std::exchange(space, " ") << pdisk->PDiskId;

                    if (allocator.OldGroupContent.contains(pdisk->PDiskId)) {
                        s << "*";
                    }
                    const char *minus = "-";
                    if (allocator.ForbiddenDisks.contains(pdisk->PDiskId)) {
                        s << std::exchange(minus, "") << "f";
                    }
                    if (!pdisk->Usable) {
                        s << std::exchange(minus, "") << "u";
                    }
                    if (pdisk->Decommitted) {
                        s << std::exchange(minus, "") << "d";
                    }
                    if (pdisk->NumSlots >= pdisk->MaxSlots) {
                        s << std::exchange(minus, "") << "s[" << pdisk->NumSlots << "/" << pdisk->MaxSlots << "]";
                    }
                    if (pdisk->SpaceAvailable < allocator.RequiredSpace) {
                        s << std::exchange(minus, "") << "v";
                    }
                    if (!pdisk->Operational) {
                        s << std::exchange(minus, "") << "o";
                    }
                    if (allocator.DiskIsUsable(*pdisk)) {
                        s << "+";
                    }

                    prevPosition = position;
                }
                s << ")]}";
            } else {
                s << "<empty>";
            }

            return s.Str();
        }

        bool AllocateGroup(ui32 groupId, TGroupDefinition& groupDefinition, const THashMap<TVDiskIdShort, TPDiskId>& replacedDisks,
                TForbiddenPDisks forbid, i64 requiredSpace, bool requireOperational,
                TString& error) {
            if (Dirty) {
                std::sort(PDiskByPosition.begin(), PDiskByPosition.end());
                Dirty = false;
            }

            // create group of required size, if it is not created yet
            if (!Geom.ResizeGroup(groupDefinition)) {
                error = "incorrect existing group";
                return false;
            }

            // fill in the allocation context
            TAllocator allocator(*this, Geom, requiredSpace, requireOperational, std::move(forbid), replacedDisks);
            TGroup group = allocator.ProcessExistingGroup(groupDefinition, error);
            if (group.empty()) {
                return false;
            }
            bool ok = true;
            for (TPDiskInfo *pdisk : group) {
                if (!pdisk) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                return true;
            }

            // calculate score table
            std::vector<ui32> scores;
            for (const auto& [pdiskId, pdisk] : PDisks) {
                if (allocator.DiskIsUsable(pdisk)) {
                    scores.push_back(pdisk.GetPickerScore());
                }
            }
            std::sort(scores.begin(), scores.end());
            scores.erase(std::unique(scores.begin(), scores.end()), scores.end());

            // bisect scores to find optimal working one
            std::optional<TGroup> result;
            ui32 begin = 0, end = scores.size();
            while (begin < end) {
                const ui32 mid = begin + (end - begin) / 2;
                TAllocator::TUndoLog undo;
                if (allocator.FillInGroup(scores[mid], undo, group)) {
                    result = group;
                    allocator.Revert(undo, group, 0);
                    end = mid;
                } else {
                    begin = mid + 1;
                }
            }

            if (result) {
                for (const auto& [vdiskId, pdiskId] : replacedDisks) {
                    const auto it = PDisks.find(pdiskId);
                    Y_VERIFY(it != PDisks.end());
                    TPDiskInfo& pdisk = it->second;
                    --pdisk.NumSlots;
                    pdisk.EraseGroup(groupId);
                }
                ui32 numZero = 0;
                for (ui32 i = 0; i < allocator.Topology.GetTotalVDisksNum(); ++i) {
                    if (!group[i]) {
                        ++numZero;
                        TPDiskInfo *pdisk = result->at(i);
                        ++pdisk->NumSlots;
                        pdisk->InsertGroup(groupId);
                    }
                }
                Y_VERIFY(numZero == allocator.Topology.GetTotalVDisksNum() || numZero == replacedDisks.size());
                allocator.Decompose(*result, groupDefinition);
                return true;
            } else {
                error = "no group options " + FormatPDisks(allocator);
                return false;
            }
        }
    };

    TGroupMapper::TGroupMapper(TGroupGeometryInfo geom, bool randomize)
        : Impl(new TImpl(std::move(geom), randomize))
    {}

    TGroupMapper::~TGroupMapper() = default;

    bool TGroupMapper::RegisterPDisk(const TPDiskRecord& pdisk) {
        return Impl->RegisterPDisk(pdisk);
    }

    void TGroupMapper::UnregisterPDisk(TPDiskId pdiskId) {
        return Impl->UnregisterPDisk(pdiskId);
    }

    void TGroupMapper::AdjustSpaceAvailable(TPDiskId pdiskId, i64 increment) {
        return Impl->AdjustSpaceAvailable(pdiskId, increment);
    }

    bool TGroupMapper::AllocateGroup(ui32 groupId, TGroupDefinition& group, const THashMap<TVDiskIdShort, TPDiskId>& replacedDisks,
            TForbiddenPDisks forbid, i64 requiredSpace, bool requireOperational, TString& error) {
        return Impl->AllocateGroup(groupId, group, replacedDisks, std::move(forbid), requiredSpace, requireOperational, error);
    }

} // NKikimr::NBsController
