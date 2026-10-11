/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Util/DcBossStandDown.h"

#include <vector>

#include "CombatManager.h"
#include "Creature.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

namespace
{
    // (b) the no-script fallback: is any member held in PvE combat by a unit
    // whose entry is a roster BOSS? Walked off the leader's roster value so the
    // answer matches what the run is actually clearing. (A unit merely SHARING
    // a boss's encounter credit is not matched here — every scripted classic
    // raid answers through IsEncounterInProgress first, so this fallback only
    // carries instances with no usable script.)
    bool RosterBossHoldsAnyMember(Player* leader, PlayerbotAI* leaderAI)
    {
        std::vector<DungeonBossInfo> const& bosses =
            leaderAI->GetAiObjectContext()
                ->GetValue<std::vector<DungeonBossInfo>>(DcKey::DungeonBosses)
                ->Get();
        if (bosses.empty())
            return false;

        auto const isRosterBoss = [&bosses](uint32 entry)
        {
            for (DungeonBossInfo const& info : bosses)
                if (info.kind == DungeonAnchorKind::Boss && info.entry == entry)
                    return true;
            return false;
        };

        auto const heldByBoss = [&](Player* member)
        {
            if (!member || member->GetMap() != leader->GetMap())
                return false;
            for (auto const& kv : member->GetCombatManager().GetPvECombatRefs())
            {
                Unit* other = kv.second->GetOther(member);
                if (other && other->GetTypeId() == TYPEID_UNIT &&
                    isRosterBoss(other->GetEntry()))
                    return true;
            }
            return false;
        };

        Group* group = leader->GetGroup();
        if (!group)
            return heldByBoss(leader);
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (heldByBoss(ref->GetSource()))
                return true;
        return false;
    }

    // The raw per-tick observation both stand-down signals fold into.
    bool EncounterSignal(Player* leader, PlayerbotAI* leaderAI)
    {
        InstanceScript* const script = DcTargeting::GetInstanceScript(leader);
        if (script && script->IsEncounterInProgress())
            return true;
        return RosterBossHoldsAnyMember(leader, leaderAI);
    }

    // (c) a five-man TABLE boss: alive and in combat. Neither signal above can see
    // one — The Oculus never calls SetBossState (IsEncounterInProgress is always
    // false) and Eregos is not a roster boss row — so the instance's own guid slot
    // resolves him.
    bool TableBossEngaged(Player* at, DcBossStandDown::TableRow const& row)
    {
        InstanceScript* const script = DcTargeting::GetInstanceScript(at);
        if (!script)
            return false;
        ObjectGuid const guid = script->GetGuidData(row.guidSlot);
        if (guid.IsEmpty())
            return false;
        Creature* const boss = ObjectAccessor::GetCreature(*at, guid);
        return boss && boss->IsAlive() && boss->GetEntry() == row.bossEntry && boss->IsInCombat();
    }
}

namespace DcBossStandDown
{
    bool IsActive(Player* bot)
    {
        if (!bot)
            return false;
        Map* const map = bot->GetMap();
        if (!map)
            return false;
        TableRow const* const tableRow = FindTableRow(map->GetId());
        if (!map->IsRaid() && !tableRow)
            return false;

        // The verdict is the RUN's, so it lives on the run's leader. No leader
        // (DC off / not in a DC party) -> nothing of DC's needs gating.
        Player* const leader = DcLeaderSignal::FindRunOwner(bot);
        if (!leader)
            return false;
        PlayerbotAI* const leaderAI = GET_PLAYERBOT_AI(leader);
        if (!leaderAI)
            // Human-led edge: no leader-owned state to hysteresis through. Gate
            // on the instant script signal alone — still correct for every
            // scripted raid, just without the exit grace.
            {
                if (tableRow)
                    return TableBossEngaged(bot, *tableRow);
                InstanceScript* const script = DcTargeting::GetInstanceScript(bot);
                return script && script->IsEncounterInProgress();
            }

        DcRunState& run = DcRun::Of(leaderAI);
        uint32 const now = getMSTime();

        // Per-tick memo: the first member through this window evaluates for
        // everyone (same dedupe contract as DcTickMemo — never across ticks).
        if (run.standDownEvalMs && now - run.standDownEvalMs < kEvalWindowMs)
            return run.standDownActive;
        run.standDownEvalMs = now;

        bool const signal = tableRow ? TableBossEngaged(leader, *tableRow) : EncounterSignal(leader, leaderAI);
        Verdict const v = Update(run.standDownActive, run.standDownSignalMs, signal, now);
        run.standDownActive   = v.active;
        run.standDownSignalMs = v.lastSignalMs;
        return v.active;
    }
}
