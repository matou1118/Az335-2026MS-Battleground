/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCDIFFICULTY_H
#define _PLAYERBOT_DCDIFFICULTY_H

#include "DBCEnums.h"
#include "Define.h"

class Map;

// The module's difficulty key: (isRaid, raw difficulty) as ONE value, because a
// raw Difficulty is meaningless without the map type it came from. The core
// overloads the same small integers across two unrelated enums — on a dungeon
// map difficulty 1 is DUNGEON_DIFFICULTY_HEROIC, on a raid map the SAME 1 is
// RAID_DIFFICULTY_25MAN_NORMAL — so any predicate written as
// `GetDifficulty() == DUNGEON_DIFFICULTY_HEROIC` silently classifies a 25-man
// normal raid as heroic. Two such bugs shipped (DcSettings, DcEngageGeometry)
// before this key existed; every new difficulty test goes through DcDiffKey so
// the collision is impossible to reintroduce.
//
// The key and its predicates are pure, header-only arithmetic on the two
// fields, so kernel tests exercise the whole matrix without a Map; only the
// Of(Map*) reader lives in the .cpp (keeping Map.h out of this header and the
// registry headers that include it).
struct DcDiffKey
{
    bool  isRaid     = false;
    uint8 difficulty = 0;  // raw Map::GetDifficulty(); meaning depends on isRaid

    // Heroic TIER, decoded per map type — mirrors Map::IsHeroic().
    bool IsHeroic() const
    {
        return isRaid ? difficulty >= RAID_DIFFICULTY_10MAN_HEROIC
                      : difficulty >= DUNGEON_DIFFICULTY_HEROIC;
    }

    // 25-man raid (normal or heroic) — mirrors Map::Is25ManRaid().
    bool Is25Man() const
    {
        return isRaid && (difficulty & RAID_DIFFICULTY_MASK_25MAN);
    }

    bool operator==(DcDiffKey const& o) const
    {
        return isRaid == o.isRaid && difficulty == o.difficulty;
    }

    // Named factories so a call site reads as the map type it means — a bare
    // `{false, 1}` hides exactly the encoding this type exists to expose.
    static DcDiffKey Dungeon(uint32 difficulty)
    {
        return { false, static_cast<uint8>(difficulty) };
    }
    static DcDiffKey Raid(uint32 difficulty)
    {
        return { true, static_cast<uint8>(difficulty) };
    }
};

namespace DcDifficulty
{
    // Defined in DcDifficulty.cpp so this header stays free of Map.h — it is
    // included from the Data registry headers, whose test TUs must not have the
    // whole grid system dumped into their namespace. null map -> default key
    // (dungeon normal).
    DcDiffKey Of(Map const* map);
}

#endif  // _PLAYERBOT_DCDIFFICULTY_H
