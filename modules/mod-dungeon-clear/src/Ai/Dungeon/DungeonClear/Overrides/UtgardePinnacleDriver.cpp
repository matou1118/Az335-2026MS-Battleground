/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <algorithm>
#include <cstdint>
#include <list>
#include <vector>

#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransit.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"

// --- Utgarde Pinnacle (map 575) — the imperative half ----------------------
//
// Four hooks, and only one of them is a controller. The other three are the
// short kind that would ordinarily live in ObjectiveHookRegistry.cpp's own
// table; they are here instead because they share this map's constants and its
// reasoning with the driver, and splitting a dungeon's four hooks across two
// files to satisfy a size rule would cost more than it saves.
//
//   25  StartSvalaEncounter   — forge areatrigger 5140
//   26  EnterSkadiGauntlet    — gather, then forge areatrigger 4991
//   27  DriveGraufHarpoon     — THE CONTROLLER: phase 1 of Skadi
//   28  HoldSvalaRitual       — retarget off an airborne boss onto the channelers
//
// WHY TWO OF THEM FORGE PACKETS. A bot never autonomously sends
// CMSG_AREATRIGGER, and both of this dungeon's first-and-third encounters are
// started by one. In a `.dc test` run the harness relay already fires both
// (DcTestAreaTriggers::Arm picks up any trigger with an areatrigger_scripts row
// and no areatrigger_teleport row, which is exactly 5140 and 4991 on this map),
// so these hooks are what make Utgarde Pinnacle work for an ORDINARY bot party —
// and the belt-and-braces that stops a run hanging if the relay's own gate ever
// fails. Same supported path ReachAreaTriggerAction uses; the core range-checks
// the bot and runs the SmartAI script for real. This is Blackrock Depths' Ring
// of Law shape (hook 1) on a map with two of them.
//
// THE RETURN CONTRACT IS BLACKWING LAIR'S throughout, and getting it backwards
// wipes parties:
//
//   Running => "I am steering." Claims the tick.
//   Done    => "Nothing to steer." YIELDS the tick (the stepsOwnMovement branch
//              in DcRunEventAction) so the stock combat engine can fight.
//
// On this map the yield matters more than on any other the module runs, because
// from the moment AT 4991 trips the party is in permanent combat against a pump
// with no end condition. A driver that claimed every tick of phase 1 would leave
// five bots standing in an add stream with no rotation.

namespace
{
    using namespace DcUtgardePinnacle;

    // Interact reach for the launcher click. The same 5yd the event executor's
    // UseGO step enforces (DC_EVENT_GO_USE_RANGE), kept here rather than borrowed
    // because this hook calls GameObject::Use directly and must not silently
    // diverge from the contract the declarative path advertises.
    constexpr float HARPOON_USE_RANGE = 5.0f;

    // How close a glide's destination has to be to the pocket before the driver
    // believes it is already on its way there. Wide enough to absorb the
    // pathfinder's own arrival slop, far tighter than the 161yd leg it gates.
    constexpr float POCKET_GLIDE_EPSILON = 4.0f;

    // Fire the real area trigger from `bot`. Identical to the Ring of Law's forge
    // (ObjectiveHookRegistry.cpp hook 1) and deliberately so: the core validates
    // that the bot is inside the trigger's own volume and then runs the script,
    // which means a forge from outside the box is a harmless no-op rather than a
    // cheat, and the hooks below can call it every tick without bookkeeping.
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
    // and how many there are in total. One walk of the group for both.
    //
    // LIVING MEMBERS ON THIS MAP ONLY, which is the whole reason this is not a
    // plain distance loop. A corpse-running member is the rez ladder's business
    // and a member who never zoned in must never pin a gate — count either and
    // the quorum below becomes unreachable rather than strict.
    struct UpPartyView
    {
        uint32 living = 0;  // leader included
        uint32 near_ = 0;   // ...of which, inside the radius
        float  furthest = 0.0f;
    };

