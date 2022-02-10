#pragma once
#include <ydb/core/yq/libs/checkpointing_common/defs.h>

#include <library/cpp/actors/core/actor.h>

namespace NYq { 

struct TPendingCheckpointStats {
    const TInstant CreatedAt = TInstant::Now();
    ui64 StateSize = 0;
};

class TPendingCheckpoint {
    THashSet<NActors::TActorId> NotYetAcknowledged;
    TPendingCheckpointStats Stats;

public:
    explicit TPendingCheckpoint(THashSet<NActors::TActorId> toBeAcknowledged, TPendingCheckpointStats stats = TPendingCheckpointStats());

    void Acknowledge(const NActors::TActorId& actorId);

    void Acknowledge(const NActors::TActorId& actorId, ui64 stateSize);

    [[nodiscard]]
    bool GotAllAcknowledges() const;

    [[nodiscard]]
    size_t NotYetAcknowledgedCount() const;

    [[nodiscard]]
    const TPendingCheckpointStats& GetStats() const;
};

class TPendingRestoreCheckpoint {
public:
    TPendingRestoreCheckpoint(TCheckpointId checkpointId, bool commitAfterRestore, THashSet<NActors::TActorId> toBeAcknowledged);

    void Acknowledge(const NActors::TActorId& actorId);

    [[nodiscard]]
    bool GotAllAcknowledges() const;

    [[nodiscard]]
    size_t NotYetAcknowledgedCount() const;

public:
    TCheckpointId CheckpointId;
    bool CommitAfterRestore;

private:
    THashSet<NActors::TActorId> NotYetAcknowledged;
};

class TPendingInitCoordinator { 
public: 
    explicit TPendingInitCoordinator(size_t actorsCount) 
        : ActorsCount(actorsCount) 
    { 
    } 
 
    void OnNewCheckpointCoordinatorAck(); 
    bool AllNewCheckpointCoordinatorAcksProcessed() const; 
 
    bool CanInjectCheckpoint() const; 
 
public: 
    const size_t ActorsCount; 
    size_t NewCheckpointCoordinatorAcksGot = 0; 
    TMaybe<TCheckpointId> CheckpointId; 
}; 
 
} // namespace NYq 
