/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcCombatPurge.h"

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/DcCombatPurgeRegistry.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcBossStandDown.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRunProgress.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStatusPublisher.h"

#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "AiObjectContext.h"
#include "Creature.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"
#include "Unit.h"

namespace
{
    // The sweep is a second-granularity clock, so a per-world-tick walk of the
    // active tanks buys nothing. Same throttle shape as the status pusher.
    constexpr uint32 DC_PURGE_SWEEP_MS = 1000;
    uint32 g_purgeSweepAccumMs = 0;

    // Post-purge target bars: (instanceId, entry) -> getMSTime() the bar expires.
    //
    // Keyed by INSTANCE, not by leader: every bot in the instance must see the
    // same answer, and the bar is read from whichever bot is currently picking a
    // target (DcTargetExclusionRegistry hands us the picker, which may be any
    // follower). A plain map is right at this size — a bar exists only in the
    // seconds after a purge, on the one map that has rows.
    std::map<std::pair<uint32, uint32>, uint32> g_purgeBars;
    std::mutex g_purgeBarsMutex;

    void ArmBar(uint32 instanceId, uint32 entry, uint32 untilMs)
    {
        std::lock_guard<std::mutex> lock(g_purgeBarsMutex);
        uint32& slot = g_purgeBars[{ instanceId, entry }];
        if (slot == 0 || static_cast<int32>(untilMs - slot) > 0)
            slot = untilMs;
    }

    // The purge window in ms, or 0 when the operator has switched the failsafe
    // off. Doubles as the bar's duration: the party must not be able to re-open
    // the same unendable fight faster than the clock that detects it.
    uint32 PurgeWindowMs(Player* bot)
    {
        return DcSettings::GetUInt(bot, "UnreachableCombatPurgeSecs") * 1000;
    }
}

namespace DcCombatPurge
{
    bool IsBarred(Player* bot, uint32 entry)
    {
        if (!bot)
            return false;

        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> lock(g_purgeBarsMutex);
        auto const it = g_purgeBars.find({ bot->GetInstanceId(), entry });
        if (it == g_purgeBars.end())
            return false;
        if (static_cast<int32>(it->second - now) <= 0)
        {
            g_purgeBars.erase(it);
            return false;
        }
        return true;
    }

    void ClearBars(Player* bot)
    {
        if (!bot)
            return;

        uint32 const instanceId = bot->GetInstanceId();
        std::lock_guard<std::mutex> lock(g_purgeBarsMutex);
        for (auto it = g_purgeBars.begin(); it != g_purgeBars.end();)
            it = (it->first.first == instanceId) ? g_purgeBars.erase(it) : std::next(it);
    }

    uint32 Purge(Player* leader)
    {
        if (!leader || !leader->IsInWorld())
            return 0;

        uint32 const mapId = leader->GetMapId();
        if (!DcCombatPurgeRegistry::HasRowsFor(mapId))
            return 0;

        // The party, as one list. The leader is included; a same-map HUMAN is too,
        // and deliberately — the raider holds a reference on the human as well,
        // and a human left flagged keeps the party gates shut just as a bot does.
        // Walking their references is safe because nothing below acts on a member:
        // the only thing this function stops is the CREATURE.
        std::vector<Player*> party;
        party.push_back(leader);
        if (Group* group = leader->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == leader || !member->IsInWorld())
                    continue;
                if (member->GetMapId() != mapId)
                    continue;
                party.push_back(member);
            }

        // COLLECT BEFORE CLEARING. CombatReference::EndCombat deletes the
        // reference it is iterating and CombatStop mutates the attacker set, so
        // both containers are unsafe to walk while dropping. GUID-deduped because
        // the two sources overlap and several members hold the same raider.
        //
        // And the holders live in the COMBAT MANAGER, not in getAttackers(): that
        // set holds only units whose CURRENT VICTIM is this member, and a mob
        // stranded across water has no reachable victim at all, so it is not in it
        // — while its CombatReference goes on holding the member flagged. The
        // attacker set stays as a superset guard for anything mid-swing that has
        // not registered a reference yet.
        std::vector<Creature*> doomed;
        std::unordered_set<uint64> seen;
        auto const consider = [&doomed, &seen, mapId](Unit* u)
        {
            if (!u || !u->IsInWorld())
                return;
            if (!seen.insert(u->GetGUID().GetRawValue()).second)
                return;
            Creature* creature = u->ToCreature();
            if (!creature)
                return;
            // The evade read the Evading rows are gated on. Same predicate as
            // DcCombatFlag and the teardown diag's EVADING column, so the blame
            // table a triage reads and the gate that acted on it can never
            // disagree about what evading means.
            if (!DcCombatPurgeRegistry::IsPurgeable(
                    mapId, creature->GetEntry(),
                    creature->GetCombatManager().IsInEvadeMode()))
                return;
            doomed.push_back(creature);
        };

        for (Player* member : party)
        {
            for (auto const& kv : member->GetCombatManager().GetPvECombatRefs())
                if (CombatReference* const ref = kv.second)
                    consider(ref->GetOther(member));
            for (Unit* const attacker : member->getAttackers())
                consider(attacker);
        }

        if (doomed.empty())
            return 0;

        uint32 const now = getMSTime();
        uint32 const barMs = PurgeWindowMs(leader);
        std::unordered_set<uint64> dropped;
        uint32 cleared = 0;

