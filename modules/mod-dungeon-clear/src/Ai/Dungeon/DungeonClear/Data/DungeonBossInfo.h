/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONBOSSINFO_H
#define _PLAYERBOT_DUNGEONBOSSINFO_H

#include <string>

#include "Common.h"

// A boss list entry is either a real combat boss (the default — killed for
// encounter credit) or a non-combat "objective": a navmesh anchor the tank
// travels to in order to progress an event (trigger a spawn, traverse a gate),
// after which it is marked done and the clear advances. See BossRosterRegistry.
enum class DungeonAnchorKind : uint8
{
    Boss,
    Objective,
};

struct DungeonBossInfo
{
    uint32 entry{0};
    uint32 encounterIndex{0};
    std::string name;
    uint32 mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    // --- Roster-override extensions (defaulted; untouched for auto-derived
    //     bosses from BossSpawnIndex) ----------------------------------------

    // Boss vs travel-objective. Objectives skip the combat/engage machinery and
    // complete on arrival (see DcObjectiveArriveAction).
    DungeonAnchorKind kind{DungeonAnchorKind::Boss};

    // Objective only: arrival/completion radius in yards. 0 => use the
    // ObjectiveArriveRadius config default.
    float arriveRadius{0.0f};

    // Objective only: optional creature whose presence (alive) also satisfies
    // the objective — e.g. the real boss has spawned as a result of the event.
    uint32 gateEntry{0};

    // Objective only: id into ObjectiveHookRegistry for an on-arrival handler.
    // 0 => no hook; the objective simply completes and the clear advances.
    // Superseded for new content by eventId (a declarative step list); the hook
    // is kept as the executor's `Custom` step and for legacy objectives.
    uint32 onArriveHook{0};

    // Objective only: id into DungeonEventRegistry for a declarative step-based
    // event (move/use-GO/gossip/wait/kill) run on arrival. 0 => no event; the
    // objective falls back to onArriveHook (or arrive-then-continue). When set it
    // takes precedence over onArriveHook. See DungeonEventExecutor.
    uint32 eventId{0};

    // Boss only (roster patch authoring aid): copy this base entry's
    // encounterIndex onto this added boss so it borrows that boss's kill-bit
    // for completion detection. Resolved and cleared by BossRosterRegistry::Apply.
    uint32 inheritCompletionFrom{0};

    // Completion via the instance script's OWN boss-state slot
    // (InstanceScript::GetBossState(index) == DONE), for a boss that has NO
    // DungeonEncounter.dbc entry — so it never sets a GetCompletedEncounterMask
    // bit and can't be tracked the normal way (e.g. The Mechanar's two
    // Gatewatchers: door-gating mini-bosses the DBC doesn't list, but the
    // instance script does track as DATA_GATEWATCHER_* / bosses[] slots). This is
    // the ONE sanctioned use of GetBossState (which is otherwise banned here — its
    // DATA_* index space only aligns with the DBC encounterIndex by coincidence):
    // the index is authored EXPLICITLY from the instance header, not reused from
    // encounterIndex, so there is no index-space confusion. -1 => not used
    // (completion keys on the DBC mask / corpse as usual).
    //
    // Also valid on an OBJECTIVE whose event runs a whole encounter (Karazhan's
    // Opera and Nightbane urn): the objective is finished once that slot is DONE,
    // so a re-entered instance does not replay a completed event.
    int32 doneBossStateIndex{-1};

    // Completion via an InstanceScript GetData() VALUE, for an encounter whose
    // script tracks it in neither a boss-state slot nor a DBC kill bit — the
    // anchor is finished while GetData(doneInstanceDataId) ==
    // doneInstanceDataValue. Karazhan's chess event is the case: its script
    // never calls SetBossState for it, its DBC credit (encounter 660, the Status
    // Bar) is a creature that never dies, and the ONLY completion signal the
    // core has is GetData(DATA_CHESS_EVENT) == DONE.
    //
    // READ LIVE, every time, never latched — the same contract as
    // doneBossStateIndex. It matters more here: `_chessEvent` is not saved, so
    // after an instance reload it reads NOT_STARTED again and the chess exit
    // door is shut; an anchor that had latched "done" would walk the raid at
    // Prince through a closed door. -1 => not used.
    int32 doneInstanceDataId{-1};
    uint32 doneInstanceDataValue{0};

    // Clear-ORDER override. When >= 0 the clear orders this anchor by this value
    // INSTEAD of encounterIndex, while completion still keys on encounterIndex
    // (the real DBC kill-bit, untouched). Lets a roster patch reorder a real
    // boss whose DBC bit doesn't match the intended clear path without breaking
    // its kill detection — e.g. Stratholme's Magistrate Barthilas (bit 10) must
    // be reached BEFORE the ziggurats (bits 7-9). -1 => order by encounterIndex
    // (every auto-derived boss and every anchor that doesn't opt in).
    int32 orderOverride{-1};

    // Present but SKIPPED BY DESIGN: the roster keeps the row (so the panel can
    // show why the clear passes it over) but the target picker never selects it
    // — authored for content bots cannot or must not drive (vehicle-piloted
    // bosses, externally-controlled set pieces like Karazhan's Chess). Unlike
    // the per-run `dc skip` set this is data, not player state: it applies to
    // every run of the map. Stamped by a BossRosterPatch::skipByDesign entry.
    bool skipByDesign{false};
};

// The value the clear ORDERS an anchor by: the explicit orderOverride when set,
// else the DBC encounterIndex. Completion detection never uses this — it keys on
// encounterIndex directly — so reordering a boss can't desync its kill-bit.
inline uint32 BossOrderKey(DungeonBossInfo const& b)
{
    return b.orderOverride >= 0 ? static_cast<uint32>(b.orderOverride)
                                : b.encounterIndex;
}

// Which instance-script signal says an anchor is finished.
enum class DcAnchorDoneVia : uint8
{
    None,
    BossState,     // doneBossStateIndex reads DONE
    InstanceData,  // GetData(doneInstanceDataId) == doneInstanceDataValue
};

// Does the instance script say this anchor is finished — by its boss-state slot
// (doneBossStateIndex) or by a GetData value (doneInstanceData)? Pure: the caller
// reads the two live values off the InstanceScript (DcAnchorDoneByInstance in
// Util/DcAnchorDone.h) so the rule itself is testable without one. `bossState` is
// the raw EncounterState of slot doneBossStateIndex (DONE == 3); `instanceData`
// is GetData(doneInstanceDataId). Either configured signal reading done is enough.
inline DcAnchorDoneVia AnchorDoneByInstanceValues(DungeonBossInfo const& b, uint32 bossState,
                                                  uint32 instanceData)
{
    constexpr uint32 ENCOUNTER_DONE = 3;
    if (b.doneBossStateIndex >= 0 && bossState == ENCOUNTER_DONE)
        return DcAnchorDoneVia::BossState;
    if (b.doneInstanceDataId >= 0 && instanceData == b.doneInstanceDataValue)
        return DcAnchorDoneVia::InstanceData;
    return DcAnchorDoneVia::None;
}

#endif
