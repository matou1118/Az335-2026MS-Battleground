/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcFirstContact.h"

#include "DungeonClearUtil.h"   // DC_PULL_* log macros
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"

#include "CombatManager.h"
#include "Creature.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StringFormat.h"
#include "Unit.h"

#include <optional>
#include <string>

namespace
{
    // How many party members were ALREADY flagged when this one was grabbed. 0 is
    // the signature of a fresh pull; anything else means this contact joined a fight
    // that was already running, which is the case the pull record cannot show.
    uint32 CountPartyAlreadyInCombat(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return 0;
        uint32 count = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() &&
                member->GetMapId() == bot->GetMapId() && member->IsInCombat())
                ++count;
        }
        return count;
    }
}

void DcFirstContact::OnEnterCombat(Player* bot, Unit* enemy)
{
    if (!bot || !enemy)
        return;

    // Same cheapest-gate-first discipline as DcPullBrake, and for the same reason:
    // this hook fires for every player on the realm on every 0->1 combat transition.
    Map const* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    // Only inside a live, unpaused run — a paused run is a deliberate hand-off to
    // the player and their fights are not the harness's business.
    Player* owner = DcLeaderSignal::FindRunOwner(bot);
    PlayerbotAI* ownerAI = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
    if (!ownerAI)
        return;
    DcRunState const& run = DcRun::Of(ownerAI);
    if (!run.enabled || run.paused)
        return;

    AiObjectContext* ctx = botAI->GetAiObjectContext();
    if (!ctx)
        return;

    Creature const* asCreature = enemy->ToCreature();
    uint32 const entry = enemy->GetEntry();

    // Is this the thing the run is currently walking towards? The single most
    // load-bearing field: an objective appearing here at all means it was NOT
    // pulled, because a pulled one goes through the pull FSM and gets a pull row.
    bool isObjective = false;
    std::optional<DungeonBossInfo> const next =
        ctx->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
    if (next.has_value() && next->entry == entry)
        isObjective = true;

    // How far the enemy is from where it SPAWNED. A mob standing on its home
    // position was reached by us; one a long way off came to us, or was dragged.
    float homeDist = -1.0f;
    if (asCreature)
        homeDist = asCreature->GetDistance(asCreature->GetHomePosition());

    DcPullContext const& pull = ctx->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
    uint32 const refs =
        static_cast<uint32>(enemy->GetCombatManager().GetPvECombatRefs().size());
    uint32 const alreadyFighting = CountPartyAlreadyInCombat(bot);

    // INFO, and on the pull logger so dc_test_run.py's slice and SIGNALS section
    // pick it up next to the pull rows this is the missing half of. One line per
    // party member per fight — a handful per pack, not a per-tick stream.
    DC_PULL_INFO("[DC:{}] first contact: {}{} (entry {}) at {:.1f}yd, {} from its "
                 "spawn, refs={} | party already fighting: {} | pull phase {} camp {} "
                 "stage {}",
                 bot->GetName(), enemy->GetName(), isObjective ? " [OBJECTIVE]" : "",
                 entry, bot->GetExactDist(enemy),
                 homeDist >= 0.0f ? Acore::StringFormat("{:.1f}yd", homeDist)
                                  : std::string("n/a"),
                 refs, alreadyFighting,
                 static_cast<uint32>(pull.phase), pull.HasCamp() ? "set" : "none",
                 pull.scriptedStage);

    // An OBJECTIVE arriving with the party already fighting is the shape that has
    // no other record anywhere, so it gets its own greppable line at WARN. This is
    // the one that answers the Delrissa question: it fires, or it does not, and
    // either answer eliminates half the hypotheses.
    if (isObjective && alreadyFighting > 0 && pull.phase == DcPullPhase::Idle)
        LOG_WARN("playerbots.dungeonclear",
                 "[DC:{}] OBJECTIVE JOINED AN ONGOING FIGHT: {} (entry {}) at "
                 "{:.1f}yd with no pull in flight — {} other member(s) already "
                 "in combat", bot->GetName(), enemy->GetName(), entry,
                 bot->GetExactDist(enemy), alreadyFighting);

    // REMOTE AGGRO — combat established with something nobody can see, let alone
    // reach. Its own WARN because the INFO line above cannot be found by anyone
    // who does not already suspect it: the distance is one field of eleven, the
    // line is one of 285 in a bad run, and nothing about the shape says "this is
    // the thing that ends the run in ninety seconds".
    //
    // It ALWAYS means something is wrong. Nothing legitimate starts a fight from
    // beyond DC_ENGAGEMENT_RADIUS — mob spell reach tops out near 40yd — so a
    // contact out here is one of exactly three things, and the two extra fields
    // separate them:
    //
    //   * homeDist ~0 and far  -> aggro through geometry. The creature has not
    //     moved; something let it see us through a floor. BWL's approach is the
    //     worked example: anchors 5-12 are the ceiling of the upper suppression
    //     room, and in tr-20260830-142049-6 five holders contacted at 132.6-151.6yd
    //     while 0.0-4.6yd from their spawns, then walked 199.4yd into the raid.
    //   * homeDist large and far -> already being towed; somebody else's fight
    //     has been dragged onto us.
    //   * a BOSS at any distance -> a combat pulse. BossAI::_JustEngagedWith sets
    //     SetCombatPulseDelay(5) and DoZoneInCombat re-flags every player within
    //     250yd every five seconds, so the whole raid contacts it at once from
    //     wherever it happens to be standing.
    //
    // Threshold is DC_ENGAGEMENT_RADIUS itself, so this line and the flag test
    // agree on where "a fight" stops: everything this WARNs about is, by
    // construction, combat the rest of the module has already written off as
    // geometry — and that is exactly why nothing else reports it.
    if (bot->GetExactDist(enemy) > DC_ENGAGEMENT_RADIUS)
        LOG_WARN("playerbots.dungeonclear",
                 "[DC:{}] REMOTE AGGRO: {}{} (entry {}) opened combat from {:.1f}yd — "
                 "it is {} from its spawn, so it {} — {} other member(s) already in combat",
                 bot->GetName(), enemy->GetName(), isObjective ? " [OBJECTIVE]" : "", entry,
                 bot->GetExactDist(enemy),
                 homeDist >= 0.0f ? Acore::StringFormat("{:.1f}yd", homeDist)
                                  : std::string("n/a"),
                 homeDist >= 0.0f && homeDist < 5.0f
                     ? "reached us through geometry, not on foot"
                     : "is being towed",
                 alreadyFighting);
}

