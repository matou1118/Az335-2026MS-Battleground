/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearHazardsValue.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Data/DcHazardRegistry.h"

namespace
{
    // Emitters only matter for points the party might actually stand on, and
    // every consumer (camp anchors, standoff rings, skirt legs) works within a
    // pull's reach of the bot. One sight distance is comfortably wider than the
    // largest registered keep-out radius plus a camp drag, and keeps the grid
    // visitor much cheaper than the 4x FarTargets sweep — this runs on every
    // bot, on every map with a row.
    float HazardRange() { return sPlayerbotAIConfig.sightDistance; }

    // The grid check. Membership is decided INSIDE the visitor rather than by
    // AcceptUnit afterwards, so the searcher's std::list only ever holds actual
    // hazards. The old shape — AnyUnitInObjectRangeCheck, then filter — allocated
    // one list node and ran one registry lookup for EVERY unit within a sight
    // distance, which in a dungeon is the whole party plus every mob in the room,
    // once per bot per 500ms cache miss. The accepted set is unchanged: the same
    // three conditions, in the cheap-first order.
    //
    // Alive, exactly as AnyUnitInObjectRangeCheck required: the 500ms window can
    // still hand a consumer an emitter that has died since, which is deliberate
    // (see DcHazard::LiveHazard::alive), but the sweep itself has never collected
    // corpses and this must not start.
    struct RegisteredHazardInRangeCheck
    {
        RegisteredHazardInRangeCheck(WorldObject const* source, float range)
            : _source(source), _range(range)
        {
        }

        bool operator()(Unit* u) const
        {
            return u && u->IsCreature() &&
                   DcHazardRegistry::Find(u->GetMapId(), u->GetEntry()) != nullptr &&
                   u->IsAlive() && _source->IsWithinDistInMap(u, _range);
        }

        WorldObject const* _source;
        float _range;
    };
}

DungeonClearHazardsValue::DungeonClearHazardsValue(PlayerbotAI* botAI)
    // 500ms mirrors FarTargets. The registered emitters are rooted or inert, so
    // even this is generous — but a cache that can go stale across a teleport
    // would be a silent hazard miss, and 500ms bounds that to one tick's worth.
    : NearestUnitsValue(botAI, DcKey::Hazards, HazardRange(), /*ignoreLos*/ true, 500)
{
}

void DungeonClearHazardsValue::FindUnits(std::list<Unit*>& targets)
{
    // Cheap early-out: no creature rows for this map, no sweep. The walkers in
    // DcHazard already gate on this, so today the value is never pulled off an
    // emitter map at all — but the gate belongs on the sweep itself, exactly as
    // the ground-pool and trap values have it.
    if (!DcHazardRegistry::HasEmitters(bot->GetMapId()))
        return;

    // A plain unit searcher, not AnyUnfriendly*: the corpse bombs sit on a
    // neutral faction and an unfriendly-only searcher never returns them. The
    // registry membership test rides inside the check so the list stays small.
    RegisteredHazardInRangeCheck check(bot, range);
    Acore::UnitListSearcher<RegisteredHazardInRangeCheck> searcher(bot, targets, check);
    Cell::VisitObjects(bot, searcher, range);
}

bool DungeonClearHazardsValue::AcceptUnit(Unit* unit)
{
    if (!unit || !unit->IsCreature())
        return false;

    return DcHazardRegistry::Find(unit->GetMapId(), unit->GetEntry()) != nullptr;
}
