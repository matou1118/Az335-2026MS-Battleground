/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <string>
#include <vector>

#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCombatFlag.h"
#include "Ai/Dungeon/DungeonClear/Util/DcHorEscapeDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcHorWaveDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransit.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"

// --- Halls of Reflection (map 668) — the imperative half -------------------
//
// Five hooks, and two of them are controllers:
//
//   31  HorIntroGossip   — talk to Jaina/Sylvanas; the dungeon starts here
//   32  HorDriveWaves    — THE CONTROLLER for the first two thirds: hold the altar
//   33  HorThroneRoom    — gather, forge areatrigger 5605, wait out the cutscene
//   34  HorEscapeGo      — the point of no return; the pickiest gate in the module
//   35  HorDriveEscape   — THE CONTROLLER for the escape: stay ahead of him
//
// THE ARITHMETIC IS NOT HERE. It is the two pure kernels
// Util/DcHorWaveDecision.h and Util/DcHorEscapeDecision.h, unit-tested without a
// map in t/TestHallsOfReflection.cpp; this file is glue — grid scans, the party
// walk, the splines, the forged packet, the telemetry — exactly as
// PitOfSaronDriver.cpp is glue for its own kernel.
//
// THE RETURN CONTRACT IS BLACKWING LAIR'S throughout, and getting it backwards
// wipes parties:
//
//   Running => "I am steering." Claims the tick.
//   Done    => "Nothing to steer." YIELDS the tick (the stepsOwnMovement branch
//              in DcRunEventAction) so the stock combat engine can fight.
//
// On this map the yield matters more than on any other, because BOTH controllers
// are idle for most of their windows. The wave driver has nothing to steer for
// the whole of every wave — the mobs walk to the party, by design — and the
// escape driver has nothing to steer for the whole of every wall fight. Every
// tick either of them claimed while a fight was live would be a tick the tank did
// not swing, and this dungeon has ten waves, two bosses, a general and four
// summon batches to get through.
//
// ONE THING THIS FILE NEVER DOES: it never PULLS. Pit of Saron needed a
// standoff-breaker because its waves emerge passive and AzerothCore's proximity
// aggro is relocation-driven, so a parked party and a parked mob can stare at
// each other for ever. Nothing on this map can do that: every activation ends in
// SetInCombatWithZone() plus an explicit AttackStart, so the fight always starts
// itself and a driver that went looking for one would only be walking toward a
// leash.
//
// IT DOES, IN EXACTLY ONE STATE, WALK THE TANK AT A HOSTILE — and the paragraph
// above is why that is not a contradiction. Engage is not a pull. The fight has
// already started itself, the summon is already on the party, and the tank is
// already flagged; the only thing missing is the twenty yards between a bot and
// a Risen Witch Doctor that casts from range at whoever is rearmost. The stock
// engine has no rung left that closes it — on tr-20260908-225156-15 that was
// eighty-seven seconds of "no actions executed" with every ability returning
// SPELL_FAILED_OUT_OF_RANGE — so the driver takes the step. It is bounded to
// ENGAGE_REACH_YD precisely so it can never become the walk-toward-a-leash this
// paragraph warns about.

namespace
{
    using namespace DcHallsOfReflection;

    // Fire the real area trigger from `bot`. Identical to Pit of Saron's forge and
    // to Utgarde Pinnacle's before it, and deliberately so: the core validates
    // that the bot is inside the trigger's own volume and then runs the script,
    // which means a forge from outside is a harmless no-op rather than a cheat,
    // and the hook can call it every tick without bookkeeping.
    //
    // ON THIS MAP THE NO-OP IS DOUBLY FREE, and that is worth stating because it
    // is the opposite of Pit of Saron. at_hor_shadow_throne itself refuses unless
    // PERSISTENT_DATA_FROSTSWORN_GENERAL is set and PERSISTENT_DATA_LK_INTRO is
    // not — and a refusal CONSUMES NOTHING. Crossing the box early is a genuine
    // no-op that can be re-crossed later, so there is no arming state to get right
    // here and no edge to burn.
    void ForgeAreaTrigger(Player* bot, uint32 triggerId)
    {
        if (!bot || !bot->GetSession())
            return;

        WorldPacket p(CMSG_AREATRIGGER);
        p << uint32(triggerId);
        p.rpos(0);
        bot->GetSession()->HandleAreaTriggerOpcode(p);
    }

    // How many living party members on this map are within `radius` of a point,
    // how many there are in total, and how far the worst one is.
    //
    // LIVING MEMBERS ON THIS MAP ONLY, which is the whole reason this is not a
    // plain distance loop. A corpse-running member is the rez ladder's business
    // and a member who never zoned in must never pin a gate — count either and a
    // quorum becomes unreachable rather than strict.
    struct HorPartyView
    {
        uint32 living = 0;    // leader included
        uint32 near_ = 0;     // ...of which, inside the radius
        uint32 total = 0;     // living + dead, on this map
        uint32 dead = 0;
        float  furthest = 0.0f;
        float  worstManaPct = 100.0f;  // over healers and mana users only
    };

    HorPartyView HorParty(Player* bot, float x, float y, float z, float radius)
    {
        HorPartyView v;
        if (!bot)
            return v;

        auto note = [&v](Player* member, float dist)
        {
            ++v.living;
            if (dist > v.furthest)
                v.furthest = dist;
            // POWER_MANA is 0 for a rage/energy class, which would read as a
            // permanently empty healer. Only members that actually have a mana
            // bar contribute to the escape's mana floor.
            if (member->GetMaxPower(POWER_MANA) > 0)
                v.worstManaPct = std::min(v.worstManaPct, member->GetPowerPct(POWER_MANA));
        };

        v.total = 1;
        if (bot->IsAlive())
        {
            float const d = bot->GetExactDist(x, y, z);
            note(bot, d);
            if (d <= radius)
                ++v.near_;
        }
        else
        {
            ++v.dead;
        }

        Group* group = bot->GetGroup();
        if (!group)
            return v;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || member->GetMapId() != bot->GetMapId())
                continue;

            ++v.total;
            if (!member->IsAlive())
            {
                ++v.dead;
                continue;
            }

