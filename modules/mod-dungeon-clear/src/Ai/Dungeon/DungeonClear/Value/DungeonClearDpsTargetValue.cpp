/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearDpsTargetValue.h"

#include "Player.h"
#include "Playerbots.h"
#include "Unit.h"
#include "Ai/Dungeon/DungeonClear/Data/DcTargetExclusionRegistry.h"

namespace
{
    // Did the stock pick land on something the run is barred from damaging? The
    // map test comes first and is the answer everywhere but Blackwing Lair, so a
    // bot in a dungeon pays one scan of a one-row table per pick.
    bool BarredPick(Player* bot, Unit* picked)
    {
        if (!bot || !picked)
            return false;

        uint32 const mapId = bot->GetMapId();
        return DcTargetExclusionRegistry::HasRowsFor(mapId) &&
               DcTargetExclusionRegistry::IsExcluded(bot, mapId, picked->GetEntry());
    }
}

Unit* DungeonClearDpsTargetValue::Calculate()
{
    Unit* const stock = DpsTargetValue::Calculate();
    if (!BarredPick(bot, stock))
        return stock;

    // The raid icon handed us a creature we may not kill. Ask the same picker
    // again with the icon disarmed: what comes back is the ordinary attacker
    // sweep, which honours the exclusion set, so the DPS lands on the adds — or on
    // nothing at all, which is the correct answer when the barred creature is the
    // only thing in the fight.
    Unit* const legal = unmarked.Calculate();
    return BarredPick(bot, legal) ? nullptr : legal;
}

Unit* DungeonClearDpsAoeTargetValue::Calculate()
{
    Unit* const stock = DpsAoeTargetValue::Calculate();
    if (!BarredPick(bot, stock))
        return stock;

    Unit* const legal = unmarked.Calculate();
    return BarredPick(bot, legal) ? nullptr : legal;
}
