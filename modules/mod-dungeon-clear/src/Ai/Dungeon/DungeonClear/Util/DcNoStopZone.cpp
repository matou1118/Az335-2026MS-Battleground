/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcNoStopZone.h"

#include <cstddef>
#include <optional>

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTickMemo.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"

#include "AiObjectContext.h"
#include "Map.h"
#include "Player.h"
#include "Timer.h"
#include "Value.h"

namespace DcNoStopZone
{
    float DistSqToSegment(float px, float py, float pz,
                          WaypointHint const& a, WaypointHint const& b)
    {
        float const abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
        float const apx = px - a.x, apy = py - a.y, apz = pz - a.z;
        float const denom = abx * abx + aby * aby + abz * abz;

        float t = 0.0f;
        if (denom > 0.0001f)
        {
            t = (apx * abx + apy * aby + apz * abz) / denom;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        }
        float const dx = apx - abx * t, dy = apy - aby * t, dz = apz - abz * t;
        return dx * dx + dy * dy + dz * dz;
    }

    bool CoversPoint(std::vector<WaypointHint> const& route,
                     float px, float py, float pz, float radius)
    {
        float const radiusSq = radius * radius;
        for (std::size_t i = 0; i + 1 < route.size(); ++i)
        {
            if (!HasFlag(route[i].flags, AnchorFlag::NO_STOP))
                continue;
            if (DistSqToSegment(px, py, pz, route[i], route[i + 1]) <= radiusSq)
                return true;
        }
        return false;
    }

    bool IsInNoStopZone(Player* bot, AiObjectContext* context)
    {
        if (!bot || !context || !bot->IsInWorld())
            return false;

        DcTickMemo& memo = context->GetValue<DcTickMemo&>(DcKey::TickMemo)->Get();
        memo.EnsureFresh(getMSTime());
        if (memo.noStopZone >= 0)
            return memo.noStopZone == 1;

        auto const compute = [bot, context]()
        {
            Map const* const map = bot->GetMap();
            if (!map || !map->IsDungeon())
                return false;

            // Resolve the route exactly as Advance does — (map, difficulty, the
            // current objective's entry) — so the zone follows the party from
            // objective to objective with no state of its own, and every dungeon
            // that has authored no span answers false at the first miss.
            std::optional<DungeonBossInfo> const next =
                context->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
            if (!next.has_value() || next->mapId != bot->GetMapId())
                return false;

            std::vector<WaypointHint> const* const route =
                DungeonClearRouteRegistry::Get(next->mapId, map->GetDifficulty(), next->entry);
            if (!route)
                return false;

            return CoversPoint(*route, bot->GetPositionX(), bot->GetPositionY(),
                               bot->GetPositionZ(), DC_NO_STOP_CORRIDOR);
        };

        bool const inZone = compute();
        memo.noStopZone = inZone ? 1 : 0;
        return inZone;
    }
}