            float const d = member->GetExactDist(x, y, z);
            note(member, d);
            if (d <= radius)
                ++v.near_;
        }
        return v;
    }

    // --- the wave probe -----------------------------------------------------
    //
    // ALL 34 WAVE MOBS EXIST FROM MAP LOAD, which is why this is a flag test and
    // not an aliveness count. instance_halls_of_reflection spawns every one of
    // them hidden (SetVisible(false)), UNIT_FLAG_NOT_SELECTABLE and
    // SetImmuneToAll, and ActivateWave clears exactly those three on the three to
    // five it is activating. So "armed" is precisely "the instance has turned this
    // one on", and a bare `IsAlive()` probe would read thirty-four live wave mobs
    // before the party had even gossiped.
    //
    // NOT_SELECTABLE alone would very nearly do, and IMMUNE_TO_PC is checked as
    // well for the same reason Pit of Saron checks two things: the two flags are
    // cleared by the same call today, and a probe that depends on that staying
    // true is a probe that breaks silently if it ever stops.
    std::vector<uint32> const& WaveEntries()
    {
        static std::vector<uint32> const kEntries = {
            NPC_WAVE_MAGE, NPC_WAVE_FOOTMAN, NPC_WAVE_PRIEST,
            NPC_WAVE_RIFLEMAN, NPC_WAVE_MERCENARY,
        };
        return kEntries;
    }

    uint32 CountArmedWaveMobs(Player* bot)
    {
        if (!bot)
            return 0;

        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, WaveEntries(), WAVE_SCAN);

        uint32 armed = 0;
        for (Creature* c : found)
        {
            if (!c || !c->IsAlive())
                continue;
            if (c->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
                continue;
            if (c->IsImmuneToPC())
                continue;
            ++armed;
        }
        return armed;
    }

    // Is one of the two wave bosses up, and is he touchable yet?
    //
    // THE EIGHT-SECOND ARMING WINDOW IS THE REASON THIS RETURNS TWO BOOLS. On
    // wave 5 and wave 10 boss_falric/boss_marwyn DoAction(1) makes the boss
    // visible and yells, and only 8 seconds later does SetImmuneToPC(false) +
    // SetInCombatWithZone. During those eight seconds he is alive, visible and
    // completely untouchable, and a driver that yielded would hand the tick to a
    // combat engine with nothing to hit.
    struct BossProbe
    {
        bool visible = false;      // alive and not hidden
        bool attackable = false;   // ...and not IMMUNE_TO_PC
    };

    // THE TWO BOSSES ARE LOOKED UP BY DATA SLOT, NOT BY ENTRY, and this is the one
    // place on this map where the two conventions differ.
    //
    // InstanceScript::LoadObjectData stores `objectInfo[entry] = type` and
    // OnCreatureCreate then files the guid under the TYPE — so GetGuidData()'s
    // key is whatever the second column of instance_halls_of_reflection's
    // creatureData says. For most of this map's rows that column repeats the
    // entry ({ NPC_SYLVANAS_PART1, NPC_SYLVANAS_PART1 }, and likewise the part-2
    // leader, the Lich King and the Frostsworn General), which is why every other
    // lookup in this file passes an entry. Falric and Marwyn are the exceptions:
    // their rows are { NPC_FALRIC, DATA_FALRIC } and { NPC_MARWYN, DATA_MARWYN },
    // so they are filed under 0 and 1.
    //
    // A lookup on 38112 therefore returns ObjectGuid::Empty for ever — which
    // reads as "no boss is up", which would hold the party at the camp through
    // both boss fights instead of yielding the tick to the rotation. Silent, and
    // it would have looked like a throughput problem.
    BossProbe ProbeWaveBosses(InstanceScript* inst)
    {
        BossProbe p;
        if (!inst)
            return p;

        for (uint32 slot : { DATA_FALRIC, DATA_MARWYN })
        {
            Creature* c = inst->instance->GetCreature(inst->GetGuidData(slot));
            if (!c || !c->IsAlive() || !c->IsVisible())
                continue;
            p.visible = true;
            if (!c->IsImmuneToPC())
                p.attackable = true;
        }
        return p;
    }

    // --- hook 31: START THE INTRO ------------------------------------------
    //
    // ONE GOSSIP, AND THE WHOLE DUNGEON IS BEHIND IT. Idempotent by construction:
    // it re-reads instance state every tick and never latches, so a restarted
    // Drive cannot double-fire.
    //
    // THE FOUR STATES IT HAS TO TELL APART, in the order it tests them:
    //
    //   1. the intro has ALREADY RUN (a re-entered instance, or a retry after the
    //      party wiped in the waves) — PERSISTENT_DATA_INTRO. Nothing to do, and
    //      nothing CAN be done: the leader's gossip flag is stripped for ever by
    //      the first select.
    //   2. the leader is not loaded or not yet visible — she spawns hidden and
    //      npc_hor_leaderAI's Reset() shows her at +10s. Wait.
    //   3. she is visible but has no gossip flag. TWO CAUSES with opposite
    //      answers: the +19s pre-intro has not finished (wait), or somebody
    //      already selected an option (done). PERSISTENT_DATA_BATTERED_HILT tells
    //      them apart — SetData(DATA_BATTERED_HILT, 1) in OnGossipSelect stores
    //      it, and NOTHING ELSE a bot party can do ever does (the Quel'Delar
    //      chain that also sets it needs item 49766, which no bot carries).
    //      GetData(DATA_BATTERED_HILT) is NOT the probe: that case only writes the
    //      persistent slot and leaves _batteredHiltStatus at 0.
    //   4. she has the flag. Pick an option and select it.
    //
    // WHICH OPTION. Position 1 is the 75.5-second skip and exists in the menu ONLY
    // if the selecting bot has quest 24500 (A) / 24802 (H) rewarded; position 0 is
    // the full 224.5s / 209s cutscene and needs 24710 / 24712, which is also the
    // dungeon's access requirement and therefore always present on a bot
    // DcDungeonAccess::GrantEntry teleported in. So: ask for the skip when this
    // bot can see it, and fall back to the full intro otherwise. Asking for
    // position 1 without the quest would resolve to no listId at all and the
    // select would never be sent.
    ObjectiveArriveResult HorIntroGossip(Player* bot, AiObjectContext* context,
                                         DungeonBossInfo const& /*info*/)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return ObjectiveArriveResult::Done;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return ObjectiveArriveResult::Done;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;  // not in the instance yet

        // 1. The intro never replays. If it has run, this objective is spent —
        // whatever else is true of the leader.
        if (inst->GetPersistentData(PERSISTENT_DATA_INTRO))
            return ObjectiveArriveResult::Done;

        // 2. The leader, through the instance's GUID store rather than a grid
        // scan. THE KEY IS 37223 ON BOTH FACTIONS: OnCreatureCreate stores her
        // under her spawn entry before UpdateEntry()ing her to Jaina (37221) for
        // an Alliance instance, so a lookup on 37221 finds nothing at all.
        Creature* leader = inst->instance->GetCreature(inst->GetGuidData(NPC_LEADER_PART1));
        if (!leader || !leader->IsAlive() || !leader->IsVisible())
            return ObjectiveArriveResult::Running;

        // 3. No gossip flag: either the pre-intro is still running, or the select
        // already landed.
        if (!leader->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        {
            if (inst->GetPersistentData(PERSISTENT_DATA_BATTERED_HILT))
            {
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] HoR — the intro gossip has already been taken; handing over "
                         "to the altar garrison",
                         bot->GetName());
                return ObjectiveArriveResult::Done;
            }
            return ObjectiveArriveResult::Running;  // the +19s pre-intro
        }

        // 4. Pick the option and try it. TRY THE GOSSIP FIRST, THEN CLOSE THE GAP,
        // for the Violet Hold reason: the reach test here and the core's are not
        // the same test (GetNPCIfCanInteractWith is bounding-radius aware and
        // accepts ranges a plain GetExactDist rejects), so a hook that refuses to
        // try until its own stricter gate passes can sit one tick of drift outside
        // a range the server would have honoured. Trying costs one packet pair and
        // SelectGossip reports honestly.
        bool const horde = bot->GetTeamId(true) == TEAM_HORDE;
        uint32 const skipQuest = horde ? QUEST_SKIP_HORDE : QUEST_SKIP_ALLIANCE;
        bool const canSkip = bot->GetQuestRewardStatus(skipQuest);

        float const dist = bot->GetExactDist(leader);
        constexpr float kGossipReach = 5.0f;

        // TRY THE SKIP, THEN FALL BACK, rather than trusting our own prediction of
        // what is in the menu — and the fallback is the load-bearing half.
        //
        // The two tests are NOT the same test. We read GetQuestRewardStatus (the
        // m_RewardedQuests set, which is what DcDungeonAccess::GrantEntry writes);
        // npc_hor_leader::OnGossipHello reads GetQuestStatus == COMPLETE or
        // REWARDED, which consults m_QuestStatus FIRST and only falls through to
        // the rewarded set when there is no status row. They agree for an ordinary
        // pool bot and can disagree for a recycled one that has the quest sitting
        // in its log at INCOMPLETE.
        //
        // A disagreement in the direction that matters is a RUN-ENDER without this
        // fallback: we would ask for option position 1, ResolveGossipListId would
        // find no such position in a one-item menu, SelectGossip would return
        // false, and the hook would walk-and-retry until the step timed out — with
        // the full intro sitting right there at position 0 the whole time. So ask
        // for what we think we can have, and take what is actually offered.
        int32 const preferred = canSkip ? GOSSIP_OPTION_INTRO_SKIP : GOSSIP_OPTION_INTRO_FULL;
        int32 taken = -1;
        if (dist <= kGossipReach)
        {
            if (DungeonEventExecutor::SelectGossip(bot, leader, preferred))
                taken = preferred;
            else if (preferred != GOSSIP_OPTION_INTRO_FULL &&
                     DungeonEventExecutor::SelectGossip(bot, leader, GOSSIP_OPTION_INTRO_FULL))
                taken = GOSSIP_OPTION_INTRO_FULL;
        }

        if (taken >= 0)
        {
            bool const skipped = taken == GOSSIP_OPTION_INTRO_SKIP;
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] HoR — took the leader's gossip, option {} ({} intro){}. The "
                     "party has {} to walk to the altar before wave 1.",
                     bot->GetName(), taken,
                     skipped ? "SKIPPED, 75.5s" : "full, 224.5s Alliance / 209s Horde",
                     (canSkip && !skipped)
                         ? " — the skip option was expected but the menu did not offer it"
                         : "",
                     skipped ? "75 seconds" : "three and a half minutes");
            return ObjectiveArriveResult::Running;  // confirmed by the flag next tick
        }

        DcRunState& st = DcRun::Of(context);
        if (!st.Throttled(DcThrottle::HorIntroLog, TELEMETRY_MS))
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] HoR — walking to the leader to start the intro (dist {:.1f}yd, "
                      "reach {:.1f}yd, wanted option {}, {})",
                      bot->GetName(), dist, kGossipReach, preferred,
                      dist <= kGossipReach ? "tried both options, menu not ready"
                                           : "out of reach");

        DcTransit::TravelTo(bot, botAI, leader->GetPositionX(), leader->GetPositionY(),
                            leader->GetPositionZ(), kGossipReach);
        return ObjectiveArriveResult::Running;
    }

    // The wave driver's per-tick telemetry line, throttled. Without it a failed
    // wave phase says nothing about WHICH mechanism is biting: "the party is
    // standing at the altar" reads identically whether the driver is holding
    // between waves, holding through a boss's 8-second arming window, or
    // regathering after a leash wipe that silently replayed four waves.
    void HorWaveLog(Player* bot, DcRunState& st, DcHorWaves::Verdict const& v,
                    DcHorWaves::Inputs const& in)
    {
        if (st.Throttled(DcThrottle::HorWaveLog, TELEMETRY_MS))
            return;

        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] HoR altar — wave {}/10, {} — {} armed wave mob(s), boss {}, "
                  "camp {:.1f}yd, party {}/{} within {:.0f}yd of centre (furthest {:.1f}yd "
                  "of a {:.1f}yd leash) -> {}",
                  bot->GetName(), in.waveNumber, DcHorWaves::StateName(v.state),
                  in.mobsArmed,
                  in.bossAttackable ? "attackable" : in.bossVisible ? "visible but immune"
                                                                    : "not up",
                  in.distToCamp, in.nearCenter, in.living, LEASH_RESTART,
                  in.furthestFromCenter, LEASH_COMBAT,
                  v.walkToCamp  ? "walking back to the camp"
                  : v.yieldTick ? "yielding — fight"
                                : "holding");
    }

    // --- hook 32: HOLD THE ALTAR — the first controller --------------------
    ObjectiveArriveResult HorDriveWaves(Player* bot, AiObjectContext* context,
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
        BossProbe const boss = ProbeWaveBosses(inst);

        // The party view is taken against CENTERPOS rather than against the camp,
        // because both numbers it feeds are the INSTANCE's own: the 70.5yd wipe
        // line and the 40yd restart radius are measured from there and nowhere
        // else. Reporting a distance to the camp instead would be reporting a
        // number no rule on this map uses.
        HorPartyView const party = HorParty(bot, CENTER_X, CENTER_Y, CENTER_Z, LEASH_RESTART);

        DcHorWaves::Inputs in;
        in.introDone = inst->GetPersistentData(PERSISTENT_DATA_INTRO) != 0;
        in.falricDone = inst->GetBossState(DATA_FALRIC) == DONE;
        in.marwynDone = inst->GetBossState(DATA_MARWYN) == DONE;
        in.waveNumber = inst->GetData(DATA_WAVE_NUMBER);
        in.mobsArmed = CountArmedWaveMobs(bot);
        in.bossAttackable = boss.attackable;
        in.bossVisible = boss.visible;
        in.partyInCombat = DcCombatFlag::AnyPartyEngagement(bot);
        in.distToCamp = bot->GetExactDist(CAMP_X, CAMP_Y, CAMP_Z);
        in.campLeash = CAMP_LEASH;
        in.living = party.living;
        in.nearCenter = party.near_;
        in.furthestFromCenter = party.furthest;
        in.nowMs = getMSTime();
        in.state = st.horWaveState;
        in.stateSinceMs = st.horWaveStateMs;
        in.restartLogged = st.horRestartLogged;

        DcHorWaves::Verdict const v = DcHorWaves::Decide(in);

        // Store the clocks back BEFORE acting, so the stored state and the state
        // it names can never disagree even for one tick.
        bool const changed = st.horWaveState != v.storeState;
        st.horWaveState = v.storeState;
        st.horWaveStateMs = v.stateSinceMs;
        st.horRestartLogged = v.restartLogged;

        HorWaveLog(bot, st, v, in);

        if (changed && !v.complete)
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] HoR altar — {} (wave {}, {} armed mob(s), {} of {} party members "
                     "inside the {:.0f}yd restart radius)",
                     bot->GetName(), DcHorWaves::StateName(v.state), in.waveNumber,
                     in.mobsArmed, party.near_, party.living, LEASH_RESTART);

        // THE WIPE LINE, once per episode, and it is the most important line this
        // driver prints. A leash wipe respawns every dead wave mob and replays
        // waves 1-4 (or restarts at 6 if wave >= 6 was ever reached this map
        // load), and it leaves NO other trace: the counter reads 0, all 34 mobs
        // are hidden again and nothing is fightable, which is indistinguishable
        // from a healthy gap between waves. Name it, and name who was furthest —
        // because the wipe is caused by exactly one thing, somebody being more
        // than 70.5yd from the altar, and the party view is the only place that
        // number exists.
        if (v.reportRestart)
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] HoR altar — THE WAVE EVENT WIPED. GetData(DATA_WAVE_NUMBER) is "
                     "back to 0 with the intro flag still set, so either a party member left "
                     "the {:.1f}yd leash around the altar or everybody died. Waves replay from "
                     "{}. Regathering: {} of {} living members are inside the {:.0f}yd restart "
                     "radius, furthest {:.1f}yd, {} dead. The instance re-arms on its own "
                     "(5s poll) once every member is alive and inside it.",
                     bot->GetName(), LEASH_COMBAT,
                     in.falricDone ? "wave 6" : "wave 1",
                     party.near_, party.living, LEASH_RESTART, party.furthest, party.dead);

        if (v.complete)
        {
            // Marwyn is down (or the intro has not finished). Drop the block for
            // the ClearPosGauntlet reason: the event is Repeatable and a party
            // that somehow re-enters the window holding a spent restart latch
            // would replay the whole first half without ever naming the wipe.
            st.ClearHor();
            return ObjectiveArriveResult::Done;
        }

        if (v.walkToCamp)
        {
            // THROUGH LongRangePathfinder, not a bare MovePoint: the walk back
            // from wherever a wave ended can be sixty yards across the chamber,
            // and a bare MovePoint truncates silently past ~30.
            //
            // TravelTo returns false when the leader is already inside the leash,
            // which the kernel has just told us it is not — so a false here means
            // the spline could not be issued at all. CLAIM THE TICK ANYWAY, which
            // is the opposite of what Pit of Saron's gate walk does, and the
            // asymmetry is deliberate: yielding here hands the leg to
            // DcRel::Advance (15) and DcRel::AtBoss (30), and on this map the next
            // boss anchor is an INVISIBLE, IMMUNE Marwyn. A tank that cannot be
            // splined back to the camp should stand where it is, not be walked
            // onto a creature it cannot touch.
            DcTransit::TravelTo(bot, botAI, CAMP_X, CAMP_Y, CAMP_Z, CAMP_LEASH);
            return ObjectiveArriveResult::Running;
        }

        // HOLDING or YIELDING. The kernel has decided which; a yield is exactly a
        // tick the party should spend fighting rather than walking.
        //
        // Deliberately no StopBot on the hold: killing the spline every tick of a
        // wave fight is a stop packet every tick, and the tank is being held by
        // combat anyway.
        return v.yieldTick ? ObjectiveArriveResult::Done : ObjectiveArriveResult::Running;
    }

    // --- hook 33: THE THRONE ROOM ------------------------------------------
    //
    // Gather at the door, walk into the box, forge it, then wait out the
    // cutscene. Three states, no kernel: the decisions are a quorum test, a
    // distance test and two instance flags, and lifting those out of the world
    // would cost more in indirection than it returns.
    //
    // THE FORGE IS SAFE TO REPEAT AND SAFE TO SEND EARLY, which is what makes
    // this hook so much simpler than Pit of Saron's ledge. at_hor_shadow_throne
    // does nothing at all unless PERSISTENT_DATA_FROSTSWORN_GENERAL is set and
    // PERSISTENT_DATA_LK_INTRO is not, and a refused crossing consumes NOTHING —
    // unlike a Pit of Saron gate sphere, it can simply be re-crossed. So there is
    // no arming state here and no edge to burn; the hook forges every tick it
    // stands in the box and stops when the flag says it took.
    //
    // THE GATHER IS STILL 3-OF-4 AND NOT 4-OF-4. One bot that cannot path up the
    // ramp must never hold the other three, and stranded recovery (relevance 42)
    // sits above this whole ladder and owns that member. The STRICT quorum is one
    // step later, at the gossip in hook 34, where it actually buys something.
    ObjectiveArriveResult HorThroneRoom(Player* bot, AiObjectContext* context,
                                        DungeonBossInfo const& /*info*/)
    {
        if (!bot || !context || bot->GetMapId() != MAP_ID)
            return ObjectiveArriveResult::Done;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return ObjectiveArriveResult::Done;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;

        DcRunState& st = DcRun::Of(context);

        // The escape has already been started (a retry after a wipe, or a
        // re-entered instance). Nothing here is left to do.
        if (inst->GetBossState(DATA_LICH_KING) == IN_PROGRESS ||
            inst->GetBossState(DATA_LICH_KING) == DONE)
            return ObjectiveArriveResult::Done;

        // THE CUTSCENE HAS RUN. Wait for the ONE thing that matters after it: the
        // part-2 leader carrying a gossip flag at LeaderEscapePos, ~21 seconds
        // after the trigger. Note the GUID key is 37554 on both factions, for the
        // same OnCreatureCreate reason as part 1.
        if (inst->GetPersistentData(PERSISTENT_DATA_LK_INTRO))
        {
            Creature* leader =
                inst->instance->GetCreature(inst->GetGuidData(NPC_LEADER_PART2));
            if (leader && leader->IsAlive() && leader->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
            {
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] HoR — the freeze cutscene is over and the leader is offering "
                         "her escape gossip; moving up to her",
                         bot->GetName());
                return ObjectiveArriveResult::Done;
            }
            return ObjectiveArriveResult::Running;
        }

        // THE GENERAL MUST BE DEAD FIRST, and the trigger's refusal is silent. If
        // this ever reads false the run has walked past him, which is a roster /
        // ordering failure rather than anything this hook can fix — so say it once
        // per throttle window rather than holding in silence.
        if (!inst->GetPersistentData(PERSISTENT_DATA_FROSTSWORN_GENERAL))
        {
            if (!st.Throttled(DcThrottle::HorThroneLog, TELEMETRY_MS))
                LOG_WARN("playerbots.dungeonclear",
                         "[DC:{}] HoR — areatrigger {} will be REFUSED SILENTLY: the Frostsworn "
                         "General is not dead (PERSISTENT_DATA_FROSTSWORN_GENERAL is clear). "
                         "The throne-room cutscene cannot start and the run cannot finish until "
                         "he is killed.",
                         bot->GetName(), AREATRIGGER_THRONE);
            return ObjectiveArriveResult::Running;
        }

        HorPartyView const party = HorParty(bot, THRONE_X, THRONE_Y, THRONE_Z, GATHER_RADIUS);
        bool const quorum =
            party.living <= 1 ||
            static_cast<float>(party.near_) >=
                static_cast<float>(party.living) * GATHER_QUORUM;

        float const toForge = bot->GetExactDist(FORGE_X, FORGE_Y, FORGE_Z);

        if (!quorum)
        {
            // Hold the leader ON the anchor while they come in. TravelTo is a
            // no-op once inside the leash, so this is a re-walk trigger for a
            // leader shoved off the door and nothing else — which is exactly why
            // this branch must SAY SO. A quorum-starved hold and a walk-in whose
            // spline never landed are otherwise the same observation from outside:
            // a tank parked on the anchor, not moving, until the step times out.
            if (!st.Throttled(DcThrottle::HorThroneLog, TELEMETRY_MS))
                LOG_DEBUG("playerbots.dungeonclear",
                          "[DC:{}] HoR throne — gathering: {}/{} within {:.0f}yd of the west "
                          "door (quorum {:.0f}%, furthest {:.1f}yd), {:.1f}yd to the forge point",
                          bot->GetName(), party.near_, party.living, GATHER_RADIUS,
                          GATHER_QUORUM * 100.0f, party.furthest, toForge);
            DcTransit::TravelTo(bot, botAI, THRONE_X, THRONE_Y, THRONE_Z, THRONE_ARRIVE);
            return ObjectiveArriveResult::Running;
        }

        if (toForge > FORGE_LEASH)
        {
            if (!st.Throttled(DcThrottle::HorThroneLog, TELEMETRY_MS))
                LOG_DEBUG("playerbots.dungeonclear",
                          "[DC:{}] HoR throne — walking into areatrigger {} ({:.1f}yd to the "
                          "forge point, party {}/{})",
                          bot->GetName(), AREATRIGGER_THRONE, toForge, party.near_,
                          party.living);
            DcTransit::TravelTo(bot, botAI, FORGE_X, FORGE_Y, FORGE_Z, FORGE_LEASH);
            return ObjectiveArriveResult::Running;
        }

        ForgeAreaTrigger(bot, AREATRIGGER_THRONE);

        if (inst->GetPersistentData(PERSISTENT_DATA_LK_INTRO))
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] HoR — the throne-room cutscene has started (forged areatrigger "
                     "{} with {}/{} of the party at the door, furthest {:.1f}yd). ~21 seconds "
                     "until the leader offers the escape.",
                     bot->GetName(), AREATRIGGER_THRONE, party.near_, party.living,
                     party.furthest);

        return ObjectiveArriveResult::Running;
    }

    // --- hook 34: THE POINT OF NO RETURN -----------------------------------
    //
    // ONE GOSSIP, NO CONFIRMATION, NO WAY BACK. The tick it lands, the boss state
    // goes IN_PROGRESS, the ice prison drops off the Lich King and 1.5 seconds
    // later he starts walking. For the next four to six minutes he calls
    // SetInCombatWithZone() on every player once a second, which means: NO
    // DRINKING, NO EATING, NO OUT-OF-COMBAT RESURRECT. A bot that is dead when
    // this fires stays dead for the rest of the run, and a healer that is dead
    // when this fires is a wipe at wall 2 or 3.
    //
    // SO THIS IS THE ONE GATE IN THE MODULE THAT DEMANDS 5 OF 5 rather than the
    // usual 3 of 4. The asymmetry with hook 33's gather twenty yards earlier is
    // deliberate and is the whole reason they are separate steps: a straggler that
    // cannot path up the ramp must not be able to hold the CUTSCENE, but it must
    // absolutely be able to hold the ESCAPE, because the escape is the thing it
    // cannot be rescued from.
    //
    // FOUR CONDITIONS, and each one is a thing the escape takes away:
    //
    //   1. every member of the party alive        — no out-of-combat rez after this
    //   2. every member within 12yd of the leader — the Lich King's first summon
    //      batch lands 17 seconds in, at HIM, and runs to her stop; a member still
    //      walking up the ramp meets it alone
    //   3. nobody east of / behind the frozen Lich King — the escape's zap rule is
    //      (p.x - lk.x) + (p.y - lk.y) > 20 and the path runs -x -y, so anybody
    //      starting on the wrong side of that scalar starts the encounter already
    //      failing it
    //   4. every mana user at ESCAPE_MANA_PCT — there is no second chance to drink
    //
    // A failing condition returns Running rather than Blocked, and says which. The
    // followers fix 1, 2 and 4 on their own rungs (rez ladder, follow-tank, rest);
    // the tank's own rest starvation is irrelevant here because it is standing
    // still and the escape has no tanking in it.
    ObjectiveArriveResult HorEscapeGo(Player* bot, AiObjectContext* context,
                                      DungeonBossInfo const& /*info*/)
    {
        if (!bot || !context || bot->GetMapId() != MAP_ID)
            return ObjectiveArriveResult::Done;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return ObjectiveArriveResult::Done;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;

        // Already started, by this hook on an earlier tick or by anything else.
        // Hand over to the escape driver (event 5), whose predicate is this exact
        // state.
        if (inst->GetBossState(DATA_LICH_KING) == IN_PROGRESS ||
            inst->GetBossState(DATA_LICH_KING) == DONE)
            return ObjectiveArriveResult::Done;

        DcRunState& st = DcRun::Of(context);

        Creature* leader = inst->instance->GetCreature(inst->GetGuidData(NPC_LEADER_PART2));
        if (!leader || !leader->IsAlive())
            return ObjectiveArriveResult::Running;

        Creature* lk = inst->instance->GetCreature(inst->GetGuidData(NPC_LICH_KING));

        constexpr float kMusterRadius = 12.0f;
        HorPartyView const party = HorParty(bot, leader->GetPositionX(), leader->GetPositionY(),
                                            leader->GetPositionZ(), kMusterRadius);

        // Condition 3, measured on the SAME SCALAR the encounter uses. A member
        // whose (x + y) exceeds his is behind him along the path; the escape zaps
        // at +20 and the driver corrects at +6, so starting anybody above 0 is
        // starting them already on the wrong side.
        uint32 behind = 0;
        if (lk)
        {
            float const lkSum = lk->GetPositionX() + lk->GetPositionY();
            if (Group* group = bot->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* m = ref->GetSource();
                    if (!m || !m->IsAlive() || m->GetMapId() != bot->GetMapId())
                        continue;
                    if ((m->GetPositionX() + m->GetPositionY()) - lkSum > 0.0f)
                        ++behind;
                }
            }
            else if ((bot->GetPositionX() + bot->GetPositionY()) - lkSum > 0.0f)
            {
                behind = 1;
            }
        }

        bool const allAlive = party.dead == 0;
        bool const allNear = party.near_ >= party.living;
        bool const manaOk = party.worstManaPct >= ESCAPE_MANA_PCT;
        bool const clearOfHim = behind == 0;

        if (!allAlive || !allNear || !manaOk || !clearOfHim)
        {
            if (!st.Throttled(DcThrottle::HorEscapeGoLog, TELEMETRY_MS))
                LOG_DEBUG("playerbots.dungeonclear",
                          "[DC:{}] HoR — holding at the point of no return: {}{}{}{}"
                          "(party {}/{} alive, {}/{} within {:.0f}yd of the leader, worst mana "
                          "{:.0f}% of {:.0f}%, {} behind the Lich King)",
                          bot->GetName(),
                          allAlive ? "" : "someone is DEAD (no out-of-combat rez past this). ",
                          allNear ? "" : "someone is out of position. ",
                          manaOk ? "" : "a mana user is short (no drinking past this). ",
                          clearOfHim ? "" : "someone is behind the Lich King. ",
                          party.total - party.dead, party.total, party.near_, party.living,
                          kMusterRadius, party.worstManaPct, ESCAPE_MANA_PCT, behind);

            // Hold ON the muster point. The step before this walked the party
            // here; this keeps a leader that drifted from wandering off while the
            // followers top up.
            DcTransit::TravelTo(bot, botAI, MUSTER_X, MUSTER_Y, MUSTER_Z, MUSTER_LEASH);
            return ObjectiveArriveResult::Running;
        }

        if (!leader->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
            return ObjectiveArriveResult::Running;  // between the cutscene and the flag

        if (DungeonEventExecutor::SelectGossip(bot, leader, /*option*/ 0))
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] HoR — TAKING THE ESCAPE. Party {}/{} alive and formed up, worst "
                     "mana {:.0f}%. From here the Lich King flags everyone into combat every "
                     "second: no drinking, no out-of-combat resurrect, and four walls to clear "
                     "before he reaches the leader.",
                     bot->GetName(), party.living, party.total, party.worstManaPct);

        return inst->GetBossState(DATA_LICH_KING) == IN_PROGRESS
                   ? ObjectiveArriveResult::Done
                   : ObjectiveArriveResult::Running;
    }

    // --- the escape's world probes -----------------------------------------

    // Which WP_STOP the leader is at or heading for. The arithmetic is the pure
    // StopIndexNear in namespace DcHallsOfReflection (shared with the per-follower
    // stay-ahead action, which has to reach the same answer); this only unwraps
    // the creature.
    uint8 LeaderStopIndex(Creature* leader)
    {
        return leader ? StopIndexNear(leader->GetPositionX(), leader->GetPositionY()) : 0;
    }

    // Has the ice wall at this stop opened?
    //
    // The wall GO (201385) is SUMMONED at the Ice Wall Target and never
    // DB-spawned, so "absent" and "GO_STATE_ACTIVE" both mean open — the first
    // because it has not been summoned yet or has been deleted, the second
    // because WallCompleted opened it. Only "present and not ACTIVE" is shut.
    //
    // ...WITH ONE PRECONDITION, because a grid searcher cannot tell "no wall"
    // from "not looking that far". The scan is centred on the BOT, and while the
    // party is crossing a 110-180yd leg the wall it is heading for is simply out
    // of it — which is how tr-20260907-232601-10 logged "wall OPEN" at 126yd and
    // 135yd from stand 2 and "wall shut" once it had closed to 92yd, with nothing
    // in the world having changed. Out of WALL_READ_RANGE the honest answer is
    // UNKNOWN, and unknown reads SHUT: both consumers act only while the party is
    // standing at the wall (Recenter needs atStand; the stall watchdog needs him
    // inside LK_STALL_DIST of a leader who is at that same stop), so nothing is
    // lost by being conservative at range, and a false "open" mutes the one WARN
    // that explains a stalled run.
    bool WallIsOpen(Player* bot, uint8 targetStop)
    {
        if (!bot || targetStop < 1 || targetStop > 4)
            return true;

        HorPoint const& target = ICE_WALL_TARGETS[targetStop - 1];

        if (bot->GetExactDist2d(target.x, target.y) > WALL_READ_RANGE)
            return false;

        std::list<GameObject*> found;
        bot->GetGameObjectListWithEntryInGrid(found, GO_ICE_WALL, WALL_SCAN_YD);
        for (GameObject* go : found)
        {
            if (!go)
                continue;
            if (go->GetExactDist2d(target.x, target.y) > 20.0f)
                continue;
            if (go->GetGoState() != GO_STATE_ACTIVE)
                return false;
        }
        return true;
    }

    // Live escape summons anywhere on the leg, and how many of them are on THIS
    // bot.
    //
    // The second number is what tells "the batch is still alive" (fine — the
    // party is killing it) apart from "the batch is loose" (not fine — walking
    // off leaves it on whoever it did pick). The driver runs on the tank, so
    // `onMe` is literally how much of the pull it is holding.
    //
    // ESCAPE_ADD_SCAN_YD, not a party-sized radius, because the batch is cast AT
    // THE LICH KING and he is at the far end of the leg the party is crossing.
    // A 120yd census read zero for the whole of every transit and for the first
    // twenty seconds at the new stand — a false zero that mutes the stall
    // watchdog and makes `batchLoose` false in the one window Threat exists for.
    // ...AND THE NEAREST ONE THAT IS NOT ON THIS BOT, which is what the pickup
    // walks at. "Not on this bot" rather than "not on anybody": a summon beating
    // on the healer and a summon casting at nobody are the same problem to a tank
    // that cannot reach either, and the kernel's own batchLoose test already uses
    // exactly this distinction (addsAlive > addsTargetingMe).
    //
    // `looseDist` stays 0 when there is none, which is the kernel's "no candidate"
    // sentinel — it tests looseAddDist > engageMelee, so zero can never arm it.
    void CountEscapeAdds(Player* bot, uint32& alive, uint32& onMe, float& looseDist,
                         ObjectGuid& looseGuid)
    {
        alive = 0;
        onMe = 0;
        looseDist = 0.0f;
        looseGuid.Clear();
        if (!bot)
            return;

        static std::vector<uint32> const kAdds = {
            NPC_RAGING_GHOUL, NPC_RISEN_WITCH_DOCTOR, NPC_LUMBERING_ABOMINATION,
        };

        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, kAdds, ESCAPE_ADD_SCAN_YD);

        for (Creature* c : found)
        {
            if (!c || !c->IsAlive() || c->IsImmuneToPC())
                continue;
            ++alive;
            if (c->GetVictim() == bot)
            {
                ++onMe;
                continue;
            }

            float const d = bot->GetExactDist(c);
            if (!looseGuid || d < looseDist)
            {
                looseDist = d;
                looseGuid = c->GetGUID();
            }
        }
    }

    // The stall WARN's payload: which adds are still up, where they stand, and
    // whether they are inside the Lich King's own ring. That last column is the
    // whole reason this line exists — the known way this encounter stalls is a
    // Risen Witch Doctor casting from 20yd on HIS side of the party, which the
    // melee cannot finish without eating 7068 frost per second and which the
    // pressure rule keeps pulling them off.
    std::string DescribeStalledAdds(Player* bot, Creature* lk)
    {
        std::string out;
        if (!bot)
            return out;

        static std::vector<uint32> const kAdds = {
            NPC_RAGING_GHOUL, NPC_RISEN_WITCH_DOCTOR, NPC_LUMBERING_ABOMINATION,
        };

        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, kAdds, ESCAPE_ADD_SCAN_YD);
        for (Creature* c : found)
        {
            if (!c || !c->IsAlive())
                continue;
            float const toLk = lk ? c->GetExactDist2d(lk) : -1.0f;
            if (!out.empty())
                out += "; ";
            out += Acore::StringFormat("{} at {:.0f}% hp, {:.1f}yd from the party, {:.1f}yd from "
                                       "the Lich King{}",
                                       c->GetName(), c->GetHealthPct(), bot->GetExactDist(c),
                                       toLk, toLk >= 0.0f && toLk <= 12.0f
                                                 ? " (INSIDE HIS RING)" : "");
        }
        return out.empty() ? "none visible" : out;
    }

    void HorEscapeLog(Player* bot, DcRunState& st, DcHorEscape::Verdict const& v,
                      DcHorEscape::Inputs const& in)
    {
        if (st.Throttled(DcThrottle::HorEscapeLog, TELEMETRY_MS))
            return;

        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] HoR escape — leader at stop {}, party heading for stand {}, {} — "
                  "{} add(s) up, wall {}, LK {:.1f}yd from me ({:+.1f} on the behind-scalar, "
                  "zap at +20) and {:.1f}yd from the leader (catch at 12.5), {:.1f}yd to the "
                  "stand point -> {}",
                  bot->GetName(), in.leaderStop, v.targetStop,
                  DcHorEscape::StateName(v.state), in.addsAlive,
                  in.wallOpen ? "OPEN" : "shut", in.distToLk, in.sumMinusLkSum,
                  in.lkToLeaderDist, in.distToStand,
                  v.travelToAdd    ? "walking at the loose summon"
                  : v.travelToEdge ? "back to the leash edge"
                  : v.travel       ? "moving up"
                  : v.yieldTick    ? "yielding — fight"
                                   : "holding");
    }

    // --- hook 35: ESCAPE THE LICH KING — the second controller -------------
    ObjectiveArriveResult HorDriveEscape(Player* bot, AiObjectContext* context,
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

        Creature* leader = inst->instance->GetCreature(inst->GetGuidData(NPC_LEADER_PART2));
        Creature* lk = inst->instance->GetCreature(inst->GetGuidData(NPC_LICH_KING));

        DcHorEscape::Inputs in;
        in.active = inst->GetBossState(DATA_LICH_KING) == IN_PROGRESS;
        in.leaderAlive = leader && leader->IsAlive();
        in.leaderHasHarvestSoul = leader && leader->HasAura(SPELL_HARVEST_SOUL);
        in.leaderStop = LeaderStopIndex(leader);

        uint8 const target = DcHorEscape::TargetStopFor(in.leaderStop);
        HorPoint const stand = StandPointFor(target);

        in.wallOpen = WallIsOpen(bot, target);
        in.lkHasWinter = lk && lk->HasAura(SPELL_REMORSELESS_WINTER);
        in.distToLk = lk ? bot->GetExactDist(lk) : 1000.0f;
        in.sumMinusLkSum =
            lk ? (bot->GetPositionX() + bot->GetPositionY()) -
                     (lk->GetPositionX() + lk->GetPositionY())
               : -1000.0f;
        in.lkToLeaderDist = (lk && leader) ? lk->GetExactDist2d(leader) : -1.0f;
        in.distToStand = bot->GetExactDist(stand.x, stand.y, stand.z);
        in.standLeash = STAND_LEASH;
        in.standLeaveLeash = STAND_LEAVE_LEASH;
        ObjectGuid looseGuid;
        CountEscapeAdds(bot, in.addsAlive, in.addsTargetingMe, in.looseAddDist, looseGuid);
        in.engageReach = ENGAGE_REACH_YD;
        in.engageMelee = ENGAGE_MELEE_YD;
        in.engageIdleMs = ENGAGE_IDLE_MS;
        in.idleSinceMs = st.horEscapeIdleMs;
        in.reachedStand = st.horEscapeReachedStand;
        in.partyInCombat = DcCombatFlag::AnyPartyEngagement(bot);

        // The wall standoff, in 2D: the slab is a vertical thing in a corridor
        // that climbs, so height has nothing to say about how far into it a bot
        // has walked. Zeroed at stop 5, where there is no wall at all and
        // standWallDist == 0 switches the kernel's test off.
        if (target >= 1 && target <= 4)
        {
            HorPoint const& wall = ICE_WALL_TARGETS[target - 1];
            in.wallDist = bot->GetExactDist2d(wall.x, wall.y);
            in.standWallDist =
                std::sqrt((stand.x - wall.x) * (stand.x - wall.x) +
                          (stand.y - wall.y) * (stand.y - wall.y));
        }
        in.wallStandoffSlack = WALL_STANDOFF_SLACK;
        in.wallStandoffClear = WALL_STANDOFF_CLEAR;
        in.pressureDist = LK_PRESSURE_DIST;
        in.behindSum = LK_BEHIND_SUM;
        in.nowMs = getMSTime();
        in.state = st.horEscapeState;
        in.stateSinceMs = st.horEscapeStateMs;
        in.stallStop = st.horEscapeStop;
        in.stallReported = st.horEscapeStallReported;
        in.stallDist = LK_STALL_DIST;
        in.stallMs = ESCAPE_STALL_MS;
        in.stopHoldSinceMs = st.horEscapeStopHoldMs;
        in.targetStopSinceMs = st.horEscapeTargetStopMs;
        in.grabThreatMs = GRAB_THREAT_MS;

        DcHorEscape::Verdict const v = DcHorEscape::Decide(in);

        bool const changed = st.horEscapeState != v.storeState;

        // THE DRIVER'S FIRST TICK. Whatever is carrying the bot right now was
        // issued by something that did not know the escape had started — on
        // tp-20260907-221612-1 that was DcRel::Advance's off-line rejoin, aimed at
        // the throne-room anchor 54yd BEHIND the party, issued in the very tick
        // the gossip landed. Kill the glide so this driver's own spline is not
        // queued behind it (the event sets StepsOwnMovement, so nothing else
        // cancels it) and clear the move wait so the steer lands now rather than
        // up to maxWaitForMove later.
        if (st.horEscapeState == static_cast<uint8>(DcHorEscape::State::Done) &&
            v.storeState != static_cast<uint8>(DcHorEscape::State::Done))
        {
            DcMovement::ResolveEscortConflict(bot);
            DcMovement::ClearMovementWait(bot);
        }

        st.horEscapeState = v.storeState;
        st.horEscapeStateMs = v.stateSinceMs;
        st.horEscapeStop = v.stallStop;
        st.horEscapeStallReported = v.stallReported;
        st.horEscapeStopHoldMs = v.stopHoldSinceMs;
        st.horEscapeTargetStopMs = v.targetStopSinceMs;
        st.horEscapeIdleMs = v.idleSinceMs;
        st.horEscapeReachedStand = v.reachedStand;

        HorEscapeLog(bot, st, v, in);

        if (changed && !v.complete)
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] HoR escape — {} (leader at stop {} of 5, wall {}, {} add(s) up)",
                     bot->GetName(), DcHorEscape::StateName(v.state), in.leaderStop,
                     in.wallOpen ? "open" : "shut", in.addsAlive);

        if (v.reportDoomed)
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] HoR escape — THE LEADER HAS BEEN CAUGHT at stop {}. Harvest Soul "
                     "is on her, summonsCount is now 255 so no further wall can ever open, and "
                     "Fury of Frostmourne wipes the party in three seconds. The wall was not "
                     "cleared in time; nothing from here recovers it.",
                     bot->GetName(), in.leaderStop);

        if (v.reportStall)
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] HoR escape — STALLED at wall {}. The Lich King is {:.1f}yd from "
                     "the leader (he catches her at 12.5) and the wall is still shut after "
                     "{}ms. Adds still up: {}. If one of them is inside his ring the melee "
                     "cannot reach it — that is the ranged group's kill.",
                     bot->GetName(), v.targetStop, in.lkToLeaderDist, ESCAPE_STALL_MS,
                     DescribeStalledAdds(bot, lk));

        if (v.complete)
        {
            st.ClearHor();
            return ObjectiveArriveResult::Done;
        }

        if (v.travel)
        {
            // WHERE THIS TICK'S STEER IS AIMED. Three destinations, and which one
            // is chosen is the kernel's call, not this file's:
            //
            //   the stand point   — a leg (Prelude/Advance), the pressure step,
            //                       and the step back off the slab (Recenter);
            //   the band's EDGE   — a drift-out at a stop the party already owns
            //                       (Regroup), so the tank returns to the fight
            //                       rather than to the middle of the floor;
            //   the loose summon  — the pickup (Engage).
            float dx = stand.x, dy = stand.y, dz = stand.z;

            // The arrival leash goes with the destination. STAND_LEASH is wider
            // than every clipping episode there is, which is what made Recenter a
            // twenty-seven-times-per-run no-op — TravelTo returns without moving
            // when the bot is already inside the leash it is handed.
            float leash = STAND_LEASH;
            bool forcePath = false;
            float epsilon = 2.0f;

            if (v.state == DcHorEscape::State::Recenter)
                leash = WALL_RECENTER_LEASH;

            if (v.travelToEdge)
            {
                // No authored polyline to fall back on here — this corridor is a
                // route of anchors, not a transit slice — so HoldPoint gets a null
                // row and returns the snapped chord. That is the right answer on
                // this ground: the recall is a few yards back along a corridor the
                // party has just walked, not a crossing of a C-shaped shelf.
                Position const anchor(stand.x, stand.y, stand.z, 0.0f);
                DcTransit::HoldTarget const hold =
                    DcTransit::HoldPoint(bot, anchor, STAND_LEAVE_LEASH, STAND_EDGE_MARGIN,
                                         STAND_EDGE_SNAP_RADIUS, STAND_EDGE_SNAP_TOLERANCE,
                                         /*route*/ nullptr);
                dx = hold.x;
                dy = hold.y;
                dz = hold.z;
                leash = STAND_EDGE_ARRIVE_LEASH;
                forcePath = hold.viaRoute;
            }
            else if (v.travelToAdd)
            {
                Creature* const add =
                    looseGuid ? inst->instance->GetCreature(looseGuid) : nullptr;
                if (!add || !add->IsAlive())
                {
                    // It died between the census and here. Nothing to walk at and
                    // nothing to correct; hand the tick to the rotation.
                    return ObjectiveArriveResult::Done;
                }

                dx = add->GetPositionX();
                dy = add->GetPositionY();
                dz = add->GetPositionZ();
                leash = ENGAGE_MELEE_YD;

                // A WIDER RE-ISSUE EPSILON, because this destination MOVES. Two
                // yards against a summon running at the party re-plots the spline
                // on most ticks, and re-plotting is what tears down the melee
                // approach the engine lays over it. Four yards is inside the
                // arrival leash, so the walk still ends on the add.
                epsilon = 4.0f;
            }

            // RE-ISSUE FLOOR, the same one the per-follower rung has carried since
            // it was written (DungeonClearHorStayAheadAction::Execute) and the
            // driver did not. The stand point only moves when the leader reaches
            // her next stop, so without this the driver re-plots the identical
            // spline every tick it steers — and because TravelTo has no
            // "already moving" guard by design, each re-plot tears down the melee
            // approach the combat engine laid over it. Paired with the kernel's
            // Schmitt trigger this is what ends the fifty-seven-transition thrash.
            //
            // CLAIMS THE TICK on suppression rather than yielding, for the reason
            // spelled out below: a yield here hands the leg to DcRel::Advance.
            if (st.ThrottledIssue(DcThrottle::HorEscapeIssue, dx, dy, dz, epsilon,
                                  /*windowMs*/ 1500))
                return ObjectiveArriveResult::Running;

            // FORWARD, ALWAYS, and never radially away from him. TravelTo through
            // LongRangePathfinder because the legs between stops run 100-176yd
            // and a bare MovePoint truncates silently past ~30.
            //
            // Claims the tick even when the spline could not be issued, for the
            // same reason the wave driver does: a yield here hands the leg to
            // DcRel::Advance, and while the advance would at least walk FORWARD
            // (the authored route is NO_STOP end to end for exactly that reason),
            // it would do it at the pace of a clear rather than at the pace of a
            // Lich King.
            DcTransit::TravelTo(bot, botAI, dx, dy, dz, leash, forcePath);
            return ObjectiveArriveResult::Running;
        }

        return v.yieldTick ? ObjectiveArriveResult::Done : ObjectiveArriveResult::Running;
    }
}

// Ids 31-35. Violet Hold's are 15-19, Blackwing Lair's 20-21, Halls of Stone's
// 22-23, Halls of Lightning's 24, Utgarde Pinnacle's 25-28 and Pit of Saron's
// 29-30; ids are ONE FLAT SPACE across every dungeon, and AddHook LOG_ERRORs a
// collision rather than silently dropping one.
void RegisterHallsOfReflectionHooks(ObjectiveHookRegistry::HookTable& out)
{
    using namespace DcHallsOfReflection;

    ObjectiveHookRegistry::AddHook(out, HOOK_HOR_INTRO_GOSSIP, &HorIntroGossip);
    ObjectiveHookRegistry::AddHook(out, HOOK_HOR_WAVES,        &HorDriveWaves);
    ObjectiveHookRegistry::AddHook(out, HOOK_HOR_THRONE,       &HorThroneRoom);
    ObjectiveHookRegistry::AddHook(out, HOOK_HOR_ESCAPE_GO,    &HorEscapeGo);
    ObjectiveHookRegistry::AddHook(out, HOOK_HOR_ESCAPE,       &HorDriveEscape);
}
