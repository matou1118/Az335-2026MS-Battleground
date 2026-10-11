/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONBOSSESVALUE_H
#define _PLAYERBOT_DUNGEONBOSSESVALUE_H

#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Value.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

class PlayerbotAI;
class Player;

class DungeonBossesValue : public CalculatedValue<std::vector<DungeonBossInfo>>
{
public:
    DungeonBossesValue(PlayerbotAI* botAI)
        : CalculatedValue<std::vector<DungeonBossInfo>>(botAI, DcKey::DungeonBosses, 5)
    {
    }

    // The roster Calculate() builds (roster patches, faction rules, navmesh snap)
    // WITHOUT the wing filter: every wing's bosses on a split map. Uncached — for
    // the boss panel and `dc go`, which on a per-run-wing map (Blackrock Spire)
    // show and accept the other wing's bosses too.
    static std::vector<DungeonBossInfo> AllWings(Player* bot);

protected:
    std::vector<DungeonBossInfo> Calculate() override;
};

#endif