    UpPartyView UpParty(Player* bot, float x, float y, float z, float radius)
    {
        UpPartyView v;
        if (!bot)
            return v;

        v.living = 1;
        v.near_ = bot->GetExactDist(x, y, z) <= radius ? 1 : 0;

        Group* group = bot->GetGroup();
        if (!group)
            return v;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() ||
                member->GetMapId() != bot->GetMapId())
                continue;

            ++v.living;
            float const d = member->GetExactDist(x, y, z);
            if (d <= radius)
                ++v.near_;
            if (d > v.furthest)
                v.furthest = d;
        }
        return v;
    }

    // Is an escort glide already in flight toward (x,y,z)?
    //
    // This is what lets the harpoon driver YIELD while it walks. DcTransit::TravelTo
    // returns true both when it ISSUES a spline and when it finds one already
    // riding, and a driver that claimed the tick on both would spend the whole
    // 161yd walk east with no rotation — twenty-five seconds of a tank not
    // swinging inside an add stream that never stops. Asking the question
    // separately lets the driver keep the movement and give the tick back.
    bool UpGlidingTo(Player* bot, float x, float y, float z)
    {
        if (!bot)
            return false;

        MotionMaster* mm = bot->GetMotionMaster();
        if (!mm || mm->GetCurrentMovementGeneratorType() != ESCORT_MOTION_TYPE)
            return false;

        float dx, dy, dz;
        if (!mm->GetDestination(dx, dy, dz))
            return false;

        float const ex = dx - x, ey = dy - y, ez = dz - z;
        return ex * ex + ey * ey + ez * ez <= POCKET_GLIDE_EPSILON * POCKET_GLIDE_EPSILON;
    }

    // Point the leader at `target` and start swinging.
    //
    // Lifted from the Violet Hold driver's VhEngageTarget for the same reason it
    // exists there: EngageWithTarget / AttackStart make the CREATURE attack the
    // BOT, and nothing in that direction ever gives the bot a victim. A player
    // does not auto-retaliate — something has to call Player::Attack. Normally
    // that is stock targeting; here the driver has to be sure the tank commits to
    // a CHANNELER specifically, on the tick the ritual starts rather than whenever
    // stock targeting next re-picks.
    //
    // Idempotent by the GetVictim() guard, so calling it every tick never fights
    // stock combat's target management once the tank is already on the right mob.
    bool UpEngageTarget(Player* bot, AiObjectContext* context, Creature* target)
    {
        if (!bot || !target || !target->IsAlive())
            return false;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;

        if (bot->GetVictim() == target)
            return false;

        bot->SetSelection(target->GetGUID());
        if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
            ServerFacade::instance().SetFacingTo(bot, target);
        if (context)
            context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(target);
        bot->Attack(target, botAI->IsMelee(bot));

        // FLIP THE BOT ONTO THE COMBAT ENGINE. Engine transitions in
        // mod-playerbots are ACTION-DRIVEN, not derived from bot->IsInCombat():
        // Player::Attack alone gives the bot a victim and the CORE then drives
        // auto-attack swings with no AI involvement, so a force-attacked bot that
        // is not flipped sits on the NON-combat engine with no class rotation and
        // melees the mob down one white hit at a time. ChangeEngine no-ops when
        // the engine already matches.
        if (botAI->GetState() != BOT_STATE_COMBAT)
        {
            botAI->ChangeEngine(BOT_STATE_COMBAT);
            botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
        }
        return true;
    }

    // --- hook 25: START SVALA'S ENCOUNTER ---------------------------------
    //
    // The ONLY entry point to boss_svala's gate is smart_scripts source_type 2
    // entry 5140 -> SET_DATA 1,1, and its SetData handler's first act is
    // instance->SetData(DATA_SVALA, IN_PROGRESS). So the instance's own state
    // word is an exact, race-free receipt for "the trigger took", and the hook
    // needs no bookkeeping of its own.
    //
    // DONE ON ANYTHING BUT NOT_STARTED, which covers three cases with one test:
    // the intro is running (IN_PROGRESS), she is already dead (DONE), and — the
    // one worth naming — a WIPE RETRY. boss_svalaAI's `Started` is a member that
    // Reset() does not clear, so after a wipe the encounter is re-entered simply
    // by walking back and attacking; the SetData handler would refuse a second
    // trigger anyway (`|| Started ||`), and the event's WaitForSpawn is long since
    // satisfied. The whole event falls through in one tick, which is correct.
    ObjectiveArriveResult StartSvalaEncounter(Player* bot, AiObjectContext* /*context*/,
                                              DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;  // not in the instance yet

        if (inst->GetData(DATA_SVALA) != NOT_STARTED)
            return ObjectiveArriveResult::Done;

        ForgeAreaTrigger(bot, AREATRIGGER_SVALA);

        // Log the fire that TOOK, not every fire: while the leader is short of the
        // box this runs every tick, and one line per start is the whole signal.
        if (inst->GetData(DATA_SVALA) != NOT_STARTED)
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Utgarde Pinnacle — Svala's intro started by forged areatrigger "
                     "{} at ({:.1f},{:.1f},{:.1f})",
                     bot->GetName(), AREATRIGGER_SVALA, bot->GetPositionX(),
                     bot->GetPositionY(), bot->GetPositionZ());
        return ObjectiveArriveResult::Running;
    }

    // --- hook 26: ENTER SKADI'S GAUNTLET ----------------------------------
    //
    // The same forge as hook 25, with a GATHER GATE in front of it, because this
    // trigger is a ONE-WAY DOOR and the one before it is not.
    //
    // Tripping AT 4991 arms a summon pump with no end condition and a hall-wide
    // DoZoneInCombat, and there is no way back out: spell_area autocasts 47546 on
    // every player in area 1196 (the whole instance), 47547 fires off it every 5s
    // restricted to Flame Breath Triggers within 40yd with a 7s aura, and every 6s
    // the reset trigger at (397.0, -511.5) counts triggers still carrying it and,
    // on ZERO, evades Skadi and resets everything. So a party that enters strung
    // out has no option to regroup outside and come back — it either holds the
    // hall or loses the attempt.
    //
    // THE QUORUM IS THREE OF FOUR FOLLOWERS, not four of four, and the difference
    // is deliberate: one bot that cannot path in must never hold the other three
    // on the threshold, and stranded recovery (relevance 42) sits above this whole
    // ladder and owns that member. Dead members do not count against it either —
    // UpParty counts the LIVING — so the gate opens as soon as the party is up,
    // and the rez ladder, not this hook, is what gets it there.
    //
    // NO WATCHDOG OF ITS OWN. The step's Timeout bounds the wait, and a REQUIRED
    // step that fails Stalls the run and parks the tank at the threshold with the
    // human told why — which for a one-way door is the right failure. A soft
    // release that entered anyway would trade a visible stall for an invisible
    // wipe.
    ObjectiveArriveResult EnterSkadiGauntlet(Player* bot, AiObjectContext* /*context*/,
                                             DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;

        if (inst->GetData(DATA_SKADI) != NOT_STARTED)
            return ObjectiveArriveResult::Done;

        UpPartyView const party =
            UpParty(bot, AT_SKADI_X, AT_SKADI_Y, AT_SKADI_Z, GAUNTLET_GATHER_RADIUS);
        bool const quorum =
            party.living <= 1 ||
            static_cast<float>(party.near_) >=
                static_cast<float>(party.living) * GAUNTLET_GATHER_QUORUM;
        if (!quorum)
            return ObjectiveArriveResult::Running;

        ForgeAreaTrigger(bot, AREATRIGGER_SKADI);

        if (inst->GetData(DATA_SKADI) != NOT_STARTED)
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Utgarde Pinnacle — Skadi's gauntlet entered by forged "
                     "areatrigger {} with {}/{} of the party on the threshold (furthest "
                     "{:.1f}yd)",
                     bot->GetName(), AREATRIGGER_SKADI, party.near_, party.living,
                     party.furthest);
        return ObjectiveArriveResult::Running;
    }

    // --- hook 28: SVALA'S RITUAL — HOLD -----------------------------------
    //
    // Twenty-five seconds during which the boss is rooted twenty yards above her
    // own platform and the three Ritual Channelers are the entire fight.
    //
    // ONE ACT: point the leader at the nearest living channeler. That is the whole
    // hook, and the smallness is the point — the failure it fixes is a TARGETING
    // one. She is not flagged non-attackable up there (AzerothCore's ritual never
    // adds the flag its own FINISH branch removes), so she stays selectable and
    // simply cannot be reached: melee bots hold their target, walk under her and
    // swing at nothing, while the channelers — NullCreatureAI, never move, never
    // melee, ten million threat — hold Paralyze 48278 on the sacrificed member.
    // That stun has INFINITE duration and ends only when the caster dies, so a
    // party that does not switch never gets its fifth member back.
    //
    // The followers need no hook of their own: the assist ladder brings them onto
    // whatever the leader is fighting, and that is the cheapest correct answer.
    //
    // NEAREST, NOT LOWEST-HEALTH. All three are identical, they stand 4-8yd apart
    // around one altar, and the leader is standing among them — so "nearest" costs
    // nothing to compute and never makes the tank cross the fight.
    ObjectiveArriveResult HoldSvalaRitual(Player* bot, AiObjectContext* context,
                                          DungeonBossInfo const& /*info*/)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return ObjectiveArriveResult::Done;

        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, NPC_RITUAL_CHANNELER, RITUAL_SCAN);

        Creature* best = nullptr;
        float bestDist = 0.0f;
        for (Creature* c : found)
        {
            if (!c || !c->IsAlive())
                continue;
            float const d = bot->GetExactDist(c);
            if (!best || d < bestDist)
            {
                best = c;
                bestDist = d;
            }
        }

        // No channeler in reach. Nothing to redirect onto, so give the tick back
        // rather than hold a party that is already fighting the right thing.
        if (!best)
            return ObjectiveArriveResult::Done;

        // Claim the tick ONLY on the tick the target actually changed. Once the
        // tank is on a channeler the stock combat engine owns the fight, and a
        // rung above the movers that kept returning true would starve the rotation
        // it exists to redirect.
        return UpEngageTarget(bot, context, best) ? ObjectiveArriveResult::Running
                                                  : ObjectiveArriveResult::Done;
    }

    // --- hook 27: BRING DOWN GRAUF — the controller ------------------------
    //
    // The per-tick telemetry line, throttled. Without it a failed phase 1 says
    // nothing about WHICH mechanism is still biting: "the leader never reached the
    // pocket" reads exactly like "the shot window never opened" in a log that only
    // shows a party dying in a hallway.
    void HarpoonLog(Player* bot, DcRunState& st, Creature* grauf, float toPocket,
                    float toBreach, char const* verdict)
    {
        if (st.Throttled(DcThrottle::UpHarpoonLog, HARPOON_TELEMETRY_MS))
            return;

        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] UP harpoon — pocket {:.1f}yd, Grauf at ({:.0f},{:.0f},{:.0f}) "
                  "{:.1f}yd from the breach, {:.0f}% hp -> {}",
                  bot->GetName(), toPocket, grauf->GetPositionX(), grauf->GetPositionY(),
                  grauf->GetPositionZ(), toBreach,
                  grauf->GetMaxHealth()
                      ? 100.0f * float(grauf->GetHealth()) / float(grauf->GetMaxHealth())
                      : 0.0f,
                  verdict);
    }

    // How far Grauf is from the NEARER of the two breach hold points.
    //
    // TWO POINTS, because the first hover is the end of PATH_INITIAL (2689300,
    // last waypoint 523.20/-548.99/114.87) and every later one the end of
    // PATH_LEFT / PATH_RIGHT (2689302 / 2689301, last waypoint
    // 520.48/-541.56/119.84). They are 8.6yd apart, both ~46-52yd from launcher
    // 192175 and both within 15 degrees of the World Trigger's own facing, so one
    // launcher serves every lap of the fight.
    float DistToBreach(Creature* grauf)
    {
        float const a = grauf->GetExactDist(BREACH_A_X, BREACH_A_Y, BREACH_A_Z);
        float const b = grauf->GetExactDist(BREACH_B_X, BREACH_B_Y, BREACH_B_Z);
        return std::min(a, b);
    }

    ObjectiveArriveResult DriveGraufHarpoon(Player* bot, AiObjectContext* context,
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

        // PHASE 1 IS OVER THE MOMENT THE DRAKE DIES. boss_skadi_graufAI::JustDied
        // calls skadi->ExitVehicle() + ACTION_PHASE2, she lands at (476.8, -511.2)
        // — ten yards from the pocket the party is already standing in — becomes
        // selectable and goes REACT_AGGRESSIVE. That is an ordinary boss fight the
        // clear already handles, so yield and stop steering.
        //
        // Read through the ObjectData store rather than a grid scan: he flies a lap
        // that takes him 200yd west and 30yd up, and any scan radius honest enough
        // to be called a room scan reads "not there" for most of it.
        Creature* grauf = inst->GetCreature(DATA_GRAUF);
        if (!grauf || !grauf->IsAlive())
            return ObjectiveArriveResult::Done;

        DcRunState& st = DcRun::Of(context);

        float const toPocket = bot->GetExactDist(POCKET_X, POCKET_Y, POCKET_Z);
        float const toBreach = DistToBreach(grauf);

        // --- 1. get to the pocket ------------------------------------------
        //
        // 161yd dead east down the gauntlet hall (detour x1.03), which is past what
        // a bare MovePoint delivers — the engine's PathGenerator caps a generated
        // path at 74 points and TRUNCATES SILENTLY, leaving the bot standing still
        // with no failure to observe ([[ac-moveto-caps-at-74-smoothed-points]]).
        // So it goes through LongRangePathfinder and a real spline, which is what
        // DcTransit::TravelTo is.
        //
        // No forcePath: the hall is straight, so TravelTo's own straight-line
        // proxy for "is this long enough to truncate" is exactly right here.
        //
        // AND THE TICK IS GIVEN BACK WHILE IT RIDES. TravelTo returns true both
        // when it issues a spline and when it finds one already in flight; claiming
        // the tick on both would mean twenty-five seconds of a tank walking through
        // an add stream with no rotation. UpGlidingTo separates the two.
        if (DcTransit::TravelTo(bot, botAI, POCKET_X, POCKET_Y, POCKET_Z, POCKET_LEASH))
        {
            HarpoonLog(bot, st, grauf, toPocket, toBreach, "walking to the pocket");
            return UpGlidingTo(bot, POCKET_X, POCKET_Y, POCKET_Z)
                       ? ObjectiveArriveResult::Done
                       : ObjectiveArriveResult::Running;
        }

        // --- 2. is the shot window open? -----------------------------------
        //
        // Grauf announces the window with Talk(EMOTE_ON_RANGE) and then hovers at
        // the breach for TEN SECONDS before flying his next lap, and it repeats
        // every lap. POSITION is the signal rather than the emote: creature_text is
        // not readable from here and a missed emote would cost a whole lap, where a
        // position test cannot miss.
        //
        // Outside the window, YIELD. There is nothing to steer and four bots plus
        // the tank have an add stream to fight — this is the branch the encounter
        // spends most of its time in, and it is the reason StepsOwnMovement is on
        // the event row.
        if (toBreach > BREACH_RADIUS)
        {
            HarpoonLog(bot, st, grauf, toPocket, toBreach, "holding — mid-lap");
            return ObjectiveArriveResult::Done;
        }

        // --- 3. fire ---------------------------------------------------------
        if (st.Throttled(DcThrottle::UpHarpoonFire, HARPOON_REFIRE_MS))
            return ObjectiveArriveResult::Done;

        GameObject* launcher = bot->FindNearestGameObject(GO_HARPOON_LAUNCHER, HARPOON_SEARCH);
        if (!launcher)
        {
            // Standing in the pocket with no launcher in range is an authoring
            // failure (a moved pocket, a changed GO spawn), not a game state, and
            // it is invisible from the outside — the party simply never kills the
            // drake. Say so, throttled.
            if (!st.Throttled(DcThrottle::UpHarpoonMissingLog, HARPOON_TELEMETRY_MS))
                LOG_WARN("playerbots.dungeonclear",
                         "[DC:{}] UP harpoon — in the pocket ({:.1f}yd) and Harpoon Launcher "
                         "{} is not within {:.0f}yd; phase 1 cannot be won from here",
                         bot->GetName(), toPocket, GO_HARPOON_LAUNCHER, HARPOON_SEARCH);
            return ObjectiveArriveResult::Done;
        }

        // GO_FLAG_IN_USE is set by the GOOBER branch for the length of the 1000ms
        // autoclose and cleared by GameObject::Update; GameObject::Use() early-
        // returns on either flag, so a click here would be swallowed. Wait a tick.
        if (launcher->HasGameObjectFlag(GO_FLAG_IN_USE) ||
            launcher->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
            return ObjectiveArriveResult::Done;

        // The pocket leash is 3yd and the launcher sits 1.2yd from the pocket, so
        // this cannot normally fail — but the leash is a re-walk trigger, not a
        // guarantee, and a bot shoved by an add on the tick the window opens must
        // not burn its re-fire floor on a click the core would reject.
        if (!bot->IsWithinDistInMap(launcher, HARPOON_USE_RANGE))
            return ObjectiveArriveResult::Done;

        // THE WHOLE MECHANISM, in one call. Use() -> the GOOBER branch casts 48641
        // from the BOT at the World Trigger (19871) two yards away; the TRIGGER —
        // a creature, which is exactly how this bypasses Grauf's IMMUNE_TO_PC —
        // casts 48642 in a 60yd cone already aimed at the breach, and its script
        // sets the damage to 35% of the drake's maximum health. Three of these is
        // 105%, and nothing else in the encounter can hurt him at all.
        //
        // The LOCK_KEY_ITEM 37372 on the launcher is not enforced server-side for
        // GOOBER use — HandleGameObjectUseOpcode checks only distance and mover
        // state and the GOOBER branch never consults the lock — so no harpoon is
        // farmed, carried or granted anywhere in this module.
        launcher->Use(bot);

        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] UP harpoon — fired launcher {} at Grauf ({:.1f}yd from the breach, "
                 "{:.0f}% hp)",
                 bot->GetName(), GO_HARPOON_LAUNCHER, toBreach,
                 grauf->GetMaxHealth()
                     ? 100.0f * float(grauf->GetHealth()) / float(grauf->GetMaxHealth())
                     : 0.0f);
        return ObjectiveArriveResult::Running;
    }
}

// Ids 25-28. The Violet Hold's are 15-19, Blackwing Lair's 20-21, Halls of
// Stone's 22-23 and Halls of Lightning's 24; ids are ONE FLAT SPACE across every
// dungeon, and AddHook LOG_ERRORs a collision rather than silently dropping one.
void RegisterUtgardePinnacleHooks(ObjectiveHookRegistry::HookTable& out)
{
    using namespace DcUtgardePinnacle;

    ObjectiveHookRegistry::AddHook(out, HOOK_SVALA_AREATRIGGER, &StartSvalaEncounter);
    ObjectiveHookRegistry::AddHook(out, HOOK_SKADI_AREATRIGGER, &EnterSkadiGauntlet);
    ObjectiveHookRegistry::AddHook(out, HOOK_GRAUF_HARPOON,     &DriveGraufHarpoon);
    ObjectiveHookRegistry::AddHook(out, HOOK_SVALA_RITUAL_HOLD, &HoldSvalaRitual);
}