        for (Creature* creature : doomed)
        {
            if (!creature->IsInWorld())
                continue;

            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] combat purge: dropping {} ({}) at ({:.1f},{:.1f},{:.1f}), "
                     "{:.0f}yd from the tank, {}% hp — it can neither be reached nor let go",
                     leader->GetName(), creature->GetName(), creature->GetEntry(),
                     creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(),
                     leader->GetDistance(creature),
                     creature->GetMaxHealth()
                         ? 100 * creature->GetHealth() / creature->GetMaxHealth()
                         : 0);

            dropped.insert(creature->GetGUID().GetRawValue());
            ArmBar(leader->GetInstanceId(), creature->GetEntry(), now + barMs);

            // Both directions or it does not stick: clearing only the party's side
            // leaves the creature's threat reference to re-flag them on its next
            // update. Stopping the CREATURE ends the reference for every member at
            // once, which is why nothing here touches a member's combat.
            creature->GetThreatMgr().ClearAllThreat();
            creature->CombatStop(true);
            ++cleared;
        }

        if (!cleared)
            return 0;

        // Drop the stale targets. CombatStop leaves `current target` set, and a bot
        // still pointed at a purged mob re-attacks it on the very next tick — which
        // both re-opens the fight and sends the party walking at water they cannot
        // cross (dc-relocation-must-drop-the-target). Bots only: a human's target is
        // never ours to clear, and the bar below is what keeps the bots off it.
        for (Player* member : party)
        {
            PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
            if (!memberAI)
                continue;
            Unit* const victim = member->GetVictim();
            bool const onDoomed =
                victim && dropped.count(victim->GetGUID().GetRawValue()) != 0;
            AiObjectContext* ctx = memberAI->GetAiObjectContext();
            Unit* const picked =
                ctx ? ctx->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Get() : nullptr;
            bool const pickedDoomed =
                picked && dropped.count(picked->GetGUID().GetRawValue()) != 0;
            if (!onDoomed && !pickedDoomed)
                continue;

            member->AttackStop();
            member->SetTarget();
            if (ctx)
                ctx->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(nullptr);
        }

        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] combat purge: {} unreachable holder(s) dropped after the run stalled; "
                 "barred from target selection for {}s",
                 leader->GetName(), cleared, barMs / 1000);

        if (PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader))
            DcStatusPublisher::SendAddonMessage(
                leaderAI, "CHAT\tBroke combat with " + std::to_string(cleared) +
                              (cleared == 1 ? " unreachable enemy." : " unreachable enemies."));

        return cleared;
    }

    void Tick(uint32 diff)
    {
        g_purgeSweepAccumMs += diff;
        if (g_purgeSweepAccumMs < DC_PURGE_SWEEP_MS)
            return;
        g_purgeSweepAccumMs = 0;

        std::vector<ObjectGuid> const tanks = DcStatusPublisher::ActiveTanks();
        if (tanks.empty())
            return;

        uint32 const now = getMSTime();

        for (ObjectGuid guid : tanks)
        {
            Player* leader = ObjectAccessor::FindPlayer(guid);
            if (!leader || !leader->IsInWorld())
                continue;
            // DELIBERATELY NOT gated on the leader being alive. A dead tank is the
            // state this failsafe is most needed in — the party is held in a fight
            // that cannot end, so it cannot rest, cannot regroup, and cannot rez the
            // one member whose corpse the run is waiting on. Skipping the sweep
            // there switched the purge off in exactly the runs it was written for
            // (Gundrak tp-20260830-231921-1, five of them).
            //
            // Safe because nothing below acts on a MEMBER: Purge() only ever stops
            // CREATURES, and it reads the leader for its map, group, instance and
            // AI — all of which a corpse still has. Its own combat references are
            // empty by then, which costs one no-op walk.
            // Only maps that have a row can deadlock this way, and the check is a
            // handful of integer compares — take it before touching the run state.
            if (!DcCombatPurgeRegistry::HasRowsFor(leader->GetMapId()))
                continue;

            PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
            if (!leaderAI)
                continue;

            DcRunState& run = DcRun::Of(leaderAI);
            if (!run.enabled)
                continue;

            uint32 const windowMs = PurgeWindowMs(leader);
            if (!windowMs)
                continue;

            // A pause is an intentional hold, not a stall — and a raid encounter is
            // the playerbots strategy's fight, whose own pacing this must not read
            // as a freeze. Same clock treatment as the stranded failsafe gives
            // them, so the purge re-arms clean on resume.
            if (run.paused || DcBossStandDown::IsActive(leader) || DcKarazhan::ChessIsOn(leader))
            {
                DcRunProgress::Stamp(run.purgeProgress, now);
                continue;
            }

            // COMBAT-BLIND, unlike DcStrandedRecovery's clock: see the header. The
            // fight IS the thing being detected, so engagement must not re-arm it.
            bool const progressed = DcRunProgress::Detect(leader, leaderAI, run.purgeProgress);
            if (progressed || run.purgeProgress.stampMs == 0)
            {
                DcRunProgress::Stamp(run.purgeProgress, now);
                continue;
            }

            if (!DcRunProgress::Stale(run.purgeProgress, now, windowMs))
                continue;

            // Re-arm before acting, whether or not anything was found. A stalled run
            // with no registered holder on it is stalled for some other reason and
            // is not this failsafe's to solve — re-stamping keeps it from re-walking
            // every party member's combat references every second for the rest of
            // the run.
            DcRunProgress::Stamp(run.purgeProgress, now);
            Purge(leader);
        }
    }
}
