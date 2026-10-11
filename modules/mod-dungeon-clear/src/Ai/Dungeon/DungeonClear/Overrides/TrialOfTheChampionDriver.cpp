/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <cmath>
#include <list>
#include <vector>

#include "Creature.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Vehicle.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCombatFlag.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPartyState.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSmartRest.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTocDriverDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"

// --- Trial of the Champion (map 650) — the imperative half -------------------
//
// ONE hook, a controller:
//
//   37  TocDriveArena — the muster, the three announcer clicks, the side packs
//
// THE DECISIONS ARE NOT HERE. They are the pure kernel in
// Util/DcTocDriverDecision.h, tested without a map in
// t/TestTrialOfTheChampion.cpp; this file is the censuses, the moves, the click,
// the force-pull and the telemetry.
//
// THE ONE THING IN THIS FILE WITH NO PRECEDENT IN THE MODULE: it moves a horse.
// Nothing else in DC knows about vehicles — the escort glide splines the BOT,
// which on a vehicle is the passenger — so while the tank rides, this driver
// steers the vehicle base itself (the MovementAction::MoveTo vehicle branch, by
// hand: a hook has no MovementAction to call it on).
//
// THE RETURN CONTRACT is the module's:
//
//   Running => "I am steering." Claims the tick.
//   Done    => "Nothing to steer." YIELDS the tick (the stepsOwnMovement branch
//              in DcRunEventAction) to the combat engine, or out of combat to
//              the anchored hold, which re-parks the tank and rests the party.

namespace
{
    using namespace DcTrialOfTheChampion;

    bool IsTocHorse(Unit const* u)
    {
        return u && (u->GetEntry() == NPC_ARGENT_WARHORSE || u->GetEntry() == NPC_ARGENT_BATTLEWORG);
    }

    bool RidesTocHorse(Player const* p)
    {
        return p && IsTocHorse(p->GetVehicleBase());
    }

    // Attackable BY THIS BOT, which is the question every gate here asks. The
    // soldiers and the boss come in NON_ATTACKABLE + IMMUNE_TO_PC and have both
    // cleared by script; the defeated Argent champion turns friendly. One call
    // answers all three.
    bool Attackable(Player* bot, Unit* u)
    {
        return bot && u && u->IsAlive() && bot->IsValidAttackTarget(u);
    }

    // --- the censuses ---------------------------------------------------------

    struct PartyCensus
    {
        uint32 size = 0;     // alive, same map, tank included
        uint32 mounted = 0;  // ...of which on a ToC horse
    };

