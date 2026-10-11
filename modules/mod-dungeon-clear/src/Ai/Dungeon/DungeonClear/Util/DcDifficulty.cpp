/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Util/DcDifficulty.h"

#include "Map.h"

namespace DcDifficulty
{
    DcDiffKey Of(Map const* map)
    {
        if (!map)
            return {};
        return { map->IsRaid(), static_cast<uint8>(map->GetDifficulty()) };
    }
}
