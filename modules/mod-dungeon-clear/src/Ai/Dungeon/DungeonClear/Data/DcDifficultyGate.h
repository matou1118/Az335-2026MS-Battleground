/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCDIFFICULTYGATE_H
#define _PLAYERBOT_DCDIFFICULTYGATE_H

#include "Define.h"

#include "Ai/Dungeon/DungeonClear/Util/DcDifficulty.h"

// Difficulty gate shared by the dungeon-events framework (DungeonEvent::gate)
// and the boss-roster patch table (BossRosterPatch::gate). A heroic-only
// boss (Sethekk's Anzu, Shattered Halls' Porung) or a heroic-only event gets
// HeroicOnly so it never surfaces on a normal run, and vice versa. Matching
// takes the DcDiffKey (not a raw Difficulty) because the raw values collide
// across map types — on a raid map difficulty 1 is 25-man NORMAL, which a raw
// `== DUNGEON_DIFFICULTY_HEROIC` compare would read as heroic. Normal/Heroic
// here mean the heroic TIER of whatever the map is: dungeon heroic, or raid
// heroic (10H/25H) when WotLK raids arrive. Classic raids are all difficulty
// 0 and read as normal-tier.
enum class DcDifficultyGate : uint8
{
    Any,         // every difficulty (the default — most content is shared)
    NormalOnly,  // non-heroic tier only
    HeroicOnly,  // heroic tier only
};

inline bool DcGateMatches(DcDifficultyGate gate, DcDiffKey key)
{
    switch (gate)
    {
        case DcDifficultyGate::NormalOnly:
            return !key.IsHeroic();
        case DcDifficultyGate::HeroicOnly:
            return key.IsHeroic();
        default:
            return true;
    }
}

#endif