    PartyCensus CountParty(Player* bot)
    {
        PartyCensus c;
        Group* group = bot->GetGroup();
        if (!group)
        {
            c.size = 1;
            c.mounted = RidesTocHorse(bot) ? 1 : 0;
            return c;
        }
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (!m || !m->IsInWorld() || !m->IsAlive() || m->GetMapId() != bot->GetMapId())
                continue;
            ++c.size;
            if (RidesTocHorse(m))
                ++c.mounted;
        }
        return c;
    }

    // A Grand Champion who has been "killed" off his horse: mount display gone,
    // NON_ATTACKABLE, alive, walking to a spare. The trample target.
    uint32 CountWalkingChampions(Player* bot)
    {
        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, TocChampionEntries(), ARENA_SCAN);
        uint32 n = 0;
        for (Creature* c : found)
            if (c && c->IsAlive() && c->GetMountID() == 0 && c->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE))
                ++n;
        return n;
    }

    struct SoldierCensus
    {
        uint32    alive = 0;
        uint32    attackable = 0;
        bool      engaged = false;
        Creature* nearest = nullptr;  // the nearest attackable one
    };

    SoldierCensus CountSoldiers(Player* bot)
    {
        SoldierCensus c;
        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, TocSoldierEntries(), ARENA_SCAN);
        float best = -1.0f;
        for (Creature* cr : found)
        {
            if (!cr || !cr->IsAlive())
                continue;
            ++c.alive;
            if (cr->IsInCombat())
                c.engaged = true;
            if (!Attackable(bot, cr))
                continue;
            ++c.attackable;
            float const d = bot->GetExactDist(cr);
            if (best < 0.0f || d < best)
            {
                best = d;
                c.nearest = cr;
            }
        }
        return c;
    }

    Creature* InstanceCreature(Player* bot, InstanceScript* inst, uint32 slot)
    {
        ObjectGuid const guid = inst->GetGuidData(slot);
        if (guid.IsEmpty())
            return nullptr;
        Creature* c = ObjectAccessor::GetCreature(*bot, guid);
        return c && c->IsAlive() ? c : nullptr;
    }

    // Has the party recovered enough to start the next phase? The Culling
    // driver's CosPartyRecovered, for the same reason: a hook has no action to
    // ask EventRestDecision on. Spread is not part of it.
    bool TocPartyReady(Player* bot, AiObjectContext* context)
    {
        if (!bot || !context)
            return true;
        if (DcSmartRest::Enabled(bot))
            return !DcSmartRest::UpdateLatch(bot, context);

        DcPartyState::RestGate const rest = DcPartyState::GetRestGate(bot, context);
        if (rest.minHp <= 0.0f && rest.minMp <= 0.0f)
            return true;
        return DcPartyState::IsPartyReady(bot, rest.minHp, rest.minMp, /*maxSpread*/ 100000.0f);
    }

    // --- the mover ---------------------------------------------------------------
    //
    // A point move for whichever unit carries the tank: the HORSE when it rides
    // (the seat has CAN_CONTROL, so the vehicle base is what walks), the tank
    // itself otherwise. Every leg in the bowl is under ~70yd, well inside what one
    // generated path covers, so this is a plain MovePoint rather than a
    // long-range spline. Returns true when movement is issued or under way.
    bool TocMoveTo(Player* bot, PlayerbotAI* botAI, DcRunState& st, float x, float y, float z)
    {
        if (!DcMovement::DcMovementAllowed(botAI))
            return false;

        Unit* mover = bot;
        if (Vehicle* veh = bot->GetVehicle())
        {
            VehicleSeatEntry const* seat = veh->GetSeatForPassenger(bot);
            Unit* base = veh->GetBase();
            if (!base || !seat || !seat->CanControl())
                return false;  // a passenger that cannot steer has nothing to move
            mover = base;
        }

        if (st.ThrottledIssue(DcThrottle::TocMoveIssue, x, y, z, /*epsilon*/ 1.0f, MOVE_REISSUE_MS))
            return true;

        // On foot, drop a stale escort glide first — the one thing a MovePoint
        // would otherwise coast on top of. Never on the passenger: its movement is
        // the seat's, and a stop there is meaningless.
        if (mover == bot)
            DcMovement::ResolveEscortConflict(bot);

        MotionMaster* mm = mover->GetMotionMaster();
        if (!mm)
            return false;
        mm->Clear();
        mm->MovePoint(/*id*/ 0, x, y, z, FORCED_MOVEMENT_NONE, /*speed*/ 0.0f,
                      /*orientation*/ 0.0f, /*generatePath*/ true, /*forceDestination*/ false);
        return true;
    }

    // --- the pull ---------------------------------------------------------------
    //
    // The two halves of a pull, the Culling's CosForcePull / CosEngageTarget: make
    // the creature attack the bot, then make the bot attack the creature and flip
    // it onto the combat engine (engine transitions are action-driven, so a
    // force-attacked bot left on the non-combat engine has no rotation to run).
    void TocForcePull(Player* bot, AiObjectContext* context, PlayerbotAI* botAI, Creature* c)
    {
        if (!c || !c->IsAlive())
            return;

        if (!c->IsInCombat())
        {
            if (c->IsInEvadeMode())
                c->ClearUnitState(UNIT_STATE_EVADE);
            c->EngageWithTarget(bot);
            if (c->AI())
                c->AI()->AttackStart(bot);
        }

        if (bot->GetVictim() != c)
        {
            bot->SetSelection(c->GetGUID());
            if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, c))
                ServerFacade::instance().SetFacingTo(bot, c);
            context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(c);
            bot->Attack(c, botAI->IsMelee(bot));
        }

        if (botAI->GetState() != BOT_STATE_COMBAT)
        {
            botAI->ChangeEngine(BOT_STATE_COMBAT);
            botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
        }
    }

    // --- telemetry ---------------------------------------------------------------
    //
    // `DcToc progress=<n> mounted=<k>/<n> walking=<m>` on every progress change and
    // every TELEMETRY_MS, at INFO so tools/dc_test_run.py sees it by default: the
    // live assertions for this dungeon (the progress timeline, riders at the
    // click, a progress-4 dwell that means the trample is failing) all read it.
    void TocTelemetry(Player* bot, DcRunState& st, DcTocDriver::Inputs const& in,
                      DcTocDriver::Verdict const& v)
    {
        bool const changed = in.progress != st.tocLastProgress;
        bool const due = !st.Throttled(DcThrottle::TocTelemetryLog, TELEMETRY_MS);
        if (!changed && !due)
            return;
        st.tocLastProgress = in.progress;

        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] DcToc progress={} mounted={}/{} walking={} soldiers={}/{} — {}{}",
                 bot->GetName(), in.progress, in.mountedMembers, in.partySize,
                 in.walkingChampions, in.soldiersAttackable, in.soldiersAlive,
                 DcTocDriver::StateName(v.state), in.partyInCombat ? " (in combat)" : "");
    }

    // --- hook 37: THE CHAMPION'S ARENA --------------------------------------------
    ObjectiveArriveResult TocDriveArena(Player* bot, AiObjectContext* context,
                                        DungeonBossInfo const& /*info*/)
    {
        if (!bot || !context || bot->GetMapId() != MAP_ID)
            return ObjectiveArriveResult::Done;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return ObjectiveArriveResult::Done;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Done;

        DcRunState& st = DcRun::Of(context);
        uint32 const progress = inst->GetData(DATA_INSTANCE_PROGRESS);

        // --- the snapshot ------------------------------------------------------
        //
        // Each census only in the phase that reads it: the arena scans are cheap
        // but this runs every tick of the run.
        bool const joust = progress <= PROGRESS_GROUP_DIED_3;
        PartyCensus const party = CountParty(bot);
        Creature* const announcer = InstanceCreature(bot, inst, DATA_ANNOUNCER);

        DcTocDriver::Inputs in;
        in.progress = progress;
        in.alive = bot->IsAlive();
        in.partyInCombat = DcCombatFlag::AnyPartyEngagement(bot);
        // IN THE JOUST, ENGAGEMENT IS NOT ENOUGH. The champions and their minions
        // attack the HORSES, and a rider's seat has no CAN_ATTACK, so a rider is
        // often neither a victim nor has one — AnyPartyEngagement can read a live
        // joust as idle, and the kernel would then ride the horse back to the post
        // under `toc mounted`'s Charge cycle. Every wave DoZoneInCombat's the whole
        // party, so the raw flag on the tank or its horse is the honest signal
        // here. Everywhere else — progress 0 included — it stays engagement-only:
        // a phantom flag must not hold a click.
        if (progress >= PROGRESS_REACHED_DEST && joust && !in.partyInCombat)
        {
            Unit* const horse = bot->GetVehicleBase();
            in.partyInCombat = bot->IsInCombat() || (horse && horse->IsInCombat());
        }
        in.tankMounted = RidesTocHorse(bot);
        in.mountedMembers = party.mounted;
        in.partySize = party.size;
        in.walkingChampions = joust ? CountWalkingChampions(bot) : 0;
        in.distToPost = bot->GetExactDist(CHAMPIONS_X, CHAMPIONS_Y, CHAMPIONS_Z);
        in.announcerPresent = announcer != nullptr;
        in.announcerFlagged = announcer && announcer->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP);
        in.announcerDist = announcer ? bot->GetExactDist(announcer) : -1.0f;

        SoldierCensus soldiers;
        if (progress == PROGRESS_CHAMPIONS_DEAD)
            soldiers = CountSoldiers(bot);
        in.soldiersAlive = soldiers.alive;
        in.soldiersAttackable = soldiers.attackable;
        in.soldiersEngaged = soldiers.engaged;

        Creature* argentBoss = nullptr;
        if (progress == PROGRESS_SOLDIERS_DIED)
            argentBoss = InstanceCreature(bot, inst, DATA_ARGENT_CHAMPION);
        in.argentBossPresent = argentBoss != nullptr;
        in.argentBossAttackable = Attackable(bot, argentBoss);
        in.argentBossEngaged = argentBoss && argentBoss->IsInCombat();

        if (progress == PROGRESS_ARGENT_CHALLENGE_DIED)
        {
            Creature* knight = bot->FindNearestCreature(NPC_BLACK_KNIGHT, ARENA_SCAN, /*alive*/ true);
            in.knightPresent = knight != nullptr;
            in.knightAttackable = Attackable(bot, knight);
        }

        // Rest is only ever consulted between phases, and asking refreshes Smart
        // Rest's latch — so do not ask during the joust, where nobody can drink.
        in.partyReady = (progress >= PROGRESS_CHAMPIONS_DEAD && !in.partyInCombat)
                            ? TocPartyReady(bot, context)
                            : true;

        in.nowMs = getMSTime();
        in.mountedSinceMs = st.tocMountedSinceMs;
        in.unmountedSinceMs = st.tocUnmountedSinceMs;
        in.strandSinceMs = st.tocStrandSinceMs;

        DcTocDriver::Verdict const v = DcTocDriver::Decide(in);

        st.tocMountedSinceMs = v.mountedSinceMs;
        st.tocUnmountedSinceMs = v.unmountedSinceMs;
        st.tocStrandSinceMs = v.strandSinceMs;

        if (v.complete)
        {
            st.ClearToc();
            return ObjectiveArriveResult::Done;
        }

        TocTelemetry(bot, st, in, v);

        if (st.tocState != static_cast<uint8>(v.state))
        {
            st.tocState = static_cast<uint8>(v.state);
            st.tocStateMs = in.nowMs;
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] ToC — {} (progress {}, {}/{} mounted)",
                     bot->GetName(), DcTocDriver::StateName(v.state), progress,
                     in.mountedMembers, in.partySize);
        }

        if (v.neverMounted && !st.Throttled(DcThrottle::TocWarn, WARN_THROTTLE_MS))
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] ToC — the tank has been on foot for {}s of the joust (progress {}). "
                     "Only mod-playerbots' `toc lance` / `toc mount` mount it; is `wotlk-toc` on its "
                     "engines (AiPlayerbot.ApplyInstanceStrategies), and is a lance rack and a free "
                     "horse in sight?",
                     bot->GetName(), (in.nowMs - v.unmountedSinceMs) / 1000, progress);

        if (v.stranded && !st.Throttled(DcThrottle::TocWarn, WARN_THROTTLE_MS))
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] toc: black knight evaded — stranded until full wipe. He despawns on "
                     "evade and had already killed the announcer, so progress 8 has nothing to click "
                     "and nothing to fight until every player is dead and InstanceCleanup respawns "
                     "the announcer.",
                     bot->GetName());

        // --- act ---------------------------------------------------------------
        //
        // A move the mover refuses (paused run, a seat that cannot steer) falls
        // back to HOLD during the joust — a mounted tank must never be handed to
        // the DC ladder — and to a yield afterwards, where the anchored hold
        // re-parks the tank 3yd from the announcer anyway.
        ObjectiveArriveResult const moveRefused =
            joust ? ObjectiveArriveResult::Running : ObjectiveArriveResult::Done;

        switch (v.action)
        {
            case DcTocDriver::Action::ApproachAnnouncer:
            {
                if (!announcer)
                    return moveRefused;
                // GOSSIP_STANDOFF short of him on our own bearing, at his z.
                float const ax = announcer->GetPositionX(), ay = announcer->GetPositionY();
                float dx = bot->GetPositionX() - ax, dy = bot->GetPositionY() - ay;
                float const d = std::hypot(dx, dy);
                if (d > 0.1f)
                {
                    dx /= d;
                    dy /= d;
                }
                else
                {
                    dx = 1.0f;
                    dy = 0.0f;
                }
                return TocMoveTo(bot, botAI, st, ax + dx * GOSSIP_STANDOFF, ay + dy * GOSSIP_STANDOFF,
                                 announcer->GetPositionZ())
                           ? ObjectiveArriveResult::Running
                           : moveRefused;
            }

            case DcTocDriver::Action::ReturnToPost:
                return TocMoveTo(bot, botAI, st, CHAMPIONS_X, CHAMPIONS_Y, CHAMPIONS_Z)
                           ? ObjectiveArriveResult::Running
                           : moveRefused;

            case DcTocDriver::Action::Gossip:
            {
                if (!announcer)
                    return ObjectiveArriveResult::Running;
                if (st.Throttled(DcThrottle::TocClick, CLICK_RETRY_MS))
                    return ObjectiveArriveResult::Running;

                bool const sent = DungeonEventExecutor::SelectGossip(bot, announcer, v.gossipOption);
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] ToC — progress {}: {} {} option {} from {:.1f}yd{} ({}/{} mounted)",
                         bot->GetName(), progress, sent ? "selected" : "could not select",
                         announcer->GetName(), v.gossipOption, in.announcerDist,
                         in.tankMounted ? " on horseback" : "", in.mountedMembers, in.partySize);
                return ObjectiveArriveResult::Running;
            }

            case DcTocDriver::Action::EngageSoldier:
                if (!soldiers.nearest)
                    return ObjectiveArriveResult::Running;
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] ToC — {} idle Argent soldier(s), nobody engaged: pulling {} at {:.1f}yd",
                         bot->GetName(), soldiers.attackable, soldiers.nearest->GetName(),
                         bot->GetExactDist(soldiers.nearest));
                TocForcePull(bot, context, botAI, soldiers.nearest);
                return ObjectiveArriveResult::Running;

            case DcTocDriver::Action::EngageBoss:
                if (!argentBoss)
                    return ObjectiveArriveResult::Running;
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] ToC — {} is idle and attackable at progress 7 (evaded after a "
                         "partial wipe): pulling it back",
                         bot->GetName(), argentBoss->GetName());
                TocForcePull(bot, context, botAI, argentBoss);
                return ObjectiveArriveResult::Running;

            case DcTocDriver::Action::Hold:
                return ObjectiveArriveResult::Running;

            case DcTocDriver::Action::Yield:
            default:
                return ObjectiveArriveResult::Done;
        }
    }
}

// Id 37. The Culling of Stratholme's is 36; ids are ONE FLAT SPACE across every
// dungeon, and AddHook LOG_ERRORs a collision rather than silently dropping one.
void RegisterTrialOfTheChampionHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, DcTrialOfTheChampion::HOOK_TOC_DRIVER, &TocDriveArena);
}
