/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcRunProgress.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

#include <optional>
#include <unordered_set>

#include "AiObjectContext.h"
#include "InstanceScript.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

namespace
{
    // Progress epsilon (yards): the tank must close on the next anchor by at least
    // this much versus its closest-ever approach to count as progress. Matches the
    // harness livelock net so the two agree on what "closing distance" means.
    constexpr float DC_PROGRESS_EPSILON_YD = 1.0f;
}

namespace DcRunProgress
{
    bool Detect(Player* leader, PlayerbotAI* leaderAI, Mark& mark)
    {
        if (!leader || !leaderAI)
            return false;

        AiObjectContext* ctx = leaderAI->GetAiObjectContext();
        if (!ctx)
            return false;

        bool progressed = false;

        uint32 encounterMask = mark.mask;
        if (InstanceScript* inst = DcTargeting::GetInstanceScript(leader))
            encounterMask = inst->GetCompletedEncounterMask();
        if (encounterMask != mark.mask)
        {
            mark.mask = encounterMask;
            progressed = true;
        }

        std::size_t const anchors =
            ctx->GetValue<std::unordered_set<uint32>&>(DcKey::ClearedAnchors)->Get().size();
        if (static_cast<uint32>(anchors) != mark.anchors)
        {
            mark.anchors = static_cast<uint32>(anchors);
            progressed = true;
        }

        std::optional<DungeonBossInfo> const next =
            ctx->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
        if (next.has_value() && next->mapId == leader->GetMapId())
        {
            // Re-arm on target change: distance to a NEW anchor is unrelated to the
            // best held for the old one, and would otherwise read as an instant
            // regression that never recovers.
            if (next->entry != mark.bossEntry)
            {
                mark.bossEntry = next->entry;
                mark.bestDist = -1.0f;
            }
            float const dist = leader->GetDistance(next->x, next->y, next->z);
            if (mark.bestDist < 0.0f || dist < mark.bestDist - DC_PROGRESS_EPSILON_YD)
            {
                mark.bestDist = dist;
                progressed = true;
            }
        }

        return progressed;
    }

    void Stamp(Mark& mark, std::uint32_t nowMs)
    {
        mark.stampMs = nowMs ? nowMs : 1;
    }

    bool Stale(Mark const& mark, std::uint32_t nowMs, std::uint32_t windowMs)
    {
        if (mark.stampMs == 0 || windowMs == 0)
            return false;
        // Unsigned wraparound is correct here: both stamps come off the same
        // monotonic getMSTime() clock, so the difference stays right across the
        // 32-bit rollover.
        return (nowMs - mark.stampMs) >= windowMs;
    }
}