void DcFirstContact::OnCreatureEngage(Creature* creature, Unit* target)
{
    if (!creature || !target)
        return;

    // Realm-wide hook: cheapest gates first.
    Map const* map = creature->GetMap();
    if (!map || !map->IsDungeon())
        return;
    Player* bot = target->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!bot)
        return;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;
    Player* owner = DcLeaderSignal::FindRunOwner(bot);
    PlayerbotAI* ownerAI = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
    if (!ownerAI)
        return;
    DcRunState const& run = DcRun::Of(ownerAI);
    if (!run.enabled || run.paused)
        return;

    // A fresh fight is OnEnterCombat's line; only a join into a running one is
    // missing from the record.
    uint32 const alreadyFighting = CountPartyAlreadyInCombat(bot) + (bot->IsInCombat() ? 1 : 0);
    if (alreadyFighting == 0)
        return;

    Position const home = creature->GetHomePosition();
    float const homeDist = creature->GetDistance(home);

    // Nearest living party member: the one most likely to have walked into it.
    Player* nearest = nullptr;
    float nearestDist = 0.0f;
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (!m || !m->IsAlive() || m->GetMapId() != creature->GetMapId())
                continue;
            float const d = creature->GetExactDist(m);
            if (!nearest || d < nearestDist)
            {
                nearest = m;
                nearestDist = d;
            }
        }

    std::string campText = "none";
    if (AiObjectContext* ownerCtx = ownerAI->GetAiObjectContext())
    {
        DcPullContext const& pull =
            ownerCtx->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
        if (pull.HasCamp())
            campText = Acore::StringFormat("({:.1f},{:.1f},{:.1f}) {:.1f}yd off",
                                           pull.camp.GetPositionX(), pull.camp.GetPositionY(),
                                           pull.camp.GetPositionZ(),
                                           creature->GetExactDist(&pull.camp));
    }

    DC_PULL_INFO("[DC:{}] late joiner: {} (entry {}) engaged {} at {:.1f}yd (LOS {}) | "
                 "mob ({:.1f},{:.1f},{:.1f}) {:.1f}yd from spawn ({:.1f},{:.1f},{:.1f}) | "
                 "nearest member {} {:.1f}yd | tank {} ({:.1f},{:.1f},{:.1f}) | camp {} | "
                 "party fighting: {}",
                 bot->GetName(), creature->GetName(), creature->GetEntry(), target->GetName(),
                 creature->GetExactDist(target), creature->IsWithinLOSInMap(target) ? "y" : "n",
                 creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(),
                 homeDist, home.GetPositionX(), home.GetPositionY(), home.GetPositionZ(),
                 nearest ? nearest->GetName() : std::string("-"), nearestDist,
                 owner->GetName(), owner->GetPositionX(), owner->GetPositionY(),
                 owner->GetPositionZ(), campText, alreadyFighting);
}
